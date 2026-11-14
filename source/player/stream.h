#ifndef PILIS3_STREAM_H
#define PILIS3_STREAM_H

#include <ppu-types.h>

typedef struct
{
    u8 *buf;
    u32 cap;
    u32 head;
    u32 tail;
    u32 used;
    volatile int eof;
    volatile int error;
    volatile int abort;
    volatile int curl_code;   /* CURLcode of the last transfer, 0 = never ran */
    volatile long http_status; /* CURLINFO_RESPONSE_CODE, 0 = no response */
    int started;
    char url[1024];
    long long range_start;
    void *lock;
    int sem;
    u64 thread;
} NetStream;

int stream_start(NetStream *s, const char *url, u32 capacity, long long range_start);
int stream_start_prefixed(NetStream *s, const char *url, u32 capacity,
                          const u8 *prefix, u32 prefix_len,
                          long long range_start);
/* Blocks until data is available or the stream ends. Returns bytes copied,
 * 0 on EOF, -1 on abort/error. */
int stream_read(NetStream *s, u8 *dst, u32 len);
void stream_abort(NetStream *s);
void stream_stop(NetStream *s);

#endif
