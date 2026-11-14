#include "vdec.h"
#include "platform/log.h"
#include "platform/sys.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <malloc.h>
#include <sys/memory.h>
#include <sys/systime.h>
#include <sysmodule/sysmodule.h>

/* PSL1GHT's vdecH264Info has the wrong pic_order_count offset; the fields
 * the player reads live at these offsets in CellVdecAvcInfo. */
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
} VdecH264InfoFixed;

static opd32 g_vdec_opd;

static u32 make_opd32(opd32 *out, void *func)
{
    const u64 *opd = (const u64 *)func;

    out->func = (u32)opd[0];
    out->rtoc = (u32)opd[1];
    return (u32)(uintptr_t)out;
}

static u32 vdec_cb(u32 handle, u32 msgtype, u32 msgdata, u32 arg)
{
    VdecCtx *c = (VdecCtx *)(uintptr_t)arg;

    (void)handle;
    (void)msgdata;

    if (!c)
        return 0;

    switch (msgtype)
    {
    case VDEC_CALLBACK_AUDONE:
        c->audone++;
        break;
    case VDEC_CALLBACK_PICOUT:
        c->picout++;
        break;
    case VDEC_CALLBACK_SEQDONE:
        c->seqdone++;
        break;
    case VDEC_CALLBACK_ERROR:
        c->errors++;
        break;
    default:
        break;
    }
    return 0;
}

static int try_open(VdecCtx *c, int level, int spus)
{
    vdecConfig cfg;
    vdecClosure closure;
    sys_mem_addr_t addr = 0;
    s32 rc;
    s32 mrc;

    memset(c, 0, sizeof(*c));
    c->type.codec_type = VDEC_CODEC_TYPE_H264;
    c->type.profile_level = (u32)level;
    c->level = level;
    c->spus = spus;

    memset(&c->attr, 0, sizeof(c->attr));
    rc = vdecQueryAttr(&c->type, &c->attr);
    if (rc < 0 || !c->attr.mem_size)
        return -1;

    c->mem_size = c->attr.mem_size;
    c->alloc_size = (c->mem_size + (1024 * 1024 - 1)) & ~(1024 * 1024 - 1);
    mrc = sysMemoryAllocate(c->alloc_size, SYS_MEMORY_PAGE_SIZE_1M, &addr);
    log_printf("[vdec] alloc %u bytes rc=%08x addr=%08x", c->alloc_size,
               (u32)mrc, (u32)addr);
    if (mrc != 0 || !addr)
        return -1;

    c->mem = (void *)(uintptr_t)addr;
    memset(c->mem, 0, c->mem_size);

    memset(&cfg, 0, sizeof(cfg));
    cfg.mem_addr = (u32)(uintptr_t)c->mem;
    cfg.mem_size = c->mem_size;
    cfg.ppu_thread_prio = 1000;
    cfg.ppu_thread_stack_size = 0x10000;
    cfg.spu_thread_prio = 100;
    cfg.num_spus = (u32)spus;

    memset(&closure, 0, sizeof(closure));
    closure.fn = make_opd32(&g_vdec_opd, (void *)vdec_cb);
    closure.arg = (u32)(uintptr_t)c;

    log_printf("[vdec] open level=%d spus=%d mem=%p", level, spus, c->mem);
    rc = vdecOpen(&c->type, &cfg, &closure, &c->handle);
    log_printf("[vdec] open rc=%08x handle=%u", (u32)rc, c->handle);
    if (rc < 0)
    {
        sysMemoryFree(addr);
        c->mem = NULL;
        return -1;
    }

    rc = vdecStartSequence(c->handle);
    log_printf("[vdec] startSequence rc=%08x", (u32)rc);
    if (rc < 0)
    {
        vdecClose(c->handle);
        sysMemoryFree(addr);
        c->mem = NULL;
        return -1;
    }

    c->open = 1;
    return 0;
}

int vdec_open(VdecCtx *c, int width, int height)
{
    static const int levels[] = {42, 41, 40};
    static const int spus[] = {4, 3, 2, 1};
    int li, si;

    (void)width;

    sysModuleLoad(SYSMODULE_VDEC);
    sysModuleLoad(SYSMODULE_VDEC_H264);

    for (li = 0; li < (int)(sizeof(levels) / sizeof(levels[0])); ++li)
    {
        for (si = 0; si < (int)(sizeof(spus) / sizeof(spus[0])); ++si)
        {
            if (try_open(c, levels[li], spus[si]) == 0)
            {
                c->disp_w = width;
                c->disp_h = height;
                log_printf("[vdec] ready %dx%d level=%d spus=%d", width, height,
                           levels[li], spus[si]);
                return 0;
            }
            /* try_open may have been called with a stale c; reset it. */
            memset(c, 0, sizeof(*c));
        }
    }

    log_printf("[vdec] no working configuration");
    return -1;
}

