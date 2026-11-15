#include "player.h"

#include "audio.h"
#include "danmaku.h"
#include "dash.h"
#include "h264info.h"
#include "platform/gfx.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "platform/sys.h"
#include "stream.h"
#include "ui/theme.h"
#include "vdec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>

#include <io/pad.h>
#include <limits.h>
#include <malloc.h>
#include <tiny3d.h>
#include <sys/sem.h>
#include <sys/thread.h>
#include <sys/systime.h>

#define VQ_SLOTS 3
#define VSTREAM_BUF (12 * 1024 * 1024)
#define VDEC_MAX_W 1920
#define VDEC_MAX_H 1088
#define VFRAME_MAX (VDEC_MAX_W * VDEC_MAX_H * 3 / 2 + 4096)

struct Player
{
    PlayUrl play;
    VideoItem item;

    NetStream vstream;
    sys_ppu_thread_t vthread;
    int thread_started;
    sys_sem_t free_sem;
    sys_sem_t data_sem;
    u32 q_read;
    u32 q_write;
    u8 *buffers[VQ_SLOTS];
    VideoFrame slots[VQ_SLOTS];

    AudioPlayer *audio;
    DanmakuCtx *danmaku;
    int danmaku_on;

    volatile int abort;
    volatile int error;
    volatile int eof;
    volatile int vdec_stalled;
    volatile int first_frame;
    u32 last_w;
    u32 last_h;
    u32 last_disp_h;

    double start_time;
    double clock_base;
    double video_base;
    int quality_delta;
    u8 *vprefix;
    u32 vprefix_len;
    long long vrange;
    u8 *aprefix;
    u32 aprefix_len;
    long long arange;
    char error_text[160];

    int vdec_emit_count;
    volatile int vdec_skipped;
    long long vdec_last_order;
    long long vdec_next_pts;
    long long vdec_frame_dur;
    long long vdec_max_order;
    long long vdec_flush_to;
    int vdec_have_order;
    int vdec_forced;
    int audio_waiting;
    int present_count;
    int shown_total;
    u64 next_stat_ms;

    /* The VDEC session outlives a seek: EndSequence/StartSequence flush the
     * decoder instead of tearing down and reopening it. */
    VdecCtx *vc;
    int vdec_restart;

    /* DASH indexes are cached so only the first seek pays for the download,
     * and prefetched in the background right after playback starts. */
    DashIndex vidx;
    DashIndex aidx;
    int have_vidx;
    int have_aidx;
    sys_ppu_thread_t idx_thread;
    int idx_started;
    volatile int idx_cancel;

    int paused;
    u64 start_ms;
    u64 osd_until;
    char hint[128];
    u64 hint_until;
};

static void *g_tex_y;
static void *g_tex_u;
static void *g_tex_v;
static u32 g_off_y;
static u32 g_off_u;
static u32 g_off_v;
static int g_tex_ready;
static int g_vdec_disabled;

static int player_gfx_init(void)
{
    if (g_tex_ready)
        return 0;

    g_tex_y = tiny3d_AllocTexture(VDEC_MAX_W * VDEC_MAX_H);
    g_tex_u = tiny3d_AllocTexture((VDEC_MAX_W / 2) * (VDEC_MAX_H / 2));
    g_tex_v = tiny3d_AllocTexture((VDEC_MAX_W / 2) * (VDEC_MAX_H / 2));
    if (!g_tex_y || !g_tex_u || !g_tex_v)
    {
        log_printf("[player] video texture alloc failed");
        return -1;
    }

    memset(g_tex_y, 0, VDEC_MAX_W * VDEC_MAX_H);
    memset(g_tex_u, 0, (VDEC_MAX_W / 2) * (VDEC_MAX_H / 2));
    memset(g_tex_v, 0, (VDEC_MAX_W / 2) * (VDEC_MAX_H / 2));

    g_off_y = tiny3d_TextureOffset(g_tex_y);
    g_off_u = tiny3d_TextureOffset(g_tex_u);
    g_off_v = tiny3d_TextureOffset(g_tex_v);
    g_tex_ready = 1;
    log_printf("[player] video textures y=%u u=%u v=%u", g_off_y, g_off_u, g_off_v);
    return 0;
}

static void player_set_error(Player *p, const char *text)
{
    if (!p->error)
        snprintf(p->error_text, sizeof(p->error_text), "%s", text);
    p->error = 1;
    log_printf("[player] error: %s", text);
}

/* Stream failures all surface as the same text; carry the two numbers that
 * tell them apart (CURLE_* code and the last HTTP status, 0 = no response). */
static void player_set_stream_error(Player *p, const char *what)
{
    char msg[160];

    snprintf(msg, sizeof(msg), "%s (curl=%d http=%ld)", what, p->vstream.curl_code,
             p->vstream.http_status);
    player_set_error(p, msg);
}

static void set_hint(Player *p, const char *text)
{
    snprintf(p->hint, sizeof(p->hint), "%s", text);
    p->hint_until = sys_ms() + 2000;
    p->osd_until = sys_ms() + 5000;
}

static int player_avio_read(void *opaque, uint8_t *buf, int size)
{
    int n = stream_read((NetStream *)opaque, buf, (u32)size);
    if (n <= 0)
        return AVERROR_EOF;
    return n;
}

/* One plane. The fast path matters: at 720P the decoder's stride usually equals
 * the width, so each plane becomes a single memcpy instead of 360-720 rows. */
static void copy_plane(u8 *dst, const u8 *src, int stride, u32 w, u32 h)
{
    u32 y;

    if (stride == (int)w)
    {
        memcpy(dst, src, (size_t)w * h);
        return;
    }

    for (y = 0; y < h; ++y)
        memcpy(dst + (size_t)y * w, src + (size_t)y * stride, w);
}

/* Pack one decoded frame into the tightly packed layout the presenter wants:
 * stride == width and the three planes back to back. The decoder's linesize is
 * usually padded, so this cannot be a plain memcpy. */
