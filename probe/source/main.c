/*
 * pilis3 VDEC probe
 *
 * Goal: prove (on real hardware) that the PS3 firmware VDEC block can be used
 * from a HEN/CFW homebrew app through PSL1GHT's libvdec stubs, and pin down the
 * facts the SDK headers do not state:
 *   - sysModuleLoad(VDEC / VDEC_H264) result
 *   - vdecQueryAttr memory size / version
 *   - vdecOpen num_spus and profile_level values that work
 *   - picture_size, H.264 coded/display size, PTS
 *   - YUV420P plane layout (height/stride padding)
 *   - decode timing
 *
 * The package has its own APPID (NP00VDEC0) so it never overwrites pilis3.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>

#include <ppu-types.h>
#include <sys/memory.h>
#include <sys/systime.h>
#include <sys/process.h>
#include <sysmodule/sysmodule.h>
#include <sysutil/sysutil.h>
#include <sysutil/video.h>
#include <codec/vdec.h>

#include <tiny3d.h>

#include "sample_1080p.h"
#include "sample_720p.h"
#include "sample_1080p_red.h"

#define LOG_PATH "/dev_hdd0/tmp/pilis3-vdec.log"
#define PROBE_VERSION "0.1"

static FILE *g_log;

static void log_open(void)
{
    g_log = fopen(LOG_PATH, "w");
    if (g_log)
    {
        fprintf(g_log, "==== pilis3 vdec probe %s ====\n", PROBE_VERSION);
        fprintf(g_log, "build %s %s\n", __DATE__, __TIME__);
        fflush(g_log);
    }
}

static void log_close(void)
{
    if (g_log)
    {
        fflush(g_log);
        fclose(g_log);
        g_log = NULL;
    }
}

static void probe_log(const char *fmt, ...)
{
    va_list ap;

    if (g_log)
    {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fputc('\n', g_log);
        fflush(g_log);
    }

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/* The tiny3d package in the build image is patched to call this; the probe
 * routes it into the same log file. */
void ps3_boot_trace(const char *fmt, ...)
{
    char line[192];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    probe_log("[t3d] %s", line);
}

static u64 now_us(void)
{
    u64 sec = 0, nsec = 0;
    if (sysGetCurrentTime(&sec, &nsec) < 0)
        return 0;
    return sec * 1000000ULL + nsec / 1000ULL;
}

/* ------------------------------------------------------------------------- */
/* VDEC callback                                                             */
/* ------------------------------------------------------------------------- */

static volatile u32 g_cb_audone;
static volatile u32 g_cb_picout;
static volatile u32 g_cb_seqdone;
static volatile u32 g_cb_error;
static volatile u32 g_cb_last_msg;
static volatile u32 g_cb_last_data;
static volatile int g_vdec_stuck;

/* vdecClosure.fn is a 32-bit OPD pointer, not a C function pointer: on this
 * ABI a function pointer points at a 64-bit {code, toc} descriptor. Passing
 * (u32)&vdec_callback makes Cell OS read the high/low halves of the code
 * address as {fn, rtoc}, so no callback ever dispatches. */
static opd32 g_vdec_cb_opd;

static u32 make_opd32(opd32 *out, void *func)
{
    const u64 *opd = (const u64 *)func;

    out->func = (u32)opd[0];
    out->rtoc = (u32)opd[1];
    return (u32)(uintptr_t)out;
}

static u32 vdec_callback(u32 handle, u32 msgtype, u32 msgdata, u32 arg)
{
    (void)handle;
    (void)arg;

    g_cb_last_msg = msgtype;
    g_cb_last_data = msgdata;

    switch (msgtype)
    {
    case VDEC_CALLBACK_AUDONE:
        g_cb_audone++;
        break;
    case VDEC_CALLBACK_PICOUT:
        g_cb_picout++;
        break;
    case VDEC_CALLBACK_SEQDONE:
        g_cb_seqdone++;
        break;
    case VDEC_CALLBACK_ERROR:
        g_cb_error++;
        break;
    default:
        break;
    }
    return 0;
}

/* PSL1GHT's vdecH264Info declares pic_order_count as s8[2] at 0x0d, but the
 * firmware fills s16[2] at 0x0e and everything after it shifts. This prefix
 * matches the firmware's CellVdecAvcInfo layout for the fields the probe
 * reads; the offset checks fail the build if the compiler disagrees. */
