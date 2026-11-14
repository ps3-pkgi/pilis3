#include "api.h"
#include "http.h"
#include "platform/log.h"
#include "platform/sys.h"

#include <stdio.h>
#include <string.h>

#include <sys/mutex.h>
#include <sys/sem.h>
#include <sys/thread.h>
#include <sys/systime.h>

typedef enum
{
    TASK_NONE = 0,
    TASK_POPULAR,
    TASK_RECOMMEND,
    TASK_REGION,
    TASK_DYNAMIC,
    TASK_SEARCH,
    TASK_VIEW,
    TASK_PLAYURL,
    TASK_NAV,
    TASK_TOVIEW,
    TASK_HISTORY,
    TASK_FAV_FOLDERS,
    TASK_FAV_LIST,
    TASK_TOVIEW_ADD,
    TASK_FAV_ADD,
    TASK_QR_GENERATE,
    TASK_QR_POLL
} TaskType;

typedef struct
{
    TaskType type;
    int page;
    int rid;
    long long media_id;
    long long aid;
    char keyword[128];
    char bvid[16];
    long long cid;
    int qn;
    char qr_key[128];
} Task;

static sys_mutex_t g_lock;
static sys_sem_t g_sem;
static sys_ppu_thread_t g_thread;
static int g_thread_started;
static volatile int g_quit;

static Task g_task;
static ApiState g_state = API_IDLE;
static char g_error[256];

static VideoItem g_items[BILI_MAX_ITEMS];
static int g_item_count;
static VideoItem g_view;
static PlayUrl g_play;
static BiliUser g_user;
static FavFolder g_folders[20];
static int g_folder_count;
static char g_qr_url[512];
static char g_qr_key[128];
static int g_qr_state = -1;

static void lock(void)
{
    sysMutexLock(g_lock, 0);
}

static void unlock(void)
{
    sysMutexUnlock(g_lock);
}

static void thread_main(void *arg)
{
    (void)arg;

    for (;;)
    {
        Task task;
        int rc = -1;
        VideoItem items[BILI_MAX_ITEMS];
        int count = 0;
        VideoItem view;
        PlayUrl play;
        BiliUser user;
        FavFolder folders[20];
        int folder_count = 0;
        char err[256] = {0};
        char qr_url[512] = {0};
        char qr_key[128] = {0};
        int qr_state = -1;

        sysSemWait(g_sem, 0);
        if (g_quit)
            break;

        lock();
        task = g_task;
        unlock();

        log_printf("[api] task %d start", (int)task.type);

        switch (task.type)
        {
        case TASK_POPULAR:
            rc = bili_popular(task.page, items, BILI_MAX_ITEMS, &count, err,
                              sizeof(err));
            break;
        case TASK_RECOMMEND:
            rc = bili_recommend(task.page, items, BILI_MAX_ITEMS, &count, err,
                                sizeof(err));
            break;
        case TASK_REGION:
            rc = bili_region(task.rid, task.page, items, BILI_MAX_ITEMS, &count, err,
                             sizeof(err));
            break;
        case TASK_DYNAMIC:
            rc = bili_dynamic(task.page, items, BILI_MAX_ITEMS, &count, err,
                              sizeof(err));
            break;
        case TASK_SEARCH:
            rc = bili_search(task.keyword, task.page, items, BILI_MAX_ITEMS, &count,
                             err, sizeof(err));
            break;
        case TASK_VIEW:
            rc = bili_view(task.bvid, &view, err, sizeof(err));
            break;
        case TASK_PLAYURL:
            rc = bili_playurl(task.bvid, task.cid, task.qn, &play, err,
                              sizeof(err));
            break;
        case TASK_NAV:
            rc = bili_nav(&user, err, sizeof(err));
            break;
        case TASK_TOVIEW:
            rc = bili_toview(items, BILI_MAX_ITEMS, &count, err, sizeof(err));
            break;
        case TASK_HISTORY:
            rc = bili_history(items, BILI_MAX_ITEMS, &count, err, sizeof(err));
            break;
        case TASK_FAV_FOLDERS:
            rc = bili_fav_folders(folders, 20, &folder_count, err, sizeof(err));
            break;
        case TASK_FAV_LIST:
            rc = bili_fav_list(task.media_id, items, BILI_MAX_ITEMS, &count, err,
                               sizeof(err));
            break;
        case TASK_TOVIEW_ADD:
            rc = bili_toview_add(task.aid, err, sizeof(err));
            break;
        case TASK_FAV_ADD:
            rc = bili_fav_add(task.aid, err, sizeof(err));
            break;
        case TASK_QR_GENERATE:
            rc = bili_qr_generate(qr_url, sizeof(qr_url), qr_key, sizeof(qr_key),
                                  err, sizeof(err));
            break;
        case TASK_QR_POLL:
            rc = bili_qr_poll(task.qr_key, &qr_state, err, sizeof(err));
            break;
        default:
            break;
        }

        log_printf("[api] task %d done rc=%d", (int)task.type, rc);

        lock();
        if (rc == 0)
        {
            switch (task.type)
            {
            case TASK_POPULAR:
            case TASK_RECOMMEND:
            case TASK_REGION:
            case TASK_DYNAMIC:
            case TASK_SEARCH:
            case TASK_TOVIEW:
            case TASK_HISTORY:
            case TASK_FAV_LIST:
                memcpy(g_items, items, sizeof(VideoItem) * count);
                g_item_count = count;
                break;
            case TASK_FAV_FOLDERS:
                memcpy(g_folders, folders, sizeof(FavFolder) * folder_count);
                g_folder_count = folder_count;
                break;
            case TASK_VIEW:
                g_view = view;
                break;
            case TASK_PLAYURL:
                g_play = play;
                break;
            case TASK_NAV:
                g_user = user;
                break;
            case TASK_QR_GENERATE:
                snprintf(g_qr_url, sizeof(g_qr_url), "%s", qr_url);
                snprintf(g_qr_key, sizeof(g_qr_key), "%s", qr_key);
                break;
            case TASK_QR_POLL:
                g_qr_state = qr_state;
                break;
            default:
                break;
            }
            g_state = API_READY;
        }
        else
        {
            snprintf(g_error, sizeof(g_error), "%s", err[0] ? err : "unknown");
            g_state = API_ERROR;
            log_printf("[api] task %d failed: %s", (int)task.type, g_error);
        }
        unlock();
    }

    sysThreadExit(0);
}