static int frame_to_slot(const AVFrame *f, u8 *dst, u32 cap, VideoFrame *meta,
                         long long pts)
{
    u32 w, h, cw, ch, need;

    if (!f || !f->data[0] || !f->data[1] || !f->data[2])
        return -1;
    /* Planar 8-bit only; anything else (NV12, 10-bit) would be copied as
     * garbage, so drop it instead. */
    if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P)
        return -1;

    w = (u32)f->width;
    h = (u32)f->height & ~1u;
    cw = w / 2;
    ch = h / 2;
    need = w * h + cw * ch * 2;
    if (!w || !h || need > cap)
        return -1;

    copy_plane(dst, f->data[0], f->linesize[0], w, h);
    copy_plane(dst + (size_t)w * h, f->data[1], f->linesize[1], cw, ch);
    copy_plane(dst + (size_t)w * h + (size_t)cw * ch, f->data[2], f->linesize[2], cw, ch);

    meta->data = dst;
    meta->size = need;
    meta->w = (int)w;
    meta->h = (int)h;
    meta->layout_h = (int)h;
    meta->stride = (int)w;
    meta->pts = pts;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* VDEC hardware path                                                        */
/* ------------------------------------------------------------------------- */

#define VDEC_REORDER 12

typedef struct
{
    u32 slot;
    VideoFrame meta;
    int used;
} VdecReorder;

/* Grab a display slot. The decode thread must never sleep on the presenter:
 * when the queue is full the caller drops the frame and keeps decoding. Only
 * a presenter that has not freed a slot for three seconds counts as wedged. */
/* Flow control: the player thread waits here while the display queue is
 * full. The decoder itself never blocks on this (the PICOUT callback drains
 * it), so waiting only paces the player thread. A presenter that frees no
 * slot for three seconds is treated as wedged. */
static int vdec_take_slot(Player *p)
{
    u64 start = sys_ms();
    int logged = 0;

    while (!p->abort && sysSemTryWait(p->free_sem) != 0)
    {
        if (!logged && sys_ms() - start > 500)
        {
            log_printf("[player] vdec queue full, waiting for presenter");
            logged = 1;
        }
        if (sys_ms() - start > 3000)
        {
            log_printf("[player] vdec presenter stalled for 3s");
            p->vdec_stalled = 1;
            player_set_error(p, "视频显示卡住");
            return -1;
        }
        sysUsleep(5000);
    }
    return p->abort ? -1 : 0;
}

static long long vdec_smallest_order(VdecReorder *r)
{
    int i;
    long long best = 0;
    int have = 0;

    for (i = 0; i < VDEC_REORDER; ++i)
    {
        if (!r[i].used)
            continue;
        if (!have || r[i].meta.order < best)
        {
            best = r[i].meta.order;
            have = 1;
        }
    }
    return have ? best : LLONG_MAX;
}

/* Emit the smallest POC order held by the reorder buffer, but only when it is
 * no greater than limit (Movian's flush_to lookahead).
 * Returns 0 when a frame was emitted or deliberately skipped, -1 when the
 * player is shutting down or the presenter stopped consuming entirely. */
static int vdec_emit_smallest(Player *p, VdecCtx *vc, VdecReorder *r,
                              int *count, long long limit)
{
    int i, best = -1;
    long long best_order = 0;

    if (*count <= 0)
        return 0;

    for (i = 0; i < VDEC_REORDER; ++i)
    {
        if (!r[i].used)
            continue;
        if (best < 0 || r[i].meta.order < best_order)
        {
            best = i;
            best_order = r[i].meta.order;
        }
    }
    if (best < 0)
        return 0;
    if (limit != LLONG_MAX && best_order > limit)
        return 0;

    /* Use the AU PTS from userdata when VDEC provided one; fall back to a
     * monotonic frame clock only if it did not. The clock advances for
     * skipped frames too, so pacing does not fall behind. */
    if (r[best].meta.pts <= 0)
        r[best].meta.pts = p->vdec_next_pts;
    p->vdec_next_pts = r[best].meta.pts +
                       (p->vdec_frame_dur > 0 ? p->vdec_frame_dur : 3000);

    if (vdec_take_slot(p) < 0)
        return -1;

    if (p->vdec_emit_count < 16)
        log_printf("[player] vdec emit #%d order=%lld pts=%lld count=%d",
                   p->vdec_emit_count, r[best].meta.order, r[best].meta.pts,
                   *count);
    if (p->vdec_emit_count > 0 && r[best].meta.order < p->vdec_last_order)
        log_printf("[player] vdec emit order backwards %lld -> %lld",
                   p->vdec_last_order, r[best].meta.order);
    p->vdec_last_order = r[best].meta.order;
    p->vdec_emit_count++;

    {
        u32 idx = p->q_write;
        VideoFrame m = r[best].meta;

        memcpy(p->buffers[idx], m.data, m.size);
        m.data = p->buffers[idx];
        p->slots[idx] = m;
        p->q_write = (idx + 1) % VQ_SLOTS;
        sysSemPost(p->data_sem, 1);
    }

    /* The slot is copied out, so the decoder may reuse it now. */
    vdec_pic_release(vc, r[best].slot);

    if (p->audio_waiting && p->audio)
    {
        p->audio_waiting = 0;
        if (!p->paused)
            audio_player_set_paused(p->audio, 0);
        log_printf("[player] audio starts with video");
    }

    r[best].used = 0;
    (*count)--;
    return 0;
}