int vdec_end_sequence(VdecCtx *c)
{
    int i;

    if (!c->open || c->ended)
        return 0;

    c->seqdone = 0;
    vdecEndSequence(c->handle);
    for (i = 0; i < 5000 && !c->seqdone; ++i)
        sysUsleep(1000);
    c->ended = 1;
    log_printf("[vdec] endSequence seqdone=%u", c->seqdone);

    if (!c->seqdone)
    {
        log_printf("[vdec] SEQDONE missing after 5s");
        return -1;
    }
    return 0;
}

int vdec_close(VdecCtx *c)
{
    int closed = 0;

    if (!c->open)
    {
        if (c->mem)
            sysMemoryFree((sys_mem_addr_t)(uintptr_t)c->mem);
        c->mem = NULL;
        if (c->au_buf)
            free(c->au_buf);
        c->au_buf = NULL;
        c->au_cap = 0;
        return 0;
    }

    if (c->open)
    {
        if (vdec_end_sequence(c) == 0)
        {
            vdecClose(c->handle);
            log_printf("[vdec] close returned");
            closed = 1;
        }
        else
        {
            log_printf("[vdec] close skipped (sequence stuck)");
        }
        c->open = 0;
    }

    if (!closed)
    {
        /* Leave the context and work area alive: the decoder thread still
         * holds c as its callback argument. Freeing it here would let the
         * firmware write into freed stack/heap after we return. */
        return -1;
    }

    if (c->mem)
        sysMemoryFree((sys_mem_addr_t)(uintptr_t)c->mem);
    c->mem = NULL;

    if (c->au_buf)
        free(c->au_buf);
    c->au_buf = NULL;
    c->au_cap = 0;
    return 0;
}

int vdec_feed(VdecCtx *c, const u8 *au, u32 len, long long pts, long long dts)
{
    vdecAU info;
    s32 rc;
    u32 audone_before;
    int i;

    if (!c->open || c->ended || !au || !len)
        return -1;

    if (len > c->au_cap)
    {
        free(c->au_buf);
        c->au_cap = (len + 127u) & ~127u;
        c->au_buf = (u8 *)memalign(128, c->au_cap);
        if (!c->au_buf)
        {
            c->au_cap = 0;
            return -1;
        }
    }
    memcpy(c->au_buf, au, len);

    if (c->feed_count < 8)
    {
        log_printf("[vdec] feed[%d] len=%u pts=%lld dts=%lld", c->feed_count, len,
                   pts, dts);
        c->feed_count++;
    }

    memset(&info, 0, sizeof(info));
    info.packet_addr = (u32)(uintptr_t)c->au_buf;
    info.packet_size = len;
    info.userdata = c->au_seq;
    c->au_pts[c->au_seq & (VDEC_AU_RING - 1)] = pts;
    c->au_seq++;
    info.pts.low = (u32)((u64)pts & 0xffffffffULL);
    info.pts.hi = (u32)((u64)pts >> 32);
    info.dts.low = (u32)((u64)dts & 0xffffffffULL);
    info.dts.hi = (u32)((u64)dts >> 32);

    audone_before = c->audone;
    for (i = 0; i < 100; ++i)
    {
        rc = vdecDecodeAu(c->handle, VDEC_DECODER_MODE_NORMAL, &info);
        if ((u32)rc == VDEC_ERROR_BUSY)
        {
            sysUsleep(2000);
            continue;
        }
        break;
    }

    if (rc < 0)
    {
        log_printf("[vdec] decode rc=%08x len=%u", (u32)rc, len);
        return -1;
    }

    /* Do not wait for AUDONE here: the caller must keep draining PICOUT
     * while it waits, otherwise a full picture queue can block AUDONE and
     * deadlock the feed. */
    (void)audone_before;
    return 0;
}