typedef struct
{
    u16 width;
    u16 height;
    u8 picture_type[2];
    u8 idr_picture_flag;
    u8 aspect_ratio_idc;
    u16 sar_height;
    u16 sar_width;
    u8 pic_struct;
    u8 pad0;
    s16 pic_order_count[2];
    u8 vui_parameters_present_flag;
    u8 frame_mbs_only_flag;
    u8 video_signal_type_present_flag;
    u8 video_format;
    u8 video_full_range_flag;
    u8 color_description_present_flag;
    u8 color_primaries;
    u8 transfer_characteristics;
    u8 matrix_coefficients;
    u8 timing_info_present_flag;
    u8 frame_rate;
    u8 fixed_frame_rate_flag;
    u8 low_delay_hrd_flag;
    u8 entropy_coding_mode_flag;
    u16 nalUnitPresentFlags;
} ProbeH264Info;

typedef char probe_h264_poc_offset_check[
    (offsetof(ProbeH264Info, pic_order_count) == 0x0e) ? 1 : -1];
typedef char probe_h264_nal_offset_check[
    (offsetof(ProbeH264Info, nalUnitPresentFlags) == 0x20) ? 1 : -1];

/* ------------------------------------------------------------------------- */
/* Annex-B access-unit splitter (probe only: one slice per frame)            */
/* ------------------------------------------------------------------------- */

typedef struct
{
    const u8 *ptr;
    u32 len;
} Au;

static int find_start_code(const u8 *p, const u8 *end, const u8 **sc, u32 *sc_len)
{
    const u8 *q;

    for (q = p; q + 3 <= end; ++q)
    {
        if (q[0] == 0 && q[1] == 0)
        {
            if (q[2] == 1)
            {
                *sc = q;
                *sc_len = 3;
                return 1;
            }
            if (q + 4 <= end && q[2] == 0 && q[3] == 1)
            {
                *sc = q;
                *sc_len = 4;
                return 1;
            }
        }
    }
    return 0;
}

static int next_au(const u8 **cursor, const u8 *end, Au *out)
{
    const u8 *p = *cursor;
    const u8 *sc;
    u32 sc_len;
    const u8 *nal;
    u32 nal_sc_len;
    int have_vcl = 0;

    if (!find_start_code(p, end, &sc, &sc_len))
        return 0;

    nal = sc;
    nal_sc_len = sc_len;

    for (;;)
    {
        const u8 *payload = nal + nal_sc_len;
        const u8 *next;
        u32 next_sc_len;
        int type;

        if (!find_start_code(payload, end, &next, &next_sc_len))
        {
            /* Last NAL in the buffer. */
            type = (payload < end) ? (payload[0] & 0x1f) : 0;
            if (type == 1 || type == 5)
            {
                if (have_vcl)
                {
                    /* It starts the next AU; the current one ends here. */
                    out->ptr = sc;
                    out->len = (u32)(nal - sc);
                    *cursor = nal;
                    return 1;
                }
                have_vcl = 1;
            }
            if (have_vcl)
            {
                out->ptr = sc;
                out->len = (u32)(end - sc);
                *cursor = end;
                return 1;
            }
            return 0;
        }

        type = (payload < next) ? (payload[0] & 0x1f) : 0;
        if (type == 1 || type == 5)
        {
            if (have_vcl)
            {
                /* This NAL starts the next access unit: cut at "nal", not
                 * at "next", or every AU swallows the first slice of the
                 * following frame. */
                out->ptr = sc;
                out->len = (u32)(nal - sc);
                *cursor = nal;
                return 1;
            }
            have_vcl = 1;
        }

        nal = next;
        nal_sc_len = next_sc_len;
    }
}

/* ------------------------------------------------------------------------- */
/* Plane-layout probe                                                        */
/* ------------------------------------------------------------------------- */

static void variance_u8(const u8 *p, u32 w, u32 h, u32 stride, double *mean_out,
                        double *var_out)
{
    u32 x, y, n = 0;
    double sum = 0.0, sum2 = 0.0;

    if (!p || !w || !h)
    {
        *mean_out = 0.0;
        *var_out = 1e30;
        return;
    }

    for (y = 0; y < h; y += 8)
    {
        const u8 *row = p + (size_t)y * stride;
        for (x = 0; x < w; x += 8)
        {
            double v = row[x];
            sum += v;
            sum2 += v * v;
            n++;
        }
    }

    if (!n)
    {
        *mean_out = 0.0;
        *var_out = 1e30;
        return;
    }

    *mean_out = sum / n;
    *var_out = sum2 / n - (*mean_out) * (*mean_out);
}

