#include "h264info.h"

#include <string.h>

typedef struct
{
    const u8 *buf;
    int size;
    int pos;
} BitReader;

static int br_bit(BitReader *b)
{
    int byte, shift;

    if (b->pos >= b->size * 8)
        return 0;
    byte = b->buf[b->pos >> 3];
    shift = 7 - (b->pos & 7);
    b->pos++;
    return (byte >> shift) & 1;
}

static u32 br_bits(BitReader *b, int n)
{
    u32 v = 0;

    while (n-- > 0)
        v = (v << 1) | (u32)br_bit(b);
    return v;
}

static u32 br_ue(BitReader *b)
{
    int zeros = 0;

    while (!br_bit(b))
    {
        if (++zeros > 31)
            return 0;
    }
    return ((1u << zeros) - 1) + br_bits(b, zeros);
}

static int br_se(BitReader *b)
{
    u32 v = br_ue(b);

    if (v & 1)
        return (int)((v + 1) / 2);
    return -(int)(v / 2);
}

/* Strip emulation prevention bytes (00 00 03 -> 00 00). */
static int rbsp_copy(const u8 *src, int len, u8 *dst, int cap)
{
    int i, o = 0, zeros = 0;

    for (i = 0; i < len && o < cap; ++i)
    {
        if (zeros >= 2 && src[i] == 3)
        {
            zeros = 0;
            continue;
        }
        dst[o++] = src[i];
        zeros = (src[i] == 0) ? zeros + 1 : 0;
    }
    return o;
}

static const u8 *find_sps(const u8 *ed, int size, int *len)
{
    int i;

    if (!ed || size < 5)
        return NULL;

    if (ed[0] == 1)
    {
        const u8 *p = ed + 6;
        const u8 *end = ed + size;
        int n;

        if (size < 7)
            return NULL;
        n = ed[5] & 0x1f;
        while (n-- > 0 && p + 2 <= end)
        {
            int l = (p[0] << 8) | p[1];

            p += 2;
            if (l <= 0 || p + l > end)
                break;
            if ((p[0] & 0x1f) == 7)
            {
                *len = l;
                return p;
            }
            p += l;
        }
        return NULL;
    }

    for (i = 0; i + 4 < size; ++i)
    {
        if (ed[i] == 0 && ed[i + 1] == 0 && ed[i + 2] == 1)
        {
            const u8 *nal = ed + i + 3;
            int j = i + 3;

            if ((nal[0] & 0x1f) != 7)
            {
                i = j;
                continue;
            }
            while (j + 3 < size &&
                   !(ed[j] == 0 && ed[j + 1] == 0 &&
                     (ed[j + 2] == 1 ||
                      (ed[j + 2] == 0 && ed[j + 3] == 1))))
                j++;
            *len = (int)(ed + j - nal);
            return nal;
        }
    }
    return NULL;
}

static int parse_sps(const u8 *nal, int len, H264StreamInfo *out)
{
    u8 rbsp[512];
    BitReader br;
    int rlen, profile, chroma = 1, frame_mbs_only;
    int crop_l = 0, crop_r = 0, crop_t = 0, crop_b = 0;
    int sub_w, sub_h, map_units, poc_type;

    if (len < 4 || (nal[0] & 0x1f) != 7)
        return -1;

    rlen = rbsp_copy(nal + 1, len - 1, rbsp, (int)sizeof(rbsp));
    if (rlen < 4)
        return -1;

    br.buf = rbsp;
    br.size = rlen;
    br.pos = 0;

    profile = (int)br_bits(&br, 8);
    br_bits(&br, 8);
    out->profile_idc = profile;
    out->level_idc = (int)br_bits(&br, 8);
    br_ue(&br);

    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 ||
        profile == 44 || profile == 83 || profile == 86 || profile == 118 ||
        profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
        profile == 135)
    {
        int i, lists;

        chroma = (int)br_ue(&br);
        if (chroma == 3)
            br_bits(&br, 1);
        br_ue(&br);
        br_ue(&br);
        br_bits(&br, 1);

        if (br_bits(&br, 1))
        {
            lists = (chroma != 3) ? 8 : 12;
            for (i = 0; i < lists; ++i)
            {
                if (br_bits(&br, 1))
                {
                    int j, count = (i < 6) ? 16 : 64;
                    int last = 8, next = 8;

                    for (j = 0; j < count; ++j)
                    {
                        if (next != 0)
                        {
                            int delta = br_se(&br);

                            next = (last + delta + 256) % 256;
                        }
                        if (next != 0)
                            last = next;
                    }
                }
            }
        }
    }

    br_ue(&br);                 /* log2_max_frame_num_minus4 */
    poc_type = (int)br_ue(&br);
    if (poc_type == 0)
    {
        br_ue(&br);             /* log2_max_pic_order_cnt_lsb_minus4 */
    }
    else if (poc_type == 1)
    {
        int i, n;

        br_bits(&br, 1);
        br_se(&br);
        br_se(&br);
        n = (int)br_ue(&br);
        if (n > 255)
            return -1;
        for (i = 0; i < n; ++i)
            br_se(&br);
    }

    out->num_ref_frames = (int)br_ue(&br);
    br_bits(&br, 1);            /* gaps_in_frame_num_value_allowed */
    out->mb_width = (int)br_ue(&br) + 1;
    map_units = (int)br_ue(&br) + 1;
    frame_mbs_only = br_bit(&br);
    if (!frame_mbs_only)
        br_bits(&br, 1);
    br_bits(&br, 1);            /* direct_8x8_inference */

    out->mb_height = map_units * (2 - frame_mbs_only);

    if (br_bit(&br))
    {
        crop_l = (int)br_ue(&br);
        crop_r = (int)br_ue(&br);
        crop_t = (int)br_ue(&br);
        crop_b = (int)br_ue(&br);
    }

    sub_w = (chroma == 3) ? 1 : 2;
    sub_h = (chroma == 1) ? 2 : 1;

    out->width = out->mb_width * 16 - (crop_l + crop_r) * sub_w;
    out->height = out->mb_height * 16 -
                  (crop_t + crop_b) * sub_h * (2 - frame_mbs_only);
    if (out->width <= 0 || out->height <= 0)
        return -1;
    return 0;
}

int h264_parse_extradata(const u8 *extradata, int size, H264StreamInfo *out)
{
    const u8 *sps;
    int len = 0;

    if (!out)
        return -1;
    memset(out, 0, sizeof(*out));

    sps = find_sps(extradata, size, &len);
    if (!sps)
        return -1;
    return parse_sps(sps, len, out);
}