static void vdec_pump(Player *p, VdecCtx *vc, VdecReorder *r, int *count)
{
    for (;;)
    {
        int i, free_idx = -1;
        VdecPic pic;

        if (p->abort || p->vdec_stalled)
            break;

        for (i = 0; i < VDEC_REORDER; ++i)
        {
            if (!r[i].used)
            {
                free_idx = i;
                break;
            }
        }

        if (free_idx < 0)
        {
            if (p->vdec_flush_to >= 0)
            {
                while (*count > 0 &&
                       vdec_smallest_order(r) <= p->vdec_flush_to)
                {
                    if (vdec_emit_smallest(p, vc, r, count,
                                           p->vdec_flush_to) < 0)
                        return;
                }
            }
            if (*count >= VDEC_REORDER)
            {
                /* POC gap wider than the window: emit the smallest anyway.
                 * Every frame carries its real packet PTS now, so this only
                 * gives up the lookahead a full window cannot afford. */
                if (!p->vdec_forced)
                {
                    log_printf("[player] reorder window full, emitting anyway");
                    p->vdec_forced = 1;
                }
                if (vdec_emit_smallest(p, vc, r, count, LLONG_MAX) < 0)
                    return;
            }
            continue;
        }

        if (vdec_pic_take(vc, &pic) <= 0)
            break;

        if (!p->vdec_have_order)
        {
            p->vdec_max_order = pic.f.order;
            p->vdec_have_order = 1;
        }
        else if (pic.f.order > p->vdec_max_order)
        {
            p->vdec_flush_to = p->vdec_max_order;
            p->vdec_max_order = pic.f.order;
        }

        r[free_idx].slot = pic.slot;
        r[free_idx].meta = pic.f;
        r[free_idx].used = 1;
        (*count)++;

        if (p->vdec_flush_to >= 0)
        {
            while (*count > 0 && vdec_smallest_order(r) <= p->vdec_flush_to)
            {
                if (vdec_emit_smallest(p, vc, r, count,
                                       p->vdec_flush_to) < 0)
                    return;
            }
        }
    }
}

static int vdec_wait_audone(Player *p, VdecCtx *vc, VdecReorder *r, int *count,
                            u32 before)
{
    u64 start = sys_ms();

    while (!p->abort && !p->vdec_stalled && vc->audone == before)
    {
        vdec_pump(p, vc, r, count);
        if (sys_ms() - start > 5000)
        {
            log_printf("[vdec] AUDONE missing after 5s");
            return -1;
        }
        sysUsleep(1000);
    }
    return (p->abort || p->vdec_stalled) ? -1 : 0;
}

/* CellVdec sizes its work area from the level alone, so the DPB cannot hold
 * as many reference frames as a desktop decoder. Movian refuses streams past
 * the hardware limit; without this the decoder stalls mid-stream and never
 * reports AUDONE. Limits from its h264_parser check. */
static int vdec_refs_supported(const AVCodecParameters *par)
{
    H264StreamInfo si;

    if (h264_parse_extradata(par->extradata, par->extradata_size, &si) < 0)
        return 1;

    if (si.mb_height >= 68 && si.num_ref_frames > 4)
    {
        log_printf("[vdec] %dx%d needs %d ref frames; CellVdec supports 4 at "
                   "this size", si.width, si.height, si.num_ref_frames);
        return 0;
    }
    if (si.mb_height >= 45 && si.num_ref_frames > 9)
    {
        log_printf("[vdec] %dx%d needs %d ref frames; CellVdec supports 9 at "
                   "this size", si.width, si.height, si.num_ref_frames);
        return 0;
    }
    return 1;
}

/* Close the VDEC session owned by the player. A stuck sequence is left alive
 * (the firmware still holds the context) and VDEC is disabled for the run. */
static void vdec_release(Player *p)
{
    if (!p->vc)
        return;

    if (vdec_close(p->vc) == 0)
        free(p->vc);
    else
    {
        g_vdec_disabled = 1;
        log_printf("[player] vdec disabled for this run (session stuck)");
    }
    p->vc = NULL;
}

