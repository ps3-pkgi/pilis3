#include "stream.h"
#include "platform/log.h"
#include "platform/sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <sys/mutex.h>
#include <sys/sem.h>
#include <sys/thread.h>
#include <sys/systime.h>

static void lock_stream(NetStream *s)
{
    sysMutexLock((sys_mutex_t)(u64)s->lock, 0);
}

static void unlock_stream(NetStream *s)
{
    sysMutexUnlock((sys_mutex_t)(u64)s->lock);
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    NetStream *s = (NetStream *)userdata;
    u32 len = (u32)(size * nmemb);
    u32 written = 0;

    while (written < len)
    {
        u32 chunk;

        if (s->abort)
            return 0;

        lock_stream(s);
        chunk = s->cap - s->used;
        if (chunk == 0)
        {
            unlock_stream(s);
            sysUsleep(2000);
            continue;
        }
        if (chunk > len - written)
            chunk = len - written;

        {
            u32 end = (s->head + chunk) % s->cap;
            if (end > s->head)
            {
                memcpy(s->buf + s->head, ptr + written, chunk);
            }
            else
            {
                u32 first = s->cap - s->head;
                if (first > chunk)
                    first = chunk;
                memcpy(s->buf + s->head, ptr + written, first);
                memcpy(s->buf, ptr + written + first, chunk - first);
            }
        }

        s->head = (s->head + chunk) % s->cap;
        s->used += chunk;
        unlock_stream(s);

        sysSemPost((sys_sem_t)(u64)s->sem, 1);
        written += chunk;
    }

    return len;
}

static void stream_thread(void *arg)
{
    NetStream *s = (NetStream *)arg;
    CURL *curl;
    CURLcode rc;
    long status = 0;
    char range[64];

    curl = curl_easy_init();
    if (!curl)
    {
        s->curl_code = -1;
        s->error = 1;
        s->eof = 1;
        sysSemPost((sys_sem_t)(u64)s->sem, 1);
        sysThreadExit(0);
        return;
    }

    curl_easy_setopt(curl, CURLOPT_URL, s->url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "wiliwili");
    curl_easy_setopt(curl, CURLOPT_REFERER, "https://www.bilibili.com/");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, s);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    if (s->range_start > 0)
    {
        snprintf(range, sizeof(range), "%lld-", s->range_start);
        curl_easy_setopt(curl, CURLOPT_RANGE, range);
    }

    log_printf("[stream] GET %.120s%s", s->url,
               strlen(s->url) > 120 ? "..." : "");

    rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    s->curl_code = (int)rc;
    s->http_status = status;

    if (rc != CURLE_OK)
    {
        if (s->abort)
            log_printf("[stream] aborted rc=%d http=%ld", (int)rc, status);
        else
        {
            log_printf("[stream] curl rc=%d http=%ld", (int)rc, status);
            s->error = 1;
        }
    }
    else if (status >= 400 && !s->abort)
    {
        log_printf("[stream] HTTP %ld", status);
        s->error = 1;
    }

    s->eof = 1;
    sysSemPost((sys_sem_t)(u64)s->sem, 1);
    curl_easy_cleanup(curl);
    sysThreadExit(0);
}

int stream_start(NetStream *s, const char *url, u32 capacity, long long range_start)
{
    return stream_start_prefixed(s, url, capacity, NULL, 0, range_start);
}

int stream_start_prefixed(NetStream *s, const char *url, u32 capacity,
                          const u8 *prefix, u32 prefix_len,
                          long long range_start)
{
    sys_mutex_attr_t mattr;
    int rc;

    memset(s, 0, sizeof(*s));
    s->cap = capacity;
    s->range_start = range_start;
    snprintf(s->url, sizeof(s->url), "%s", url);

    if (prefix_len > capacity)
        return -1;

    s->buf = (u8 *)malloc(capacity);
    if (!s->buf)
        return -1;

    if (prefix && prefix_len)
    {
        memcpy(s->buf, prefix, prefix_len);
        s->head = prefix_len;
        s->used = prefix_len;
    }

    memset(&mattr, 0, sizeof(mattr));
    mattr.attr_protocol = SYS_MUTEX_PROTOCOL_FIFO;
    mattr.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
    mattr.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED;
    mattr.attr_adaptive = SYS_MUTEX_ATTR_ADAPTIVE;
    snprintf(mattr.name, sizeof(mattr.name), "strm");

    sys_mutex_t mutex;
    rc = sysMutexCreate(&mutex, &mattr);
    if (rc < 0)
    {
        free(s->buf);
        s->buf = NULL;
        return -1;
    }
    s->lock = (void *)(u64)mutex;

    sys_sem_t sem;
    rc = sys_sem_create(&sem, "p3strm", 0, 1);
    if (rc < 0)
    {
        sysMutexDestroy(mutex);
        free(s->buf);
        s->buf = NULL;
        return -1;
    }
    s->sem = (int)(u64)sem;

    sys_ppu_thread_t thread;
    rc = sysThreadCreate(&thread, stream_thread, s, 1000, 0x20000, 0, "pilis3str");
    if (rc < 0)
    {
        sysSemDestroy(sem);
        sysMutexDestroy(mutex);
        free(s->buf);
        s->buf = NULL;
        return -1;
    }

    s->thread = (u64)thread;
    s->started = 1;
    return 0;
}

int stream_read(NetStream *s, u8 *dst, u32 len)
{
    for (;;)
    {
        u32 n = 0;

        if (s->abort)
            return -1;

        lock_stream(s);
        if (s->used > 0)
        {
            n = len < s->used ? len : s->used;
            {
                u32 end = (s->tail + n) % s->cap;
                if (end > s->tail)
                {
                    memcpy(dst, s->buf + s->tail, n);
                }
                else
                {
                    u32 first = s->cap - s->tail;
                    if (first > n)
                        first = n;
                    memcpy(dst, s->buf + s->tail, first);
                    memcpy(dst + first, s->buf, n - first);
                }
            }
            s->tail = (s->tail + n) % s->cap;
            s->used -= n;
            unlock_stream(s);
            return (int)n;
        }

        if (s->eof)
        {
            unlock_stream(s);
            return 0;
        }
        unlock_stream(s);

        sysSemWait((sys_sem_t)(u64)s->sem, 100000);
    }
}

void stream_abort(NetStream *s)
{
    if (!s || !s->started)
        return;
    s->abort = 1;
    sysSemPost((sys_sem_t)(u64)s->sem, 1);
}

void stream_stop(NetStream *s)
{
    if (!s->started)
    {
        if (s->buf)
            free(s->buf);
        s->buf = NULL;
        return;
    }

    s->abort = 1;
    sysSemPost((sys_sem_t)(u64)s->sem, 1);
    sysThreadJoin((sys_ppu_thread_t)s->thread, NULL);

    sysSemDestroy((sys_sem_t)(u64)s->sem);
    sysMutexDestroy((sys_mutex_t)(u64)s->lock);

    if (s->buf)
        free(s->buf);
    s->buf = NULL;
    s->started = 0;
}
