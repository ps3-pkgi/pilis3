#ifndef PILIS3_API_H
#define PILIS3_API_H

#include "bili.h"

typedef enum
{
    API_IDLE = 0,
    API_LOADING,
    API_READY,
    API_ERROR
} ApiState;

int api_init(void);
void api_shutdown(void);

int api_start_popular(int page);
int api_start_recommend(int page);
int api_start_region(int rid, int page);
int api_start_search(const char *keyword, int page);
int api_start_dynamic(int page);
int api_start_view(const char *bvid);
int api_start_playurl(const char *bvid, long long cid, int qn);
int api_start_nav(void);
int api_start_toview(void);
int api_start_history(void);
int api_start_fav_folders(void);
int api_start_fav_list(long long media_id);
int api_start_toview_add(long long aid);
int api_start_fav_add(long long aid);
int api_start_qr_generate(void);
int api_start_qr_poll(const char *key);

ApiState api_state(void);
const char *api_error_text(void);
void api_ack(void);

VideoItem *api_items(int *count);
VideoItem *api_view(void);
PlayUrl *api_playurl(void);
BiliUser *api_user(void);
FavFolder *api_folders(int *count);
const char *api_qr_url(void);
const char *api_qr_key(void);
int api_qr_state(void);

#endif
