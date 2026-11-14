#include "bili.h"
#include "http.h"
#include "md5.h"
#include "platform/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <json-c/json.h>

/* ------------------------------------------------------------------------- */
/* JSON helpers                                                              */
/* ------------------------------------------------------------------------- */

static json_object *jobj(json_object *o, const char *key)
{
    json_object *v = NULL;
    if (!o)
        return NULL;
    json_object_object_get_ex(o, key, &v);
    return v;
}

static const char *jstr(json_object *o, const char *key)
{
    json_object *v = jobj(o, key);
    return v ? json_object_get_string(v) : NULL;
}

static long long jint64(json_object *o, const char *key)
{
    json_object *v = jobj(o, key);
    return v ? json_object_get_int64(v) : 0;
}

static int jint(json_object *o, const char *key)
{
    return (int)jint64(o, key);
}

static void copy_str(char *dst, int size, const char *src)
{
    if (!dst || size <= 0)
        return;
    snprintf(dst, size, "%s", src ? src : "");
}

static int json_error(json_object *root, char *err, int err_size)
{
    int code = jint(root, "code");
    if (code == 0)
        return 0;

    snprintf(err, err_size, "B站错误 %d: %s", code,
             jstr(root, "message") ? jstr(root, "message") : "unknown");
    return -1;
}

static void make_cover_url(const char *src, char *out, int size)
{
    char buf[1024];
    char *cut;

    out[0] = 0;
    if (!src || !*src)
        return;

    if (!strncmp(src, "//", 2))
        snprintf(buf, sizeof(buf), "https:%s", src);
    else if (!strncmp(src, "http://", 7))
        snprintf(buf, sizeof(buf), "https://%s", src + 7);
    else
        snprintf(buf, sizeof(buf), "%s", src);

    cut = strchr(buf, '@');
    if (cut)
        *cut = 0;
    cut = strchr(buf, '?');
    if (cut)
        *cut = 0;

    snprintf(out, size, "%s@240w_135h_1c.jpg", buf);
}

/* ------------------------------------------------------------------------- */
/* Item parsing                                                              */
/* ------------------------------------------------------------------------- */

static int parse_item(json_object *o, VideoItem *it)
{
    json_object *owner, *stat, *dim;
    const char *bvid;

    if (!o)
        return -1;

    memset(it, 0, sizeof(*it));

    bvid = jstr(o, "bvid");
    if (!bvid || !*bvid)
        bvid = jstr(o, "id");
    if (!bvid || !*bvid)
        return -1;

    copy_str(it->bvid, sizeof(it->bvid), bvid);
    copy_str(it->title, sizeof(it->title), jstr(o, "title"));
    copy_str(it->desc, sizeof(it->desc), jstr(o, "desc"));
    {
        const char *pic = jstr(o, "pic");
        if (!pic || !*pic)
            pic = jstr(o, "cover");
        make_cover_url(pic, it->cover, sizeof(it->cover));
    }

    it->aid = jint64(o, "aid");
    it->cid = jint64(o, "cid");
    it->duration = jint(o, "duration");
    it->page_count = 1;

    owner = jobj(o, "owner");
    if (owner)
        copy_str(it->up, sizeof(it->up), jstr(owner, "name"));

    stat = jobj(o, "stat");
    if (stat)
    {
        it->view = jint64(stat, "view");
        it->danmaku = jint64(stat, "danmaku");
    }

    dim = jobj(o, "dimension");
    if (dim)
    {
        it->width = jint(dim, "width");
        it->height = jint(dim, "height");
    }

    return 0;
}

/* ------------------------------------------------------------------------- */
/* WBI signing                                                               */
/* ------------------------------------------------------------------------- */

typedef struct
{
    char key[64];
    char value[512];
} WbiParam;

static char g_wbi_mixin[33];
static time_t g_wbi_time;

static const int k_mixin_table[64] = {
    46, 47, 18, 2,  53, 8,  23, 32, 15, 50, 10, 31, 58, 3,  45, 35,
    27, 43, 5,  49, 33, 9,  42, 19, 29, 28, 14, 39, 12, 38, 41, 13,
    37, 48, 7,  16, 24, 55, 40, 61, 26, 17, 0,  1,  60, 51, 30, 4,
    22, 25, 54, 21, 56, 59, 6,  63, 57, 62, 11, 36, 20, 34, 44, 52,
};

static void extract_wbi_key(const char *url, char *out, int size)
{
    const char *slash;
    const char *dot;

    out[0] = 0;
    if (!url)
        return;

    slash = strrchr(url, '/');
    if (!slash)
        slash = url;
    else
        slash++;

    dot = strrchr(slash, '.');
    if (!dot)
        dot = slash + strlen(slash);

    int n = (int)(dot - slash);
    if (n <= 0 || n >= size)
        return;
    memcpy(out, slash, n);
    out[n] = 0;
}

static void make_mixin_key(const char *img, const char *sub)
{
    char raw[128];
    int i;

    snprintf(raw, sizeof(raw), "%s%s", img, sub);
    if (strlen(raw) < 64)
    {
        g_wbi_mixin[0] = 0;
        return;
    }

    for (i = 0; i < 32; ++i)
        g_wbi_mixin[i] = raw[k_mixin_table[i]];
    g_wbi_mixin[32] = 0;
}

