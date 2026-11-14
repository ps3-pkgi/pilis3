#ifndef PILIS3_LOG_H
#define PILIS3_LOG_H

#include <ppu-types.h>

void log_open(void);
void log_close(void);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* tiny3d in the build image calls this; route it into the same file. */
void ps3_boot_trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