/* The red frame has constant chroma. Whichever candidate plane offset produces
 * the flattest U/V planes is the real layout. */
static u32 probe_layout(const u8 *yuv, u32 size, u32 w, u32 h)
{
    u32 heights[3];
    u32 nh = 0;
    u32 i;
    u32 best_h = h;
    double best_score = 1e30;

    heights[nh++] = h;
    if (((h + 15) & ~15u) != h)
        heights[nh++] = (h + 15) & ~15u;
    if (((h + 127) & ~127u) != h && ((h + 127) & ~127u) != heights[nh - 1])
        heights[nh++] = (h + 127) & ~127u;

    probe_log("[layout] source %ux%u, picture_size=%u", w, h, size);

    for (i = 0; i < nh; ++i)
    {
        u32 hh = heights[i];
        u32 u_off = w * hh;
        u32 v_off = u_off + (w * hh) / 4;
        u32 total = v_off + (w * hh) / 4;
        double ym, yv, um, uv, vm, vv;
        double score;

        if (total > size)
        {
            probe_log("[layout] cand h=%u skipped (needs %u > %u)", hh, total, size);
            continue;
        }

        variance_u8(yuv, w, h, w, &ym, &yv);
        variance_u8(yuv + u_off, w / 2, hh / 2, w / 2, &um, &uv);
        variance_u8(yuv + v_off, w / 2, hh / 2, w / 2, &vm, &vv);
        score = uv + vv;

        probe_log("[layout] cand h=%u  Y(mean=%.1f var=%.1f)  U(mean=%.1f var=%.2f)  "
             "V(mean=%.1f var=%.2f)  score=%.2f",
             hh, ym, yv, um, uv, vm, vv, score);

        if (score < best_score)
        {
            best_score = score;
            best_h = hh;
        }
    }

    probe_log("[layout] chosen height=%u (score=%.2f)", best_h, best_score);
    return best_h;
}

/* ------------------------------------------------------------------------- */
/* VDEC session                                                              */
/* ------------------------------------------------------------------------- */

typedef struct
{
    vdecType type;
    vdecAttr attr;
    u32 handle;
    void *mem;
    u32 mem_size;
    u32 alloc_size;
    u32 num_spus;
    u32 level;
    int open;
} VdecSession;

static int open_vdec(VdecSession *s, u32 level, u32 spus)
{
    s32 rc;
    vdecConfig cfg;
    vdecClosure closure;
    u64 addr;

    memset(s, 0, sizeof(*s));
    s->type.codec_type = VDEC_CODEC_TYPE_H264;
    s->type.profile_level = level;
    s->level = level;
    s->num_spus = spus;

    memset(&s->attr, 0, sizeof(s->attr));
    rc = vdecQueryAttr(&s->type, &s->attr);
    probe_log("[vdec] queryAttr(level=%u) rc=%08x mem_size=%u cmd_depth=%u ver=%u.%u",
         level, (u32)rc, s->attr.mem_size, s->attr.cmd_depth, s->attr.ver_major,
         s->attr.ver_minor);
    if (rc < 0 || !s->attr.mem_size)
        return -1;

    s->mem_size = s->attr.mem_size;
    s->alloc_size = (s->mem_size + (1024 * 1024 - 1)) & ~(1024 * 1024 - 1);
    {
        sys_mem_addr_t sysaddr = 0;
        s32 mrc = sysMemoryAllocate(s->alloc_size, SYS_MEMORY_PAGE_SIZE_1M,
                                    &sysaddr);

        probe_log("[vdec] sysMemoryAllocate(%u, 1M) rc=%08x addr=%08x",
                  s->alloc_size, (u32)mrc, (u32)sysaddr);
        if (mrc != 0 || !sysaddr)
            return -1;
        s->mem = (void *)(uintptr_t)sysaddr;
    }
    memset(s->mem, 0, s->mem_size);

    addr = (u64)(uintptr_t)s->mem;
    probe_log("[vdec] mem=%p (u32=%08x) size=%u alloc=%u%s", s->mem, (u32)addr,
         s->mem_size, s->alloc_size,
         (addr >> 32) ? "  WARNING: above 4G" : "");

    memset(&cfg, 0, sizeof(cfg));
    cfg.mem_addr = (u32)addr;
    cfg.mem_size = s->mem_size;
    cfg.ppu_thread_prio = 1000;
    cfg.ppu_thread_stack_size = 0x10000;
    cfg.spu_thread_prio = 100;
    cfg.num_spus = spus;

    memset(&closure, 0, sizeof(closure));
    closure.fn = make_opd32(&g_vdec_cb_opd, (void *)vdec_callback);
    closure.arg = 0;

    probe_log("[vdec] callback opd=%08x func=%08x rtoc=%08x", closure.fn,
              g_vdec_cb_opd.func, g_vdec_cb_opd.rtoc);
    probe_log("[vdec] calling open level=%u spus=%u mem=%p", level, spus,
              s->mem);
    rc = vdecOpen(&s->type, &cfg, &closure, &s->handle);
    probe_log("[vdec] open(spus=%u) rc=%08x handle=%u", spus, (u32)rc, s->handle);
    if (rc < 0)
    {
        sysMemoryFree((sys_mem_addr_t)(uintptr_t)s->mem);
        s->mem = NULL;
        return -1;
    }

    s->open = 1;
    return 0;
}

