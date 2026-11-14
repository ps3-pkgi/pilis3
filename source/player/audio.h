#ifndef PILIS3_AUDIO_H
#define PILIS3_AUDIO_H

#include <ppu-types.h>

typedef struct AudioPlayer AudioPlayer;

AudioPlayer *audio_player_start(const char *url);
AudioPlayer *audio_player_start_at(const char *url, const u8 *prefix,
                                   u32 prefix_len, long long range_start,
                                   double clock_base);
void audio_player_stop(AudioPlayer *a);
double audio_player_clock(AudioPlayer *a);
int audio_player_ready(AudioPlayer *a);
void audio_player_set_paused(AudioPlayer *a, int paused);
int audio_player_eof(AudioPlayer *a);

#endif
