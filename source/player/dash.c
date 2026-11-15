#include "dash.h"
#include "net/http.h"
#include "platform/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u32 rd32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static u64 rd64(const u8 *p)
{
    return ((u64)rd32(p) << 32) | rd32(p + 4);
}

static int parse_range(const char *s, u64 *start, u64 *end)
{
    long long a = 0, b = 0;

    if (!s || !*s)
        return -1;
    if (sscanf(s, "%lld-%lld", &a, &b) == 2 && a >= 0 && b >= a)
    {
        *start = (u64)a;
        *end = (u64)b;
        return 0;
    }
    if (sscanf(s, "%lld-", &a) == 1 && a >= 0)
    {
        *start = (u64)a;
        *end = (u64)a + 65535;
        return 0;
    }
    return -1;
}

int dash_load(const char *url, const char *init_range, const char *index_range,
              DashIndex *idx, char *err, int err_size)
{
    return dash_load_ex(url, init_range, index_range, idx, err, err_size, NULL);
}

int dash_load_ex(const char *url, const char *init_range, const char *index_range,
                 DashIndex *idx, char *err, int err_size,
                 const volatile int *cancel)
{
    u64 init_start, init_end, index_start, index_end;
    HttpResponse res;
    u8 *index = NULL;
    u32 index_len = 0;
    u64 pos = 0;

    memset(idx, 0, sizeof(*idx));

    if (parse_range(init_range, &init_start, &init_end) < 0 ||
        parse_range(index_range, &index_start, &index_end) < 0)
    {
        snprintf(err, err_size, "缺少 DASH 索引范围");
        return -1;
    }

    if (http_get_range_ex(url, (long long)init_start, (long long)init_end, &res,
                          15, cancel) < 0 ||
        !res.body || res.len == 0)
    {
        snprintf(err, err_size, "初始化段下载失败: %s", res.error);
        http_response_free(&res);
        return -1;
    }

    idx->init = (u8 *)malloc(res.len);
    if (!idx->init)
    {
        http_response_free(&res);
        snprintf(err, err_size, "内存不足");
        return -1;
    }
    memcpy(idx->init, res.body, res.len);
    idx->init_len = (u32)res.len;
    http_response_free(&res);

    if (cancel && *cancel)
    {
        snprintf(err, err_size, "已取消");
        dash_free(idx);
        return -1;
    }

    if (http_get_range_ex(url, (long long)index_start, (long long)index_end, &res,
                          15, cancel) < 0 ||
        !res.body || res.len == 0)
    {
        snprintf(err, err_size, "索引段下载失败: %s", res.error);
        http_response_free(&res);
        dash_free(idx);
        return -1;
    }

    index = (u8 *)malloc(res.len);
    if (!index)
    {
        http_response_free(&res);
        snprintf(err, err_size, "内存不足");
        dash_free(idx);
        return -1;
    }
    memcpy(index, res.body, res.len);
    index_len = (u32)res.len;
    http_response_free(&res);

    while (pos + 8 <= index_len)
    {
        u64 box_size = rd32(index + pos);
        const u8 *type = index + pos + 4;
        u64 header = 8;

        if (box_size == 1 && pos + 16 <= index_len)
        {
            box_size = rd64(index + pos + 8);
            header = 16;
        }
        if (box_size < header || pos + box_size > index_len)
            break;

        if (!memcmp(type, "sidx", 4))
        {
            const u8 *p = index + pos + header;
            u8 version = *p++;
            u32 timescale;
            u64 ept, first_offset;
            u16 count;
            u64 time = 0;
            u64 byte_off;

            p += 3; /* flags */
            p += 4; /* reference_ID */
            timescale = rd32(p);
            p += 4;
            if (version == 0)
            {
                ept = rd32(p);
                p += 4;
                first_offset = rd32(p);
                p += 4;
            }
            else
            {
                ept = rd64(p);
                p += 8;
                first_offset = rd64(p);
                p += 8;
            }
            p += 2;
            count = (u16)((p[0] << 8) | p[1]);
            p += 2;

            if (!timescale || count == 0 || count > 10000 ||
                (u64)(p - index) + (u64)count * 12 > index_len)
            {
                break;
            }

            idx->sidx_end = index_start + pos + box_size;
            idx->first_offset = first_offset;
            idx->timescale = timescale;
            idx->earliest = ept;
            idx->count = count;
            idx->sizes = (u32 *)calloc(count, sizeof(u32));
            idx->offsets = (u64 *)calloc(count, sizeof(u64));
            idx->times = (u64 *)calloc(count, sizeof(u64));
            if (!idx->sizes || !idx->offsets || !idx->times)
            {
                snprintf(err, err_size, "内存不足");
                free(index);
                dash_free(idx);
                return -1;
            }

            byte_off = idx->sidx_end + first_offset;
            for (int i = 0; i < count; ++i)
            {
                u32 w = rd32(p);
                u32 dur = rd32(p + 4);
                u32 size = w & 0x7fffffff;

                p += 12;
                idx->times[i] = ept + time;
                idx->offsets[i] = byte_off;
                idx->sizes[i] = size;
                time += dur;
                byte_off += size;
            }

            free(index);
            log_printf("[dash] sidx: %d segs, timescale=%u, first=%llu", count,
                       timescale, (unsigned long long)idx->offsets[0]);
            return 0;
        }

        pos += box_size;
    }

    free(index);
    snprintf(err, err_size, "没有找到 sidx");
    dash_free(idx);
    return -1;
}

int dash_find(const DashIndex *idx, double target, u64 *offset, double *seg_time)
{
    int lo = 0, hi, found = 0;
    u64 want;

    if (!idx || idx->count <= 0 || !idx->timescale)
        return -1;

    want = (u64)(target * (double)idx->timescale);
    hi = idx->count - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        if (idx->times[mid] <= want)
        {
            found = mid;
            lo = mid + 1;
        }
        else
        {
            hi = mid - 1;
        }
    }

    *offset = idx->offsets[found];
    *seg_time = (double)idx->times[found] / (double)idx->timescale;
    return 0;
}

void dash_free(DashIndex *idx)
{
    if (!idx)
        return;
    if (idx->init)
        free(idx->init);
    if (idx->sizes)
        free(idx->sizes);
    if (idx->offsets)
        free(idx->offsets);
    if (idx->times)
        free(idx->times);
    memset(idx, 0, sizeof(*idx));
}