static void close_vdec(VdecSession *s)
{
    if (!s->open)
    {
        if (s->mem)
            sysMemoryFree((sys_mem_addr_t)(uintptr_t)s->mem);
        s->mem = NULL;
        return;
    }

    probe_log("[vdec] endSequence");
    g_cb_seqdone = 0;
    {
        s32 rc = vdecEndSequence(s->handle);
        probe_log("[vdec] endSequence rc=%08x", (u32)rc);
    }

    for (int i = 0; i < 5000 && !g_cb_seqdone; ++i)
        sysUsleep(1000);

    if (!g_cb_seqdone)
    {
        /* Never call vdecClose without SEQDONE: that is exactly where the
         * old probe wedged, with no way to tell it from a real decode. */
        probe_log("[vdec] close skipped: SEQDONE missing after 5s");
        g_vdec_stuck = 1;
        s->open = 0;
        return;
    }

    probe_log("[vdec] close handle=%u seqdone=%u", s->handle, g_cb_seqdone);
    vdecClose(s->handle);
    probe_log("[vdec] close returned");
    s->open = 0;

    if (s->mem)
    {
        s32 frc = sysMemoryFree((sys_mem_addr_t)(uintptr_t)s->mem);
        probe_log("[vdec] sysMemoryFree rc=%08x", (u32)frc);
        s->mem = NULL;
    }
    probe_log("[vdec] memory released");
}

