#include "app.h"
#include "platform/gfx.h"
#include "platform/log.h"
#include "platform/sys.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    log_open();
    log_printf("[main] pilis3 %s", APP_VERSION);

    if (sys_init() < 0)
        return 1;

    if (gfx_init() < 0)
    {
        log_printf("[main] gfx_init failed");
        sys_shutdown();
    }

    app_run();

    gfx_shutdown();
    sys_shutdown();
    return 0;
}
