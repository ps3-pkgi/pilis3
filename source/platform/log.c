#include "log.h"

#include <stdarg.h>
#include <stdio.h>

#define LOG_PATH "/dev_hdd0/tmp/pilis3.log"

static FILE *g_log;

void log_open(void)
{
    if (g_log)
        return;

    g_log = fopen(LOG_PATH, "w");
    if (!g_log)
        return;

    setvbuf(g_log, NULL, _IOLBF, 0);
    fprintf(g_log, "==== pilis3 %s ====\n", APP_VERSION);
    fprintf(g_log, "build %s %s\n", __DATE__, __TIME__);
    fflush(g_log);
}

void log_close(void)
{
    if (!g_log)
        return;

    fflush(g_log);
    fclose(g_log);
    g_log = NULL;
}

void log_printf(const char *fmt, ...)
{
    va_list ap;

    if (g_log)
    {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fputc('\n', g_log);
        fflush(g_log);
    }
}

void ps3_boot_trace(const char *fmt, ...)
{
    char line[192];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    log_printf("[t3d] %s", line);
}
