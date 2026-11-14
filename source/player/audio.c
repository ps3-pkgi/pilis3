#include "audio.h"
#include "stream.h"
#include "platform/log.h"
#include "platform/sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>

#include <audio/audio.h>
#include <sys/event_queue.h>
#include <sys/mutex.h>
#include <sys/thread.h>
#include <sys/systime.h>

#define PCM_RATE 48000
#define PCM_CAP_FRAMES (PCM_RATE * 4)
#define AUDIO_STREAM_BUF (2 * 1024 * 1024)

struct AudioPlayer
{
    NetStream stream;
    sys_ppu_thread_t decode_thread;
    sys_ppu_thread_t out_thread;
    sys_mutex_t lock;
    volatile int abort;
    volatile int ready;
    volatile int eof;
    volatile int paused;
    double clock;
    u32 port;
    audioPortConfig config;
    sys_event_queue_t queue;
    sys_ipc_key_t queue_key;
    float *pcm;
    u32 pcm_read;
    u32 pcm_write;
    u32 pcm_count;
    int src_rate;
    u8 *prefix;
    u32 prefix_len;
    long long range_start;
    double clock_base;
    double res_pos;
    float res_prev_l;
    float res_prev_r;
    int res_have;
    char url[1024];
};

static void audio_lock(AudioPlayer *a)
{
    sysMutexLock(a->lock, 0);
}

static void audio_unlock(AudioPlayer *a)
{
    sysMutexUnlock(a->lock);
}

static u32 pcm_available(AudioPlayer *a)
{
    u32 n;
    audio_lock(a);
    n = a->pcm_count;
    audio_unlock(a);
    return n;
}

static void pcm_push(AudioPlayer *a, const float *samples, u32 frames)
{
    u32 left = frames;
    const float *src = samples;

    while (left > 0)
    {
        u32 space, chunk;

        if (a->abort)
            return;

        audio_lock(a);
        space = PCM_CAP_FRAMES - a->pcm_count;
        chunk = left < space ? left : space;
        if (chunk > 0)
        {
            for (u32 i = 0; i < chunk; ++i)
            {
                u32 pos = (a->pcm_write + i) % PCM_CAP_FRAMES;
                a->pcm[pos * 2 + 0] = src[i * 2 + 0];
                a->pcm[pos * 2 + 1] = src[i * 2 + 1];
            }
            a->pcm_write = (a->pcm_write + chunk) % PCM_CAP_FRAMES;
            a->pcm_count += chunk;
        }
        audio_unlock(a);

        if (chunk == 0)
        {
            sysUsleep(5000);
            continue;
        }

        src += chunk * 2;
        left -= chunk;
    }
}

static u32 pcm_pop(AudioPlayer *a, float *dst, u32 frames)
{
    u32 got;

    audio_lock(a);
    got = a->pcm_count < frames ? a->pcm_count : frames;
    for (u32 i = 0; i < got; ++i)
    {
        u32 pos = (a->pcm_read + i) % PCM_CAP_FRAMES;
        dst[i * 2 + 0] = a->pcm[pos * 2 + 0];
        dst[i * 2 + 1] = a->pcm[pos * 2 + 1];
    }
    a->pcm_read = (a->pcm_read + got) % PCM_CAP_FRAMES;
    a->pcm_count -= got;
    audio_unlock(a);

    if (got < frames)
    {
        memset(dst + got * 2, 0, (frames - got) * 2 * sizeof(float));
    }
    return got;
}

static float read_sample(const AVFrame *frame, int ch, int i, int channels)
{
    switch (frame->format)
    {
    case AV_SAMPLE_FMT_FLTP:
        return ((const float *)frame->data[ch])[i];
    case AV_SAMPLE_FMT_FLT:
        return ((const float *)frame->data[0])[i * channels + ch];
    case AV_SAMPLE_FMT_S16P:
        return ((const int16_t *)frame->data[ch])[i] / 32768.0f;
    case AV_SAMPLE_FMT_S16:
        return ((const int16_t *)frame->data[0])[i * channels + ch] / 32768.0f;
    default:
        return 0.0f;
    }
}

