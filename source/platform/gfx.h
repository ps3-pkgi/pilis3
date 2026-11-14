#ifndef PILIS3_GFX_H
#define PILIS3_GFX_H

#include <ppu-types.h>
#include <ya2d/ya2d.h>

#define GFX_W 1920
#define GFX_H 1080

#define GFX_RGBA(r, g, b, a) \
    (((u32)(r) << 24) | ((u32)(g) << 16) | ((u32)(b) << 8) | (u32)(a))

int gfx_init(void);
void gfx_shutdown(void);
void gfx_begin(void);
void gfx_begin_overlay(void);
void gfx_end(void);
void gfx_clear(u32 color);

void gfx_rect(int x, int y, int w, int h, u32 color);
void gfx_rect_outline(int x, int y, int w, int h, u32 color);
void gfx_line_h(int x, int y, int w, u32 color);

void gfx_text(int x, int y, int size, u32 color, const char *text);
int gfx_text_width(int size, const char *text);
void gfx_text_center(int x, int w, int y, int size, u32 color, const char *text);
void gfx_text_ellipsis(int x, int y, int max_w, int size, u32 color,
                       const char *text);
void gfx_text_wrapped(int x, int y, int w, int h, int size, u32 color,
                      const char *text);

void gfx_texture(ya2d_Texture *tex, int x, int y, int w, int h);

#endif
