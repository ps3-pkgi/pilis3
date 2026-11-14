#ifndef PILIS3_COVER_H
#define PILIS3_COVER_H

#include <ppu-types.h>
#include <ya2d/ya2d.h>

void cover_init(void);
void cover_shutdown(void);
void cover_frame(void);

void cover_request(const char *key, const char *url);
ya2d_Texture *cover_get(const char *key);
void cover_draw(const char *key, int x, int y, int w, int h, u32 placeholder);

#endif