static int decode_stream(VdecSession *s, const u8 *data, u32 size, const char *tag,
                         u32 expect_w, u32 expect_h, u8 **frame_out,
                         u32 *frame_size_out, u32 *frame_w_out, u32 *frame_h_out,
                         u32 *pics_out)
{
    const u8 *cursor = data;
    const u8 *end = data + size;
    Au au;
    u32 index = 0;
    u32 pics = 0;
    u32 misses = 0;
    u8 *frame = NULL;
    u32 frame_size = 0;
    u8 *au_buf = NULL;
    u32 au_buf_cap = 0;
    u32 frame_w = expect_w, frame_h = expect_h;
    u64 t_start = now_us();
    u64 t_decode = 0;

    *frame_out = NULL;
    *frame_size_out = 0;
    *frame_w_out = 0;
    *frame_h_out = 0;
    *pics_out = 0;

    while (next_au(&cursor, end, &au))
    {
        vdecAU info;
        s32 rc;
        int got = 0;
        u32 audone_before = g_cb_audone;
        u64 t0 = now_us();

        /* The sample arrays are byte aligned and AU N+1 starts mid-stream, but
         * the decoder expects aligned input: copy each AU into a 128-byte
         * scratch buffer instead of handing it .rodata pointers. */
        if (au.len > au_buf_cap)
        {
            free(au_buf);
            au_buf_cap = (au.len + 127u) & ~127u;
            au_buf = (u8 *)memalign(128, au_buf_cap);
        }
        if (!au_buf)
        {
            probe_log("[%s] au buffer alloc failed (%u)", tag, au.len);
            break;
        }
        memcpy(au_buf, au.ptr, au.len);

        memset(&info, 0, sizeof(info));
        info.packet_addr = (u32)(uintptr_t)au_buf;
        info.packet_size = au.len;
        info.pts.low = index * 3000u;
        info.pts.hi = 0;
        info.dts = info.pts;
        info.userdata = index;

        probe_log("[%s] au %u feed size=%u", tag, index, au.len);
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            rc = vdecDecodeAu(s->handle, VDEC_DECODER_MODE_NORMAL, &info);
            if ((u32)rc == VDEC_ERROR_BUSY)
            {
                sysUsleep(2000);
                continue;
            }
            break;
        }

        probe_log("[%s] au %u size=%u decode rc=%08x", tag, index, au.len, (u32)rc);
        if (rc < 0)
        {
            probe_log("[%s] decode failed: rc=%08x", tag, (u32)rc);
            break;
        }

        /* The AU buffer is reused next iteration; wait for AUDONE before
         * touching it again, or the decoder may read the next frame's bytes
         * out of the same address. Movian keeps the packet alive the same
         * way (5 s timeout). */
        for (int wait = 0; wait < 1000 && g_cb_audone == audone_before; ++wait)
            sysUsleep(5000);

        if (g_cb_audone == audone_before)
        {
            probe_log("[%s] au %u: no AUDONE after 5s (callbacks au=%u pic=%u "
                      "seq=%u err=%u)",
                      tag, index, g_cb_audone, g_cb_picout, g_cb_seqdone,
                      g_cb_error);
            break;
        }

        /* 100 x 5 ms: a live decoder answers within a frame or two, and a
         * dead one must not burn 3 s per AU now that every AU is fed. */
        for (int spin = 0; spin < 100; ++spin)
        {
            u32 item = 0;
            s32 r = vdecGetPicItem(s->handle, &item);

            if (r == 0 && item)
            {
                vdecPicture *pic = (vdecPicture *)(uintptr_t)item;
                u32 need;

                probe_log("[%s] pic %u: codec=%u size=%u status=%u attr=%u pics=%u "
                     "pts=%08x:%08x",
                     tag, pics, pic->codec_type, pic->picture_size, pic->status,
                     pic->attr, pic->access_units, pic->pts[0].low, pic->pts[0].hi);

                if (pic->codec_specific_addr)
                {
                    ProbeH264Info *hi =
                        (ProbeH264Info *)(uintptr_t)pic->codec_specific_addr;
                    probe_log("[%s] avc %ux%u idr=%u type=%u/%u poc=%d/%d "
                              "nal=0x%04x",
                         tag, hi->width, hi->height, hi->idr_picture_flag,
                         hi->picture_type[0], hi->picture_type[1],
                         hi->pic_order_count[0], hi->pic_order_count[1],
                         hi->nalUnitPresentFlags);
                    if (hi->width && hi->height)
                    {
                        frame_w = hi->width;
                        frame_h = hi->height;
                    }
                }

                need = pic->picture_size;
                if (need < (u32)(frame_w * frame_h * 3 / 2))
                    need = (u32)(frame_w * frame_h * 3 / 2);
                need += 4096;

                if (need > frame_size)
                {
                    if (frame)
                        free(frame);
                    frame = (u8 *)memalign(128, need);
                    frame_size = frame ? need : 0;
                }

                if (frame)
                {
                    vdecPictureFormat fmt;
                    memset(&fmt, 0, sizeof(fmt));
                    fmt.format_type = VDEC_PICFMT_YUV420P;
                    fmt.color_matrix = VDEC_COLOR_MATRIX_BT601;
                    fmt.alpha = 0;

                    rc = vdecGetPicture(s->handle, &fmt, frame);
                    probe_log("[%s] getPicture rc=%08x (buf=%p size=%u)", tag, (u32)rc,
                         frame, frame_size);
                    if (rc < 0)
                    {
                        free(frame);
                        frame = NULL;
                        frame_size = 0;
                        break;
                    }

                    pics++;
                }

                got = 1;
                break;
            }

            if ((u32)r != VDEC_ERROR_EMPTY)
            {
                probe_log("[%s] getPicItem rc=%08x", tag, (u32)r);
                break;
            }
            sysUsleep(5000);
        }

        t_decode += now_us() - t0;
        if (!got)
        {
            /* H.264 output lags the input by a few AUs, so a miss here says
             * nothing about the next AU: keep feeding. */
            misses++;
            probe_log("[%s] no picture for au %u (miss %u)", tag, index, misses);
        }
        else
        {
            misses = 0;
        }

        index++;
        if (index >= 60)
            break;
    }

    probe_log("[%s] done: aus=%u pics=%u decode_ms=%llu total_ms=%llu cb(au=%u pic=%u "
         "seq=%u err=%u)",
         tag, index, pics, (unsigned long long)(t_decode / 1000),
         (unsigned long long)((now_us() - t_start) / 1000), g_cb_audone, g_cb_picout,
         g_cb_seqdone, g_cb_error);

    free(au_buf);
    *frame_out = frame;
    *frame_size_out = frame_size;
    *frame_w_out = frame_w;
    *frame_h_out = frame_h;
    *pics_out = pics;
    return pics > 0 ? 0 : -1;
}

