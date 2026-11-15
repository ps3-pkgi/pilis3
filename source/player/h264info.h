#ifndef PILIS3_H264INFO_H
#define PILIS3_H264INFO_H

#include <ppu-types.h>

typedef struct
{
    int profile_idc;
    int level_idc;
    int width;
    int height;
    int mb_width;
    int mb_height;
    int num_ref_frames;
} H264StreamInfo;

/* Parse the SPS out of avcC or Annex-B extradata. Returns 0 on success. */
int h264_parse_extradata(const u8 *extradata, int size, H264StreamInfo *out);

#endif
