#ifndef PILIS3_DANMAKU_H
#define PILIS3_DANMAKU_H

#include <ppu-types.h>

typedef struct DanmakuCtx DanmakuCtx;

DanmakuCtx *danmaku_load_start(long long cid);
int danmaku_ready(DanmakuCtx *ctx);
int danmaku_count(DanmakuCtx *ctx);
void danmaku_stop(DanmakuCtx *ctx);

void danmaku_reset(void);
void danmaku_draw(DanmakuCtx *ctx, double clock, int enabled);

#endif
