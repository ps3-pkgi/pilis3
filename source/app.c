#include "app.h"

#include "net/api.h"
#include "net/http.h"
#include "player/player.h"
#include "platform/gfx.h"
#include "platform/input.h"
#include "platform/log.h"
#include "platform/osk.h"
#include "platform/settings.h"
#include "platform/sys.h"
#include "ui/cover.h"
#include "ui/qrcodegen.h"
#include "ui/theme.h"

#include <io/pad.h>
#include <stdio.h>
#include <string.h>

#define HEADER_H 96
#define FOOTER_H 76
#define ROW_H 136
#define ROW_GAP 8
#define LIST_TOP (HEADER_H + 28)
#define VISIBLE_ROWS 5

typedef enum
{
    TAB_RECOMMEND = 0,
    TAB_POPULAR,
    TAB_REGION,
    TAB_DYNAMIC,
    TAB_MINE,
    TAB_COUNT
} Tab;

typedef enum
{
    SCREEN_MAIN = 0,
    SCREEN_DETAIL,
    SCREEN_LOGIN,
    SCREEN_MINE,
    SCREEN_LIST,
    SCREEN_FOLDERS,
    SCREEN_SETTINGS,
    SCREEN_PLAYER
} Screen;

typedef enum
{
    LIST_NONE = 0,
    LIST_HISTORY,
    LIST_TOVIEW,
    LIST_FAV,
    LIST_SEARCH
} ListKind;

typedef enum
{
    PEND_NONE = 0,
    PEND_DETAIL,
    PEND_PLAY,
    PEND_NAV,
    PEND_TOVIEW,
    PEND_HISTORY,
    PEND_FAV_FOLDERS,
    PEND_FAV_LIST,
    PEND_TOVIEW_ADD,
    PEND_FAV_ADD,
    PEND_SEARCH,
    PEND_QR_GEN,
    PEND_QR_POLL
} Pending;

typedef enum
{
    NEXT_NONE = 0,
    NEXT_NAV,
    NEXT_QR_GEN
} NextAction;

typedef struct
{
    const char *name;
    int rid;
} Area;

static const char *k_tab_names[TAB_COUNT] = {"推荐", "热门", "分区", "动态", "我的"};

static const Area k_areas[] = {
    {"动画", 1},     {"番剧", 13},    {"国创", 167},   {"音乐", 3},
    {"舞蹈", 129},   {"游戏", 4},     {"知识", 36},    {"科技", 188},
    {"运动", 234},   {"汽车", 223},   {"生活", 160},   {"美食", 211},
    {"动物圈", 217}, {"鬼畜", 119},   {"时尚", 155},   {"资讯", 202},
    {"娱乐", 5},     {"影视", 181},   {"纪录片", 177}, {"电影", 23},
};

#define AREA_COUNT ((int)(sizeof(k_areas) / sizeof(k_areas[0])))

static Tab g_tab = TAB_RECOMMEND;
static Tab g_prev_tab = TAB_RECOMMEND;
static Screen g_screen = SCREEN_MAIN;
static Pending g_pending;
static NextAction g_next;
static int g_selected;
static int g_first;
static int g_area_index;
static int g_region_selected = -1;
static int g_running = 1;
static ListKind g_list_kind;
static Screen g_detail_return = SCREEN_MAIN;
static Screen g_settings_return = SCREEN_MINE;
static char g_list_title[80];
static FavFolder g_folders[20];
static int g_folder_count;
static int g_folder_selected;
static int g_loading;
static int g_logged_in;
static double g_resume_time;
/* 软解只保证 720P 流畅；1080P 留给 L1/R1 手动切换。 */
static int g_requested_qn = 64;
static char g_search_keyword[128];
static char g_toast[160];
static u64 g_toast_until;

static VideoItem g_items[BILI_MAX_ITEMS];
static int g_count;
static VideoItem g_detail;
static PlayUrl g_play;
static Player *g_player;
static BiliUser g_user;
static char g_error[256];
static char g_login_error[160];
static char g_qr_url[512];
static char g_qr_key[128];
static int g_qr_state = -1;
static u64 g_next_qr_poll;

static const u32 k_cover_colors[8] = {
    GFX_RGBA(58, 76, 110, 255), GFX_RGBA(86, 64, 54, 255),
    GFX_RGBA(44, 88, 78, 255),  GFX_RGBA(84, 52, 92, 255),
    GFX_RGBA(96, 74, 42, 255),  GFX_RGBA(40, 70, 96, 255),
    GFX_RGBA(70, 60, 84, 255),  GFX_RGBA(90, 56, 62, 255),
};

static void toast_show(const char *text)
{
    snprintf(g_toast, sizeof(g_toast), "%s", text);
    g_toast_until = sys_ms() + 2200;
    log_printf("[ui] toast: %s", text);
}

static void fmt_count(long long v, char *out, int size)
{
    if (v >= 100000000LL)
        snprintf(out, size, "%lld.%lld亿", v / 100000000LL,
                 (v % 100000000LL) / 10000000LL);
    else if (v >= 10000LL)
        snprintf(out, size, "%lld.%lld万", v / 10000LL,
                 (v % 10000LL) / 1000LL);
    else
        snprintf(out, size, "%lld", v);
}

static void fmt_duration(int sec, char *out, int size)
{
    if (sec < 0)
        sec = 0;
    if (sec >= 3600)
        snprintf(out, size, "%d:%02d:%02d", sec / 3600, (sec / 60) % 60, sec % 60);
    else
        snprintf(out, size, "%d:%02d", sec / 60, sec % 60);
}

static void start_search(const char *keyword)
{
    if (!keyword || !*keyword)
        return;

    snprintf(g_search_keyword, sizeof(g_search_keyword), "%s", keyword);
    g_list_kind = LIST_SEARCH;
    snprintf(g_list_title, sizeof(g_list_title), "搜索：%s", keyword);
    g_loading = 1;
    g_error[0] = 0;
    g_selected = 0;
    g_first = 0;
    g_count = 0;
    g_screen = SCREEN_LIST;

    if (api_start_search(keyword, 1) == 0)
        g_pending = PEND_SEARCH;
    else
    {
        snprintf(g_error, sizeof(g_error), "请求繁忙");
        g_loading = 0;
        g_pending = PEND_NONE;
    }
}

