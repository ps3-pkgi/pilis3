#ifndef PILIS3_SYS_H
#define PILIS3_SYS_H

#include <ppu-types.h>
#include <sys/sem.h>

#define APP_DIR "/dev_hdd0/game/NP00WILI1/USRDIR"
#define APP_CACHE_DIR APP_DIR "/cache"
#define APP_CONFIG_DIR APP_DIR "/config"
#define APP_COOKIE_FILE APP_CONFIG_DIR "/cookies.txt"

int sys_init(void);
void sys_poll(void);
int sys_exit_requested(void);
void sys_register_default_callback(void);
void sys_shutdown(void) __attribute__((noreturn));

u64 sys_ms(void);
const char *sys_app_dir(void);
void sys_app_path(char *out, int size, const char *rel);
int sys_mkdirs(const char *path);
int sys_file_exists(const char *path);

u32 sys_ok_button(void);
u32 sys_cancel_button(void);

/* sysSemCreate() rejects a NULL attribute (returns 0x8001000d on hardware);
 * this fills in the protocol/pshared/name LV2 expects. */
int sys_sem_create(sys_sem_t *sem, const char *name, s32 init, s32 max);

#endif