static int vdec_playback(Player *p, AVFormatContext *fmt, int vstream, AVRational tb)
{
    const AVStream *st = fmt->streams[vstream];
    const AVBitStreamFilter *filter;
    AVBSFContext *bsfc = NULL;
    AVPacket *pkt = NULL;
    AVPacket *out = NULL;
    VdecCtx *vc = NULL;
    VdecReorder reorder[VDEC_REORDER];
    int count = 0;
    int failed = 0;
    u32 dropped = 0;
    int ret = -1;
    long long last_pts = 0;

    if (st->codecpar->codec_id == AV_CODEC_ID_H264 &&
        !vdec_refs_supported(st->codecpar))
        return -1;

    if (!p->vc)
    {
        p->vc = (VdecCtx *)calloc(1, sizeof(*p->vc));
        if (!p->vc)
            return -1;
        if (vdec_open(p->vc, st->codecpar->width, st->codecpar->height,
                      VFRAME_MAX) < 0)
        {
            free(p->vc);
            p->vc = NULL;
            return -1;
        }
    }
    else if (p->vdec_restart)
    {
        /* Same codec, new position: flush in place instead of reopening. */
        p->vdec_restart = 0;
        if (vdec_flush_sequence(p->vc) < 0)
        {
            log_printf("[player] vdec flush failed, falling back to software");
            g_vdec_disabled = 1;
            return -1;
        }
    }

    vc = p->vc;
    p->vdec_flush_to = -1;
    p->vdec_have_order = 0;
    p->vdec_max_order = 0;
    memset(reorder, 0, sizeof(reorder));

    filter = av_bsf_get_by_name("h264_mp4toannexb");
    if (!filter)
    {
        log_printf("[player] h264_mp4toannexb unavailable");
        goto done;
    }

    if (av_bsf_alloc(filter, &bsfc) < 0 ||
        avcodec_parameters_copy(bsfc->par_in, st->codecpar) < 0)
        goto done;
    bsfc->time_base_in = st->time_base;
    if (av_bsf_init(bsfc) < 0)
        goto done;
    log_printf("[player] vdec bsf ready");

    {
        double fps = av_q2d(st->avg_frame_rate);
        if (fps < 1.0)
            fps = av_q2d(st->r_frame_rate);
        if (fps < 1.0)
            fps = 30.0;
        p->vdec_frame_dur = (long long)(90000.0 / fps + 0.5);
        p->vdec_next_pts = (long long)(p->video_base * 90000.0);
        log_printf("[player] vdec fps_x100=%d frame_dur=%lld start_pts=%lld",
                   (int)(fps * 100.0 + 0.5), p->vdec_frame_dur,
                   p->vdec_next_pts);
    }

    pkt = av_packet_alloc();
    out = av_packet_alloc();
    if (!pkt || !out)
        goto done;

    ret = 0;

    while (!p->abort)
    {
        int rr = av_read_frame(fmt, pkt);

        if (rr < 0)
        {
            log_printf("[player] vdec demux eof %d", rr);
            break;
        }

        if (pkt->stream_index != vstream)
        {
            av_packet_unref(pkt);
            continue;
        }

        if (av_bsf_send_packet(bsfc, pkt) < 0)
        {
            av_packet_unref(pkt);
            continue;
        }
        av_packet_unref(pkt);

        while (!p->abort && av_bsf_receive_packet(bsfc, out) == 0)
        {
            long long pts, dts;

            if (out->pts == AV_NOPTS_VALUE)
                pts = last_pts + 3000;
            else
                pts = av_rescale_q(out->pts, st->time_base, tb);
            if (out->dts == AV_NOPTS_VALUE)
                dts = pts;
            else
                dts = av_rescale_q(out->dts, st->time_base, tb);
            last_pts = pts;

            {
                u32 aud_before = vc->audone;

                if (vdec_feed(vc, out->data, (u32)out->size, pts, dts) < 0)
                {
                    failed = 1;
                    av_packet_unref(out);
                    break;
                }

                av_packet_unref(out);
                vdec_pump(p, vc, reorder, &count);
                if (vdec_wait_audone(p, vc, reorder, &count, aud_before) < 0)
                {
                    failed = 1;
                    break;
                }
            }
        }

        if (failed)
            break;
    }

    if (!p->abort && !failed)
    {
        av_bsf_send_packet(bsfc, NULL);
        while (av_bsf_receive_packet(bsfc, out) == 0)
        {
            long long pts, dts;

            if (out->pts == AV_NOPTS_VALUE)
                pts = last_pts + 3000;
            else
                pts = av_rescale_q(out->pts, st->time_base, tb);
            if (out->dts == AV_NOPTS_VALUE)
                dts = pts;
            else
                dts = av_rescale_q(out->dts, st->time_base, tb);
            last_pts = pts;

            {
                u32 aud_before = vc->audone;

                if (vdec_feed(vc, out->data, (u32)out->size, pts, dts) < 0)
                {
                    av_packet_unref(out);
                    failed = 1;
                    break;
                }

                av_packet_unref(out);
                vdec_pump(p, vc, reorder, &count);
                if (vdec_wait_audone(p, vc, reorder, &count, aud_before) < 0)
                {
                    failed = 1;
                    break;
                }
            }
        }

        log_printf("[player] vdec end sequence");
        if (vdec_end_sequence(vc) == 0)
            vdec_pump(p, vc, reorder, &count);
        while (count > 0 && !p->abort)
        {
            if (vdec_emit_smallest(p, vc, reorder, &count, LLONG_MAX) < 0)
                break;
        }
    }

done:
    /* An abort (pause/seek/close) is not a decoder failure. */
    if (failed && !p->abort)
        ret = -1;
    dropped = vc ? vc->pic_dropped : 0;

    av_packet_free(&pkt);
    av_packet_free(&out);
    av_bsf_free(&bsfc);

    log_printf("[player] vdec done, frames left=%d failed=%d emitted=%d "
               "skipped=%d dropped=%u",
               count, failed, p->vdec_emit_count, p->vdec_skipped, dropped);
    return ret;
}

static void video_thread(void *arg)
{
    Player *p = (Player *)arg;
    AVFormatContext *fmt = NULL;
    AVIOContext *avio = NULL;
    AVCodecContext *dec = NULL;
    AVFrame *frame = NULL;
    AVPacket *pkt = NULL;
    int vstream = -1;
    AVRational tb = {1, 90000};
    long long last_pts = 0;

    if (stream_start_prefixed(&p->vstream, p->play.video_url, VSTREAM_BUF,
                              p->vprefix, p->vprefix_len, p->vrange) < 0)
    {
        player_set_error(p, "视频流连接失败");
        p->eof = 1;
        sysThreadExit(0);
    }

    fmt = avformat_alloc_context();
    avio = avio_alloc_context((unsigned char *)av_malloc(65536), 65536, 0,
                              &p->vstream, player_avio_read, NULL, NULL);
    if (!fmt || !avio)
    {
        player_set_error(p, "FFmpeg IO 初始化失败");
        p->eof = 1;
        sysThreadExit(0);
    }

    fmt->pb = avio;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

    if (avformat_open_input(&fmt, NULL, NULL, NULL) < 0)
    {
        player_set_stream_error(p, "视频解封装失败");
        p->eof = 1;
        sysThreadExit(0);
    }

    if (avformat_find_stream_info(fmt, NULL) < 0)
    {
        player_set_stream_error(p, "视频流信息读取失败");
        p->eof = 1;
        sysThreadExit(0);
    }

    vstream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vstream < 0)
    {
        player_set_error(p, "没有视频轨");
        p->eof = 1;
        sysThreadExit(0);
    }

    if (fmt->streams[vstream]->codecpar->codec_id != AV_CODEC_ID_H264)
    {
        player_set_error(p, "当前只支持 H.264/AVC 视频流");
        p->eof = 1;
        sysThreadExit(0);
    }

    /* Hardware first unless the user selected software in Settings. If VDEC
     * cannot open or the BSF is missing, fall through to libavcodec. */
    if (!settings_sw_decode() && !g_vdec_disabled)
    {
        log_printf("[player] trying VDEC hardware decode");
        if (vdec_playback(p, fmt, vstream, tb) == 0)
        {
            p->eof = 1;
            log_printf("[player] vdec video thread done");
            avformat_close_input(&fmt);
            if (avio)
            {
                av_freep(&avio->buffer);
                avio_context_free(&avio);
            }
            sysThreadExit(0);
        }
        log_printf("[player] VDEC unavailable, falling back to software");
        vdec_release(p);
    }

    /* Software path: packets are AVCC; libavcodec handles them directly. */
    {
        const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);

        if (!codec)
        {
            player_set_error(p, "没有可用的 H.264 解码器");
            p->eof = 1;
            sysThreadExit(0);
        }

        dec = avcodec_alloc_context3(codec);
        if (!dec ||
            avcodec_parameters_to_context(dec, fmt->streams[vstream]->codecpar) < 0 ||
            avcodec_open2(dec, codec, NULL) < 0)
        {
            player_set_error(p, "H.264 软解初始化失败");
            p->eof = 1;
            sysThreadExit(0);
        }

        frame = av_frame_alloc();
        if (!frame)
        {
            player_set_error(p, "解码帧缓冲分配失败");
            p->eof = 1;
            sysThreadExit(0);
        }

        log_printf("[player] software decode %dx%d",
                   fmt->streams[vstream]->codecpar->width,
                   fmt->streams[vstream]->codecpar->height);
    }

    if (p->audio_waiting && p->audio)
    {
        p->audio_waiting = 0;
        if (!p->paused)
            audio_player_set_paused(p->audio, 0);
        log_printf("[player] audio starts with software decode");
    }

    pkt = av_packet_alloc();

    while (!p->abort)
    {
        int rc = av_read_frame(fmt, pkt);

        if (rc < 0)
            break;

        if (pkt->stream_index != vstream)
        {
            av_packet_unref(pkt);
            continue;
        }

        if (avcodec_send_packet(dec, pkt) < 0)
        {
            av_packet_unref(pkt);
            continue;
        }
        av_packet_unref(pkt);

        while (!p->abort && avcodec_receive_frame(dec, frame) == 0)
        {
            long long pts;

            if (frame->pts == AV_NOPTS_VALUE)
                pts = last_pts + 3000;
            else
                pts = av_rescale_q(frame->pts, fmt->streams[vstream]->time_base, tb);
            last_pts = pts;

            sysSemWait(p->free_sem, 0);
            if (p->abort)
            {
                av_frame_unref(frame);
                break;
            }

            {
                u32 idx = p->q_write;
                VideoFrame meta;

                if (frame_to_slot(frame, p->buffers[idx], VFRAME_MAX, &meta, pts) == 0)
                {
                    p->slots[idx] = meta;
                    p->q_write = (idx + 1) % VQ_SLOTS;
                    sysSemPost(p->data_sem, 1);
                }
                else
                {
                    sysSemPost(p->free_sem, 1);
                }
            }

            av_frame_unref(frame);
        }
    }

    p->eof = 1;
    log_printf("[player] video thread done, error=%d", p->error);

    avcodec_free_context(&dec);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avformat_close_input(&fmt);
    if (avio)
    {
        av_freep(&avio->buffer);
        avio_context_free(&avio);
    }
    sysThreadExit(0);
}