static void resample_frame(AudioPlayer *a, const AVFrame *frame)
{
    int channels = frame->ch_layout.nb_channels;
    int rate = frame->sample_rate ? frame->sample_rate : a->src_rate;
    int nb = frame->nb_samples;
    float *tmp;
    float *out;
    int out_samples = 0;
    double step;
    double pos = a->res_pos;
    float prev_l = a->res_prev_l;
    float prev_r = a->res_prev_r;
    int have_prev = a->res_have;

    if (channels <= 0 || nb <= 0 || rate <= 0)
        return;

    tmp = (float *)malloc((size_t)nb * 2 * sizeof(float));
    out = (float *)malloc((size_t)nb * 6 * sizeof(float));
    if (!tmp || !out)
    {
        free(tmp);
        free(out);
        return;
    }

    for (int i = 0; i < nb; ++i)
    {
        float l = read_sample(frame, 0, i, channels);
        float r = channels > 1 ? read_sample(frame, 1, i, channels) : l;
        tmp[i * 2 + 0] = l;
        tmp[i * 2 + 1] = r;
    }

    if (rate == PCM_RATE)
    {
        pcm_push(a, tmp, (u32)nb);
        free(tmp);
        free(out);
        return;
    }

    step = (double)rate / (double)PCM_RATE;
    if (step <= 0.0)
        step = 1.0;

    if (!have_prev)
    {
        prev_l = tmp[0];
        prev_r = tmp[1];
        pos = 0.0;
    }

    int i = 0;
    while (i < nb)
    {
        float cur_l = tmp[i * 2 + 0];
        float cur_r = tmp[i * 2 + 1];

        while (pos < 1.0)
        {
            out[out_samples * 2 + 0] =
                prev_l + (cur_l - prev_l) * (float)pos;
            out[out_samples * 2 + 1] =
                prev_r + (cur_r - prev_r) * (float)pos;
            out_samples++;
            pos += step;
        }
        pos -= 1.0;
        prev_l = cur_l;
        prev_r = cur_r;
        have_prev = 1;
        i++;
    }

    if (out_samples > 0)
        pcm_push(a, out, (u32)out_samples);

    a->res_pos = pos;
    a->res_prev_l = prev_l;
    a->res_prev_r = prev_r;
    a->res_have = have_prev;

    free(tmp);
    free(out);
}

static int audio_avio_read(void *opaque, uint8_t *buf, int size)
{
    int n = stream_read((NetStream *)opaque, buf, (u32)size);
    if (n <= 0)
        return AVERROR_EOF;
    return n;
}

static void decode_thread(void *arg)
{
    AudioPlayer *a = (AudioPlayer *)arg;
    AVFormatContext *fmt = NULL;
    AVIOContext *avio = NULL;
    AVCodecContext *dec = NULL;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    int astream = -1;

    if (stream_start_prefixed(&a->stream, a->url, AUDIO_STREAM_BUF, a->prefix,
                              a->prefix_len, a->range_start) < 0)
    {
        log_printf("[audio] stream start failed");
        a->eof = 1;
        sysThreadExit(0);
    }

    fmt = avformat_alloc_context();
    avio = avio_alloc_context((unsigned char *)av_malloc(32768), 32768, 0,
                              &a->stream, audio_avio_read, NULL, NULL);
    if (!fmt || !avio)
    {
        log_printf("[audio] avio alloc failed");
        a->eof = 1;
        sysThreadExit(0);
    }

    fmt->pb = avio;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

    if (avformat_open_input(&fmt, NULL, NULL, NULL) < 0)
    {
        log_printf("[audio] open_input failed");
        a->eof = 1;
        sysThreadExit(0);
    }

    if (avformat_find_stream_info(fmt, NULL) < 0)
    {
        log_printf("[audio] find_stream_info failed");
        a->eof = 1;
        sysThreadExit(0);
    }

    astream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (astream < 0)
    {
        log_printf("[audio] no audio stream");
        a->eof = 1;
        sysThreadExit(0);
    }

    {
        AVStream *st = fmt->streams[astream];
        const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!codec)
        {
            log_printf("[audio] no decoder for codec %d", st->codecpar->codec_id);
            a->eof = 1;
            sysThreadExit(0);
        }

        dec = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(dec, st->codecpar);
        if (avcodec_open2(dec, codec, NULL) < 0)
        {
            log_printf("[audio] avcodec_open2 failed");
            a->eof = 1;
            sysThreadExit(0);
        }

        a->src_rate = dec->sample_rate;
        log_printf("[audio] stream: %d Hz, %d ch, codec=%s", dec->sample_rate,
                   dec->ch_layout.nb_channels, codec->name);
    }

    pkt = av_packet_alloc();
    frame = av_frame_alloc();

    while (!a->abort && av_read_frame(fmt, pkt) >= 0)
    {
        if (pkt->stream_index == astream)
        {
            if (avcodec_send_packet(dec, pkt) >= 0)
            {
                while (avcodec_receive_frame(dec, frame) == 0)
                {
                    resample_frame(a, frame);
                    av_frame_unref(frame);
                }
            }
        }
        av_packet_unref(pkt);
    }

    if (!a->abort)
    {
        avcodec_send_packet(dec, NULL);
        while (avcodec_receive_frame(dec, frame) == 0)
        {
            resample_frame(a, frame);
            av_frame_unref(frame);
        }
    }

    a->eof = 1;
    log_printf("[audio] decode done, pcm=%u", pcm_available(a));

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    if (avio)
    {
        av_freep(&avio->buffer);
        avio_context_free(&avio);
    }
    sysThreadExit(0);
}