static void start_mine_list(ListKind kind)
{
    int rc = -1;

    g_list_kind = kind;
    g_loading = 1;
    g_error[0] = 0;
    g_selected = 0;
    g_first = 0;
    g_count = 0;

    if (kind == LIST_HISTORY)
    {
        snprintf(g_list_title, sizeof(g_list_title), "历史记录");
        rc = api_start_history();
        g_pending = PEND_HISTORY;
    }
    else if (kind == LIST_TOVIEW)
    {
        snprintf(g_list_title, sizeof(g_list_title), "稍后再看");
        rc = api_start_toview();
        g_pending = PEND_TOVIEW;
    }
    else if (kind == LIST_FAV)
    {
        rc = api_start_fav_folders();
        g_pending = PEND_FAV_FOLDERS;
        if (rc == 0)
            g_screen = SCREEN_FOLDERS;
        else
        {
            snprintf(g_error, sizeof(g_error), "请求繁忙");
            g_loading = 0;
            g_pending = PEND_NONE;
        }
        return;
    }

    if (rc == 0)
    {
        g_screen = SCREEN_LIST;
    }
    else
    {
        snprintf(g_error, sizeof(g_error), "请求繁忙");
        g_loading = 0;
        g_pending = PEND_NONE;
    }
}

static void start_region(int rid)
{
    if (api_start_region(rid, 1) == 0)
    {
        g_region_selected = rid;
        g_loading = 1;
        g_error[0] = 0;
        g_selected = 0;
        g_first = 0;
        g_count = 0;
        g_pending = PEND_NONE;
    }
    else
    {
        toast_show("请求繁忙，请稍后再试");
    }
}

static void start_dynamic(void)
{
    if (!g_logged_in && !http_has_login())
    {
        snprintf(g_error, sizeof(g_error), "动态需要登录：请在“我的”里扫码");
        g_count = 0;
        g_loading = 0;
        return;
    }

    if (api_start_dynamic(1) == 0)
    {
        g_loading = 1;
        g_error[0] = 0;
        g_selected = 0;
        g_first = 0;
        g_count = 0;
        g_pending = PEND_NONE;
    }
    else
    {
        snprintf(g_error, sizeof(g_error), "请求繁忙");
        g_loading = 0;
    }
}

static void start_nav(void)
{
    if (api_start_nav() == 0)
    {
        g_pending = PEND_NAV;
        g_loading = 1;
        g_error[0] = 0;
    }
    else
    {
        snprintf(g_error, sizeof(g_error), "请求繁忙");
        g_loading = 0;
    }
}

static void start_qr(void)
{
    if (api_start_qr_generate() == 0)
    {
        g_pending = PEND_QR_GEN;
        g_loading = 1;
        g_login_error[0] = 0;
        g_qr_url[0] = 0;
        g_qr_key[0] = 0;
        g_qr_state = -1;
    }
    else
    {
        snprintf(g_login_error, sizeof(g_login_error), "二维码请求繁忙");
    }
}

static void start_tab(Tab tab)
{
    g_loading = 0;
    g_error[0] = 0;
    g_selected = 0;
    g_first = 0;
    g_count = 0;
    g_region_selected = -1;

    if (tab == TAB_RECOMMEND)
    {
        if (api_start_recommend(1) == 0)
        {
            g_loading = 1;
            g_pending = PEND_NONE;
        }
        else
            snprintf(g_error, sizeof(g_error), "上一请求尚未结束");
    }
    else if (tab == TAB_POPULAR)
    {
        if (api_start_popular(1) == 0)
        {
            g_loading = 1;
            g_pending = PEND_NONE;
        }
        else
            snprintf(g_error, sizeof(g_error), "上一请求尚未结束");
    }
    else if (tab == TAB_DYNAMIC)
    {
        start_dynamic();
    }
    else if (tab == TAB_MINE)
    {
        if (g_logged_in || http_has_login())
            start_nav();
        else
        {
            snprintf(g_error, sizeof(g_error), "请先在“我的”里扫码登录");
            g_screen = SCREEN_MINE;
        }
    }
}

static void tab_switch(int delta)
{
    int t = ((int)g_tab + delta + TAB_COUNT) % TAB_COUNT;

    if (t == (int)g_tab)
        return;

    g_prev_tab = g_tab;
    g_tab = (Tab)t;
    g_screen = SCREEN_MAIN;
    start_tab(g_tab);
}