static double player_clock(Player *p);

static void present_frame(Player *p)
{
    VideoFrame *f = &p->slots[p->q_read];
    u32 disp_h = (u32)f->h & ~1u;
    u32 y_size;
    u32 src_stride = f->stride > 0 ? (u32)f->stride : (u32)f->w;
    const u8 *src_u;
    const u8 *src_v;

    if (!g_tex_ready || !f->data || f->w <= 0 || f->h <= 0)
        return;

    if (disp_h > VDEC_MAX_H)
        disp_h = VDEC_MAX_H;
    if (f->w > VDEC_MAX_W)
        return;

    y_size = (u32)f->w * disp_h;
    if (src_stride == (u32)f->w)
    {
        memcpy(g_tex_y, f->data, y_size);
    }
    else
    {
        for (u32 y = 0; y < disp_h; ++y)
            memcpy((u8 *)g_tex_y + (size_t)y * f->w,
                   f->data + (size_t)y * src_stride, f->w);
    }

    src_u = f->data + (size_t)src_stride * f->layout_h;
    src_v = src_u + (size_t)(src_stride * f->layout_h) / 4;
    for (u32 y = 0; y < disp_h / 2; ++y)
    {
        const u8 *su = src_u + (size_t)y * (src_stride / 2);
        const u8 *sv = src_v + (size_t)y * (src_stride / 2);

        memcpy((u8 *)g_tex_u + (size_t)y * (f->w / 2), su, f->w / 2);
        memcpy((u8 *)g_tex_v + (size_t)y * (f->w / 2), sv, f->w / 2);
    }

    p->last_w = f->w;
    p->last_h = f->h;
    p->last_disp_h = disp_h;

    p->shown_total++;

    if (p->present_count < 16)
    {
        double clk = player_clock(p) - p->clock_base;
        log_printf("[player] present #%d order=%lld pts=%lld ms clock=%ld ms",
                   p->present_count, f->order, (long long)(f->pts / 90),
                   (long)(clk * 1000.0));
        p->present_count++;
    }

    p->q_read = (p->q_read + 1) % VQ_SLOTS;
    sysSemPost(p->free_sem, 1);
    p->first_frame = 1;
}

static void draw_video(Player *p)
{
    int dw, dh, ox, oy;

    if (!p->first_frame || !p->last_w || !p->last_h)
        return;

    dw = GFX_W;
    dh = (int)((long long)GFX_W * p->last_h / p->last_w);
    if (dh > GFX_H)
    {
        dh = GFX_H;
        dw = (int)((long long)GFX_H * p->last_w / p->last_h);
    }
    ox = (GFX_W - dw) / 2;
    oy = (GFX_H - dh) / 2;

    tiny3d_Clear(0xff000000, TINY3D_CLEAR_ALL);
    tiny3d_Enable_YUV(USE_YUV_8BIT_TEXTURES);
    tiny3d_SetTexture(0, g_off_y, p->last_w, p->last_disp_h, p->last_w,
                      TINY3D_TEX_FORMAT_L8, 0);
    tiny3d_SetTexture(1, g_off_u, p->last_w / 2, p->last_disp_h / 2,
                      p->last_w / 2, TINY3D_TEX_FORMAT_L8, 0);
    tiny3d_SetTexture(2, g_off_v, p->last_w / 2, p->last_disp_h / 2,
                      p->last_w / 2, TINY3D_TEX_FORMAT_L8, 0);

    tiny3d_SetPolygon(TINY3D_QUADS);
    tiny3d_VertexPos(ox, oy, 0);
    tiny3d_VertexTexture(0.0f, 0.0f);
    tiny3d_VertexPos(ox + dw, oy, 0);
    tiny3d_VertexTexture(1.0f, 0.0f);
    tiny3d_VertexPos(ox + dw, oy + dh, 0);
    tiny3d_VertexTexture(1.0f, 1.0f);
    tiny3d_VertexPos(ox, oy + dh, 0);
    tiny3d_VertexTexture(0.0f, 1.0f);
    tiny3d_End();
    tiny3d_Disable_YUV();
}

