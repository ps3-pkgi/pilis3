#include "sys.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/process.h>
#include <sys/systime.h>
#include <sysutil/sysutil.h>

static u32 g_ok_button;
static u32 g_cancel_button;
static volatile int g_exit_requested;

static void sys_callback(u64 status, u64 param, void *userdata)
{
    (void)param;
    (void)userdata;

    if (status == SYSUTIL_EXIT_GAME)
    {
        log_printf("[sys] SYSUTIL_EXIT_GAME");
        g_exit_requested = 1;
    }
}

int sys_init(void)
{
    int ret = 0;

    sys_register_default_callback();

    sysUtilGetSystemParamInt(SYSUTIL_SYSTEMPARAM_ID_ENTER_BUTTON_ASSIGN, &ret);
    if (ret == 0)
    {
        g_ok_button = 0x0040; /* PAD_CTRL_CROSS */
        g_cancel_button = 0x0020; /* PAD_CTRL_CIRCLE */
    }
    else
    {
        g_ok_button = 0x0020;
        g_cancel_button = 0x0040;
    }

    log_printf("[sys] enter assign=%d ok=%04x cancel=%04x", ret, g_ok_button,
               g_cancel_button);
    return 0;
}

void sys_register_default_callback(void)
{
    sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sys_callback, NULL);
}

void sys_poll(void)
{
    sysUtilCheckCallback();
}

int sys_exit_requested(void)
{
    return g_exit_requested;
}

void sys_shutdown(void)
{
    log_printf("[sys] shutdown");
    log_close();
    sysProcessExit(0);
}

u64 sys_ms(void)
{
    u64 sec = 0, nsec = 0;

    if (sysGetCurrentTime(&sec, &nsec) < 0)
        return 0;
    return sec * 1000ULL + nsec / 1000000ULL;
}

const char *sys_app_dir(void)
{
    return APP_DIR;
}

void sys_app_path(char *out, int size, const char *rel)
{
    snprintf(out, size, "%s/%s", APP_DIR, rel);
}

int sys_file_exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

int sys_mkdirs(const char *dir)
{
    char path[256];
    char *ptr;

    snprintf(path, sizeof(path), "%s", dir);

    ptr = path + 1;
    while (*ptr)
    {
        while (*ptr && *ptr != '/')
            ptr++;

        char last = *ptr;
        *ptr = 0;

        if (!sys_file_exists(path))
        {
            if (mkdir(path, 0777) < 0)
            {
                log_printf("[sys] mkdir %s failed", path);
                return 0;
            }
        }

        *ptr++ = last;
        if (last == 0)
            break;
    }

    return 1;
}

int sys_sem_create(sys_sem_t *sem, const char *name, s32 init, s32 max)
{
    sys_sem_attr_t attr;

    memset(&attr, 0, sizeof(attr));
    attr.attr_protocol = SYS_SEM_ATTR_PROTOCOL;
    attr.attr_pshared = SYS_SEM_ATTR_PSHARED;
    snprintf(attr.name, sizeof(attr.name), "%s", name);
    return sysSemCreate(sem, &attr, init, max);
}

u32 sys_ok_button(void)
{
    return g_ok_button;
}

u32 sys_cancel_button(void)
{
    return g_cancel_button;
}