static int wbi_init(void)
{
    HttpResponse res;
    json_object *root;
    json_object *data, *img;
    char img_key[64], sub_key[64];

    if (g_wbi_mixin[0] && time(NULL) - g_wbi_time < 3600)
        return 0;

    if (http_get("https://api.bilibili.com/x/web-interface/nav", &res, 15) < 0)
        return -1;

    root = json_tokener_parse(res.body ? res.body : "");
    http_response_free(&res);
    if (!root)
        return -1;

    /* Do not gate on code == 0: nav answers -101 ("账号未登录") on a logged-out
     * console, but data.wbi_img is still there and recommend/search work
     * without a session. Only the keys matter here. */
    data = jobj(root, "data");
    img = jobj(data, "wbi_img");
    extract_wbi_key(jstr(img, "img_url"), img_key, sizeof(img_key));
    extract_wbi_key(jstr(img, "sub_url"), sub_key, sizeof(sub_key));
    make_mixin_key(img_key, sub_key);

    json_object_put(root);

    if (!g_wbi_mixin[0])
    {
        log_printf("[bili] wbi mixin key empty");
        return -1;
    }

    g_wbi_time = time(NULL);
    log_printf("[bili] wbi ready (img=%s sub=%s)", img_key, sub_key);
    return 0;
}

static int wbi_param_cmp(const void *a, const void *b)
{
    return strcmp(((const WbiParam *)a)->key, ((const WbiParam *)b)->key);
}

static void filter_wbi_value(const char *in, char *out, int size)
{
    int n = 0;

    while (*in && n < size - 1)
    {
        if (*in != '!' && *in != '\'' && *in != '(' && *in != ')' && *in != '*')
            out[n++] = *in;
        in++;
    }
    out[n] = 0;
}

static int wbi_sign(WbiParam *params, int count, char *out, int size)
{
    char query[3072];
    char filtered[1024];
    char sign_src[4096];
    char hash[33];
    int n = 0;

    if (!g_wbi_mixin[0] && wbi_init() < 0)
        return -1;

    snprintf(params[count].key, sizeof(params[count].key), "wts");
    snprintf(params[count].value, sizeof(params[count].value), "%ld",
             (long)time(NULL));
    count++;

    qsort(params, count, sizeof(params[0]), wbi_param_cmp);

    query[0] = 0;
    for (int i = 0; i < count; ++i)
    {
        char ek[128];
        char ev[1536];

        http_url_encode(params[i].key, ek, sizeof(ek));
        filter_wbi_value(params[i].value, filtered, sizeof(filtered));
        http_url_encode(filtered, ev, sizeof(ev));

        int add = snprintf(query + n, sizeof(query) - (size_t)n, "%s=%s&", ek, ev);
        if (add < 0 || n + add >= (int)sizeof(query))
            return -1;
        n += add;
    }
    if (n > 0)
        query[n - 1] = 0;

    snprintf(sign_src, sizeof(sign_src), "%s%s", query, g_wbi_mixin);
    md5_hex(sign_src, (u32)strlen(sign_src), hash);

    if (snprintf(out, size, "%s&w_rid=%s", query, hash) >= size)
        return -1;

    return 0;
}