/* ------------------------------------------------------------------------- */
/* Display                                                                   */
/* ------------------------------------------------------------------------- */

#define TEX_MAX_W 1920
#define TEX_MAX_H 1088

static int g_video_ready;
static void *g_tex_rgb[2];
static u32 g_off_rgb[2];
static int g_rgb_index;

static int video_init(void)
{
    int rc;

    sysModuleLoad(SYSMODULE_GCM_SYS);
    rc = tiny3d_Init(1024 * 1024);
    probe_log("[video] tiny3d_Init rc=%d res=%ux%u", rc, Video_Resolution.width,
         Video_Resolution.height);
    if (rc != TINY3D_OK || !Video_Resolution.width || !Video_Resolution.height)
        return -1;

    tiny3d_UserViewport(1, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    tiny3d_Project2D();
    tiny3d_Clear(0xff000000, TINY3D_CLEAR_ALL);
    tiny3d_Flip();

    /* Movian's RSX path renders RGB, not tiny3d's fixed YUV shader. Use one
     * ARGB texture and convert YUV in software so the color matrix is
     * explicit and can follow the frame's BT.601/BT.709 selection. */
    for (int i = 0; i < 2; ++i)
    {
        g_tex_rgb[i] = tiny3d_AllocTexture(TEX_MAX_W * TEX_MAX_H * 4);
        if (!g_tex_rgb[i])
        {
            probe_log("[video] texture alloc failed");
            return -1;
        }
        memset(g_tex_rgb[i], 0, TEX_MAX_W * TEX_MAX_H * 4);
        g_off_rgb[i] = tiny3d_TextureOffset(g_tex_rgb[i]);
        probe_log("[video] rgb tex[%d] offset=%u", i, g_off_rgb[i]);
    }

    g_video_ready = 1;
    return 0;
}

static void draw_solid(u32 color, int frames)
{
    if (!g_video_ready)
        return;

    for (int i = 0; i < frames; ++i)
    {
        tiny3d_Clear(color, TINY3D_CLEAR_ALL);
        tiny3d_Flip();
    }
}

/* Movian's YUV->RGB matrix, done on the PPU for the probe so the result does
 * not depend on tiny3d's fixed YUV8 shader. The sample is BT.601 limited. */
static void draw_frame(const u8 *yuv, u32 w, u32 h, u32 layout_h)
{
    u32 disp_h = h & ~1u;
    const u8 *yp = yuv;
    const u8 *up;
    const u8 *vp;
    int idx;
    void *tex;
    u32 off;
    u32 *dst;

    if (!g_video_ready || !yuv || !w || !h)
        return;

    if (!layout_h)
        layout_h = h;
    if (disp_h > TEX_MAX_H)
        disp_h = TEX_MAX_H;
    if (w > TEX_MAX_W)
        return;

    /* Double-buffer the RGB texture: writing the frame into the texture the
     * RSX is currently scanning out shows the old red frame and the new test
     * pattern in the same picture (a visible tear). */
    idx = g_rgb_index;
    g_rgb_index ^= 1;
    tex = g_tex_rgb[idx];
    off = g_off_rgb[idx];
    dst = (u32 *)tex;

    up = yuv + (size_t)w * layout_h;
    vp = up + (size_t)(w * layout_h) / 4;

    for (u32 y = 0; y < disp_h; ++y)
    {
        const u8 *yr = yp + (size_t)y * w;
        const u8 *ur = up + (size_t)(y / 2) * (w / 2);
        const u8 *vr = vp + (size_t)(y / 2) * (w / 2);
        u32 *row = dst + (size_t)y * w;

        for (u32 x = 0; x < w; ++x)
        {
            int c = (int)yr[x] - 16;
            int d = (int)ur[x >> 1] - 128;
            int e = (int)vr[x >> 1] - 128;
            int r, g, b;

            if (c < 0)
                c = 0;

            r = (298 * c + 409 * e + 128) >> 8;
            g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            b = (298 * c + 516 * d + 128) >> 8;

            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;

            row[x] = 0xff000000u | ((u32)r << 16) | ((u32)g << 8) | (u32)b;
        }
    }

    /* Movian-Next found this necessary: the PPU store queue must be flushed
     * before the RSX samples the freshly written texture. */
    __asm__ volatile("sync" ::: "memory");

    tiny3d_Clear(0xff000000, TINY3D_CLEAR_ALL);
    tiny3d_Disable_YUV();
    tiny3d_SetTexture(0, off, w, disp_h, w * 4,
                      TINY3D_TEX_FORMAT_A8R8G8B8, 0);

    tiny3d_SetPolygon(TINY3D_QUADS);
    tiny3d_VertexPos(0, 0, 0);
    tiny3d_VertexTexture(0.0f, 0.0f);
    tiny3d_VertexPos(Video_Resolution.width, 0, 0);
    tiny3d_VertexTexture(1.0f, 0.0f);
    tiny3d_VertexPos(Video_Resolution.width, Video_Resolution.height, 0);
    tiny3d_VertexTexture(1.0f, 1.0f);
    tiny3d_VertexPos(0, Video_Resolution.height, 0);
    tiny3d_VertexTexture(0.0f, 1.0f);
    tiny3d_End();
    tiny3d_Flip();

    probe_log("[video] presented %ux%u (layout_h=%u) rgb BT.601", w, disp_h,
              layout_h);
}

/* ------------------------------------------------------------------------- */
/* Main                                                                      */
/* ------------------------------------------------------------------------- */

static void sys_callback(u64 status, u64 param, void *userdata)
{
    (void)param;
    (void)userdata;

    if (status == SYSUTIL_EXIT_GAME)
    {
        probe_log("[sys] SYSUTIL_EXIT_GAME");
        log_close();
        sysProcessExit(0);
    }
}

static void idle_for(int seconds)
{
    u64 end = now_us() + (u64)seconds * 1000000ULL;

    while (now_us() < end)
    {
        sysUtilCheckCallback();
        sysUsleep(100000);
    }
}

typedef struct
{
    u8 *frame;
    u32 frame_size;
    u32 w, h;
    u32 pics;
    u32 level, spus;
} SampleResult;

static void sample_result_free(SampleResult *r)
{
    if (r->frame)
        free(r->frame);
    memset(r, 0, sizeof(*r));
}

static int run_sample(const u8 *data, u32 size, const char *tag, u32 expect_w,
                      u32 expect_h, SampleResult *r)
{
    static const u32 levels[] = {42, 41, 40};
    /* Movian hands H.264 four SPUs and that is the config that produces
     * frames; the old probe tried 2 first, which opened but never emitted an
     * AUDONE, so it never reached 4. */
    static const u32 spus[] = {4, 3, 2, 1};
    u32 li, si;

    memset(r, 0, sizeof(*r));

    for (li = 0; li < sizeof(levels) / sizeof(levels[0]); ++li)
    {
        for (si = 0; si < sizeof(spus) / sizeof(spus[0]); ++si)
        {
            VdecSession s;
            s32 rc;

            if (open_vdec(&s, levels[li], spus[si]) < 0)
                continue;

            rc = vdecStartSequence(s.handle);
            probe_log("[%s] startSequence(level=%u spus=%u) rc=%08x", tag, levels[li],
                 spus[si], (u32)rc);
            if (rc >= 0)
            {
                /* startSequence returns before the decoder thread is running;
                 * feeding AUs in the same millisecond used to lose them. */
                sysUsleep(100000);
                int ok = decode_stream(&s, data, size, tag, expect_w, expect_h,
                                       &r->frame, &r->frame_size, &r->w, &r->h,
                                       &r->pics);
                close_vdec(&s);
                if (g_vdec_stuck)
                    return -1;
                if (ok == 0)
                {
                    r->level = levels[li];
                    r->spus = spus[si];
                    return 0;
                }
            }
            else
            {
                close_vdec(&s);
                if (g_vdec_stuck)
                    return -1;
            }

            sample_result_free(r);
        }
    }

    return -1;
}

int main(int argc, char **argv)
{
    SampleResult red, test, test720;
    u32 layout_h = 0;
    int red_ok, test_ok, test720_ok;
    s32 load_vdec, load_h264;

    (void)argc;
    (void)argv;

    log_open();
    probe_log("==== pilis3 vdec probe %s ====", PROBE_VERSION);
    probe_log("build %s %s", __DATE__, __TIME__);

    sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sys_callback, NULL);

    load_vdec = sysModuleLoad(SYSMODULE_VDEC);
    load_h264 = sysModuleLoad(SYSMODULE_VDEC_H264);
    probe_log("[sys] load VDEC rc=%08x H264 rc=%08x", (u32)load_vdec, (u32)load_h264);
    probe_log("[sys] loaded VDEC=%d H264=%d", (int)sysModuleIsLoaded(SYSMODULE_VDEC),
         (int)sysModuleIsLoaded(SYSMODULE_VDEC_H264));

    if (video_init() < 0)
        probe_log("[video] init failed; continuing headless");

    /* 1) Red 1080p: constant chroma, used to pin down the plane layout. */
    red_ok = run_sample(sample_1080p_red, sample_1080p_red_size, "red1080", 1920,
                        1080, &red);
    if (red_ok == 0 && red.frame && red.w && red.h)
    {
        /* The blue strip at the bottom of the red frame proves the planes
         * are packed at the visible height (1080), not 1088: picture_size
         * has trailing padding, it does not shift U/V. Use the flat-frame
         * detector's answer. */
        u32 detected = probe_layout(red.frame, red.frame_size, red.w, red.h);
        layout_h = detected ? detected : red.h;
        probe_log("[layout] using plane height %u (picture %ux%u)",
                  layout_h, red.w, red.h);
        if (g_video_ready)
        {
            draw_frame(red.frame, red.w, red.h, layout_h);
            idle_for(6);
        }
    }
    else
    {
        probe_log("[red1080] sample failed");
    }

    /* 2) testsrc 1080p: real visual pattern, same session settings. */
    test_ok = run_sample(sample_1080p, sample_1080p_size, "test1080", 1920, 1080,
                         &test);
    if (test_ok == 0 && test.frame && g_video_ready)
    {
        if (!layout_h)
            layout_h = test.h;
        draw_frame(test.frame, test.w, test.h, layout_h);
    }
    else
    {
        draw_solid(0xff0000ff, 1); /* red: decode failed */
    }

    /* 3) testsrc 720p: tells us whether 1080p alone is the problem. */
    idle_for(4);
    test720_ok = run_sample(sample_720p, sample_720p_size, "test720", 1280, 720,
                            &test720);
    if (test720_ok == 0 && test720.frame && g_video_ready)
    {
        draw_frame(test720.frame, test720.w, test720.h, test720.h);
    }
    else
    {
        probe_log("[test720] sample failed");
    }

    probe_log("[done] summary: red_ok=%d test_ok=%d test720_ok=%d layout_h=%u "
         "red=%ux%u(%u pics) test=%ux%u(%u pics) test720=%ux%u(%u pics) "
         "level=%u spus=%u cb(au=%u pic=%u seq=%u err=%u) lastmsg=%u",
         red_ok, test_ok, test720_ok, layout_h, red.w, red.h, red.pics, test.w,
         test.h, test.pics, test720.w, test720.h, test720.pics, test720.level,
         test720.spus, g_cb_audone, g_cb_picout, g_cb_seqdone, g_cb_error,
         g_cb_last_msg);
    probe_log("[done] log: %s", LOG_PATH);

    sample_result_free(&red);
    sample_result_free(&test);
    sample_result_free(&test720);
    idle_for(20);
    log_close();
    sysProcessExit(0);
    return 0;
}
