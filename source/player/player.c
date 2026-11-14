#include "player.h"

#include "audio.h"
#include "danmaku.h"
#include "dash.h"
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
    double seek_target;
    int quality_delta;
    u8 *vprefix;
    u32 vprefix_len;
    long long vrange;
    u8 *aprefix;
    u32 aprefix_len;
    long long arange;
    char error_text[160];

    int vdec_emit_count;
    long long vdec_last_order;
    long long vdec_next_pts;
    long long vdec_frame_dur;
    long long vdec_max_order;
    long long vdec_flush_to;
    int vdec_have_order;
    int audio_waiting;
    int present_count;

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

#define VDEC_REORDER 8

typedef struct
{
    u8 *buf;
    VideoFrame meta;
    int used;
} VdecReorder;

static int vdec_wait_free(Player *p)
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
 * no greater than limit (Movian's flush_to lookahead). */
static void vdec_emit_smallest(Player *p, VdecReorder *r, int *count,
                               long long limit)
{
    int i, best = -1;
    long long best_order = 0;

    if (*count <= 0)
        return;

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
        return;
    if (limit != LLONG_MAX && best_order > limit)
        return;
    if (vdec_wait_free(p) < 0)
        return;

    /* Use the AU PTS from userdata when VDEC provided one; fall back to a
     * monotonic frame clock only if it did not. */
    if (r[best].meta.pts <= 0)
        r[best].meta.pts = p->vdec_next_pts;
    p->vdec_next_pts = r[best].meta.pts +
                       (p->vdec_frame_dur > 0 ? p->vdec_frame_dur : 3000);

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

    if (p->audio_waiting && p->audio)
    {
        p->audio_waiting = 0;
        if (!p->paused)
            audio_player_set_paused(p->audio, 0);
        log_printf("[player] audio starts with video");
    }

    r[best].used = 0;
    (*count)--;
}

static void vdec_pump(Player *p, VdecCtx *vc, VdecReorder *r, int *count)
{
    for (;;)
    {
        int i, free_idx = -1;
        VideoFrame meta;
        int got;

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
                    vdec_emit_smallest(p, r, count, p->vdec_flush_to);
            }
            if (*count >= VDEC_REORDER)
                break;
            continue;
        }

        got = vdec_get_picture(vc, r[free_idx].buf, VFRAME_MAX, &meta);
        if (got <= 0)
            break;

        if (!p->vdec_have_order)
        {
            p->vdec_max_order = meta.order;
            p->vdec_have_order = 1;
        }
        else if (meta.order > p->vdec_max_order)
        {
            p->vdec_flush_to = p->vdec_max_order;
            p->vdec_max_order = meta.order;
        }

        r[free_idx].meta = meta;
        r[free_idx].used = 1;
        (*count)++;

        if (p->vdec_flush_to >= 0)
        {
            while (*count > 0 && vdec_smallest_order(r) <= p->vdec_flush_to)
                vdec_emit_smallest(p, r, count, p->vdec_flush_to);
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
    int i;
    int ret = -1;
    long long last_pts = 0;

    vc = (VdecCtx *)calloc(1, sizeof(*vc));
    if (!vc)
        return -1;

    p->vdec_flush_to = -1;
    p->vdec_have_order = 0;
    p->vdec_max_order = 0;
    memset(reorder, 0, sizeof(reorder));

    if (vdec_open(vc, st->codecpar->width, st->codecpar->height) < 0)
        goto done;

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

    for (i = 0; i < VDEC_REORDER; ++i)
    {
        reorder[i].buf = (u8 *)memalign(128, VFRAME_MAX);
        if (!reorder[i].buf)
            goto done;
    }

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
            vdec_emit_smallest(p, reorder, &count, LLONG_MAX);
    }

done:
    if (vc)
    {
        if (vdec_close(vc) == 0)
            free(vc);
        else
        {
            g_vdec_disabled = 1;
            log_printf("[player] vdec disabled for this run (session stuck)");
        }
    }

    for (i = 0; i < VDEC_REORDER; ++i)
    {
        if (reorder[i].buf)
            free(reorder[i].buf);
    }
    av_packet_free(&pkt);
    av_packet_free(&out);
    av_bsf_free(&bsfc);

    log_printf("[player] vdec done, frames left=%d failed=%d emitted=%d", count,
               failed, p->vdec_emit_count);
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
    if (start_time > 0.5 && p->play.video_index[0])
    {
        DashIndex vi, ai;
        char err[128];
        u64 voff = 0, aoff = 0;
        double vtime = 0.0, atime = 0.0;
        int vok;

        vok = dash_load(p->play.video_url, p->play.video_init, p->play.video_index,
                        &vi, err, sizeof(err)) == 0 &&
              dash_find(&vi, start_time, &voff, &vtime) == 0;
        if (vok)
        {
            p->vprefix = vi.init;
            p->vprefix_len = vi.init_len;
            p->vrange = (long long)voff;
            vi.init = NULL;
            p->video_base = vtime;
            p->clock_base = vtime;
            dash_free(&vi);

            if (p->play.audio_url[0] && p->play.audio_index[0])
            {
                if (dash_load(p->play.audio_url, p->play.audio_init,
                              p->play.audio_index, &ai, err, sizeof(err)) == 0 &&
                    dash_find(&ai, start_time, &aoff, &atime) == 0)
                {
                    p->aprefix = ai.init;
                    p->aprefix_len = ai.init_len;
                    p->arange = (long long)aoff;
                    ai.init = NULL;
                    p->clock_base = atime;
                    dash_free(&ai);
                }
                else
                {
                    dash_free(&ai);
                    log_printf("[player] audio seek index failed: %s", err);
                    p->play.audio_url[0] = 0;
                }
            }
            else
            {
                p->play.audio_url[0] = 0;
            }

            log_printf("[player] seek %.1fs -> video %.1fs off=%lld audio %.1fs",
                       start_time, vtime, p->vrange, p->clock_base);
        }
        else
        {
            dash_free(&vi);
            log_printf("[player] video seek index failed: %s; start from 0", err);
            start_time = 0.0;
        }
    }

    p->start_time = start_time;
    p->start_ms = sys_ms();
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

    log_printf("[player] opened %s (%dx%d) audio=%d start=%.1f",
               p->play.video_url, p->play.width, p->play.height,
               p->audio != NULL, p->start_time);
    return p;
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
            p->seek_target = player_clock(p) - 10.0;
            if (p->seek_target < 0.0)
                p->seek_target = 0.0;
            p->osd_until = sys_ms() + 5000;
            return 2;
        }
        set_hint(p, "该视频不支持跳转");
    }
    if (in->pressed & PAD_NAV_RIGHT)
    {
        if (p->play.video_index[0])
        {
            double dur = p->play.duration > 0 ? p->play.duration : p->item.duration;
            p->seek_target = player_clock(p) + 10.0;
            if (dur > 2.0 && p->seek_target > dur - 2.0)
                p->seek_target = dur - 2.0;
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

    if (p->vprefix)
        free(p->vprefix);
    if (p->aprefix)
        free(p->aprefix);

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

double player_seek_target(Player *p)
{
    return p ? p->seek_target : 0.0;
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
