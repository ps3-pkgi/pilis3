#include "http.h"
#include "platform/log.h"
#include "platform/sys.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/systime.h>

#include <curl/curl.h>
#include <lv2/system.h>
#include <net/net.h>
#include <net/netctl.h>
#include <sysmodule/sysmodule.h>

/* Per-request curl phase tracing. It was essential while bringing up TLS;
 * once HTTPS is stable it only crowds out app/decoder logs. Flip to 1 when
 * debugging the transport again. */
#define HTTP_TRACE 0

#define MAX_COOKIES 24
#define MAX_COOKIE_VALUE 768
#define MAX_BODY (8 * 1024 * 1024)

static char g_cookie_names[MAX_COOKIES][40];
static char g_cookie_values[MAX_COOKIES][MAX_COOKIE_VALUE];
static int g_cookie_count;
static char g_buvid[33];
static int g_http_ready;
static int g_request_seq;

typedef struct
{
    char *data;
    size_t len;
    size_t cap;
    int overflow;
} BodyBuf;

/* Per-request identity for the [curl] trace lines. Kept on the stack of
 * http_do(); both callbacks below only run inside curl_easy_perform(). */
typedef struct
{
    int id;
    u64 start_ms;
} HttpReq;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    BodyBuf *buf = (BodyBuf *)userdata;
    size_t len = size * nmemb;

    if (buf->overflow)
        return 0;

    if (buf->len + len + 1 > MAX_BODY)
    {
        buf->overflow = 1;
        return 0;
    }

    if (buf->len + len + 1 > buf->cap)
    {
        size_t cap = buf->cap ? buf->cap : 8192;
        while (cap < buf->len + len + 1)
            cap *= 2;
        char *p = (char *)realloc(buf->data, cap);
        if (!p)
        {
            buf->overflow = 1;
            return 0;
        }
        buf->data = p;
        buf->cap = cap;
    }

    memcpy(buf->data + buf->len, ptr, len);
    buf->len += len;
    buf->data[buf->len] = 0;
    return len;
}

static void cookie_set_raw(const char *name, const char *value)
{
    int i;

    if (!name || !*name || !value)
        return;

    if (!strcmp(name, "buvid3"))
    {
        snprintf(g_buvid, sizeof(g_buvid), "%s", value);
        return;
    }

    for (i = 0; i < g_cookie_count; ++i)
    {
        if (!strcmp(g_cookie_names[i], name))
        {
            snprintf(g_cookie_values[i], sizeof(g_cookie_values[i]), "%s", value);
            return;
        }
    }

    if (g_cookie_count >= MAX_COOKIES)
        return;

    snprintf(g_cookie_names[g_cookie_count], sizeof(g_cookie_names[0]), "%s", name);
    snprintf(g_cookie_values[g_cookie_count], sizeof(g_cookie_values[0]), "%s",
             value);
    g_cookie_count++;
}

static void cookie_capture(const char *header)
{
    const char *p = header;
    const char *eq;
    char name[40];
    char value[MAX_COOKIE_VALUE];
    int n;

    while (*p == ' ')
        p++;

    eq = strchr(p, '=');
    if (!eq)
        return;

    n = (int)(eq - p);
    if (n <= 0 || n >= (int)sizeof(name))
        return;
    memcpy(name, p, n);
    name[n] = 0;

    p = eq + 1;
    n = 0;
    while (*p && *p != ';' && n < (int)sizeof(value) - 1)
        value[n++] = *p++;
    value[n] = 0;

    /* Skip cookie attributes; they are not sent back. */
    if (!strcasecmp(name, "path") || !strcasecmp(name, "domain") ||
        !strcasecmp(name, "expires") || !strcasecmp(name, "max-age") ||
        !strcasecmp(name, "secure") || !strcasecmp(name, "httponly") ||
        !strcasecmp(name, "samesite"))
        return;

    cookie_set_raw(name, value);
}

static size_t header_cb(char *buffer, size_t size, size_t nitems, void *userdata)
{
    size_t len = size * nitems;
    (void)userdata;

    if (len > 11 && !strncasecmp(buffer, "Set-Cookie:", 11))
    {
        char line[1024];
        size_t copy = len - 11;

        if (copy >= sizeof(line))
            copy = sizeof(line) - 1;
        memcpy(line, buffer + 11, copy);
        line[copy] = 0;

        char *nl = strpbrk(line, "\r\n");
        if (nl)
            *nl = 0;

        cookie_capture(line);
    }

    return len;
}

