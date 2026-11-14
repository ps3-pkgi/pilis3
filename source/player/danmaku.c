#include "danmaku.h"
#include "net/http.h"
#include "platform/gfx.h"
#include "platform/log.h"
#include "ui/theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/thread.h>
#include <sys/systime.h>
#include <zlib.h>

#define DM_MAX_ITEMS 4000
#define DM_MAX_ACTIVE 128
#define DM_LANES 12
#define DM_FONT 32

typedef struct
{
    float t;
    int mode;
    u32 color;
    float width;
    char text[128];
} DmItem;

struct DanmakuCtx
{
    DmItem *items;
    int count;
    int cap;
    long long cid;
    volatile int ready;
    volatile int failed;
    sys_ppu_thread_t thread;
    int started;
};

typedef struct
{
    int used;
    int fixed;
    int mode_bottom;
    int lane;
    double start;
    float width;
    int item;
} ActiveDm;

static ActiveDm g_active[DM_MAX_ACTIVE];
static float g_lane_free[DM_LANES];
static int g_next_index;
static double g_last_clock;
static int g_render_inited;

static void render_reset(void)
{
    memset(g_active, 0, sizeof(g_active));
    for (int i = 0; i < DM_LANES; ++i)
        g_lane_free[i] = 0.0f;
    g_next_index = 0;
    g_last_clock = 0.0;
    g_render_inited = 1;
}

static char *inflate_all(const u8 *in, u32 in_len, u32 *out_len)
{
    z_stream s;
    u32 cap = in_len * 4 + 65536;
    char *out;
    int ret;

    if (cap > 8 * 1024 * 1024)
        cap = 8 * 1024 * 1024;
    out = (char *)malloc(cap + 1);
    if (!out)
        return NULL;

    memset(&s, 0, sizeof(s));
    if (inflateInit2(&s, -MAX_WBITS) != Z_OK)
    {
        free(out);
        return NULL;
    }

    s.next_in = (Bytef *)in;
    s.avail_in = in_len;
    s.next_out = (Bytef *)out;
    s.avail_out = cap;

    for (;;)
    {
        ret = inflate(&s, Z_NO_FLUSH);
        if (ret == Z_STREAM_END)
            break;
        if (ret != Z_OK)
        {
            inflateEnd(&s);
            free(out);
            return NULL;
        }
        if (s.avail_out == 0)
        {
            u32 used = (u32)s.total_out;
            u32 new_cap = cap * 2;
            char *p;
            if (new_cap > 8 * 1024 * 1024)
            {
                inflateEnd(&s);
                free(out);
                return NULL;
            }
            p = (char *)realloc(out, new_cap + 1);
            if (!p)
            {
                inflateEnd(&s);
                free(out);
                return NULL;
            }
            out = p;
            cap = new_cap;
            s.next_out = (Bytef *)(out + used);
            s.avail_out = cap - used;
        }
    }

    *out_len = (u32)s.total_out;
    out[*out_len] = 0;
    inflateEnd(&s);
    return out;
}

static void xml_unescape(const char *src, char *dst, int size)
{
    int n = 0;

    while (*src && n < size - 1)
    {
        if (!strncmp(src, "&amp;", 5))
        {
            dst[n++] = '&';
            src += 5;
        }
        else if (!strncmp(src, "&lt;", 4))
        {
            dst[n++] = '<';
            src += 4;
        }
        else if (!strncmp(src, "&gt;", 4))
        {
            dst[n++] = '>';
            src += 4;
        }
        else if (!strncmp(src, "&quot;", 6))
        {
            dst[n++] = '"';
            src += 6;
        }
        else if (!strncmp(src, "&apos;", 6))
        {
            dst[n++] = '\'';
            src += 6;
        }
        else
        {
            dst[n++] = *src++;
        }
    }
    dst[n] = 0;
}

