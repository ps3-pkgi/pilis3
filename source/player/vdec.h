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

/* Decoded pictures live in this ring so the VDEC callback thread can always
 * hand a picture back to the decoder without waiting for the presenter.
 * Must be larger than the reorder window (VdecReorder) plus the copy in
 * flight, or pictures get dropped while the player is merely busy. */
#define VDEC_PIC_RING 16

enum
{
    VDEC_PIC_FREE = 0,
    VDEC_PIC_FILLING,
    VDEC_PIC_READY,
    VDEC_PIC_IN_USE
};

typedef struct
{
    u8 *buf;
    VideoFrame f;
    volatile u32 state;
} VdecPicSlot;

typedef struct
{
    u32 slot;
    VideoFrame f;
} VdecPic;

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
    int drop_count;
    int disp_w;
    int disp_h;
    long long order_base;
    int poc_ext;

#define VDEC_AU_RING 64
    long long au_pts[VDEC_AU_RING];
    u32 au_seq;
    u32 pic_seq;

    volatile u32 audone;
    volatile u32 seqdone;
    volatile u32 picout;
    volatile u32 errors;

    u8 *au_buf;
    u32 au_cap;

    VdecPicSlot pic[VDEC_PIC_RING];
    u32 pic_cap;
    u32 pic_dropped;
} VdecCtx;

int vdec_open(VdecCtx *c, int width, int height, u32 frame_cap);
/* 0 = closed, -1 = close skipped because SEQDONE never arrived. */
int vdec_close(VdecCtx *c);

/* Copy one Annex-B AU into VDEC's buffer and wait for AUDONE. */
int vdec_feed(VdecCtx *c, const u8 *au, u32 len, long long pts, long long dts);

/* Take one picture filled in by the VDEC callback thread. Returns 1 with
 * out set, or 0 when nothing is ready. The slot stays borrowed until
 * vdec_pic_release() is called. */
int vdec_pic_take(VdecCtx *c, VdecPic *out);
void vdec_pic_release(VdecCtx *c, u32 slot);

/* End the sequence and wait for SEQDONE; vdec_close() calls this too. */
int vdec_end_sequence(VdecCtx *c);

/* End the current sequence and start a new one on the same handle, resetting
 * the POC/order and picture ring. Used for seek instead of close/open. */
int vdec_flush_sequence(VdecCtx *c);

#endif