static void poll_api(void)
{
    ApiState state = api_state();
    Pending pending;
    int st;

    if (state != API_READY && state != API_ERROR)
        return;

    pending = g_pending;
    g_pending = PEND_NONE;
    g_next = NEXT_NONE;

    if (state == API_READY)
    {
        if (pending == PEND_DETAIL)
        {
            g_detail = *api_view();
            g_screen = SCREEN_DETAIL;
            toast_show("已加载视频详情");
        }
        else if (pending == PEND_PLAY)
        {
            PlayUrl *play = api_playurl();
            if (play->video_url[0] || play->mp4_url[0])
            {
                g_play = *play;
                if (g_player)
                {
                    player_close(g_player);
                    g_player = NULL;
                }
                g_player = player_open_at(&g_play, &g_detail, g_resume_time);
                g_resume_time = 0.0;
                if (g_player && !player_failed(g_player))
                {
                    g_screen = SCREEN_PLAYER;
                    g_error[0] = 0;
                }
                else
                {
                    const char *err = g_player ? player_error(g_player) : "播放器初始化失败";
                    snprintf(g_error, sizeof(g_error), "%s", err);
                    if (g_player)
                    {
                        player_close(g_player);
                        g_player = NULL;
                    }
                    g_screen = SCREEN_DETAIL;
                }
            }
            else
            {
                snprintf(g_error, sizeof(g_error), "没有可用的视频流");
            }
        }
        else if (pending == PEND_NAV)
        {
            g_user = *api_user();
            g_logged_in = 1;
            g_screen = SCREEN_MINE;
            g_error[0] = 0;
            toast_show("已登录");
            log_printf("[ui] logged in as %s", g_user.name);
        }
        else if (pending == PEND_TOVIEW || pending == PEND_HISTORY ||
                 pending == PEND_FAV_LIST || pending == PEND_SEARCH)
        {
            int count = 0;
            VideoItem *items = api_items(&count);
            if (count > BILI_MAX_ITEMS)
                count = BILI_MAX_ITEMS;
            memcpy(g_items, items, sizeof(VideoItem) * count);
            g_count = count;
            g_selected = 0;
            g_first = 0;
            g_screen = SCREEN_LIST;
            g_error[0] = 0;
        }
        else if (pending == PEND_TOVIEW_ADD)
        {
            toast_show("已加入稍后再看");
        }
        else if (pending == PEND_FAV_ADD)
        {
            toast_show("已收藏到默认收藏夹");
        }
        else if (pending == PEND_FAV_FOLDERS)
        {
            int count = 0;
            FavFolder *folders = api_folders(&count);
            if (count > 20)
                count = 20;
            memcpy(g_folders, folders, sizeof(FavFolder) * count);
            g_folder_count = count;
            g_folder_selected = 0;
            g_screen = SCREEN_FOLDERS;
            g_error[0] = 0;
        }
        else if (pending == PEND_QR_GEN)
        {
            snprintf(g_qr_url, sizeof(g_qr_url), "%s", api_qr_url());
            snprintf(g_qr_key, sizeof(g_qr_key), "%s", api_qr_key());
            g_qr_state = 86101;
            g_next_qr_poll = sys_ms() + 1500;
            g_screen = SCREEN_LOGIN;
            log_printf("[ui] qr generated");
        }
        else if (pending == PEND_QR_POLL)
        {
            g_qr_state = api_qr_state();
            g_next_qr_poll = sys_ms() + 2000;
            log_printf("[ui] qr poll state=%d", g_qr_state);
            if (g_qr_state == 0)
            {
                g_login_error[0] = 0;
                g_next = NEXT_NAV;
            }
            else if (g_qr_state == 86038)
            {
                snprintf(g_login_error, sizeof(g_login_error),
                         "二维码已过期，按 × 重新生成");
            }
            else if (g_qr_state < 0)
            {
                snprintf(g_login_error, sizeof(g_login_error), "%s",
                         api_error_text());
            }
        }
        else
        {
            int count = 0;
            VideoItem *items = api_items(&count);
            if (count > BILI_MAX_ITEMS)
                count = BILI_MAX_ITEMS;
            memcpy(g_items, items, sizeof(VideoItem) * count);
            g_count = count;
            g_selected = 0;
            g_first = 0;
            g_error[0] = 0;
        }
    }
    else
    {
        if (pending == PEND_NAV)
        {
            snprintf(g_login_error, sizeof(g_login_error), "%s", api_error_text());
            if (strstr(api_error_text(), "未登录"))
            {
                g_logged_in = 0;
                g_screen = SCREEN_LOGIN;
                g_next = NEXT_QR_GEN;
            }
            else
            {
                snprintf(g_error, sizeof(g_error), "%s", api_error_text());
            }
        }
        else if (pending == PEND_QR_GEN || pending == PEND_QR_POLL)
        {
            snprintf(g_login_error, sizeof(g_login_error), "%s", api_error_text());
            if (pending == PEND_QR_POLL)
                g_next_qr_poll = sys_ms() + 3000;
        }
        else if (pending == PEND_TOVIEW || pending == PEND_HISTORY ||
                 pending == PEND_FAV_FOLDERS || pending == PEND_FAV_LIST ||
                 pending == PEND_SEARCH)
        {
            snprintf(g_error, sizeof(g_error), "%s", api_error_text());
        }
        else if (pending == PEND_TOVIEW_ADD || pending == PEND_FAV_ADD)
        {
            toast_show(api_error_text());
        }
        else
        {
            snprintf(g_error, sizeof(g_error), "%s", api_error_text());
        }
    }

    g_loading = 0;
    api_ack();

    st = (int)g_next;
    g_next = NEXT_NONE;
    if (st == NEXT_NAV)
        start_nav();
    else if (st == NEXT_QR_GEN)
        start_qr();
}

static void draw_header(void)
{
    int x;

    gfx_rect(0, 0, GFX_W, HEADER_H, COL_PANEL);
    gfx_line_h(0, HEADER_H - 1, GFX_W, COL_LINE);

    gfx_text(56, 22, 44, COL_PINK, "pilis3");

    x = 330;
    for (int i = 0; i < TAB_COUNT; ++i)
    {
        int w = gfx_text_width(30, k_tab_names[i]);
        u32 color = (i == (int)g_tab) ? COL_TEXT : COL_TEXT_DIM;

        gfx_text(x, 32, 30, color, k_tab_names[i]);
        if (i == (int)g_tab)
            gfx_rect(x, HEADER_H - 6, w, 4, COL_PINK);
        x += w + 58;
    }

    gfx_text(GFX_W - 300, 30, 24, COL_TEXT_FAINT, "L1 / R1 切换");
}