static int parse_xml(DanmakuCtx *ctx, const char *xml)
{
    const char *p = xml;

    while ((p = strstr(p, "<d p=\"")) != NULL)
    {
        const char *attr = p + 6;
        const char *attr_end = strchr(attr, '"');
        const char *text_start;
        const char *text_end;
        char attr_buf[256];
        char text_buf[160];
        float t = 0;
        int mode = 1, size = 25;
        unsigned color = 0;

        if (!attr_end)
            break;

        int alen = (int)(attr_end - attr);
        if (alen >= (int)sizeof(attr_buf))
            alen = (int)sizeof(attr_buf) - 1;
        memcpy(attr_buf, attr, alen);
        attr_buf[alen] = 0;

        if (sscanf(attr_buf, "%f,%d,%d,%u", &t, &mode, &size, &color) < 2)
        {
            p = attr_end + 1;
            continue;
        }

        text_start = strchr(attr_end, '>');
        if (!text_start)
            break;
        text_start++;
        text_end = strstr(text_start, "</d>");
        if (!text_end)
            break;

        int tlen = (int)(text_end - text_start);
        if (tlen >= (int)sizeof(text_buf))
            tlen = (int)sizeof(text_buf) - 1;
        {
            char raw[160];
            memcpy(raw, text_start, tlen);
            raw[tlen] = 0;
            xml_unescape(raw, text_buf, sizeof(text_buf));
        }

        if (text_buf[0] && ctx->count < ctx->cap)
        {
            DmItem *it = &ctx->items[ctx->count++];
            it->t = t;
            it->mode = mode;
            it->color = color;
            it->width = 0.0f;
            snprintf(it->text, sizeof(it->text), "%s", text_buf);
        }

        p = text_end + 4;
    }

    return ctx->count;
}

static void load_thread(void *arg)
{
    DanmakuCtx *ctx = (DanmakuCtx *)arg;
    char url[256];
    HttpResponse res;
    char *xml = NULL;
    u32 xml_len = 0;
    int ok = 0;

    snprintf(url, sizeof(url), "https://api.bilibili.com/x/v1/dm/list.so?oid=%lld",
             ctx->cid);

    if (http_get(url, &res, 20) == 0 && res.body && res.len > 0)
    {
        if (res.body[0] == '<')
        {
            xml = (char *)malloc(res.len + 1);
            if (xml)
            {
                memcpy(xml, res.body, res.len);
                xml[res.len] = 0;
                xml_len = (u32)res.len;
            }
        }
        else
        {
            xml = inflate_all((const u8 *)res.body, (u32)res.len, &xml_len);
        }
    }
    http_response_free(&res);

    if (xml)
    {
        ctx->items = (DmItem *)malloc(sizeof(DmItem) * DM_MAX_ITEMS);
        if (ctx->items)
        {
            ctx->cap = DM_MAX_ITEMS;
            ctx->count = 0;
            parse_xml(ctx, xml);
            ok = 1;
            log_printf("[danmaku] cid=%lld comments=%d xml=%u", ctx->cid,
                       ctx->count, xml_len);
        }
        free(xml);
    }

    if (!ok)
        log_printf("[danmaku] cid=%lld load failed", ctx->cid);

    ctx->failed = !ok;
    ctx->ready = 1;
    sysThreadExit(0);
}

DanmakuCtx *danmaku_load_start(long long cid)
{
    DanmakuCtx *ctx;
    int rc;

    if (cid <= 0)
        return NULL;

    ctx = (DanmakuCtx *)calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;

    ctx->cid = cid;
    rc = sysThreadCreate(&ctx->thread, load_thread, ctx, 1200, 0x20000, 0,
                         "pilis3dm");
    if (rc < 0)
    {
        free(ctx);
        return NULL;
    }

    ctx->started = 1;
    return ctx;
}

int danmaku_ready(DanmakuCtx *ctx)
{
    return ctx ? ctx->ready : 0;
}

int danmaku_count(DanmakuCtx *ctx)
{
    return ctx ? ctx->count : 0;
}

