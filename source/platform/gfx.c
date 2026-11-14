#include "gfx.h"
#include "log.h"
#include "text.h"

#include <tiny3d.h>

int gfx_init(void)
{
    float sx, sy;

    if (!ya2d_init())
    {
        log_printf("[gfx] ya2d_init failed");
        return -1;
    }

    if (!ya2d_texturePointer || !Video_Resolution.width || !Video_Resolution.height)
    {
        log_printf("[gfx] RSX/texture memory unavailable");
        return -1;
    }

    /* ya2d's default 2D canvas is 848x512. Use a 1920x1080 canvas instead so
     * the 1080p output is 1:1 and text is not upscaled. */
    sx = (float)Video_Resolution.width / 1920.0f;
    sy = (float)Video_Resolution.height / 1080.0f;
    tiny3d_UserViewport(1, 0.0f, 0.0f, sx, sy, sx, sy);
    tiny3d_Project2D();

    ya2d_screenSetClearColor(GFX_RGBA(10, 12, 16, 255));
    text_init();

    log_printf("[gfx] screen=%ux%u canvas=1920x1080 scale=%.3f/%.3f",
               (unsigned)Video_Resolution.width,
               (unsigned)Video_Resolution.height, sx, sy);
    return 0;
}

void gfx_shutdown(void)
{
    ya2d_deinit();
}

void gfx_begin(void)
{
    ya2d_screenClear();
    ya2d_screenBeginDrawing();
    reset_ttf_frame();
}

void gfx_begin_overlay(void)
{
    tiny3d_BlendFunc(1,
                     TINY3D_BLEND_FUNC_SRC_RGB_SRC_ALPHA |
                         TINY3D_BLEND_FUNC_SRC_ALPHA_SRC_ALPHA,
                     TINY3D_BLEND_FUNC_DST_RGB_ONE_MINUS_SRC_ALPHA |
                         TINY3D_BLEND_FUNC_DST_ALPHA_ZERO,
                     TINY3D_BLEND_RGB_FUNC_ADD | TINY3D_BLEND_ALPHA_FUNC_ADD);
    tiny3d_Project2D();
    reset_ttf_frame();
}

void gfx_end(void)
{
    ya2d_screenFlip();
}

void gfx_clear(u32 color)
{
    ya2d_screenSetClearColor(color);
    tiny3d_Clear(color, TINY3D_CLEAR_ALL);
}

void gfx_rect(int x, int y, int w, int h, u32 color)
{
    if (w <= 0 || h <= 0)
        return;
    ya2d_drawFillRect(x, y, w, h, color);
}

void gfx_rect_outline(int x, int y, int w, int h, u32 color)
{
    if (w <= 0 || h <= 0)
        return;
    ya2d_drawRect(x, y, w, h, color);
}

void gfx_line_h(int x, int y, int w, u32 color)
{
    gfx_rect(x, y, w, 1, color);
}

void gfx_text(int x, int y, int size, u32 color, const char *text)
{
    text_draw(x, y, size, color, text);
}

int gfx_text_width(int size, const char *text)
{
    return text_width(size, text);
}

void gfx_text_center(int x, int w, int y, int size, u32 color, const char *text)
{
    int tw = text_width(size, text);
    text_draw(x + (w - tw) / 2, y, size, color, text);
}

void gfx_text_ellipsis(int x, int y, int max_w, int size, u32 color,
                       const char *text)
{
    text_draw_ellipsis(x, y, max_w, size, color, text);
}

void gfx_text_wrapped(int x, int y, int w, int h, int size, u32 color,
                      const char *text)
{
    text_draw_wrapped(x, y, w, h, size, color, text);
}

void gfx_texture(ya2d_Texture *tex, int x, int y, int w, int h)
{
    if (!tex)
        return;
    ya2d_drawTextureEx(tex, x, y, YA2D_DEFAULT_Z, w, h);
}