static void cookie_load(void)
{
    FILE *f = fopen(APP_COOKIE_FILE, "r");
    char line[1024];

    if (!f)
        return;

    while (fgets(line, sizeof(line), f))
    {
        char *eq = strchr(line, '=');
        char *nl;
        if (!eq)
            continue;
        nl = strpbrk(eq + 1, "\r\n");
        if (nl)
            *nl = 0;
        *eq = 0;
        if (*line && eq[1])
            cookie_set_raw(line, eq + 1);
    }

    fclose(f);
    log_printf("[http] loaded %d cookies, login=%d", g_cookie_count,
               http_has_login());
}

void http_save_cookies(void)
{
    FILE *f;

    sys_mkdirs(APP_CONFIG_DIR);
    f = fopen(APP_COOKIE_FILE, "w");
    if (!f)
    {
        log_printf("[http] cannot write %s", APP_COOKIE_FILE);
        return;
    }

    if (g_buvid[0])
        fprintf(f, "buvid3=%s\n", g_buvid);
    for (int i = 0; i < g_cookie_count; ++i)
        fprintf(f, "%s=%s\n", g_cookie_names[i], g_cookie_values[i]);
    fclose(f);

    log_printf("[http] saved %d cookies", g_cookie_count);
}

static void buvid_load_or_create(void)
{
    char path[256];
    FILE *f;

    snprintf(path, sizeof(path), "%s/buvid3.txt", APP_CONFIG_DIR);
    f = fopen(path, "r");
    if (f)
    {
        if (fgets(g_buvid, sizeof(g_buvid), f))
        {
            char *nl = strpbrk(g_buvid, "\r\n");
            if (nl)
                *nl = 0;
        }
        fclose(f);
    }

    if (strlen(g_buvid) != 32)
    {
        static const char hex[] = "0123456789ABCDEF";
        unsigned seed = (unsigned)sys_ms() ^ 0x9e3779b9u;
        for (int i = 0; i < 32; ++i)
        {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            g_buvid[i] = hex[seed & 15u];
        }
        g_buvid[32] = 0;

        sys_mkdirs(APP_CONFIG_DIR);
        f = fopen(path, "w");
        if (f)
        {
            fputs(g_buvid, f);
            fclose(f);
        }
    }

    log_printf("[http] buvid3=%s", g_buvid);
}

static void proxy_apply(CURL *curl)
{
    union net_ctl_info info;

    memset(&info, 0, sizeof(info));
    if (netCtlGetInfo(NET_CTL_INFO_HTTP_PROXY_CONFIG, &info) == 0 &&
        info.http_proxy_config == NET_CTL_HTTP_PROXY_ON)
    {
        char server[256];
        unsigned short port = 0;

        memset(&info, 0, sizeof(info));
        netCtlGetInfo(NET_CTL_INFO_HTTP_PROXY_SERVER, &info);
        snprintf(server, sizeof(server), "%s", info.http_proxy_server);

        memset(&info, 0, sizeof(info));
        netCtlGetInfo(NET_CTL_INFO_HTTP_PROXY_PORT, &info);
        port = info.http_proxy_port;

        curl_easy_setopt(curl, CURLOPT_PROXY, server);
        curl_easy_setopt(curl, CURLOPT_PROXYPORT, (long)port);
        curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
        log_printf("[http] proxy %s:%u", server, port);
    }
}

/* PolarSSL 1.3.9 seeds its CTR_DRBG from platform_entropy_poll(), which opens
 * /dev/urandom -- PS3 has no such device, so ctr_drbg_init() fails with
 * -0x0034 and every TLS handshake dies right after TCP connect.
 *
 * entropy.o references exactly these two poll symbols, so defining them here
 * keeps entropy_poll.o out of the link (objects resolve before the archive)
 * and routes the entropy pool to the LV2 RNG instead. */
int platform_entropy_poll(void *data, unsigned char *output, size_t len,
                          size_t *olen)
{
    static u64 counter;
    u64 sec = 0, nsec = 0, state;
    size_t i, j;

    (void)data;

    if (!output || !olen)
        return -1;

    *olen = 0;
    if (!len)
        return 0;

    /* Do not call sysGetRandomNumber() here: on this firmware the call can
     * sit inside ssl_handshake forever, which looked exactly like a stalled
     * handshake (ALPN printed, then nothing, no timeout). The wilips3 build
     * seeds the same way and handshakes complete; PolarSSL runs the bytes
     * through SHA-512 before the CTR_DRBG, so clocks + a counter are enough
     * to initialise it. */
    sysGetCurrentTime(&sec, &nsec);
    state = (u64)sys_ms() ^ (sec * 1000000000ULL + nsec) ^
            ((u64)(uintptr_t)&state * 0x9e3779b97f4a7c15ULL) ^
            (++counter * 0xbf58476d1ce4e5b9ULL);

    for (i = 0; i < len; i += 8)
    {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        state *= 2685821657736338717ULL;
        for (j = 0; j < 8 && i + j < len; ++j)
            output[i + j] = (unsigned char)(state >> (j * 8));
    }

    *olen = len;
    return 0;
}

