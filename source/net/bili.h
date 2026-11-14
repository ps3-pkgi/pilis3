#ifndef PILIS3_BILI_H
#define PILIS3_BILI_H

#include <ppu-types.h>

#define BILI_MAX_ITEMS 40

typedef struct
{
    char bvid[16];
    long long aid;
    long long cid;
    char title[256];
    char up[128];
    char cover[512];
    char desc[1024];
    int duration;
    long long view;
    long long danmaku;
    int width;
    int height;
    int page_count;
} VideoItem;

typedef struct
{
    char video_url[1024];
    char audio_url[1024];
    char mp4_url[1024];
    char video_init[64];
    char video_index[64];
    char audio_init[64];
    char audio_index[64];
    int width;
    int height;
    int qn;
    int codecid;
    int duration;
} PlayUrl;

typedef struct
{
    long long mid;
    char name[128];
    char face[512];
    int level;
} BiliUser;

typedef struct
{
    long long id;
    char title[80];
    int media_count;
} FavFolder;

int bili_init(void);
int bili_popular(int page, VideoItem *items, int max, int *count, char *err,
                 int err_size);
int bili_recommend(int page, VideoItem *items, int max, int *count, char *err,
                   int err_size);
int bili_search(const char *keyword, int page, VideoItem *items, int max,
                int *count, char *err, int err_size);
int bili_region(int rid, int page, VideoItem *items, int max, int *count,
                char *err, int err_size);
int bili_dynamic(int page, VideoItem *items, int max, int *count, char *err,
                 int err_size);
int bili_toview(VideoItem *items, int max, int *count, char *err, int err_size);
int bili_history(VideoItem *items, int max, int *count, char *err, int err_size);
int bili_fav_folders(FavFolder *out, int max, int *count, char *err, int err_size);
int bili_fav_list(long long media_id, VideoItem *items, int max, int *count,
                  char *err, int err_size);
int bili_toview_add(long long aid, char *err, int err_size);
int bili_fav_add(long long aid, char *err, int err_size);
int bili_view(const char *bvid, VideoItem *item, char *err, int err_size);
int bili_playurl(const char *bvid, long long cid, int qn, PlayUrl *out, char *err,
                 int err_size);
int bili_nav(BiliUser *user, char *err, int err_size);

int bili_qr_generate(char *url, int url_size, char *key, int key_size, char *err,
                     int err_size);
/* state: 0 = success, 86101 = waiting, 86090 = scanned, 86038 = expired */
int bili_qr_poll(const char *key, int *state, char *err, int err_size);

#endif