static void add_param(WbiParam *p, int *n, const char *key, const char *value)
{
    snprintf(p[*n].key, sizeof(p[*n].key), "%s", key);
    snprintf(p[*n].value, sizeof(p[*n].value), "%s", value);
    (*n)++;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

int bili_init(void)
{
    return wbi_init();
}

int bili_popular(int page, VideoItem *items, int max, int *count, char *err,
                 int err_size)
{
    char url[256];
    HttpResponse res;
    json_object *root, *data, *list;
    int total, n = 0;

    *count = 0;
    if (max > 20)
        max = 20;

    snprintf(url, sizeof(url),
             "https://api.bilibili.com/x/web-interface/popular?ps=%d&pn=%d", max,
             page);

    if (http_get(url, &res, 20) < 0 || !res.body)
    {
        snprintf(err, err_size, "网络错误: %s", res.error);
        http_response_free(&res);
        return -1;
    }

    root = json_tokener_parse(res.body);
    http_response_free(&res);
    if (!root)
    {
        snprintf(err, err_size, "热门 JSON 解析失败");
        return -1;
    }

    if (json_error(root, err, err_size) < 0)
    {
        json_object_put(root);
        return -1;
    }

    data = jobj(root, "data");
    list = jobj(data, "list");
    total = list ? json_object_array_length(list) : 0;

    for (int i = 0; i < total && n < max; ++i)
    {
        json_object *o = json_object_array_get_idx(list, i);
        if (parse_item(o, &items[n]) == 0)
            n++;
    }

    json_object_put(root);

    *count = n;
    if (n == 0)
    {
        snprintf(err, err_size, "热门列表为空");
        return -1;
    }

    log_printf("[bili] popular page %d: %d items", page, n);
    return 0;
}

int bili_recommend(int page, VideoItem *items, int max, int *count, char *err,
                   int err_size)
{
    WbiParam params[16];
    char signed_query[4096];
    char url[4600];
    HttpResponse res;
    json_object *root, *data, *list;
    char page_s[16];
    int pn = 0, total, n = 0;

    *count = 0;
    if (max > 20)
        max = 20;

    if (wbi_init() < 0)
    {
        snprintf(err, err_size, "wbi 初始化失败");
        return -1;
    }

    snprintf(page_s, sizeof(page_s), "%d", page);
    add_param(params, &pn, "ps", "20");
    add_param(params, &pn, "fresh_type", "3");
    add_param(params, &pn, "feed_version", "V1");
    add_param(params, &pn, "fresh_idx", page_s);
    add_param(params, &pn, "fresh_idx_1h", page_s);
    add_param(params, &pn, "homepage_ver", "1");
    add_param(params, &pn, "brush", "0");
    add_param(params, &pn, "web_location", "1430650");

    if (wbi_sign(params, pn, signed_query, sizeof(signed_query)) < 0)
    {
        snprintf(err, err_size, "wbi 签名失败");
        return -1;
    }

    snprintf(url, sizeof(url),
             "https://api.bilibili.com/x/web-interface/wbi/index/top/feed/rcmd?%s",
             signed_query);

    if (http_get(url, &res, 20) < 0 || !res.body)
    {
        snprintf(err, err_size, "网络错误: %s", res.error);
        http_response_free(&res);
        return -1;
    }

    root = json_tokener_parse(res.body);
    http_response_free(&res);
    if (!root)
    {
        snprintf(err, err_size, "推荐 JSON 解析失败");
        return -1;
    }

    if (json_error(root, err, err_size) < 0)
    {
        json_object_put(root);
        return -1;
    }

    data = jobj(root, "data");
    list = jobj(data, "item");
    total = list ? json_object_array_length(list) : 0;

    for (int i = 0; i < total && n < max; ++i)
    {
        json_object *o = json_object_array_get_idx(list, i);
        if (parse_item(o, &items[n]) == 0)
            n++;
    }

    json_object_put(root);

    *count = n;
    if (n == 0)
    {
        snprintf(err, err_size, "推荐列表为空");
        return -1;
    }

    log_printf("[bili] recommend page %d: %d items", page, n);
    return 0;
}

static int parse_duration_text(const char *s)
{
    int a = 0, b = 0, c = 0;

    if (!s || !*s)
        return 0;

    if (sscanf(s, "%d:%d:%d", &a, &b, &c) == 3)
        return a * 3600 + b * 60 + c;
    if (sscanf(s, "%d:%d", &a, &b) == 2)
        return a * 60 + b;
    return atoi(s);
}

static long long parse_count_text(const char *s)
{
    double v;

    if (!s || !*s)
        return 0;

    if (sscanf(s, "%lf", &v) == 1)
    {
        if (strstr(s, "\xe4\xb8\x87")) /* 万 */
            return (long long)(v * 10000.0);
        if (strstr(s, "\xe4\xba\xbf")) /* 亿 */
            return (long long)(v * 100000000.0);
    }

    return (long long)atoll(s);
}

static void html_unescape(const char *src, char *dst, int size)
{
    int n = 0;

    while (*src && n < size - 1)
    {
        if (!strncmp(src, "&amp;", 5))
        {
            dst[n++] = '&';
            src += 5;
        }
        else if (!strncmp(src, "&lt;", 4))
        {
            dst[n++] = '<';
            src += 4;
        }
        else if (!strncmp(src, "&gt;", 4))
        {
            dst[n++] = '>';
            src += 4;
        }
        else if (!strncmp(src, "&quot;", 6))
        {
            dst[n++] = '"';
            src += 6;
        }
        else if (!strncmp(src, "&#39;", 5))
        {
            dst[n++] = '\'';
            src += 5;
        }
        else
        {
            dst[n++] = *src++;
        }
    }
    dst[n] = 0;
}

static void strip_html(const char *src, char *dst, int size)
{
    char tmp[512];
    int n = 0;
    int tag = 0;

    while (*src && n < (int)sizeof(tmp) - 1)
    {
        if (*src == '<')
        {
            tag = 1;
            src++;
            continue;
        }
        if (tag && *src == '>')
        {
            tag = 0;
            src++;
            continue;
        }
        if (!tag)
            tmp[n++] = *src;
        src++;
    }
    tmp[n] = 0;
    html_unescape(tmp, dst, size);
}

static int parse_search_item(json_object *o, VideoItem *it)
{
    const char *bvid;
    char title[512];

    memset(it, 0, sizeof(*it));
    bvid = jstr(o, "bvid");
    if (!bvid || !*bvid)
        return -1;

    copy_str(it->bvid, sizeof(it->bvid), bvid);
    {
        const char *raw_title = jstr(o, "title");
        strip_html(raw_title ? raw_title : "", title, sizeof(title));
    }
    snprintf(it->title, sizeof(it->title), "%s", title);
    copy_str(it->up, sizeof(it->up), jstr(o, "author"));
    copy_str(it->desc, sizeof(it->desc), jstr(o, "description"));
    make_cover_url(jstr(o, "pic"), it->cover, sizeof(it->cover));

    it->aid = jint64(o, "aid");
    it->view = jint64(o, "play");
    it->danmaku = jint64(o, "video_review");
    it->duration = parse_duration_text(jstr(o, "duration"));
    it->page_count = 1;
    return 0;
}

int bili_search(const char *keyword, int page, VideoItem *items, int max,
                int *count, char *err, int err_size)
{
    WbiParam params[12];
    char signed_query[4096];
    char url[4600];
    char page_s[16];
    HttpResponse res;
    json_object *root, *data, *list;
    int pn = 0, total, n = 0;

    *count = 0;
    if (!keyword || !*keyword)
    {
        snprintf(err, err_size, "搜索词为空");
        return -1;
    }
    if (max > 20)
        max = 20;
    if (wbi_init() < 0)
    {
        snprintf(err, err_size, "wbi 初始化失败");
        return -1;
    }

    snprintf(page_s, sizeof(page_s), "%d", page);
    add_param(params, &pn, "search_type", "video");
    add_param(params, &pn, "keyword", keyword);
    add_param(params, &pn, "page", page_s);
    add_param(params, &pn, "page_size", "20");

    if (wbi_sign(params, pn, signed_query, sizeof(signed_query)) < 0)
    {
        snprintf(err, err_size, "wbi 签名失败");
        return -1;
    }

    snprintf(url, sizeof(url),
             "https://api.bilibili.com/x/web-interface/wbi/search/type?%s",
             signed_query);

    if (http_get(url, &res, 20) < 0 || !res.body)
    {
        snprintf(err, err_size, "网络错误: %s", res.error);
        http_response_free(&res);
        return -1;
    }

    root = json_tokener_parse(res.body);
    http_response_free(&res);
    if (!root)
    {
        snprintf(err, err_size, "搜索 JSON 解析失败");
        return -1;
    }
    if (json_error(root, err, err_size) < 0)
    {
        json_object_put(root);
        return -1;
    }

    data = jobj(root, "data");
    list = jobj(data, "result");
    total = list ? json_object_array_length(list) : 0;
    for (int i = 0; i < total && n < max; ++i)
    {
        if (parse_search_item(json_object_array_get_idx(list, i), &items[n]) == 0)
            n++;
    }

    json_object_put(root);
    *count = n;
    if (!n)
    {
        snprintf(err, err_size, "没有搜索结果");
        return -1;
    }
    log_printf("[bili] search '%s' page %d: %d", keyword, page, n);
    return 0;
}

int bili_region(int rid, int page, VideoItem *items, int max, int *count,
                char *err, int err_size)
{
    char url[256];
    HttpResponse res;
    json_object *root, *data, *list;
    int total, n = 0;

    *count = 0;
    if (max > 20)
        max = 20;

    snprintf(url, sizeof(url),
             "https://api.bilibili.com/x/web-interface/newlist?rid=%d&type=0&pn=%d&ps=%d",
             rid, page, max);

    if (http_get(url, &res, 20) < 0 || !res.body)
    {
        snprintf(err, err_size, "网络错误: %s", res.error);
        http_response_free(&res);
        return -1;
    }

    root = json_tokener_parse(res.body);
    http_response_free(&res);
    if (!root)
    {
        snprintf(err, err_size, "分区 JSON 解析失败");
        return -1;
    }

    if (json_error(root, err, err_size) < 0)
    {
        json_object_put(root);
        return -1;
    }

    data = jobj(root, "data");
    list = jobj(data, "archives");
    total = list ? json_object_array_length(list) : 0;

    for (int i = 0; i < total && n < max; ++i)
        if (parse_item(json_object_array_get_idx(list, i), &items[n]) == 0)
            n++;

    json_object_put(root);

    *count = n;
    if (n == 0)
    {
        snprintf(err, err_size, "该分区没有内容");
        return -1;
    }

    log_printf("[bili] region %d page %d: %d items", rid, page, n);
    return 0;
}

int bili_dynamic(int page, VideoItem *items, int max, int *count, char *err,
                 int err_size)
{
    char url[512];
    HttpResponse res;
    json_object *root, *data, *list;
    int total, n = 0;

    *count = 0;
    if (max > 20)
        max = 20;

    snprintf(url, sizeof(url),
             "https://api.bilibili.com/x/polymer/web-dynamic/v1/feed/all"
             "?type=video&page=%d&features=itemOpusStyle",
             page);

    if (http_get(url, &res, 20) < 0 || !res.body)
    {
        snprintf(err, err_size, "网络错误: %s", res.error);
        http_response_free(&res);
        return -1;
    }

    root = json_tokener_parse(res.body);
    http_response_free(&res);
    if (!root)
    {
        snprintf(err, err_size, "动态 JSON 解析失败");
        return -1;
    }

    if (json_error(root, err, err_size) < 0)
    {
        json_object_put(root);
        return -1;
    }

    data = jobj(root, "data");
    list = jobj(data, "items");
    total = list ? json_object_array_length(list) : 0;

    for (int i = 0; i < total && n < max; ++i)
    {
        json_object *item = json_object_array_get_idx(list, i);
        json_object *modules = jobj(item, "modules");
        json_object *author = jobj(modules, "module_author");
        json_object *dyn = jobj(modules, "module_dynamic");
        json_object *major = jobj(dyn, "major");
        json_object *arc = jobj(major, "archive");
        json_object *stat;
        VideoItem *it = &items[n];
        const char *cover;

        if (!arc)
            continue;

        memset(it, 0, sizeof(*it));
        copy_str(it->bvid, sizeof(it->bvid), jstr(arc, "bvid"));
        if (!it->bvid[0])
            continue;

        copy_str(it->title, sizeof(it->title), jstr(arc, "title"));
        copy_str(it->desc, sizeof(it->desc), jstr(arc, "desc"));
        copy_str(it->up, sizeof(it->up), jstr(author, "name"));

        cover = jstr(arc, "cover");
        if (!cover || !*cover)
            cover = jstr(arc, "pic");
        make_cover_url(cover, it->cover, sizeof(it->cover));

        it->duration = parse_duration_text(jstr(arc, "duration_text"));
        it->page_count = 1;

        stat = jobj(arc, "stat");
        if (stat)
        {
            it->view = parse_count_text(jstr(stat, "play"));
            it->danmaku = parse_count_text(jstr(stat, "danmaku"));
        }

        n++;
    }

    json_object_put(root);

    *count = n;
    if (n == 0)
    {
        snprintf(err, err_size, "关注动态里没有视频");
        return -1;
    }

    log_printf("[bili] dynamic page %d: %d items", page, n);
    return 0;
}

int bili_toview(VideoItem *items, int max, int *count, char *err, int err_size)
{
    HttpResponse res;
    json_object *root, *data, *list;
    int total, n = 0;

    *count = 0;
    if (max > 20)
        max = 20;

    if (http_get("https://api.bilibili.com/x/v2/history/toview?ps=20&pn=1", &res,
                 20) < 0 ||
        !res.body)
    {
        snprintf(err, err_size, "网络错误: %s", res.error);
        http_response_free(&res);
        return -1;
    }

    root = json_tokener_parse(res.body);
    http_response_free(&res);
    if (!root)
    {
        snprintf(err, err_size, "稍后再看 JSON 解析失败");
        return -1;
    }
    if (json_error(root, err, err_size) < 0)
    {
        json_object_put(root);
        return -1;
    }

    data = jobj(root, "data");
    list = jobj(data, "list");
    total = list ? json_object_array_length(list) : 0;
    for (int i = 0; i < total && n < max; ++i)
        if (parse_item(json_object_array_get_idx(list, i), &items[n]) == 0)
            n++;

    json_object_put(root);
    *count = n;
    if (!n)
    {
        snprintf(err, err_size, "稍后再看是空的");
        return -1;
    }
    log_printf("[bili] toview: %d items", n);
    return 0;
}

int bili_history(VideoItem *items, int max, int *count, char *err, int err_size)
{
    HttpResponse res;
    json_object *root, *data, *list;
    int total, n = 0;

    *count = 0;
    if (max > 20)
        max = 20;

    if (http_get("https://api.bilibili.com/x/web-interface/history/cursor?ps=20&type=video",
                 &res, 20) < 0 ||
        !res.body)
    {
        snprintf(err, err_size, "网络错误: %s", res.error);
        http_response_free(&res);
        return -1;
    }

    root = json_tokener_parse(res.body);
    http_response_free(&res);
    if (!root)
    {
        snprintf(err, err_size, "历史 JSON 解析失败");
        return -1;
    }
    if (json_error(root, err, err_size) < 0)
    {
        json_object_put(root);
        return -1;
    }

    data = jobj(root, "data");
    list = jobj(data, "list");
    total = list ? json_object_array_length(list) : 0;
    for (int i = 0; i < total && n < max; ++i)
    {
        json_object *o = json_object_array_get_idx(list, i);
        VideoItem *it = &items[n];
        memset(it, 0, sizeof(*it));
        copy_str(it->bvid, sizeof(it->bvid), jstr(o, "bvid"));
        if (!it->bvid[0])
            continue;
        copy_str(it->title, sizeof(it->title), jstr(o, "title"));
        copy_str(it->up, sizeof(it->up), jstr(o, "author_name"));
        make_cover_url(jstr(o, "cover"), it->cover, sizeof(it->cover));
        it->duration = jint(o, "duration");
        it->aid = jint64(o, "aid");
        it->cid = jint64(o, "cid");
        it->page_count = 1;
        n++;
    }

    json_object_put(root);
    *count = n;
    if (!n)
    {
        snprintf(err, err_size, "没有历史记录");
        return -1;
    }
    log_printf("[bili] history: %d items", n);
    return 0;
}

int bili_fav_folders(FavFolder *out, int max, int *count, char *err, int err_size)
{
    char url[256];
    HttpResponse res;
    json_object *root, *data, *list;
    BiliUser user;
    int total, n = 0;

    *count = 0;
    if (max > 20)
        max = 20;

    if (bili_nav(&user, err, err_size) < 0)
        return -1;

    snprintf(url, sizeof(url),
             "https://api.bilibili.com/x/v3/fav/folder/created/list-all?up_mid=%lld",
             user.mid);
    if (http_get(url, &res, 20) < 0 || !res.body)
    {
        snprintf(err, err_size, "网络错误: %s", res.error);
        http_response_free(&res);
        return -1;
    }

    root = json_tokener_parse(res.body);
    http_response_free(&res);
    if (!root)
    {
        snprintf(err, err_size, "收藏夹 JSON 解析失败");
        return -1;
    }
    if (json_error(root, err, err_size) < 0)
    {
        json_object_put(root);
        return -1;
    }

    data = jobj(root, "data");
    list = jobj(data, "list");
    total = list ? json_object_array_length(list) : 0;
    for (int i = 0; i < total && n < max; ++i)
    {
        json_object *o = json_object_array_get_idx(list, i);
        out[n].id = jint64(o, "id");
        copy_str(out[n].title, sizeof(out[n].title), jstr(o, "title"));
        out[n].media_count = jint(o, "media_count");
        n++;
    }

    json_object_put(root);
    *count = n;
    if (!n)
    {
        snprintf(err, err_size, "没有收藏夹");
        return -1;
    }
    log_printf("[bili] fav folders: %d", n);
    return 0;
}

static int parse_fav_media(json_object *o, VideoItem *it)
{
    json_object *upper, *cnt;

    memset(it, 0, sizeof(*it));
    copy_str(it->bvid, sizeof(it->bvid), jstr(o, "bvid"));
    if (!it->bvid[0])
        return -1;
    copy_str(it->title, sizeof(it->title), jstr(o, "title"));
    make_cover_url(jstr(o, "cover"), it->cover, sizeof(it->cover));
    it->duration = jint(o, "duration");
    it->aid = jint64(o, "id");
    it->page_count = 1;

    upper = jobj(o, "upper");
    if (upper)
        copy_str(it->up, sizeof(it->up), jstr(upper, "name"));
    cnt = jobj(o, "cnt_info");
    if (cnt)
        it->view = jint64(cnt, "play");
    return 0;
}

int bili_fav_list(long long media_id, VideoItem *items, int max, int *count,
                  char *err, int err_size)
{
    char url[256];
    HttpResponse res;
    json_object *root, *data, *list;
    int total, n = 0;

    *count = 0;
    if (max > 20)
        max = 20;

    snprintf(url, sizeof(url),
             "https://api.bilibili.com/x/v3/fav/resource/list?media_id=%lld&pn=1&ps=20&platform=web",
             media_id);
    if (http_get(url, &res, 20) < 0 || !res.body)
    {
        snprintf(err, err_size, "网络错误: %s", res.error);
        http_response_free(&res);
        return -1;
    }

    root = json_tokener_parse(res.body);
    http_response_free(&res);
    if (!root)
    {
        snprintf(err, err_size, "收藏夹内容 JSON 解析失败");
        return -1;
    }
    if (json_error(root, err, err_size) < 0)
    {
        json_object_put(root);
        return -1;
    }

    data = jobj(root, "data");
    list = jobj(data, "medias");
    total = list ? json_object_array_length(list) : 0;
    for (int i = 0; i < total && n < max; ++i)
        if (parse_fav_media(json_object_array_get_idx(list, i), &items[n]) == 0)
            n++;

    json_object_put(root);
    *count = n;
    if (!n)
    {
        snprintf(err, err_size, "收藏夹是空的");
        return -1;
    }
    log_printf("[bili] fav list %lld: %d", media_id, n);
    return 0;
}

int bili_toview_add(long long aid, char *err, int err_size)
{
    char csrf[128];
    char body[256];
    HttpResponse res;
    json_object *root;
    int code;

    if (aid <= 0)
    {
        snprintf(err, err_size, "缺少 aid");
        return -1;
    }
    if (!http_cookie("bili_jct", csrf, sizeof(csrf)))
    {
        snprintf(err, err_size, "未登录（缺少 bili_jct）");
        return -1;
    }

    snprintf(body, sizeof(body), "aid=%lld&csrf=%s", aid, csrf);
    if (http_post("https://api.bilibili.com/x/v2/history/toview/add", body, &res,
                  15) < 0 ||
        !res.body)
    {
        snprintf(err, err_size, "网络错误: %s", res.error);
        http_response_free(&res);
        return -1;
    }

    root = json_tokener_parse(res.body);
    http_response_free(&res);
    if (!root)
    {
        snprintf(err, err_size, "稍后再看返回解析失败");
        return -1;
    }
    code = jint(root, "code");
    if (code != 0)
    {
        snprintf(err, err_size, "%s", jstr(root, "message"));
        json_object_put(root);
        return -1;
    }
    json_object_put(root);
    log_printf("[bili] added aid=%lld to toview", aid);
    return 0;
}

int bili_fav_add(long long aid, char *err, int err_size)
{
    FavFolder folders[20];
    int count = 0;
    char csrf[128];
    char body[320];
    HttpResponse res;
    json_object *root;
    int code;

    if (aid <= 0)
    {
        snprintf(err, err_size, "缺少 aid");
        return -1;
    }
    if (!http_cookie("bili_jct", csrf, sizeof(csrf)))
    {
        snprintf(err, err_size, "未登录（缺少 bili_jct）");
        return -1;
    }
    if (bili_fav_folders(folders, 20, &count, err, err_size) < 0)
        return -1;
    if (count <= 0)
    {
        snprintf(err, err_size, "没有可用的收藏夹");
        return -1;
    }

    snprintf(body, sizeof(body),
             "rid=%lld&type=2&add_media_ids=%lld&del_media_ids=&csrf=%s", aid,
             folders[0].id, csrf);
    if (http_post("https://api.bilibili.com/x/v3/fav/resource/deal", body, &res,
                  15) < 0 ||
        !res.body)
    {
        snprintf(err, err_size, "网络错误: %s", res.error);
        http_response_free(&res);
        return -1;
    }

    root = json_tokener_parse(res.body);
    http_response_free(&res);
    if (!root)
    {
        snprintf(err, err_size, "收藏返回解析失败");
        return -1;
    }
    code = jint(root, "code");
    if (code != 0)
    {
        snprintf(err, err_size, "%s", jstr(root, "message"));
        json_object_put(root);
        return -1;
    }
    json_object_put(root);
    log_printf("[bili] added aid=%lld to fav %lld", aid, folders[0].id);
    return 0;
}

int bili_view(const char *bvid, VideoItem *item, char *err, int err_size)
{
    char url[256];
    HttpResponse res;
    json_object *root, *data, *pages;
    int n;

    memset(item, 0, sizeof(*item));

    snprintf(url, sizeof(url),
             "https://api.bilibili.com/x/web-interface/view?bvid=%s", bvid);
    if (http_get(url, &res, 20) < 0 || !res.body)
    {
        snprintf(err, err_size, "网络错误: %s", res.error);
        http_response_free(&res);
        return -1;
    }

    root = json_tokener_parse(res.body);
    http_response_free(&res);
    if (!root)
    {
        snprintf(err, err_size, "详情 JSON 解析失败");
        return -1;
    }

    if (json_error(root, err, err_size) < 0)
    {
        json_object_put(root);
        return -1;
    }

    data = jobj(root, "data");
    if (parse_item(data, item) < 0)
    {
        snprintf(err, err_size, "详情数据为空");
        json_object_put(root);
        return -1;
    }

    copy_str(item->desc, sizeof(item->desc), jstr(data, "desc"));
    if (!item->cid)
        item->cid = jint64(data, "cid");

    pages = jobj(data, "pages");
    n = pages ? json_object_array_length(pages) : 0;
    item->page_count = n > 0 ? n : 1;
    if (n > 0 && !item->cid)
        item->cid = jint64(json_object_array_get_idx(pages, 0), "cid");

    json_object_put(root);
    return 0;
}

static void parse_segment_base(json_object *o, char *init, int init_size,
                               char *index, int index_size)
{
    json_object *sb = jobj(o, "SegmentBase");
    const char *i;
    const char *x;

    init[0] = 0;
    index[0] = 0;
    if (!sb)
        sb = jobj(o, "segment_base");
    if (!sb)
        return;

    i = jstr(sb, "Initialization");
    if (!i || !*i)
        i = jstr(sb, "initialization");
    x = jstr(sb, "indexRange");
    if (!x || !*x)
        x = jstr(sb, "index_range");
    copy_str(init, init_size, i);
    copy_str(index, index_size, x);
}

static int media_url(json_object *v, char *out, int size)
{
    const char *u = jstr(v, "baseUrl");
    json_object *backup;

    if (!u || !*u)
        u = jstr(v, "base_url");

    if ((!u || !*u) && (backup = jobj(v, "backupUrl")) &&
        json_object_array_length(backup) > 0)
        u = json_object_get_string(json_object_array_get_idx(backup, 0));

    if (!u || !*u)
        return -1;

    copy_str(out, size, u);
    return 0;
}

int bili_playurl(const char *bvid, long long cid, int qn, PlayUrl *out, char *err,
                 int err_size)
{
    char url[512];
    HttpResponse res;
    json_object *root, *data, *dash, *videos, *audios, *durl;
    int n, best_avc_score = -1, best_any_score = -1;
    json_object *best_avc = NULL, *best_any = NULL;

    memset(out, 0, sizeof(*out));

    snprintf(url, sizeof(url),
             "https://api.bilibili.com/x/player/wbi/playurl?bvid=%s&cid=%lld&qn=%d"
             "&fnval=16&fourk=0&fnver=0",
             bvid, cid, qn);

    if (http_get(url, &res, 25) < 0 || !res.body)
    {
        snprintf(err, err_size, "网络错误: %s", res.error);
        http_response_free(&res);
        return -1;
    }

    root = json_tokener_parse(res.body);
    http_response_free(&res);
    if (!root)
    {
        snprintf(err, err_size, "取流 JSON 解析失败");
        return -1;
    }

    if (json_error(root, err, err_size) < 0)
    {
        json_object_put(root);
        return -1;
    }

    data = jobj(root, "data");
    dash = jobj(data, "dash");
    videos = jobj(dash, "video");
    audios = jobj(dash, "audio");

    n = videos ? json_object_array_length(videos) : 0;
    for (int i = 0; i < n; ++i)
    {
        json_object *v = json_object_array_get_idx(videos, i);
        int id = jint(v, "id");
        int codecid = jint(v, "codecid");
        int bw = jint(v, "bandwidth");
        int score;

        if (qn > 0 && id > qn)
            continue;
        if (bw > 40000000)
            bw = 40000000;
        score = id * 1000000 + bw;

        if (codecid == 7 && score > best_avc_score)
        {
            best_avc_score = score;
            best_avc = v;
        }
        if (score > best_any_score)
        {
            best_any_score = score;
            best_any = v;
        }
    }

    json_object *chosen = best_avc ? best_avc : best_any;
    if (chosen)
    {
        out->width = jint(chosen, "width");
        out->height = jint(chosen, "height");
        out->qn = jint(chosen, "id");
        out->codecid = jint(chosen, "codecid");
        media_url(chosen, out->video_url, sizeof(out->video_url));
        parse_segment_base(chosen, out->video_init, sizeof(out->video_init),
                           out->video_index, sizeof(out->video_index));
    }

    n = audios ? json_object_array_length(audios) : 0;
    {
        int best_bw = -1;
        json_object *best = NULL;
        for (int i = 0; i < n; ++i)
        {
            json_object *a = json_object_array_get_idx(audios, i);
            int bw = jint(a, "bandwidth");
            if (bw > best_bw)
            {
                best_bw = bw;
                best = a;
            }
        }
        if (best)
        {
            media_url(best, out->audio_url, sizeof(out->audio_url));
            parse_segment_base(best, out->audio_init, sizeof(out->audio_init),
                               out->audio_index, sizeof(out->audio_index));
        }
    }

    durl = jobj(data, "durl");
    if (durl && json_object_array_length(durl) > 0)
        copy_str(out->mp4_url, sizeof(out->mp4_url),
                 jstr(json_object_array_get_idx(durl, 0), "url"));

    out->duration = jint(dash, "duration");
    if (!out->duration)
        out->duration = jint(data, "timelength") / 1000;
    if (!out->qn)
        out->qn = jint(data, "quality");

    json_object_put(root);

    if (!out->video_url[0] && !out->mp4_url[0])
    {
        snprintf(err, err_size, "没有可用的视频流");
        return -1;
    }

    log_printf("[bili] playurl %s cid=%lld qn=%d -> %dx%d codec=%d", bvid, cid,
               out->qn, out->width, out->height, out->codecid);
    return 0;
}

int bili_nav(BiliUser *user, char *err, int err_size)
{
    HttpResponse res;
    json_object *root, *data, *level;
    int login;

    memset(user, 0, sizeof(*user));

    if (http_get("https://api.bilibili.com/x/web-interface/nav", &res, 15) < 0 ||
        !res.body)
    {
        snprintf(err, err_size, "网络错误: %s", res.error);
        http_response_free(&res);
        return -1;
    }

    root = json_tokener_parse(res.body);
    http_response_free(&res);
    if (!root)
    {
        snprintf(err, err_size, "账号 JSON 解析失败");
        return -1;
    }

    data = jobj(root, "data");
    login = jint(data, "isLogin");
    if (!login)
    {
        snprintf(err, err_size, "未登录");
        json_object_put(root);
        return -1;
    }

    user->mid = jint64(data, "mid");
    copy_str(user->name, sizeof(user->name), jstr(data, "uname"));
    copy_str(user->face, sizeof(user->face), jstr(data, "face"));
    level = jobj(data, "level_info");
    user->level = jint(level, "current_level");

    json_object_put(root);
    return 0;
}

int bili_qr_generate(char *url, int url_size, char *key, int key_size, char *err,
                     int err_size)
{
    HttpResponse res;
    json_object *root, *data;

    url[0] = 0;
    key[0] = 0;

    if (http_get("https://passport.bilibili.com/x/passport-login/web/qrcode/generate",
                 &res, 15) < 0 ||
        !res.body)
    {
        snprintf(err, err_size, "网络错误: %s", res.error);
        http_response_free(&res);
        return -1;
    }

    root = json_tokener_parse(res.body);
    http_response_free(&res);
    if (!root)
    {
        snprintf(err, err_size, "二维码 JSON 解析失败");
        return -1;
    }

    if (json_error(root, err, err_size) < 0)
    {
        json_object_put(root);
        return -1;
    }

    data = jobj(root, "data");
    copy_str(url, url_size, jstr(data, "url"));
    copy_str(key, key_size, jstr(data, "qrcode_key"));
    json_object_put(root);

    if (!url[0] || !key[0])
    {
        snprintf(err, err_size, "二维码数据为空");
        return -1;
    }

    return 0;
}

int bili_qr_poll(const char *key, int *state, char *err, int err_size)
{
    char url[512];
    HttpResponse res;
    json_object *root, *data;
    int code;

    *state = -1;

    snprintf(url, sizeof(url),
             "https://passport.bilibili.com/x/passport-login/web/qrcode/poll"
             "?qrcode_key=%s",
             key);

    if (http_get(url, &res, 15) < 0 || !res.body)
    {
        snprintf(err, err_size, "网络错误: %s", res.error);
        http_response_free(&res);
        return -1;
    }

    root = json_tokener_parse(res.body);
    http_response_free(&res);
    if (!root)
    {
        snprintf(err, err_size, "扫码结果 JSON 解析失败");
        return -1;
    }

    if (json_error(root, err, err_size) < 0)
    {
        json_object_put(root);
        return -1;
    }

    data = jobj(root, "data");
    code = jint(data, "code");
    copy_str(err, err_size, jstr(data, "message"));

    *state = code;
    if (code == 0)
    {
        http_save_cookies();
        log_printf("[bili] qr login ok, cookies saved");
    }

    json_object_put(root);
    return 0;
}