static void draw_row(const VideoItem *item, int index, int y, int selected)
{
    const int cover_x = 56;
    const int cover_y = y;
    const int cover_w = 224;
    const int cover_h = 126;
    const int text_x = 312;
    const int text_w = GFX_W - text_x - 80;
    char views[32], dans[32], dur[32], stat[128];

    if (selected)
    {
        gfx_rect(32, y - 4, GFX_W - 64, ROW_H, COL_ROW_SEL);
        gfx_rect(32, y - 4, 6, ROW_H, COL_PINK);
    }

    cover_request(item->bvid, item->cover);
    cover_draw(item->bvid, cover_x, cover_y, cover_w, cover_h,
               k_cover_colors[index % 8]);
    gfx_text_center(cover_x, cover_w, cover_y + 38, 40, COL_TEXT_DIM, "▶");

    fmt_count(item->view, views, sizeof(views));
    fmt_count(item->danmaku, dans, sizeof(dans));
    if (item->view > 0 || item->danmaku > 0)
        snprintf(stat, sizeof(stat), "%s播放 · %s弹幕", views, dans);
    else
        snprintf(stat, sizeof(stat), "最新投稿");

    fmt_duration(item->duration, dur, sizeof(dur));
    gfx_rect(cover_x + cover_w - 78, cover_y + cover_h - 30, 70, 24, COL_BADGE);
    gfx_text(cover_x + cover_w - 72, cover_y + cover_h - 29, 20, COL_TEXT, dur);

    gfx_text_ellipsis(text_x, y + 4, text_w, 32, COL_TEXT,
                      item->title[0] ? item->title : "(无标题)");
    gfx_text(text_x, y + 54, 24, COL_TEXT_DIM,
             item->up[0] ? item->up : "未知 UP");
    gfx_text(text_x, y + 92, 22, COL_TEXT_FAINT, stat);
}

static void draw_list(void)
{
    int y = LIST_TOP;

    if (g_tab == TAB_REGION && g_region_selected >= 0)
    {
        char crumb[128];
        snprintf(crumb, sizeof(crumb), "分区 / %s    ○ 返回分区",
                 k_areas[g_area_index].name);
        gfx_text(56, HEADER_H + 2, 22, COL_TEXT_FAINT, crumb);
    }
    else if (g_screen == SCREEN_LIST && g_list_title[0])
    {
        gfx_text(56, HEADER_H + 2, 24, COL_TEXT_FAINT, g_list_title);
    }

    if (g_loading)
    {
        gfx_text_center(0, GFX_W, GFX_H / 2 - 20, 32, COL_TEXT_DIM, "加载中…");
        return;
    }

    if (g_error[0])
    {
        gfx_text_center(0, GFX_W, GFX_H / 2 - 40, 30, COL_ERROR, g_error);
        gfx_text_center(0, GFX_W, GFX_H / 2 + 10, 24, COL_TEXT_FAINT,
                        "按 × 重试，或 L1/R1 切换页面");
        return;
    }

    if (g_count == 0)
    {
        gfx_text_center(0, GFX_W, GFX_H / 2 - 20, 28, COL_TEXT_DIM,
                        "这个页面还没有内容");
        return;
    }

    if (g_selected < g_first)
        g_first = g_selected;
    if (g_selected >= g_first + VISIBLE_ROWS)
        g_first = g_selected - VISIBLE_ROWS + 1;

    for (int i = 0; i < VISIBLE_ROWS; ++i)
    {
        int idx = g_first + i;
        if (idx >= g_count)
            break;
        draw_row(&g_items[idx], idx, y, idx == g_selected);
        y += ROW_H + ROW_GAP;
    }
}

static void draw_region_grid(void)
{
    const int cols = 5;
    const int cell_w = 320;
    const int cell_h = 96;
    const int gap_x = 20;
    const int gap_y = 22;
    const int start_x = (GFX_W - (cols * cell_w + (cols - 1) * gap_x)) / 2;
    const int start_y = HEADER_H + 72;

    gfx_text(56, HEADER_H + 12, 30, COL_TEXT, "分区");
    gfx_text(150, HEADER_H + 18, 22, COL_TEXT_FAINT,
             "方向键选择，× 进入");

    for (int i = 0; i < AREA_COUNT; ++i)
    {
        int col = i % cols;
        int row = i / cols;
        int x = start_x + col * (cell_w + gap_x);
        int y = start_y + row * (cell_h + gap_y);
        int sel = (i == g_area_index);

        gfx_rect(x, y, cell_w, cell_h, sel ? COL_PINK_DIM : COL_PANEL);
        gfx_rect_outline(x, y, cell_w, cell_h, sel ? COL_PINK : COL_LINE);
        gfx_text(x + 28, y + 22, 32, sel ? COL_TEXT : COL_TEXT_DIM,
                 k_areas[i].name);
        char rid[32];
        snprintf(rid, sizeof(rid), "rid %d", k_areas[i].rid);
        gfx_text(x + 28, y + 62, 18, COL_TEXT_FAINT, rid);
    }
}

static void draw_folders(void)
{
    int y = HEADER_H + 70;

    gfx_text(56, HEADER_H + 12, 30, COL_TEXT, "收藏夹");

    if (g_loading)
    {
        gfx_text_center(0, GFX_W, GFX_H / 2 - 20, 30, COL_TEXT_DIM, "加载中…");
        return;
    }

    if (g_error[0])
    {
        gfx_text_center(0, GFX_W, GFX_H / 2 - 30, 28, COL_ERROR, g_error);
        gfx_text_center(0, GFX_W, GFX_H / 2 + 20, 22, COL_TEXT_FAINT,
                        "按 ○ 返回");
        return;
    }

    for (int i = 0; i < g_folder_count; ++i)
    {
        int sel = (i == g_folder_selected);
        gfx_rect(56, y, GFX_W - 112, 84, sel ? COL_ROW_SEL : COL_PANEL);
        if (sel)
            gfx_rect(56, y, 6, 84, COL_PINK);
        gfx_text(96, y + 18, 30, sel ? COL_TEXT : COL_TEXT_DIM,
                 g_folders[i].title);
        char cnt[64];
        snprintf(cnt, sizeof(cnt), "%d 个视频", g_folders[i].media_count);
        gfx_text(GFX_W - 260, y + 28, 22, COL_TEXT_FAINT, cnt);
        y += 96;
    }
}

