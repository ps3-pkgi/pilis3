#ifndef PSP_PROFILE_H
#define PSP_PROFILE_H
/* WiliMini's H.264 stage profiler. The PSP build ships this header outside the
 * decoder sources; the PS3 port keeps the same layout for diagnostics. */
typedef struct {
    unsigned calls;
    unsigned samples;
    unsigned long long us;
} PspProfileStage;

#define PSP_PROFILE_STAGES      180
#define PSP_PROFILE_QPEL_BASE   20
#define PSP_PROFILE_CHROMA_BASE 148
#define PSP_PROFILE_INTERVAL    1

extern int psp_profile_enabled;
extern PspProfileStage psp_profile_stages[PSP_PROFILE_STAGES];
unsigned psp_profile_tick(void);
#endif