int vdec_get_picture(VdecCtx *c, u8 *out, u32 cap, VideoFrame *meta)
{
    u32 item = 0;
    s32 rc;
    vdecPicture *pic;
    VdecH264InfoFixed *hi;
    vdecPictureFormat fmt;
    u32 w, h, need, aligned_h, stride;
    u32 om, lo, hi_ts, au_idx;
    long long vpts, au_pts;
    int p;

    if (!c->open || !out || !meta)
        return -1;

    rc = vdecGetPicItem(c->handle, &item);
    if (rc != 0 || !item)
        return ((u32)rc == VDEC_ERROR_EMPTY) ? 0 : -1;

    pic = (vdecPicture *)(uintptr_t)item;
    if (!pic->codec_specific_addr)
        return -1;

    hi = (VdecH264InfoFixed *)(uintptr_t)pic->codec_specific_addr;
    w = hi->width;
    h = hi->height;
    if (!w || !h)
        return -1;

    /* CellVdec pads the coded width and height to macroblock boundaries.
     * 852x480 becomes a 864-wide Y plane; copying it as 852-wide packed
     * rows shears the picture (the "shaking left/right" symptom). Derive
     * the real row stride from picture_size, crop to the display size, and
     * copy with that stride. */
    aligned_h = (h + 15) & ~15u;
    stride = w;
    if (pic->picture_size && aligned_h)
    {
        u32 s = (u32)(((u64)pic->picture_size * 2) / (3 * aligned_h));
        if (s >= w && s <= 4096)
            stride = s;
    }
    if (stride & 1)
        stride++;

    if (c->disp_w > 0 && (u32)c->disp_w < w)
        w = (u32)c->disp_w;
    if (c->disp_h > 0 && (u32)c->disp_h < h)
        h = (u32)c->disp_h;

    /* Order by POC, not by decode/PTS order: B-frames come out of VDEC in
     * decode order and sorting them by PTS can still run backwards when the
     * stream's timestamps are DTS-like. This is Movian's POC extension. */
    om = (u32)(u16)hi->pic_order_count[0] & 0x7fff;
    p = (int)(om >> 13);
    if (hi->idr_picture_flag)
    {
        c->order_base += 0x100000000LL;
        c->poc_ext = 0;
    }
    if (p == ((c->poc_ext + 1) & 3))
    {
        c->poc_ext = p;
        if (p == 0)
            c->order_base += 0x100000000LL;
    }
    need = stride * aligned_h * 3 / 2;
    if (need < pic->picture_size)
        need = pic->picture_size;
    if (need > cap)
    {
        log_printf("[vdec] frame %ux%u stride=%u needs %u > %u", w, h, stride,
                   need, cap);
        return -1;
    }

    memset(&fmt, 0, sizeof(fmt));
    fmt.format_type = VDEC_PICFMT_YUV420P;
    fmt.color_matrix = VDEC_COLOR_MATRIX_BT601;
    fmt.alpha = 0;

    rc = vdecGetPicture(c->handle, &fmt, out);
    if (rc < 0)
    {
        log_printf("[vdec] getPicture rc=%08x", (u32)rc);
        return -1;
    }

    meta->data = out;
    meta->size = need;
    meta->w = (int)w;
    meta->h = (int)h;
    lo = pic->pts[0].low;
    hi_ts = pic->pts[0].hi;
    vpts = (long long)(((u64)hi_ts << 32) | lo);
    if (lo == 0xffffffffu || vpts <= 0)
        vpts = 0;

    /* VDEC leaves pts[] zero on these streams; the AU userdata still points
     * at the packet timestamp we fed. Movian uses the same packet-metadata
     * ring, so take the display PTS from there. */
    au_idx = (u32)pic->userdata[0];
    au_pts = c->au_pts[au_idx & (VDEC_AU_RING - 1)];
    if (au_pts > 0)
        vpts = au_pts;

    meta->layout_h = (int)aligned_h;
    meta->stride = (int)stride;
    meta->pts = vpts;
    meta->order = (long long)c->order_base + om;
    if (p == 3 && c->poc_ext == 0)
        meta->order = (long long)c->order_base + om - 0x100000000LL;

    if (c->pic_count < 8)
    {
        log_printf("[vdec] pic[%d] %ux%u stride=%u plane_h=%u poc=%u "
                   "order=%lld user=%u pts=%lld",
                   c->pic_count, w, h, stride, aligned_h, om,
                   meta->order, au_idx, meta->pts);
        c->pic_count++;
    }

    return 1;
}