static double player_clock(Player *p)
{
    if (p->audio && audio_player_ready(p->audio))
        return audio_player_clock(p->audio);
    return p->clock_base + (double)(sys_ms() - p->start_ms) / 1000.0;
}

Player *player_open(const PlayUrl *play, const VideoItem *item)
{
    return player_open_at(play, item, 0.0);
}

/* Loading the sidx costs a couple of range requests. Do it right after
 * playback starts so the first seek does not have to wait for the network. */
static void index_thread(void *arg)
{
    Player *p = (Player *)arg;
    char err[128];

    if (p->play.video_index[0])
    {
        if (dash_load_ex(p->play.video_url, p->play.video_init,
                         p->play.video_index, &p->vidx, err, sizeof(err),
                         &p->idx_cancel) == 0)
            p->have_vidx = 1;
        else if (!p->idx_cancel)
            log_printf("[player] video index prefetch failed: %s", err);
    }

    if (!p->idx_cancel && p->play.audio_url[0] && p->play.audio_index[0])
    {
        if (dash_load_ex(p->play.audio_url, p->play.audio_init,
                         p->play.audio_index, &p->aidx, err, sizeof(err),
                         &p->idx_cancel) == 0)
            p->have_aidx = 1;
        else if (!p->idx_cancel)
            log_printf("[player] audio index prefetch failed: %s", err);
    }

    if (!p->idx_cancel)
        log_printf("[player] indexes ready (v=%d a=%d)", p->have_vidx,
                   p->have_aidx);
    sysThreadExit(0);
}

static void index_prefetch_start(Player *p)
{
    if (p->idx_started || p->have_vidx || !p->play.video_index[0])
        return;

    if (sysThreadCreate(&p->idx_thread, index_thread, p, 1200, 0x40000, 0,
                        "pilis3idx") < 0)
    {
        log_printf("[player] index prefetch thread failed");
        return;
    }
    p->idx_started = 1;
}

/* Waits for the prefetch; a no-op when it already finished or never ran. */
static void index_prefetch_join(Player *p)
{
    if (!p->idx_started)
        return;
    sysThreadJoin(p->idx_thread, NULL);
    p->idx_started = 0;
}

static int replace_prefix(u8 **dst, u32 *dst_len, const u8 *src, u32 len)
{
    u8 *buf = NULL;

    if (src && len)
    {
        buf = (u8 *)malloc(len);
        if (!buf)
            return -1;
        memcpy(buf, src, len);
    }

    free(*dst);
    *dst = buf;
    *dst_len = buf ? len : 0;
    return 0;
}

/* Resolve a playback position to byte ranges. The DASH index is cached in the
 * player, so only the first seek pays for downloading it. */
static int player_setup_position(Player *p, double target)
{
    char err[128] = "";
    u64 voff = 0;
    double vtime = 0.0;

    /* The prefetch may still be in flight; waiting here is cheaper than
     * downloading the same index twice, and it makes the fields stable. */
    index_prefetch_join(p);

    if (!p->play.video_index[0])
        return -1;

    if (!p->have_vidx)
    {
        if (dash_load(p->play.video_url, p->play.video_init,
                      p->play.video_index, &p->vidx, err, sizeof(err)) < 0)
        {
            log_printf("[player] video seek index failed: %s", err);
            return -1;
        }
        p->have_vidx = 1;
    }

    if (dash_find(&p->vidx, target, &voff, &vtime) < 0)
        return -1;

    if (replace_prefix(&p->vprefix, &p->vprefix_len, p->vidx.init,
                       p->vidx.init_len) < 0)
        return -1;

    p->vrange = (long long)voff;
    p->video_base = vtime;
    p->clock_base = vtime;

    if (p->play.audio_url[0] && p->play.audio_index[0])
    {
        u64 aoff = 0;
        double atime = 0.0;

        if (!p->have_aidx)
        {
            if (dash_load(p->play.audio_url, p->play.audio_init,
                          p->play.audio_index, &p->aidx, err, sizeof(err)) < 0)
            {
                log_printf("[player] audio seek index failed: %s", err);
                p->play.audio_url[0] = 0;
            }
            else
            {
                p->have_aidx = 1;
            }
        }

        if (p->have_aidx && dash_find(&p->aidx, target, &aoff, &atime) == 0)
        {
            if (replace_prefix(&p->aprefix, &p->aprefix_len, p->aidx.init,
                               p->aidx.init_len) < 0)
                return -1;
            p->arange = (long long)aoff;
            p->clock_base = atime;
        }
        else
        {
            p->play.audio_url[0] = 0;
        }
    }

    log_printf("[player] seek %.1fs -> video %.1fs off=%lld audio %.1fs",
               target, vtime, p->vrange, p->clock_base);
    return 0;
}

