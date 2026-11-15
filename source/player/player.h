#ifndef PILIS3_PLAYER_H
#define PILIS3_PLAYER_H

#include "net/bili.h"
#include "platform/input.h"

typedef struct Player Player;

Player *player_open(const PlayUrl *play, const VideoItem *item);
Player *player_open_at(const PlayUrl *play, const VideoItem *item, double start_time);
/* update returns: 0 normal, 1 exit, 2 seek request, 3 quality request */
int player_update(Player *p, const Input *in);
/* Jump to target seconds in place: the VDEC session is flushed, not reopened. */
int player_seek(Player *p, double target);
double player_current_time(Player *p);
int player_quality_delta(Player *p);
int player_quality_qn(Player *p);
void player_show_hint(Player *p, const char *text);
void player_draw(Player *p);
void player_close(Player *p);

const char *player_error(Player *p);
int player_failed(Player *p);
int player_ready(Player *p);

#endif
