#ifndef PILIS3_OSK_H
#define PILIS3_OSK_H

int osk_start(const char *title, const char *initial);
int osk_update(void);
int osk_active(void);
void osk_take(char *out, int size);
void osk_cancel(void);

#endif