static void draw_detail(void)
{
    const VideoItem *item = &g_detail;
    char views[32], dans[32], stat[160];
    int x = 72;
    int y = HEADER_H + 40;

    fmt_count(item->view, views, sizeof(views));
    fmt_count(item->danmaku, dans, sizeof(dans));
    snprintf(stat, sizeof(stat), "%s播放 · %s弹幕", views, dans);

    cover_request(item->bvid, item->cover);
    cover_draw(item->bvid, x, y, 560, 315, k_cover_colors[0]);
    gfx_text_center(x, 560, y + 130, 56, COL_TEXT_DIM, "▶");

    gfx_text_ellipsis(x + 620, y + 6, GFX_W - x - 620 - 72, 40, COL_TEXT,
                      item->title);
    gfx_text(x + 620, y + 70, 28, COL_TEXT_DIM, item->up);
    gfx_text(x + 620, y + 112, 24, COL_TEXT_FAINT, stat);
    gfx_text(x + 620, y + 160, 24, COL_TEXT_FAINT,
             item->page_count > 1 ? "多 P 视频，播放器下一步接入分 P 选择"
                                  : "单 P 视频");

    gfx_text(x, y + 350, 26, COL_TEXT_DIM, "简介");
    gfx_text_wrapped(x, y + 390, GFX_W - 2 * x, 330, 24, COL_TEXT,
                     item->desc[0] ? item->desc : "（无简介）");

    gfx_rect(GFX_W - 280, GFX_H - FOOTER_H - 96, 200, 60, COL_PINK);
    gfx_text(GFX_W - 240, GFX_H - FOOTER_H - 82, 28, COL_TEXT, "× 播放");
}

static void draw_qr(const char *text, int x, int y, int size)
{
    uint8_t temp[qrcodegen_BUFFER_LEN_MAX];
    uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    int modules, scale, quiet = 4;

    if (!qrcodegen_encodeText(text, temp, qr, qrcodegen_Ecc_MEDIUM,
                              qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                              qrcodegen_Mask_AUTO, true))
        return;

    modules = qrcodegen_getSize(qr);
    scale = size / (modules + quiet * 2);
    if (scale < 1)
        scale = 1;
    size = (modules + quiet * 2) * scale;

    gfx_rect(x, y, size, size, GFX_RGBA(255, 255, 255, 255));
    for (int my = 0; my < modules; ++my)
    {
        for (int mx = 0; mx < modules; ++mx)
        {
            if (qrcodegen_getModule(qr, mx, my))
            {
                gfx_rect(x + (mx + quiet) * scale, y + (my + quiet) * scale,
                         scale, scale, GFX_RGBA(0, 0, 0, 255));
            }
        }
    }
}

static void draw_login(void)
{
    int qr_size = 420;
    int qx = 140;
    int qy = HEADER_H + 100;
    const char *status;

    gfx_text(120, HEADER_H + 24, 36, COL_TEXT, "扫码登录");

    if (g_loading && !g_qr_url[0])
    {
        gfx_text_center(0, GFX_W, GFX_H / 2 - 20, 30, COL_TEXT_DIM,
                        "正在获取二维码…");
        return;
    }

    if (g_qr_url[0])
    {
        draw_qr(g_qr_url, qx, qy, qr_size);
    }
    else
    {
        gfx_rect(qx, qy, qr_size, qr_size, COL_PANEL_2);
        gfx_text_center(qx, qr_size, qy + qr_size / 2 - 12, 24, COL_TEXT_FAINT,
                        "二维码不可用");
    }

    switch (g_qr_state)
    {
    case 0:
        status = "登录成功";
        break;
    case 86101:
        status = "请使用哔哩哔哩手机客户端扫码";
        break;
    case 86090:
        status = "已扫码，请在手机上确认";
        break;
    case 86038:
        status = "二维码已过期";
        break;
    default:
        status = "等待扫码";
        break;
    }

    gfx_text(qx + qr_size + 80, qy + 40, 34, COL_TEXT, status);
    gfx_text(qx + qr_size + 80, qy + 110, 24, COL_TEXT_DIM,
             "登录后可看 1080P、动态、收藏和历史");
    gfx_text(qx + qr_size + 80, qy + 160, 24, COL_TEXT_FAINT,
             "二维码 3 分钟内有效");

    if (g_login_error[0])
        gfx_text(qx + qr_size + 80, qy + 230, 24, COL_ERROR, g_login_error);

    if (g_qr_state == 86038)
        gfx_text(qx + qr_size + 80, qy + 280, 26, COL_PINK, "按 × 重新生成");
}

static void draw_mine(void)
{
    int y = HEADER_H + 40;

    if (!g_logged_in)
    {
        gfx_text_center(0, GFX_W, GFX_H / 2 - 60, 34, COL_TEXT, "还没有登录");
        gfx_text_center(0, GFX_W, GFX_H / 2, 24, COL_TEXT_DIM,
                        "按 × 扫码登录，解锁动态、收藏和历史");
        return;
    }

    gfx_rect(80, y, GFX_W - 160, 200, COL_PANEL);

    if (g_user.face[0])
    {
        cover_request("user-face", g_user.face);
        cover_draw("user-face", 112, y + 32, 136, 136, COL_PANEL_2);
    }
    else
    {
        gfx_rect(112, y + 32, 136, 136, COL_PANEL_2);
    }

    gfx_text(288, y + 36, 40, COL_TEXT, g_user.name);
    char info[160];
    snprintf(info, sizeof(info), "UID %lld   LV%d", g_user.mid, g_user.level);
    gfx_text(288, y + 96, 26, COL_TEXT_DIM, info);
    gfx_text(288, y + 140, 24, COL_TEXT_FAINT,
             g_logged_in ? "已登录，按 △ 刷新资料" : "");

    const char *menu[] = {"历史记录", "收藏夹", "稍后再看", "设置"};
    for (int i = 0; i < 4; ++i)
    {
        int my = y + 250 + i * 96;
        int sel = (i == g_selected);
        gfx_rect(80, my, GFX_W - 160, 80, sel ? COL_ROW_SEL : COL_PANEL);
        if (sel)
            gfx_rect(80, my, 6, 80, COL_PINK);
        gfx_text(120, my + 22, 30, sel ? COL_TEXT : COL_TEXT_DIM, menu[i]);
    }
}