void danmaku_stop(DanmakuCtx *ctx)
{
    if (!ctx)
        return;

    if (ctx->started)
        sysThreadJoin(ctx->thread, NULL);
    if (ctx->items)
        free(ctx->items);
    free(ctx);
}

void danmaku_reset(void)
{
    render_reset();
}

static int lane_pick(float *lane_free, double clock, int fixed)
{
    int start = fixed ? 0 : 0;
    int end = fixed ? 4 : DM_LANES;

    for (int i = start; i < end; ++i)
    {
        if ((double)lane_free[i] <= clock)
            return i;
    }
    return -1;
}

void danmaku_draw(DanmakuCtx *ctx, double clock, int enabled)
{
    if (!enabled || !ctx || !ctx->ready || !ctx->items)
        return;

    if (!g_render_inited)
        render_reset();

    if (clock < g_last_clock - 1.0 || clock > g_last_clock + 3.0)
    {
        render_reset();
    }
    g_last_clock = clock;

    for (int i = 0; i < DM_MAX_ACTIVE; ++i)
    {
        ActiveDm *a = &g_active[i];
        double age;
        float x, y;
        DmItem *it;

        if (!a->used)
            continue;
        it = &ctx->items[a->item];
        age = clock - a->start;

        if (a->fixed)
        {
            if (age > 4.0)
            {
                a->used = 0;
                continue;
            }
            x = (GFX_W - it->width) / 2;
            y = a->mode_bottom ? 900 - a->lane * 64 : 130 + a->lane * 64;
        }
        else
        {
            float speed = 300.0f;
            x = GFX_W - (float)(age * speed);
            y = 130 + a->lane * 64;
            if (x + it->width < 0)
            {
                a->used = 0;
                continue;
            }
        }

        gfx_text((int)x + 2, (int)y + 2, DM_FONT, GFX_RGBA(0, 0, 0, 255),
                 it->text);
        gfx_text((int)x, (int)y, DM_FONT, it->color, it->text);
    }

    while (g_next_index < ctx->count)
    {
        DmItem *it = &ctx->items[g_next_index];
        ActiveDm *slot = NULL;

        if (it->t > clock + 0.05)
            break;
        g_next_index++;

        if (it->t < clock - 1.0)
            continue;

        if (it->width <= 0.0f)
            it->width = (float)gfx_text_width(DM_FONT, it->text);
        if (it->width <= 0.0f || it->width > 1600.0f)
            continue;

        if (it->mode == 4 || it->mode == 5)
        {
            int lane = lane_pick(g_lane_free, clock, 1);
            if (lane < 0)
                continue;
            g_lane_free[lane] = (float)(clock + 4.2);
            for (int i = 0; i < DM_MAX_ACTIVE; ++i)
            {
                if (!g_active[i].used)
                {
                    slot = &g_active[i];
                    break;
                }
            }
            if (!slot)
                continue;
            slot->used = 1;
            slot->fixed = 1;
            slot->lane = lane;
            slot->start = clock;
            slot->width = it->width;
            slot->item = g_next_index - 1;
            slot->mode_bottom = (it->mode == 4);
            continue;
        }

        if (it->mode != 1 && it->mode != 2 && it->mode != 3)
            continue;

        {
            int lane = lane_pick(g_lane_free, clock, 0);
            if (lane < 0)
                continue;
            g_lane_free[lane] = (float)(clock + (it->width + 60.0f) / 300.0f);
            for (int i = 0; i < DM_MAX_ACTIVE; ++i)
            {
                if (!g_active[i].used)
                {
                    slot = &g_active[i];
                    break;
                }
            }
            if (!slot)
                continue;
            slot->used = 1;
            slot->fixed = 0;
            slot->lane = lane;
            slot->start = clock;
            slot->width = it->width;
            slot->item = g_next_index - 1;
            slot->mode_bottom = 0;
        }
    }
}