static void out_thread(void *arg)
{
    AudioPlayer *a = (AudioPlayer *)arg;
    audioPortParam params;
    float *data;

    if (audioInit() != 0)
    {
        log_printf("[audio] audioInit failed");
        sysThreadExit(0);
    }

    params.numChannels = AUDIO_PORT_2CH;
    params.numBlocks = AUDIO_BLOCK_8;
    params.attrib = AUDIO_PORT_INITLEVEL;
    params.level = 1.0f;

    if (audioPortOpen(&params, &a->port) != 0)
    {
        log_printf("[audio] audioPortOpen failed");
        audioQuit();
        sysThreadExit(0);
    }

    if (audioGetPortConfig(a->port, &a->config) != 0)
    {
        log_printf("[audio] audioGetPortConfig failed");
        audioPortClose(a->port);
        audioQuit();
        sysThreadExit(0);
    }

    if (audioCreateNotifyEventQueue(&a->queue, &a->queue_key) != 0 ||
        audioSetNotifyEventQueue(a->queue_key) != 0)
    {
        log_printf("[audio] event queue setup failed");
        audioPortClose(a->port);
        audioQuit();
        sysThreadExit(0);
    }

    sysEventQueueDrain(a->queue);

    while (!a->abort && pcm_available(a) < PCM_RATE / 3 && !a->eof)
        sysUsleep(20000);

    if (a->abort)
    {
        audioRemoveNotifyEventQueue(a->queue_key);
        audioPortClose(a->port);
        sysEventQueueDestroy(a->queue, 0);
        audioQuit();
        sysThreadExit(0);
    }

    while (!a->abort && a->paused)
        sysUsleep(20000);

    if (audioPortStart(a->port) != 0)
    {
        log_printf("[audio] audioPortStart failed");
        audioRemoveNotifyEventQueue(a->queue_key);
        audioPortClose(a->port);
        sysEventQueueDestroy(a->queue, 0);
        audioQuit();
        sysThreadExit(0);
    }

    a->ready = 1;
    data = (float *)(u64)a->config.audioDataStart;
    log_printf("[audio] port %u started, blocks=%llu ch=%llu", a->port,
               (unsigned long long)a->config.numBlocks,
               (unsigned long long)a->config.channelCount);

    while (!a->abort)
    {
        sys_event_t event;
        u64 current;
        u32 block;
        float *dst;

        if (a->paused)
        {
            sysUsleep(20000);
            continue;
        }

        sysEventQueueReceive(a->queue, &event, 20000);
        current = *(u64 *)(u64)a->config.readIndex;
        block = (u32)((current + 1) % a->config.numBlocks);
        dst = data + a->config.channelCount * AUDIO_BLOCK_SAMPLES * block;

        pcm_pop(a, dst, AUDIO_BLOCK_SAMPLES);
        a->clock += (double)AUDIO_BLOCK_SAMPLES / (double)PCM_RATE;
    }

    audioPortStop(a->port);
    audioRemoveNotifyEventQueue(a->queue_key);
    audioPortClose(a->port);
    sysEventQueueDestroy(a->queue, 0);
    audioQuit();
    log_printf("[audio] output stopped");
    sysThreadExit(0);
}