int hardclock_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    return platform_entropy_poll(data, output, len, olen);
}

int http_init(void)
{
    s32 state = 0;
    s32 net, mod, ctl;
    int i;

    if (g_http_ready)
        return 0;

    /* Same bring-up as the working wilimini/wilips3 build: load the modules and
     * ignore netInitialize()'s result. On HEN netInitializeNetworkEx() can
     * report a benign error while the stack is still usable; treating it as
     * fatal skipped curl_global_init() and every request afterwards. */
    mod = sysModuleLoad(SYSMODULE_NET);
    sysModuleLoad(SYSMODULE_NETCTL);
    net = netInitialize();
    ctl = netCtlInit();
    log_printf("[http] net init netInitialize=%d sysModuleLoad=%d netCtlInit=%d",
               (int)net, (int)mod, (int)ctl);

    /* Bounded: the AP state can sit below IPObtained on HEN forever. Wait a
     * little so the first request does not race the DHCP lease, then go on --
     * whether there is a route is decided per request by curl. */
    for (i = 0; i < 20; ++i)
    {
        if (netCtlGetState(&state) == 0 && state == NET_CTL_STATE_IPObtained)
            break;
        sysUsleep(100000);
    }
    log_printf("[http] net state=%d after %d00 ms", (int)state, i);

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
    {
        log_printf("[http] curl_global_init failed");
        return -1;
    }

    sys_mkdirs(APP_CONFIG_DIR);
    buvid_load_or_create();
    cookie_load();

    g_http_ready = 1;
    return 0;
}

void http_shutdown(void)
{
    if (!g_http_ready)
        return;
    curl_global_cleanup();
    g_http_ready = 0;
}

#if HTTP_TRACE
static long ms_since(u64 start)
{
    u64 now = sys_ms();
    return now >= start ? (long)(now - start) : 0;
}
#endif

/* curl reports seconds as double; keep the log integer-only (%f on the PS3
 * libc is noisy and unneeded). */
static long curl_ms(double seconds)
{
    if (seconds <= 0)
        return 0;
    return (long)(seconds * 1000.0 + 0.5);
}

#if HTTP_TRACE
/* CURLINFO_TEXT carries the connection-phase lines from curl core and the
 * PolarSSL backend: "Trying 1.2.3.4:443...", "Connected to ...", "PolarSSL:
 * Connecting to ...", "Handshake complete, cipher is ...", "Request
 * completely sent off". With elapsed time in front, the last line in the log
 * names the phase a stuck transfer is sitting in. Headers and bodies are not
 * logged (Set-Cookie would end up on disk). */
static int curl_debug_cb(CURL *curl, curl_infotype type, char *data, size_t size,
                         void *userdata)
{
    const HttpReq *req = (const HttpReq *)userdata;
    char line[220];
    char *nl;
    size_t n;

    (void)curl;

    if (type != CURLINFO_TEXT || !size || !req)
        return 0;

    n = size < sizeof(line) - 1 ? size : sizeof(line) - 1;
    memcpy(line, data, n);
    line[n] = 0;

    /* A fresh handshake makes PolarSSL dump the peer certificate as one
     * multi-line message; it is ~20 lines of noise per request. */
    nl = strpbrk(line, "\r\n");
    if (nl)
        *nl = 0;
    if (!line[0] || !strncmp(line, "Dumping cert info:", 18))
        return 0;

    log_printf("[curl #%d +%ld ms] %s", req->id, ms_since(req->start_ms), line);
    return 0;
}
#endif

#if HTTP_TRACE
static void log_request(int id, const char *method, const char *url,
                        int timeout_sec, int verify)
{
    const char *q = strchr(url, '?');
    int redact = strstr(url, "passport-login") != NULL ||
                 strstr(url, "qrcode") != NULL;

    if (redact)
        log_printf("[http] #%d request %s %.*s?<redacted> (timeout %ds, verify %d)",
                   id, method, q ? (int)(q - url) : 80, url, timeout_sec, verify);
    else
        log_printf("[http] #%d request %s %s (timeout %ds, verify %d)", id, method,
                   url, timeout_sec, verify);
}
#endif

