#ifndef PILIS3_TEXT_H
#define PILIS3_TEXT_H

#include <ppu-types.h>
#include <tiny3d.h>

/* Low-level glyph cache / renderer, adapted from pkgi-ps3's ttf_render.c
 * (GPLv3). The atlas lives in ya2d's static 8 MB region. */
int TTFLoadFont(int set, char *path, void *from_memory, int size_from_memory);
void TTFUnloadFont(void);
u16 *init_ttf_table(u16 *texture);
void reset_ttf_frame(void);

#define WIN_AUTO_LF 1
#define WIN_SKIP_LF 2
#define WIN_DOUBLE_LF 4

void set_ttf_window(int x, int y, int width, int height, u32 mode);
extern float Y_ttf;
extern float Z_ttf;

int display_ttf_string(int posx, int posy, const char *string, u32 color,
                       u32 bkcolor, int sw, int sh);

/* App-level helpers. size is the pixel height/width used for the glyph quad. */
void text_init(void);
void text_draw(int x, int y, int size, u32 color, const char *text);
int text_width(int size, const char *text);
void text_draw_wrapped(int x, int y, int w, int h, int size, u32 color,
                       const char *text);
void text_draw_ellipsis(int x, int y, int max_w, int size, u32 color,
                        const char *text);

#endif
