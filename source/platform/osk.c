#include "osk.h"
#include "log.h"
#include "sys.h"

#include <stdio.h>
#include <string.h>

#include <sys/memory.h>
#include <sysutil/sysutil.h>
#include <sysutil/osk.h>

#define OSKDIALOG_FINISHED 0x503
#define OSKDIALOG_UNLOADED 0x504
#define OSKDIALOG_INPUT_ENTERED 0x505
#define OSKDIALOG_INPUT_CANCELED 0x506

#define OSK_MAX_CHARS 128

typedef enum
{
    OSK_IDLE = 0,
    OSK_LOADING,
    OSK_ACTIVE,
    OSK_DONE
} OskState;

static OskState g_state;
static sys_mem_container_t g_container;
static u16 g_title[128];
static u16 g_initial[256];
static u16 g_input[OSK_MAX_CHARS + 1];
static oskCallbackReturnParam g_return;
static volatile int g_event;
static volatile int g_unloaded;
static volatile int g_action;
static int g_ready;
static char g_result[512];

static void convert_to_utf16(const char *utf8, u16 *out, int max_chars)
{
    int n = 0;

    while (*utf8 && n < max_chars - 1)
    {
        unsigned char c = (unsigned char)*utf8++;
        unsigned int code;

        if (c < 0x80)
            code = c;
        else if ((c & 0xe0) == 0xc0 && *utf8)
        {
            code = ((unsigned int)(c & 0x1f) << 6) |
                   ((unsigned int)(*utf8++ & 0x3f));
        }
        else if ((c & 0xf0) == 0xe0 && utf8[0] && utf8[1])
        {
            code = ((unsigned int)(c & 0x0f) << 12) |
                   ((unsigned int)(utf8[0] & 0x3f) << 6) |
                   ((unsigned int)(utf8[1] & 0x3f));
            utf8 += 2;
        }
        else if ((c & 0xf8) == 0xf0 && utf8[0] && utf8[1] && utf8[2])
        {
            code = ((unsigned int)(c & 0x07) << 18) |
                   ((unsigned int)(utf8[0] & 0x3f) << 12) |
                   ((unsigned int)(utf8[1] & 0x3f) << 6) |
                   ((unsigned int)(utf8[2] & 0x3f));
            utf8 += 3;
        }
        else
            continue;

        if (code >= 0x10000)
        {
            code -= 0x10000;
            out[n++] = (u16)(0xd800 | (code >> 10));
            if (n < max_chars - 1)
                out[n++] = (u16)(0xdc00 | (code & 0x3ff));
        }
        else
        {
            out[n++] = (u16)code;
        }
    }
    out[n] = 0;
}

static int convert_from_utf16(const u16 *in, char *out, int size)
{
    int count = 0;

    while (*in)
    {
        unsigned int code;
        u16 ch = *in++;

        if (ch < 0xd800 || ch >= 0xe000)
            code = ch;
        else
        {
            u16 ch2 = *in++;
            if (ch2 < 0xdc00 || ch2 > 0xdfff)
                break;
            code = 0x10000 + (((unsigned int)ch & 0x3ff) << 10) +
                   ((unsigned int)ch2 & 0x3ff);
        }

        if (code < 0x80)
        {
            if (size < 1)
                break;
            out[count++] = (char)code;
            size--;
        }
        else if (code < 0x800)
        {
            if (size < 2)
                break;
            out[count++] = (char)(0xc0 | (code >> 6));
            out[count++] = (char)(0x80 | (code & 0x3f));
            size -= 2;
        }
        else if (code < 0x10000)
        {
            if (size < 3)
                break;
            out[count++] = (char)(0xe0 | (code >> 12));
            out[count++] = (char)(0x80 | ((code >> 6) & 0x3f));
            out[count++] = (char)(0x80 | (code & 0x3f));
            size -= 3;
        }
        else
        {
            if (size < 4)
                break;
            out[count++] = (char)(0xf0 | (code >> 18));
            out[count++] = (char)(0x80 | ((code >> 12) & 0x3f));
            out[count++] = (char)(0x80 | ((code >> 6) & 0x3f));
            out[count++] = (char)(0x80 | (code & 0x3f));
            size -= 4;
        }
    }
    out[count] = 0;
    return count;
}

static void osk_event_handle(u64 status, u64 param, void *userdata)
{
    (void)param;
    (void)userdata;

    switch ((u32)status)
    {
    case OSKDIALOG_INPUT_CANCELED:
        g_event = OSKDIALOG_INPUT_CANCELED;
        break;
    case OSKDIALOG_UNLOADED:
        g_unloaded = 1;
        break;
    case OSKDIALOG_INPUT_ENTERED:
        g_event = OSKDIALOG_INPUT_ENTERED;
        break;
    case OSKDIALOG_FINISHED:
        g_event = OSKDIALOG_FINISHED;
        break;
    default:
        break;
    }
}

