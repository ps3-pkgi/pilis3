#ifndef PILIS3_INPUT_H
#define PILIS3_INPUT_H

#include <ppu-types.h>
#include <io/pad.h>

/* padData keeps the d-pad/START block in digital1 (button[2]) and the face
 * buttons in digital2 (button[3]). input.c packs digital1 into the high half
 * of the 32-bit mask, so direction and START checks must use these shifted
 * values; the bare PAD_CTRL_* constants would alias X/O/△/□ and cancel or
 * open things while scrolling. */
#define PAD_NAV_UP    ((u32)PAD_CTRL_UP << 16)
#define PAD_NAV_DOWN  ((u32)PAD_CTRL_DOWN << 16)
#define PAD_NAV_LEFT  ((u32)PAD_CTRL_LEFT << 16)
#define PAD_NAV_RIGHT ((u32)PAD_CTRL_RIGHT << 16)
#define PAD_NAV_START ((u32)PAD_CTRL_START << 16)

typedef struct
{
    u32 down;
    u32 pressed;
    u32 active;
    int ok;
    int cancel;
} Input;

void input_init(void);
void input_update(Input *in, u64 now_ms);
int input_is_down(const Input *in, u32 button);

#endif
