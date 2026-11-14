#ifndef PILIS3_VDEC_H
#define PILIS3_VDEC_H

#include <ppu-types.h>
#include <codec/vdec.h>

typedef struct
{
    u8 *data;
    u32 size;
    int w;
    int h;
    int layout_h;
    int stride;
    long long pts;
    long long order;
} VideoFrame;

typedef struct
{
    vdecType type;
    vdecAttr attr;
    u32 handle;
    void *mem;
    u32 mem_size;
    u32 alloc_size;
    int open;
    int ended;
    int level;
    int spus;
    int pic_count;
    int feed_count;
    int disp_w;
    int disp_h;
    long long order_base;
    int poc_ext;

#define VDEC_AU_RING 64
    long long au_pts[VDEC_AU_RING];
    u32 au_seq;

    volatile u32 audone;
    volatile u32 seqdone;
    volatile u32 picout;
    volatile u32 errors;

    u8 *au_buf;
    u32 au_cap;
} VdecCtx;

int vdec_open(VdecCtx *c, int width, int height);
/* 0 = closed, -1 = close skipped because SEQDONE never arrived. */
int vdec_close(VdecCtx *c);

/* Copy one Annex-B AU into VDEC's buffer and wait for AUDONE. */
int vdec_feed(VdecCtx *c, const u8 *au, u32 len, long long pts, long long dts);

/* 1 = picture copied into out/meta, 0 = none ready, -1 = error. */
int vdec_get_picture(VdecCtx *c, u8 *out, u32 cap, VideoFrame *meta);

/* End the sequence and wait for SEQDONE; vdec_close() calls this too. */
int vdec_end_sequence(VdecCtx *c);

#endif