static void draw_settings(void)
{
    int y = HEADER_H + 92;
    const char *mode = settings_sw_decode() ? "软件 (PPU)" : "硬件 (VDEC)";
    int w;

    gfx_text(56, HEADER_H + 12, 30, COL_TEXT, "设置");
    gfx_text(56, HEADER_H + 48, 22, COL_TEXT_FAINT,
             "视频解码方式；硬件失败会自动回退软解");

    gfx_rect(80, y, GFX_W - 160, 100, COL_ROW_SEL);
    gfx_rect(80, y, 6, 100, COL_PINK);
    gfx_text(120, y + 30, 30, COL_TEXT, "视频解码");

    w = gfx_text_width(28, mode);
    gfx_text(GFX_W - 120 - w, y + 32, 28, COL_PINK, mode);
}

static void draw_footer(void)
{
    int y = GFX_H - FOOTER_H;
    char text[200];

    gfx_rect(0, y, GFX_W, FOOTER_H, COL_PANEL);
    gfx_line_h(0, y, GFX_W, COL_LINE);

    if (g_screen == SCREEN_DETAIL)
        snprintf(text, sizeof(text),
                 "%s 播放    %s 稍后再看    %s 收藏    %s 返回", "×", "□", "△",
                 "○");
    else if (g_screen == SCREEN_LOGIN)
        snprintf(text, sizeof(text), "%s 刷新    %s 返回", "×", "○");
    else if (g_screen == SCREEN_MINE)
        snprintf(text, sizeof(text), g_logged_in ? "%s 选择    %s 返回" : "%s 登录    %s 返回",
                 "×", "○");
    else if (g_screen == SCREEN_LIST || g_screen == SCREEN_FOLDERS)
        snprintf(text, sizeof(text), "%s 打开    %s 返回", "×", "○");
    else if (g_screen == SCREEN_SETTINGS)
        snprintf(text, sizeof(text), "%s 切换    %s 返回", "×", "○");
    else
        snprintf(text, sizeof(text), "%s 打开    %s 返回    %s 搜索    %s 收藏",
                 "×", "○", "△", "□");

    gfx_text(56, y + 24, 24, COL_TEXT_DIM, text);

    if (g_screen == SCREEN_MAIN && g_tab != TAB_REGION)
    {
        snprintf(text, sizeof(text), "%s  %d/%d", k_tab_names[g_tab],
                 g_count ? g_selected + 1 : 0, g_count);
        int w = gfx_text_width(24, text);
        gfx_text(GFX_W - 56 - w, y + 24, 24, COL_TEXT_FAINT, text);
    }
}

static void draw_toast(void)
{
    int w, x, y;

    if (!g_toast[0] || sys_ms() > g_toast_until)
        return;

    w = gfx_text_width(24, g_toast) + 48;
    x = (GFX_W - w) / 2;
    y = GFX_H - FOOTER_H - 72;

    gfx_rect(x, y, w, 52, COL_PANEL_2);
    gfx_rect_outline(x, y, w, 52, COL_LINE);
    gfx_text(x + 24, y + 13, 24, COL_TEXT, g_toast);
}

static void handle_region_grid(const Input *in)
{
    int col = g_area_index % 5;
    int row = g_area_index / 5;

    if (in->pressed & PAD_NAV_LEFT && col > 0)
        g_area_index--;
    if (in->pressed & PAD_NAV_RIGHT && col < 4 && g_area_index + 1 < AREA_COUNT)
        g_area_index++;
    if (in->pressed & PAD_NAV_UP && row > 0)
        g_area_index -= 5;
    if (in->pressed & PAD_NAV_DOWN && g_area_index + 5 < AREA_COUNT)
        g_area_index += 5;

    if (in->pressed & sys_ok_button())
        start_region(k_areas[g_area_index].rid);

    /* ○ on the region grid means "leave 分区", not "quit the app". The
     * footer says 返回, and quitting here made backing out of a region look
     * like a random crash (there is no confirmation on screen). */
    if (in->cancel || (in->pressed & sys_cancel_button()))
    {
        Tab back = g_prev_tab != TAB_REGION ? g_prev_tab : TAB_RECOMMEND;

        log_printf("[app] region grid back to tab %d", (int)back);
        g_tab = back;
        start_tab(back);
    }
}

static void handle_main_input(const Input *in)
{
    if (in->pressed & PAD_CTRL_L1)
        tab_switch(-1);
    if (in->pressed & PAD_CTRL_R1)
        tab_switch(1);

    if (g_tab == TAB_REGION && g_region_selected < 0)
    {
        handle_region_grid(in);
        return;
    }

    if (in->active & PAD_NAV_UP)
    {
        if (g_selected > 0)
            g_selected--;
    }
    if (in->active & PAD_NAV_DOWN)
    {
        if (g_selected + 1 < g_count)
            g_selected++;
    }

    if (in->pressed & sys_ok_button())
    {
        if (g_count > 0)
        {
            if (api_start_view(g_items[g_selected].bvid) == 0)
            {
                g_detail_return = SCREEN_MAIN;
                g_pending = PEND_DETAIL;
                g_loading = 1;
                g_error[0] = 0;
            }
        }
        else if ((g_tab == TAB_DYNAMIC || g_tab == TAB_MINE) && !g_logged_in)
        {
            g_screen = SCREEN_LOGIN;
            start_qr();
        }
        else if (g_error[0])
        {
            if (g_tab == TAB_REGION && g_region_selected >= 0)
                start_region(g_region_selected);
            else
                start_tab(g_tab);
        }
    }

    if (in->pressed & PAD_CTRL_TRIANGLE)
    {
        if (osk_start("搜索 B站", "") < 0)
            toast_show("无法打开系统输入法");
    }
    if (in->pressed & PAD_CTRL_SQUARE)
        toast_show("收藏：在详情页按 □ 加入稍后再看");
    if (in->pressed & PAD_NAV_START)
    {
        g_settings_return = SCREEN_MAIN;
        g_screen = SCREEN_SETTINGS;
        g_selected = 0;
    }

    if (in->cancel || (in->pressed & sys_cancel_button()))
    {
        if (g_tab == TAB_REGION && g_region_selected >= 0)
        {
            g_region_selected = -1;
            g_count = 0;
            g_error[0] = 0;
            g_loading = 0;
        }
        else
        {
            log_printf("[app] exit: cancel on tab %d (screen %d)", (int)g_tab,
                       (int)g_screen);
            g_running = 0;
        }
    }
}