static void osk_cleanup(void)
{
    if ((g_state == OSK_ACTIVE || g_state == OSK_LOADING) && !g_unloaded)
    {
        oskAbort();
        oskUnloadAsync(&g_return);
    }

    sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
    if (g_container)
    {
        sysMemContainerDestroy(g_container);
        g_container = 0;
    }

    sys_register_default_callback();
    g_state = OSK_IDLE;
}

int osk_start(const char *title, const char *initial)
{
    oskParam param;
    oskInputFieldInfo field;
    oskPoint point = {0.0f, 0.0f};

    if (g_state == OSK_ACTIVE || g_state == OSK_LOADING)
        return -1;

    if (sysMemContainerCreate(&g_container, 8 * 1024 * 1024) < 0)
    {
        log_printf("[osk] mem container failed");
        return -1;
    }

    convert_to_utf16(title ? title : "", g_title, 128);
    convert_to_utf16(initial ? initial : "", g_initial, 256);
    memset(g_input, 0, sizeof(g_input));
    memset(&g_return, 0, sizeof(g_return));
    g_return.res = OSK_NO_TEXT;
    g_return.len = OSK_MAX_CHARS;
    g_return.str = g_input;

    field.message = g_title;
    field.startText = g_initial;
    field.maxLength = OSK_MAX_CHARS;

    memset(&param, 0, sizeof(param));
    param.firstViewPanel = OSK_PANEL_TYPE_ALPHABET_FULL_WIDTH;
    param.allowedPanels = OSK_PANEL_TYPE_ALPHABET_FULL_WIDTH |
                          OSK_PANEL_TYPE_ALPHABET | OSK_PANEL_TYPE_NUMERAL |
                          OSK_PANEL_TYPE_ENGLISH |
                          OSK_PANEL_TYPE_SIMPLIFIED_CHINESE;
    param.controlPoint = point;
    param.prohibitFlags = OSK_PROHIBIT_RETURN;

    if (oskSetKeyLayoutOption(OSK_10KEY_PANEL | OSK_FULLKEY_PANEL) < 0 ||
        oskAddSupportLanguage(param.allowedPanels) < 0 ||
        oskSetLayoutMode(OSK_LAYOUTMODE_HORIZONTAL_ALIGN_CENTER |
                         OSK_LAYOUTMODE_VERTICAL_ALIGN_CENTER) < 0 ||
        oskSetInitialInputDevice(OSK_DEVICE_PAD) < 0)
    {
        log_printf("[osk] setup failed");
        g_state = OSK_IDLE;
        osk_cleanup();
        return -1;
    }

    g_event = 0;
    g_unloaded = 0;
    g_action = 0;
    g_ready = 0;
    g_state = OSK_LOADING;

    sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
    sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, osk_event_handle, NULL);

    if (oskLoadAsync(g_container, &param, &field) < 0)
    {
        log_printf("[osk] load failed");
        g_state = OSK_IDLE;
        osk_cleanup();
        return -1;
    }

    g_state = OSK_ACTIVE;
    log_printf("[osk] started");
    return 0;
}

int osk_update(void)
{
    if (g_state != OSK_ACTIVE)
        return 0;

    if (g_unloaded)
    {
        if (g_return.res == OSK_OK && g_action == 1)
        {
            convert_from_utf16(g_input, g_result, sizeof(g_result));
            g_ready = 1;
        }
        osk_cleanup();
        return g_ready;
    }

    switch (g_event)
    {
    case OSKDIALOG_INPUT_ENTERED:
        oskGetInputText(&g_return);
        g_event = 0;
        break;
    case OSKDIALOG_INPUT_CANCELED:
        oskAbort();
        oskUnloadAsync(&g_return);
        g_event = 0;
        g_action = -1;
        break;
    case OSKDIALOG_FINISHED:
        if (g_action != -1)
            g_action = 1;
        oskUnloadAsync(&g_return);
        g_event = 0;
        break;
    default:
        break;
    }

    return 0;
}

int osk_active(void)
{
    return g_state == OSK_ACTIVE || g_state == OSK_LOADING;
}

void osk_take(char *out, int size)
{
    snprintf(out, size, "%s", g_result);
    g_result[0] = 0;
    g_ready = 0;
}

void osk_cancel(void)
{
    if (g_state == OSK_ACTIVE || g_state == OSK_LOADING)
        osk_cleanup();
    g_ready = 0;
}
