#ifndef PILIS3_MD5_H
#define PILIS3_MD5_H

#include <ppu-types.h>

void md5_buffer(const void *data, u32 len, u8 out[16]);
void md5_hex(const void *data, u32 len, char out[33]);

#endif