static void handle_mine_input(const Input *in)
{
    if (in->pressed & PAD_CTRL_L1)
        tab_switch(-1);
    if (in->pressed & PAD_CTRL_R1)
        tab_switch(1);

    if (!g_logged_in)
    {
        if (in->pressed & sys_ok_button())
        {
            g_screen = SCREEN_LOGIN;
            start_qr();
        }
        if (in->cancel || (in->pressed & sys_cancel_button()))
            g_screen = SCREEN_MAIN;
        return;
    }

    if (in->active & PAD_NAV_UP)
    {
        if (g_selected > 0)
            g_selected--;
    }
    if (in->active & PAD_NAV_DOWN)
    {
        if (g_selected < 3)
            g_selected++;
    }

    if (in->pressed & PAD_CTRL_TRIANGLE)
        start_nav();

    if (in->pressed & sys_ok_button())
    {
        if (g_selected == 0)
            start_mine_list(LIST_HISTORY);
        else if (g_selected == 1)
            start_mine_list(LIST_FAV);
        else if (g_selected == 2)
            start_mine_list(LIST_TOVIEW);
        else
        {
            g_settings_return = SCREEN_MINE;
            g_screen = SCREEN_SETTINGS;
            g_selected = 0;
        }
    }

    if (in->cancel || (in->pressed & sys_cancel_button()))
        g_screen = SCREEN_MAIN;
}

static void handle_settings_input(const Input *in)
{
    if (in->pressed & sys_ok_button())
    {
        settings_set_sw_decode(!settings_sw_decode());
        toast_show(settings_sw_decode() ? "视频解码：软件" : "视频解码：硬件");
    }

    if (in->cancel || (in->pressed & sys_cancel_button()))
    {
        g_screen = g_settings_return;
        if (g_screen == SCREEN_MINE)
            g_selected = 3;
    }
}

static void handle_login_input(const Input *in)
{
    if (in->pressed & sys_ok_button())
    {
        if (g_qr_state == 86038 || g_qr_state < 0 || !g_qr_url[0])
        {
            start_qr();
        }
    }

    if (in->cancel || (in->pressed & sys_cancel_button()))
        g_screen = SCREEN_MAIN;
}

static void handle_list_input(const Input *in)
{
    if (in->pressed & PAD_CTRL_L1)
    {
        g_screen = SCREEN_MAIN;
        tab_switch(-1);
    }
    if (in->pressed & PAD_CTRL_R1)
    {
        g_screen = SCREEN_MAIN;
        tab_switch(1);
    }

    if (in->active & PAD_NAV_UP)
    {
        if (g_selected > 0)
            g_selected--;
    }
    if (in->active & PAD_NAV_DOWN)
    {
        if (g_selected + 1 < g_count)
            g_selected++;
    }

    if (in->pressed & sys_ok_button())
    {
        if (g_count > 0)
        {
            if (api_start_view(g_items[g_selected].bvid) == 0)
            {
                g_detail_return = SCREEN_LIST;
                g_pending = PEND_DETAIL;
                g_loading = 1;
                g_error[0] = 0;
            }
        }
        else if (g_error[0])
        {
            if (g_list_kind == LIST_SEARCH)
                start_search(g_search_keyword);
            else
                start_mine_list(g_list_kind);
        }
    }

    if (in->cancel || (in->pressed & sys_cancel_button()))
    {
        if (g_list_kind == LIST_FAV)
            g_screen = SCREEN_FOLDERS;
        else if (g_list_kind == LIST_SEARCH)
            g_screen = SCREEN_MAIN;
        else
            g_screen = SCREEN_MINE;
        g_loading = 0;
        g_error[0] = 0;
        g_pending = PEND_NONE;
    }
}

static void handle_folders_input(const Input *in)
{
    if (in->pressed & PAD_CTRL_L1)
    {
        g_screen = SCREEN_MAIN;
        tab_switch(-1);
    }
    if (in->pressed & PAD_CTRL_R1)
    {
        g_screen = SCREEN_MAIN;
        tab_switch(1);
    }

    if (in->active & PAD_NAV_UP)
    {
        if (g_folder_selected > 0)
            g_folder_selected--;
    }
    if (in->active & PAD_NAV_DOWN)
    {
        if (g_folder_selected + 1 < g_folder_count)
            g_folder_selected++;
    }

    if ((in->pressed & sys_ok_button()) && g_folder_count > 0)
    {
        FavFolder *f = &g_folders[g_folder_selected];
        if (api_start_fav_list(f->id) == 0)
        {
            g_list_kind = LIST_FAV;
            snprintf(g_list_title, sizeof(g_list_title), "收藏夹 / %s", f->title);
            g_pending = PEND_FAV_LIST;
            g_loading = 1;
            g_error[0] = 0;
            g_selected = 0;
            g_first = 0;
            g_count = 0;
        }
    }

    if (in->cancel || (in->pressed & sys_cancel_button()))
    {
        g_screen = SCREEN_MINE;
        g_loading = 0;
        g_error[0] = 0;
        g_pending = PEND_NONE;
    }
}

