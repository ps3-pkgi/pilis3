#include "psp_profile.h"

/* The FFmpeg build carries WiliMini's PSP stage-profiler hooks. The PS3
 * player uses VDEC for video, but libavcodec still references these symbols
 * because its H.264 code is linked in. Keep them as inert stubs. */
int psp_profile_enabled = 0;
PspProfileStage psp_profile_stages[PSP_PROFILE_STAGES];

unsigned psp_profile_tick(void)
{
    return 0;
}
