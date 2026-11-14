#include "settings.h"
#include "log.h"
#include "sys.h"

#include <stdio.h>
#include <string.h>

#define SETTINGS_FILE APP_CONFIG_DIR "/settings.txt"

static int g_sw_decode;

void settings_init(void)
{
    FILE *f = fopen(SETTINGS_FILE, "r");
    char line[128];

    if (f)
    {
        while (fgets(line, sizeof(line), f))
        {
            if (!strncmp(line, "decode=sw", 9))
                g_sw_decode = 1;
            else if (!strncmp(line, "decode=hw", 9))
                g_sw_decode = 0;
        }
        fclose(f);
    }

    log_printf("[settings] decode=%s", g_sw_decode ? "software" : "hardware");
}

void settings_set_sw_decode(int sw)
{
    FILE *f;

    g_sw_decode = sw ? 1 : 0;
    sys_mkdirs(APP_CONFIG_DIR);
    f = fopen(SETTINGS_FILE, "w");
    if (f)
    {
        fprintf(f, "decode=%s\n", g_sw_decode ? "sw" : "hw");
        fclose(f);
    }
    log_printf("[settings] decode=%s", g_sw_decode ? "software" : "hardware");
}

int settings_sw_decode(void)
{
    return g_sw_decode;
}