/* Safety net only: a silent peer should fail in 15 s instead of hanging.
 * Do NOT set SO_NBIO here -- PolarSSL 1.3.9 does not recognise PSL1GHT's
 * EAGAIN and turns every non-blocking recv into "NET - Reading information
 * from the socket failed" (-0x004C), which kills the handshake right after
 * the ClientHello. */
static int sockopt_cb(void *clientp, curl_socket_t fd, curlsocktype purpose)
{
    const HttpReq *req = (const HttpReq *)clientp;
    struct timeval tv;

    (void)purpose;

    tv.tv_sec = 15;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* Reached only after name resolution succeeded: no socket line means the
     * transfer is stuck in the (blocking) resolver, not in TCP or TLS. */
#if HTTP_TRACE
    if (req)
        log_printf("[http] #%d socket fd=%d, rcv/snd timeout %ds", req->id,
                   (int)fd, (int)tv.tv_sec);
#else
    (void)req;
#endif
    return CURL_SOCKOPT_OK;
}

static int http_do(const char *method, const char *url, const char *body,
                   HttpResponse *out, int timeout_sec, int verify,
                   const char *range)
{
    CURL *curl;
    CURLcode rc;
    BodyBuf buf;
    HttpReq req;
    struct curl_slist *headers = NULL;
    char cookie_header[8192];
    char error[CURL_ERROR_SIZE];
    double dns = 0, conn = 0, tls = 0, ttfb = 0, total = 0;

    memset(out, 0, sizeof(*out));
    memset(&buf, 0, sizeof(buf));
    memset(error, 0, sizeof(error));

    req.id = __sync_add_and_fetch(&g_request_seq, 1);
    req.start_ms = sys_ms();

    if (!g_http_ready)
    {
        snprintf(out->error, sizeof(out->error), "http not initialized");
        return -1;
    }

    curl = curl_easy_init();
    if (!curl)
    {
        snprintf(out->error, sizeof(out->error), "curl init failed");
        return -1;
    }

    cookie_header[0] = 0;
    if (g_buvid[0])
        snprintf(cookie_header, sizeof(cookie_header), "Cookie: buvid3=%s", g_buvid);
    for (int i = 0; i < g_cookie_count; ++i)
    {
        size_t used = strlen(cookie_header);
        if (used + strlen(g_cookie_names[i]) + strlen(g_cookie_values[i]) + 4 <
            sizeof(cookie_header))
        {
            snprintf(cookie_header + used, sizeof(cookie_header) - used, "; %s=%s",
                     g_cookie_names[i], g_cookie_values[i]);
        }
    }

    headers = curl_slist_append(headers, "Accept: application/json, text/plain, */*");
    headers = curl_slist_append(headers, "Accept-Language: zh-CN,zh;q=0.9");
    headers = curl_slist_append(headers, "Referer: https://www.bilibili.com/");
    if (cookie_header[0])
        headers = curl_slist_append(headers, cookie_header);
    if (body)
        headers = curl_slist_append(headers, "Origin: https://www.bilibili.com");
    if (body)
        headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    /* "wiliwili" is turned away by the WBI endpoints; pkgi and wilips3 both
     * use the console UA on this SDK. */
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (PLAYSTATION 3; 1.00)");
#if HTTP_TRACE
    /* Verbose is a trace, not debugging leftover: TEXT events mark DNS, TCP,
     * TLS and "request sent" without ever touching the payload. */
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, curl_debug_cb);
    curl_easy_setopt(curl, CURLOPT_DEBUGDATA, &req);
#endif
    curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, sockopt_cb);
    curl_easy_setopt(curl, CURLOPT_SOCKOPTDATA, &req);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, NULL);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    /* A stalled body must not sit forever; the blocking resolver can swallow
     * CURLOPT_TIMEOUT, but this still bounds the transfer phase. */
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout_sec);
    if (range)
        curl_easy_setopt(curl, CURLOPT_RANGE, range);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, verify ? 2L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, verify ? 1L : 0L);
    /* Always point at our bundle: otherwise curl falls back to the build-time
     * default /usr/ssl/certs/ca-bundle.crt, which does not exist on PS3. */
    curl_easy_setopt(curl, CURLOPT_CAINFO, APP_DIR "/cacert.pem");
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    /* mbedTLS 1.3.9 writes the ALPN extension itself; if that encoding is
     * what the server chokes on, the handshake never gets a ServerHello and
     * there is no alert to log. Nothing here needs ALPN on http/1.1. */
    curl_easy_setopt(curl, CURLOPT_SSL_ENABLE_ALPN, 0L);
    proxy_apply(curl);

    if (body)
    {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    }
    else if (!strcmp(method, "HEAD"))
    {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    }

    /* Log before the transfer: if curl never returns, this line (followed by
     * nothing but [curl] phase lines, if any) is the last thing in the file
     * and names the URL it is stuck on. */
