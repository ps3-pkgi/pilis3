#ifndef PILIS3_DASH_H
#define PILIS3_DASH_H

#include <ppu-types.h>

typedef struct
{
    u8 *init;
    u32 init_len;
    u64 sidx_end;
    u64 first_offset;
    u32 timescale;
    u64 earliest;
    int count;
    u32 *sizes;
    u64 *offsets;
    u64 *times;
} DashIndex;

int dash_load(const char *url, const char *init_range, const char *index_range,
              DashIndex *idx, char *err, int err_size);
/* Aborts both range requests once *cancel becomes non-zero. */
int dash_load_ex(const char *url, const char *init_range, const char *index_range,
                 DashIndex *idx, char *err, int err_size,
                 const volatile int *cancel);
int dash_find(const DashIndex *idx, double target, u64 *offset, double *seg_time);
void dash_free(DashIndex *idx);

#endif
