#ifndef PILIS3_SETTINGS_H
#define PILIS3_SETTINGS_H

/* 0 = hardware (VDEC), 1 = software (libavcodec). */
int settings_sw_decode(void);
void settings_set_sw_decode(int sw);

void settings_init(void);

#endif