int api_init(void)
{
    sys_mutex_attr_t mattr;
    int rc;

    memset(&mattr, 0, sizeof(mattr));
    mattr.attr_protocol = SYS_MUTEX_PROTOCOL_FIFO;
    mattr.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
    mattr.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED;
    mattr.attr_adaptive = SYS_MUTEX_ATTR_ADAPTIVE;
    snprintf(mattr.name, sizeof(mattr.name), "api");

    rc = sysMutexCreate(&g_lock, &mattr);
    if (rc < 0)
    {
        log_printf("[api] mutex create failed %x", (u32)rc);
        return -1;
    }

    rc = sys_sem_create(&g_sem, "p3api", 0, 1);
    if (rc < 0)
    {
        log_printf("[api] sem create failed %x", (u32)rc);
        return -1;
    }

    if (http_init() < 0)
    {
        /* Keep the UI alive: every request will fail with "http not
         * initialized" and the caller can retry, which beats a dead boot. */
        log_printf("[api] http init failed, continuing with network disabled");
    }

    /* 512 KB, not 128 KB: thread_main alone holds VideoItem items[40] (~80 KB,
     * ~2 KB each) plus PlayUrl/view/folders, and the work calls bili_*() which
     * adds url[4600] + signed_query[4096] and then curl + the TLS handshake on
     * the same stack. At 128 KB the frame overflowed and the worker wedged
     * inside the first https request. */
    rc = sysThreadCreate(&g_thread, thread_main, NULL, 1000, 0x80000, 0, "pilis3api");
    if (rc < 0)
    {
        log_printf("[api] thread create failed %x", (u32)rc);
        return -1;
    }

    g_thread_started = 1;
    log_printf("[api] worker started");
    return 0;
}

void api_shutdown(void)
{
    if (!g_thread_started)
        return;

    g_quit = 1;
    sysSemPost(g_sem, 1);
    sysThreadJoin(g_thread, NULL);
    g_thread_started = 0;

    sysSemDestroy(g_sem);
    sysMutexDestroy(g_lock);
    http_shutdown();
}

static int start_task(const Task *task)
{
    lock();
    if (g_state == API_LOADING)
    {
        unlock();
        return -1;
    }
    g_task = *task;
    g_state = API_LOADING;
    g_error[0] = 0;
    unlock();

    sysSemPost(g_sem, 1);
    return 0;
}