Player *player_open_at(const PlayUrl *play, const VideoItem *item, double start_time)
{
    Player *p;
    int rc;

    if (!play || (!play->video_url[0] && !play->mp4_url[0]))
        return NULL;

    if (player_gfx_init() < 0)
        return NULL;

    p = (Player *)calloc(1, sizeof(*p));
    if (!p)
        return NULL;

    p->play = *play;
    if (item)
        p->item = *item;
    if (!p->play.video_url[0])
        snprintf(p->play.video_url, sizeof(p->play.video_url), "%s", p->play.mp4_url);
    if (!p->play.audio_url[0])
        snprintf(p->play.audio_url, sizeof(p->play.audio_url), "%s", p->play.mp4_url);

    if (start_time < 0.0)
        start_time = 0.0;
    if (start_time > 0.5)
    {
        if (player_setup_position(p, start_time) < 0)
        {
            log_printf("[player] cannot start at %.1fs; starting from 0",
                       start_time);
            start_time = 0.0;
            if (p->vprefix)
            {
                free(p->vprefix);
                p->vprefix = NULL;
                p->vprefix_len = 0;
            }
            if (p->aprefix)
            {
                free(p->aprefix);
                p->aprefix = NULL;
                p->aprefix_len = 0;
            }
            p->vrange = 0;
            p->arange = 0;
            p->video_base = 0.0;
            p->clock_base = 0.0;
        }
    }

    p->start_time = start_time;
    p->start_ms = sys_ms();
    p->next_stat_ms = p->start_ms + 2000;
    p->osd_until = p->start_ms + 5000;

    rc = sys_sem_create(&p->free_sem, "p3vqf", VQ_SLOTS, VQ_SLOTS);
    if (rc < 0)
    {
        if (p->vprefix)
            free(p->vprefix);
        free(p);
        return NULL;
    }
    rc = sys_sem_create(&p->data_sem, "p3vqd", 0, VQ_SLOTS);
    if (rc < 0)
    {
        sysSemDestroy(p->free_sem);
        if (p->vprefix)
            free(p->vprefix);
        free(p);
        return NULL;
    }

    for (int i = 0; i < VQ_SLOTS; ++i)
        p->buffers[i] = (u8 *)memalign(128, VFRAME_MAX);

    if (p->play.audio_url[0])
    {
        p->audio = audio_player_start_at(p->play.audio_url, p->aprefix,
                                         p->aprefix_len, p->arange,
                                         p->clock_base);
        if (p->audio)
        {
            /* Do not let the audio clock run while VDEC is still opening and
             * feeding its first AUs; otherwise the first video frame is
             * already >1 s late and the player fast-forwards to catch up. */
            audio_player_set_paused(p->audio, 1);
            p->audio_waiting = 1;
        }
    }

    if (p->aprefix)
    {
        free(p->aprefix);
        p->aprefix = NULL;
        p->aprefix_len = 0;
    }

    p->danmaku_on = 1;
    p->danmaku = danmaku_load_start(p->item.cid);
    danmaku_reset();

    rc = sysThreadCreate(&p->vthread, video_thread, p, 1100, 0x60000, 0,
                         "pilis3vid");
    if (rc < 0)
    {
        player_set_error(p, "视频线程创建失败");
        return p;
    }
    p->thread_started = 1;

    /* Fetch the sidx now so the first seek does not block the UI. */
    index_prefetch_start(p);

    log_printf("[player] opened %s (%dx%d) audio=%d start=%.1f",
               p->play.video_url, p->play.width, p->play.height,
               p->audio != NULL, p->start_time);
    return p;
}

int player_seek(Player *p, double target)
{
    double duration;
    int rc;

    if (!p)
        return -1;

    duration = p->play.duration > 0 ? p->play.duration : p->item.duration;
    if (target < 0.0)
        target = 0.0;
    if (duration > 2.0 && target > duration - 2.0)
        target = duration - 2.0;
    if (target < 0.0)
        target = 0.0;

    /* Stop the current run but keep the VDEC session: the next run flushes it
     * with EndSequence/StartSequence instead of reopening the decoder. */
    p->abort = 1;
    stream_abort(&p->vstream);
    if (p->thread_started)
    {
        sysThreadJoin(p->vthread, NULL);
        p->thread_started = 0;
    }
    stream_stop(&p->vstream);

    /* No producer left, so the display queue can be emptied. */
    while (sysSemTryWait(p->data_sem) == 0)
        sysSemPost(p->free_sem, 1);
    p->q_read = 0;
    p->q_write = 0;

    p->abort = 0;
    p->error = 0;
    p->error_text[0] = 0;
    p->eof = 0;
    p->vdec_stalled = 0;
    p->vdec_restart = 1;
    p->first_frame = 1;

    if (player_setup_position(p, target) < 0)
    {
        player_set_error(p, "该视频不支持跳转");
        return -1;
    }

    p->start_ms = sys_ms();
    p->next_stat_ms = p->start_ms + 2000;

    if (p->audio)
    {
        audio_player_stop(p->audio);
        p->audio = NULL;
    }
    if (p->play.audio_url[0])
    {
        p->audio = audio_player_start_at(p->play.audio_url, p->aprefix,
                                         p->aprefix_len, p->arange,
                                         p->clock_base);
        if (p->audio)
        {
            audio_player_set_paused(p->audio, 1);
            p->audio_waiting = 1;
        }
    }

    rc = sysThreadCreate(&p->vthread, video_thread, p, 1100, 0x60000, 0,
                         "pilis3vid");
    if (rc < 0)
    {
        player_set_error(p, "视频线程创建失败");
        return -1;
    }
    p->thread_started = 1;

    log_printf("[player] seek to %.1f s", target);
    return 0;
}

int player_update(Player *p, const Input *in)
{
    double clock;

    if (!p)
        return 1;

    if (in->pressed || in->active)
        p->osd_until = sys_ms() + 5000;

    if (in->cancel || (in->pressed & sys_cancel_button()))
        return 1;

    if (in->pressed & sys_ok_button())
    {
        p->paused = !p->paused;
        audio_player_set_paused(p->audio, p->paused);
        p->osd_until = sys_ms() + 5000;
    }

    if (in->pressed & PAD_CTRL_TRIANGLE)
    {
        p->danmaku_on = !p->danmaku_on;
        set_hint(p, p->danmaku_on ? "弹幕：开" : "弹幕：关");
    }
    if (in->pressed & PAD_NAV_LEFT)
    {
        if (p->play.video_index[0])
        {
            player_seek(p, player_clock(p) - 10.0);
            p->osd_until = sys_ms() + 5000;
            return 2;
        }
        set_hint(p, "该视频不支持跳转");
    }
    if (in->pressed & PAD_NAV_RIGHT)
    {
        if (p->play.video_index[0])
        {
            player_seek(p, player_clock(p) + 10.0);
            p->osd_until = sys_ms() + 5000;
            return 2;
        }
        set_hint(p, "该视频不支持跳转");
    }
    if (in->pressed & PAD_CTRL_L1)
    {
        p->quality_delta = -1;
        return 3;
    }
    if (in->pressed & PAD_CTRL_R1)
    {
        p->quality_delta = 1;
        return 3;
    }

    if (p->error || p->paused)
        return 0;

    clock = player_clock(p);

    if (sys_ms() >= p->next_stat_ms)
    {
        log_printf("[player] stat wall=%.1f clock=%.1f shown=%d skip=%d",
                   (double)(sys_ms() - p->start_ms) / 1000.0,
                   clock - p->clock_base, p->shown_total, p->vdec_skipped);
        p->next_stat_ms = sys_ms() + 2000;
    }

    if (p->audio && !audio_player_ready(p->audio) &&
        !audio_player_eof(p->audio) && !p->eof)
        return 0;

    if (sysSemTryWait(p->data_sem) == 0)
    {
        VideoFrame *f = &p->slots[p->q_read];
        double rel_clock = clock - p->clock_base;
        double pts = (f->pts > 0)
                         ? ((double)f->pts / 90000.0 - p->video_base)
                         : rel_clock;

        /* More than 250 ms behind the clock: drop it instead of showing stale
         * video, otherwise a slow presenter never catches up with the audio. */
        if (!p->eof && pts < rel_clock - 0.25)
        {
            p->q_read = (p->q_read + 1) % VQ_SLOTS;
            sysSemPost(p->free_sem, 1);
            p->vdec_skipped++;
            return 0;
        }

        if (p->eof || pts <= rel_clock + 0.10)
        {
            present_frame(p);
        }
        else
        {
            sysSemPost(p->data_sem, 1);
        }
    }

    return 0;
}

