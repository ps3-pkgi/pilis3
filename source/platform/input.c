#include "input.h"
#include "sys.h"

#include <string.h>

#include <io/pad.h>
#include <ya2d/ya2d.h>

#define ANALOG_CENTER 0x78
#define ANALOG_MIN 0x50
#define ANALOG_MAX 0xA0
#define REPEAT_DELAY_MS 380
#define REPEAT_RATE_MS 90

static u32 g_prev_down;
static u64 g_down_since;
static u64 g_last_repeat;

void input_init(void)
{
    g_prev_down = 0;
    g_down_since = 0;
    g_last_repeat = 0;
}

static u32 read_buttons(void)
{
    u32 down;

    ya2d_controlsRead();

    /* digital1 is the low half of the raw read on this (big-endian) target,
     * digital2 the next one; keep digital1 in the high half of our mask so
     * the face-button constants stay usable as-is. */
    down = ((u32)ya2d_paddata[0].button[PAD_BUTTON_OFFSET_DIGITAL1] << 16) |
           (u32)ya2d_paddata[0].button[PAD_BUTTON_OFFSET_DIGITAL2];

    if (ya2d_paddata[0].ANA_L_V < ANALOG_MIN)
        down |= PAD_NAV_UP;
    if (ya2d_paddata[0].ANA_L_V > ANALOG_MAX)
        down |= PAD_NAV_DOWN;
    if (ya2d_paddata[0].ANA_L_H < ANALOG_MIN)
        down |= PAD_NAV_LEFT;
    if (ya2d_paddata[0].ANA_L_H > ANALOG_MAX)
        down |= PAD_NAV_RIGHT;

    return down;
}

void input_update(Input *in, u64 now_ms)
{
    u32 down = read_buttons();
    u32 pressed = down & ~g_prev_down;

    in->down = down;
    in->pressed = pressed;
    in->active = pressed;

    if (down == g_prev_down)
    {
        if (now_ms - g_down_since >= REPEAT_DELAY_MS &&
            now_ms - g_last_repeat >= REPEAT_RATE_MS)
        {
            in->active = down;
            g_last_repeat = now_ms;
        }
    }
    else
    {
        g_down_since = now_ms;
        g_last_repeat = now_ms;
    }

    in->ok = (pressed & sys_ok_button()) != 0;
    in->cancel = (pressed & sys_cancel_button()) != 0;

    g_prev_down = down;
}

int input_is_down(const Input *in, u32 button)
{
    return (in->active & button) != 0;
}
