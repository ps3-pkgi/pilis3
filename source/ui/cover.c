#include "cover.h"
#include "net/http.h"
#include "platform/log.h"
#include "platform/sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/mutex.h>
#include <sys/sem.h>
#include <sys/thread.h>
#include <sys/systime.h>

#define COVER_SLOTS 64
#define COVER_MAX_JPEG (2 * 1024 * 1024)
#define COVER_DECODE_PER_FRAME 2

typedef enum
{
    COVER_EMPTY = 0,
    COVER_QUEUED,
    COVER_DOWNLOADING,
    COVER_READY,
    COVER_DECODING,
    COVER_LOADED,
    COVER_FAILED
} CoverState;

typedef struct
{
    char key[24];
    char url[512];
    u8 *jpeg;
    size_t jpeg_len;
    ya2d_Texture *tex;
    CoverState state;
    u64 last_used;
} CoverSlot;

static CoverSlot g_slots[COVER_SLOTS];
static sys_mutex_t g_lock;
static sys_sem_t g_sem;
static sys_ppu_thread_t g_thread;
static int g_started;
static volatile int g_quit;
static int g_decoded;

static void lock(void)
{
    sysMutexLock(g_lock, 0);
}

static void unlock(void)
{
    sysMutexUnlock(g_lock);
}

static void slot_clear(CoverSlot *s)
{
    if (s->tex)
    {
        ya2d_freeTexture(s->tex);
        s->tex = NULL;
    }
    if (s->jpeg)
    {
        free(s->jpeg);
        s->jpeg = NULL;
    }
    s->jpeg_len = 0;
    s->key[0] = 0;
    s->url[0] = 0;
    s->state = COVER_EMPTY;
    s->last_used = 0;
}

static void cover_thread(void *arg)
{
    (void)arg;

    for (;;)
    {
        CoverSlot *slot = NULL;
        char url[512];
        char key[24];

        sysSemWait(g_sem, 0);
        if (g_quit)
            break;

        lock();
        for (int i = 0; i < COVER_SLOTS; ++i)
        {
            if (g_slots[i].state == COVER_QUEUED)
            {
                slot = &g_slots[i];
                slot->state = COVER_DOWNLOADING;
                snprintf(url, sizeof(url), "%s", slot->url);
                snprintf(key, sizeof(key), "%s", slot->key);
                break;
            }
        }
        unlock();

        if (!slot)
            continue;

        HttpResponse res;
        int ok = http_get(url, &res, 15);

        lock();
        slot->state = COVER_FAILED;
        if (ok == 0 && res.body && res.len > 0 && res.len < COVER_MAX_JPEG)
        {
            slot->jpeg = (u8 *)malloc(res.len);
            if (slot->jpeg)
            {
                memcpy(slot->jpeg, res.body, res.len);
                slot->jpeg_len = res.len;
                slot->state = COVER_READY;
            }
        }
        unlock();

        if (slot->state == COVER_FAILED)
            log_printf("[cover] failed %s", key);

        http_response_free(&res);
    }

    sysThreadExit(0);
}

void cover_init(void)
{
    sys_mutex_attr_t mattr;
    int rc;

    memset(&mattr, 0, sizeof(mattr));
    mattr.attr_protocol = SYS_MUTEX_PROTOCOL_FIFO;
    mattr.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
    mattr.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED;
    mattr.attr_adaptive = SYS_MUTEX_ATTR_ADAPTIVE;
    snprintf(mattr.name, sizeof(mattr.name), "cover");

    rc = sysMutexCreate(&g_lock, &mattr);
    if (rc < 0)
        return;

    rc = sys_sem_create(&g_sem, "p3cov", 0, COVER_SLOTS);
    if (rc < 0)
        return;

    rc = sysThreadCreate(&g_thread, cover_thread, NULL, 1200, 0x20000, 0, "pilis3cov");
    if (rc < 0)
        return;

    g_started = 1;
    log_printf("[cover] worker started");
}

void cover_shutdown(void)
{
    if (!g_started)
        return;

    g_quit = 1;
    sysSemPost(g_sem, 1);
    sysThreadJoin(g_thread, NULL);
    g_started = 0;

    for (int i = 0; i < COVER_SLOTS; ++i)
        slot_clear(&g_slots[i]);

    sysSemDestroy(g_sem);
    sysMutexDestroy(g_lock);
}

void cover_frame(void)
{
    g_decoded = 0;
}

static CoverSlot *find_key(const char *key)
{
    for (int i = 0; i < COVER_SLOTS; ++i)
    {
        if (g_slots[i].key[0] && !strcmp(g_slots[i].key, key))
            return &g_slots[i];
    }
    return NULL;
}

static CoverSlot *alloc_slot(void)
{
    CoverSlot *lru = NULL;

    for (int i = 0; i < COVER_SLOTS; ++i)
    {
        if (g_slots[i].state == COVER_EMPTY)
            return &g_slots[i];
    }

    for (int i = 0; i < COVER_SLOTS; ++i)
    {
        CoverSlot *s = &g_slots[i];
        if (s->state == COVER_LOADED || s->state == COVER_FAILED)
        {
            if (!lru || s->last_used < lru->last_used)
                lru = s;
        }
    }

    return lru;
}

void cover_request(const char *key, const char *url)
{
    CoverSlot *slot;

    if (!g_started || !key || !*key || !url || !*url)
        return;

    lock();
    slot = find_key(key);
    if (slot)
    {
        slot->last_used = sys_ms();
        if (slot->state == COVER_EMPTY)
            slot->state = COVER_QUEUED;
        unlock();
        return;
    }

    slot = alloc_slot();
    if (!slot)
    {
        unlock();
        return;
    }

    slot_clear(slot);
    snprintf(slot->key, sizeof(slot->key), "%s", key);
    snprintf(slot->url, sizeof(slot->url), "%s", url);
    slot->state = COVER_QUEUED;
    slot->last_used = sys_ms();
    unlock();

    sysSemPost(g_sem, 1);
}

ya2d_Texture *cover_get(const char *key)
{
    CoverSlot *slot;
    u8 *jpeg = NULL;
    size_t jpeg_len = 0;

    if (!g_started || !key || !*key)
        return NULL;

    lock();
    slot = find_key(key);
    if (!slot)
    {
        unlock();
        return NULL;
    }

    slot->last_used = sys_ms();

    if (slot->state == COVER_LOADED)
    {
        ya2d_Texture *tex = slot->tex;
        unlock();
        return tex;
    }

    if (slot->state == COVER_READY && g_decoded < COVER_DECODE_PER_FRAME)
    {
        jpeg = slot->jpeg;
        jpeg_len = slot->jpeg_len;
        slot->jpeg = NULL;
        slot->jpeg_len = 0;
        slot->state = COVER_DECODING;
        g_decoded++;
    }

    unlock();

    if (jpeg)
    {
        ya2d_Texture *tex = ya2d_loadJPGfromBuffer(jpeg, (u32)jpeg_len);
        free(jpeg);

        lock();
        slot->state = tex ? COVER_LOADED : COVER_FAILED;
        slot->tex = tex;
        unlock();

        return tex;
    }

    return NULL;
}

void cover_draw(const char *key, int x, int y, int w, int h, u32 placeholder)
{
    ya2d_Texture *tex = cover_get(key);

    if (tex)
    {
        ya2d_drawTextureEx(tex, x, y, YA2D_DEFAULT_Z, w, h);
    }
    else
    {
        ya2d_drawFillRect(x, y, w, h, placeholder);
    }
}