#if HTTP_TRACE
    log_request(req.id, method, url, timeout_sec, verify);
#endif
    rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out->status);
    curl_easy_getinfo(curl, CURLINFO_NAMELOOKUP_TIME, &dns);
    curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &conn);
    curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME, &tls);
    curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME, &ttfb);
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total);
    out->curl_code = (int)rc;
    out->body = buf.data;
    out->len = buf.len;

    if (rc != CURLE_OK)
    {
        snprintf(out->error, sizeof(out->error), "%s",
                 error[0] ? error : curl_easy_strerror(rc));
        log_printf("[http] #%d request failed rc=%d http=%ld after %ld ms "
                   "(dns %ld / connect %ld / tls %ld / ttfb %ld): %s",
                   req.id, (int)rc, out->status, curl_ms(total), curl_ms(dns),
                   curl_ms(conn), curl_ms(tls), curl_ms(ttfb), out->error);
    }
#if HTTP_TRACE
    else
    {
        log_printf("[http] #%d -> %ld (%u bytes) in %ld ms "
                   "(dns %ld / connect %ld / tls %ld / ttfb %ld)",
                   req.id, out->status, (unsigned)out->len, curl_ms(total),
                   curl_ms(dns), curl_ms(conn), curl_ms(tls), curl_ms(ttfb));
    }
#endif

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return rc == CURLE_OK ? 0 : -1;
}

int http_get(const char *url, HttpResponse *out, int timeout_sec)
{
    int rc = http_do("GET", url, NULL, out, timeout_sec, 1, NULL);
    if (rc < 0 && (out->curl_code == CURLE_SSL_CACERT ||
                   out->curl_code == CURLE_SSL_CACERT_BADFILE ||
                   out->curl_code == CURLE_SSL_CONNECT_ERROR))
    {
        log_printf("[http] CA verify failed, retrying without verification");
        rc = http_do("GET", url, NULL, out, timeout_sec, 0, NULL);
    }
    return rc;
}

int http_get_range(const char *url, long long start, long long end,
                   HttpResponse *out, int timeout_sec)
{
    char range[64];
    int rc;

    snprintf(range, sizeof(range), "%lld-%lld", start, end);
    rc = http_do("GET", url, NULL, out, timeout_sec, 1, range);
    if (rc < 0 && (out->curl_code == CURLE_SSL_CACERT ||
                   out->curl_code == CURLE_SSL_CACERT_BADFILE ||
                   out->curl_code == CURLE_SSL_CONNECT_ERROR))
    {
        log_printf("[http] CA verify failed, retrying without verification");
        rc = http_do("GET", url, NULL, out, timeout_sec, 0, range);
    }
    return rc;
}

int http_post(const char *url, const char *body, HttpResponse *out, int timeout_sec)
{
    int rc = http_do("POST", url, body, out, timeout_sec, 1, NULL);
    if (rc < 0 && (out->curl_code == CURLE_SSL_CACERT ||
                   out->curl_code == CURLE_SSL_CACERT_BADFILE ||
                   out->curl_code == CURLE_SSL_CONNECT_ERROR))
    {
        log_printf("[http] CA verify failed, retrying without verification");
        rc = http_do("POST", url, body, out, timeout_sec, 0, NULL);
    }
    return rc;
}

void http_response_free(HttpResponse *res)
{
    if (res->body)
        free(res->body);
    memset(res, 0, sizeof(*res));
}

int http_cookie(const char *name, char *out, int size)
{
    for (int i = 0; i < g_cookie_count; ++i)
    {
        if (!strcmp(g_cookie_names[i], name))
        {
            snprintf(out, size, "%s", g_cookie_values[i]);
            return 1;
        }
    }
    return 0;
}

void http_set_cookie(const char *name, const char *value)
{
    cookie_set_raw(name, value);
}

int http_has_login(void)
{
    char v[64];
    return http_cookie("SESSDATA", v, sizeof(v)) && v[0];
}

const char *http_buvid(void)
{
    return g_buvid;
}

int http_url_encode(const char *in, char *out, int size)
{
    static const char hex[] = "0123456789ABCDEF";
    int n = 0;
    const unsigned char *p = (const unsigned char *)in;

    while (*p)
    {
        unsigned char c = *p++;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        {
            if (n + 1 >= size)
                return -1;
            out[n++] = (char)c;
        }
        else
        {
            if (n + 3 >= size)
                return -1;
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 15];
        }
    }
    out[n] = 0;
    return n;
}