int api_start_popular(int page)
{
    Task t;
    memset(&t, 0, sizeof(t));
    t.type = TASK_POPULAR;
    t.page = page;
    return start_task(&t);
}

int api_start_recommend(int page)
{
    Task t;
    memset(&t, 0, sizeof(t));
    t.type = TASK_RECOMMEND;
    t.page = page;
    return start_task(&t);
}

int api_start_region(int rid, int page)
{
    Task t;
    memset(&t, 0, sizeof(t));
    t.type = TASK_REGION;
    t.rid = rid;
    t.page = page;
    return start_task(&t);
}

int api_start_dynamic(int page)
{
    Task t;
    memset(&t, 0, sizeof(t));
    t.type = TASK_DYNAMIC;
    t.page = page;
    return start_task(&t);
}

int api_start_search(const char *keyword, int page)
{
    Task t;
    memset(&t, 0, sizeof(t));
    t.type = TASK_SEARCH;
    t.page = page;
    snprintf(t.keyword, sizeof(t.keyword), "%s", keyword ? keyword : "");
    return start_task(&t);
}

int api_start_view(const char *bvid)
{
    Task t;
    memset(&t, 0, sizeof(t));
    t.type = TASK_VIEW;
    snprintf(t.bvid, sizeof(t.bvid), "%s", bvid);
    return start_task(&t);
}

int api_start_playurl(const char *bvid, long long cid, int qn)
{
    Task t;
    memset(&t, 0, sizeof(t));
    t.type = TASK_PLAYURL;
    snprintf(t.bvid, sizeof(t.bvid), "%s", bvid);
    t.cid = cid;
    t.qn = qn;
    return start_task(&t);
}

int api_start_nav(void)
{
    Task t;
    memset(&t, 0, sizeof(t));
    t.type = TASK_NAV;
    return start_task(&t);
}

int api_start_toview(void)
{
    Task t;
    memset(&t, 0, sizeof(t));
    t.type = TASK_TOVIEW;
    return start_task(&t);
}

int api_start_history(void)
{
    Task t;
    memset(&t, 0, sizeof(t));
    t.type = TASK_HISTORY;
    return start_task(&t);
}

int api_start_fav_folders(void)
{
    Task t;
    memset(&t, 0, sizeof(t));
    t.type = TASK_FAV_FOLDERS;
    return start_task(&t);
}

int api_start_fav_list(long long media_id)
{
    Task t;
    memset(&t, 0, sizeof(t));
    t.type = TASK_FAV_LIST;
    t.media_id = media_id;
    return start_task(&t);
}

int api_start_toview_add(long long aid)
{
    Task t;
    memset(&t, 0, sizeof(t));
    t.type = TASK_TOVIEW_ADD;
    t.aid = aid;
    return start_task(&t);
}

int api_start_fav_add(long long aid)
{
    Task t;
    memset(&t, 0, sizeof(t));
    t.type = TASK_FAV_ADD;
    t.aid = aid;
    return start_task(&t);
}

int api_start_qr_generate(void)
{
    Task t;
    memset(&t, 0, sizeof(t));
    t.type = TASK_QR_GENERATE;
    return start_task(&t);
}

int api_start_qr_poll(const char *key)
{
    Task t;
    memset(&t, 0, sizeof(t));
    t.type = TASK_QR_POLL;
    snprintf(t.qr_key, sizeof(t.qr_key), "%s", key);
    return start_task(&t);
}

ApiState api_state(void)
{
    ApiState s;
    lock();
    s = g_state;
    unlock();
    return s;
}

const char *api_error_text(void)
{
    return g_error;
}

void api_ack(void)
{
    lock();
    if (g_state != API_LOADING)
        g_state = API_IDLE;
    unlock();
}

VideoItem *api_items(int *count)
{
    *count = g_item_count;
    return g_items;
}

VideoItem *api_view(void)
{
    return &g_view;
}

PlayUrl *api_playurl(void)
{
    return &g_play;
}

BiliUser *api_user(void)
{
    return &g_user;
}

FavFolder *api_folders(int *count)
{
    *count = g_folder_count;
    return g_folders;
}

const char *api_qr_url(void)
{
    return g_qr_url;
}

const char *api_qr_key(void)
{
    return g_qr_key;
}

int api_qr_state(void)
{
    return g_qr_state;
}