void player_draw(Player *p)
{
    double clock, duration, progress;
    char time_text[64];
    int bar_x = 60;
    int bar_w = GFX_W - 120;
    int show_osd;

    if (!p)
        return;

    if (p->error)
    {
        gfx_text_center(0, GFX_W, GFX_H / 2 - 30, 34, COL_ERROR, p->error_text);
        gfx_text_center(0, GFX_W, GFX_H / 2 + 20, 24, COL_TEXT_FAINT,
                        "按 ○ 返回详情");
        return;
    }

    draw_video(p);
    danmaku_draw(p->danmaku, player_clock(p), p->danmaku_on);

    if (!p->first_frame)
    {
        gfx_text_center(0, GFX_W, GFX_H / 2 - 20, 32, COL_TEXT,
                        p->paused ? "已暂停" : "缓冲中…");
    }

    if (p->paused)
        gfx_text_center(0, GFX_W, GFX_H / 2 + 40, 30, COL_TEXT, "暂停");

    show_osd = sys_ms() < p->osd_until;
    if (!show_osd)
        return;

    clock = player_clock(p);
    duration = p->play.duration > 0 ? p->play.duration : p->item.duration;
    if (duration <= 0)
        duration = 1;
    progress = clock / duration;
    if (progress < 0)
        progress = 0;
    if (progress > 1)
        progress = 1;

    gfx_rect(0, GFX_H - 210, GFX_W, 210, GFX_RGBA(0, 0, 0, 170));
    gfx_text_ellipsis(bar_x, GFX_H - 190, GFX_W - 420, 30, COL_TEXT,
                      p->item.title[0] ? p->item.title : p->play.video_url);

    snprintf(time_text, sizeof(time_text), "%d:%02d / %d:%02d", (int)clock / 60,
             (int)clock % 60, (int)duration / 60, (int)duration % 60);
    gfx_text(GFX_W - 260, GFX_H - 188, 24, COL_TEXT_DIM, time_text);

    gfx_rect(bar_x, GFX_H - 120, bar_w, 6, GFX_RGBA(255, 255, 255, 50));
    gfx_rect(bar_x, GFX_H - 120, (int)(bar_w * progress), 6, COL_PINK);

    gfx_text(bar_x, GFX_H - 86, 22, COL_TEXT_DIM,
             p->danmaku_on ? "× 暂停    ○ 返回    △ 弹幕开    L1/R1 画质"
                           : "× 暂停    ○ 返回    △ 弹幕关    L1/R1 画质");
    gfx_text(GFX_W - 260, GFX_H - 86, 22, COL_TEXT_FAINT,
             p->play.qn >= 80   ? "1080P"
             : p->play.qn >= 64 ? "720P"
             : p->play.qn >= 32 ? "480P"
                                : "AVC");

    if (p->hint[0] && sys_ms() < p->hint_until)
        gfx_text_center(0, GFX_W, GFX_H - 260, 24, COL_PINK, p->hint);
}

void player_close(Player *p)
{
    if (!p)
        return;

    p->abort = 1;
    sysSemPost(p->free_sem, VQ_SLOTS);

    if (p->danmaku)
    {
        danmaku_stop(p->danmaku);
        p->danmaku = NULL;
    }

    if (p->audio)
    {
        audio_player_stop(p->audio);
        p->audio = NULL;
    }

    stream_abort(&p->vstream);
    if (p->thread_started)
        sysThreadJoin(p->vthread, NULL);
    stream_stop(&p->vstream);

    p->idx_cancel = 1;      /* abort an in-flight sidx download */
    index_prefetch_join(p);
    vdec_release(p);

    if (p->vprefix)
        free(p->vprefix);
    if (p->aprefix)
        free(p->aprefix);
    dash_free(&p->vidx);
    dash_free(&p->aidx);

    for (int i = 0; i < VQ_SLOTS; ++i)
        if (p->buffers[i])
            free(p->buffers[i]);

    sysSemDestroy(p->data_sem);
    sysSemDestroy(p->free_sem);
    free(p);
    log_printf("[player] closed");
}

const char *player_error(Player *p)
{
    return p ? p->error_text : "player is null";
}

int player_failed(Player *p)
{
    return p ? p->error : 1;
}

int player_ready(Player *p)
{
    return p ? p->first_frame : 0;
}

double player_current_time(Player *p)
{
    return p ? player_clock(p) : 0.0;
}

int player_quality_delta(Player *p)
{
    return p ? p->quality_delta : 0;
}

int player_quality_qn(Player *p)
{
    return p ? p->play.qn : 0;
}

void player_show_hint(Player *p, const char *text)
{
    if (p)
        set_hint(p, text);
}