static void handle_detail_input(const Input *in)
{
    if (in->pressed & PAD_CTRL_SQUARE)
    {
        if (api_start_toview_add(g_detail.aid) == 0)
            g_pending = PEND_TOVIEW_ADD;
        else
            toast_show("请求繁忙");
    }

    if (in->pressed & PAD_CTRL_TRIANGLE)
    {
        if (api_start_fav_add(g_detail.aid) == 0)
            g_pending = PEND_FAV_ADD;
        else
            toast_show("请求繁忙");
    }

    if (in->pressed & sys_ok_button())
    {
        if (api_start_playurl(g_detail.bvid, g_detail.cid, g_requested_qn) == 0)
        {
            g_resume_time = 0.0;
            g_pending = PEND_PLAY;
            g_loading = 1;
            g_error[0] = 0;
        }
        else
        {
            toast_show("取流请求繁忙");
        }
    }

    if (in->cancel || (in->pressed & sys_cancel_button()))
    {
        g_screen = g_detail_return;
        g_loading = 0;
        g_error[0] = 0;
        g_pending = PEND_NONE;
    }
}

static void tick_login(void)
{
    if (g_screen != SCREEN_LOGIN || !g_qr_key[0])
        return;
    if (g_qr_state != 86101 && g_qr_state != 86090)
        return;
    if (sys_ms() < g_next_qr_poll)
        return;
    if (api_state() != API_IDLE)
        return;

    if (api_start_qr_poll(g_qr_key) == 0)
    {
        g_pending = PEND_QR_POLL;
        g_next_qr_poll = sys_ms() + 2000;
    }
}

int app_run(void)
{
    Input in;

    log_printf("[app] start");
    input_init();
    settings_init();
    memset(&in, 0, sizeof(in));

    gfx_begin();
    gfx_text_center(0, GFX_W, GFX_H / 2 - 30, 34, COL_TEXT, "正在连接网络…");
    gfx_text_center(0, GFX_W, GFX_H / 2 + 20, 22, COL_TEXT_FAINT,
                    "首次启动或网络未连接时需要几秒");
    gfx_end();

    if (api_init() < 0)
    {
        snprintf(g_error, sizeof(g_error), "网络初始化失败，详见 /dev_hdd0/tmp/pilis3.log");
    }
    else
    {
        start_tab(g_tab);
    }

    cover_init();

    while (g_running)
    {
        int osk_block = 0;

        sys_poll();
        if (sys_exit_requested())
        {
            log_printf("[app] exit: system request");
            g_running = 0;
        }

        if (osk_active())
        {
            osk_block = 1;
            if (osk_update())
            {
                char keyword[256];
                osk_take(keyword, sizeof(keyword));
                if (keyword[0])
                    start_search(keyword);
            }
        }

        poll_api();
        tick_login();

        if (!osk_block)
        {
            input_update(&in, sys_ms());

            if (g_screen == SCREEN_MAIN)
                handle_main_input(&in);
            else if (g_screen == SCREEN_DETAIL)
                handle_detail_input(&in);
            else if (g_screen == SCREEN_LOGIN)
                handle_login_input(&in);
            else if (g_screen == SCREEN_LIST)
                handle_list_input(&in);
            else if (g_screen == SCREEN_FOLDERS)
                handle_folders_input(&in);
            else if (g_screen == SCREEN_SETTINGS)
                handle_settings_input(&in);
            else if (g_screen == SCREEN_MINE)
                handle_mine_input(&in);
        }

        if (g_screen == SCREEN_PLAYER)
        {
            int pr = g_player ? player_update(g_player, &in) : 1;

            if (pr == 1)
            {
                if (g_player)
                    player_close(g_player);
                g_player = NULL;
                g_pending = PEND_NONE;
                g_loading = 0;
                g_screen = SCREEN_DETAIL;
                continue;
            }
            else if (pr == 2)
            {
                double target = player_seek_target(g_player);
                player_close(g_player);
                g_player = player_open_at(&g_play, &g_detail, target);
                if (!g_player || player_failed(g_player))
                {
                    if (g_player)
                    {
                        player_close(g_player);
                        g_player = NULL;
                    }
                    snprintf(g_error, sizeof(g_error), "跳转失败");
                    g_screen = SCREEN_DETAIL;
                }
                continue;
            }
            else if (pr == 3)
            {
                int cur = player_quality_qn(g_player);
                int delta = player_quality_delta(g_player);
                int qn;

                if (delta < 0)
                    qn = cur > 64 ? 64 : (cur > 32 ? 32 : cur);
                else
                    qn = cur < 32 ? 32 : (cur < 64 ? 64 : (cur < 80 ? 80 : cur));

                if (qn == cur)
                {
                    player_show_hint(g_player, delta < 0 ? "已是最低画质" : "已是最高画质");
                }
                else if (api_start_playurl(g_detail.bvid, g_detail.cid, qn) == 0)
                {
                    g_resume_time = player_current_time(g_player);
                    g_requested_qn = qn;
                    g_pending = PEND_PLAY;
                    g_loading = 1;
                    g_error[0] = 0;
                }
                else
                {
                    player_show_hint(g_player, "切换请求繁忙");
                }
                continue;
            }

            cover_frame();
            gfx_begin_overlay();
            player_draw(g_player);
            gfx_end();
            sysUsleep(1000);
            continue;
        }

        cover_frame();
        gfx_begin();
        draw_header();
        if (g_screen == SCREEN_LOGIN)
            draw_login();
        else if (g_screen == SCREEN_MINE)
            draw_mine();
        else if (g_screen == SCREEN_DETAIL)
            draw_detail();
        else if (g_screen == SCREEN_FOLDERS)
            draw_folders();
        else if (g_screen == SCREEN_SETTINGS)
            draw_settings();
        else if (g_tab == TAB_REGION && g_region_selected < 0)
            draw_region_grid();
        else
            draw_list();
        draw_footer();
        draw_toast();
        gfx_end();

        sysUsleep(1000);
    }

    if (g_player)
    {
        player_close(g_player);
        g_player = NULL;
    }
    cover_shutdown();
    api_shutdown();
    log_printf("[app] exit");
    return 0;
}