AudioPlayer *audio_player_start(const char *url)
{
    return audio_player_start_at(url, NULL, 0, 0, 0.0);
}

AudioPlayer *audio_player_start_at(const char *url, const u8 *prefix,
                                   u32 prefix_len, long long range_start,
                                   double clock_base)
{
    AudioPlayer *a;
    sys_mutex_attr_t mattr;
    int rc;

    if (!url || !*url)
        return NULL;

    a = (AudioPlayer *)calloc(1, sizeof(*a));
    if (!a)
        return NULL;

    snprintf(a->url, sizeof(a->url), "%s", url);
    a->range_start = range_start;
    a->clock_base = clock_base;
    if (prefix && prefix_len)
    {
        a->prefix = (u8 *)malloc(prefix_len);
        if (!a->prefix)
        {
            free(a);
            return NULL;
        }
        memcpy(a->prefix, prefix, prefix_len);
        a->prefix_len = prefix_len;
    }
    a->pcm = (float *)malloc((size_t)PCM_CAP_FRAMES * 2 * sizeof(float));
    if (!a->pcm)
    {
        free(a);
        return NULL;
    }

    memset(&mattr, 0, sizeof(mattr));
    mattr.attr_protocol = SYS_MUTEX_PROTOCOL_FIFO;
    mattr.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
    mattr.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED;
    mattr.attr_adaptive = SYS_MUTEX_ATTR_ADAPTIVE;
    snprintf(mattr.name, sizeof(mattr.name), "aude");

    rc = sysMutexCreate(&a->lock, &mattr);
    if (rc < 0)
    {
        free(a->pcm);
        free(a);
        return NULL;
    }

    rc = sysThreadCreate(&a->decode_thread, decode_thread, a, 1100, 0x40000, 0,
                         "pilis3adec");
    if (rc < 0)
    {
        sysMutexDestroy(a->lock);
        free(a->pcm);
        free(a);
        return NULL;
    }

    rc = sysThreadCreate(&a->out_thread, out_thread, a, 1000, 0x20000, 0,
                         "pilis3aout");
    if (rc < 0)
    {
        a->abort = 1;
        sysThreadJoin(a->decode_thread, NULL);
        sysMutexDestroy(a->lock);
        free(a->pcm);
        free(a);
        return NULL;
    }

    log_printf("[audio] player started");
    return a;
}

void audio_player_stop(AudioPlayer *a)
{
    if (!a)
        return;

    a->abort = 1;
    stream_abort(&a->stream);
    sysThreadJoin(a->decode_thread, NULL);
    sysThreadJoin(a->out_thread, NULL);
    stream_stop(&a->stream);

    sysMutexDestroy(a->lock);
    free(a->pcm);
    if (a->prefix)
        free(a->prefix);
    free(a);
    log_printf("[audio] player stopped");
}

void audio_player_set_paused(AudioPlayer *a, int paused)
{
    if (!a)
        return;

    if (paused)
    {
        a->paused = 1;
        if (a->ready)
            audioPortStop(a->port);
    }
    else
    {
        a->paused = 0;
        if (a->ready)
            audioPortStart(a->port);
    }
}

double audio_player_clock(AudioPlayer *a)
{
    double c;

    if (!a)
        return 0.0;
    audio_lock(a);
    c = a->clock_base + a->clock;
    audio_unlock(a);
    return c;
}

int audio_player_ready(AudioPlayer *a)
{
    return a ? a->ready : 0;
}

int audio_player_eof(AudioPlayer *a)
{
    return a ? a->eof : 1;
}
