#ifndef PILIS3_HTTP_H
#define PILIS3_HTTP_H

#include <ppu-types.h>

typedef struct
{
    char *body;
    size_t len;
    long status;
    int curl_code;
    char error[160];
} HttpResponse;

int http_init(void);
void http_shutdown(void);

int http_get(const char *url, HttpResponse *out, int timeout_sec);
int http_get_range(const char *url, long long start, long long end,
                   HttpResponse *out, int timeout_sec);
int http_post(const char *url, const char *body, HttpResponse *out, int timeout_sec);
void http_response_free(HttpResponse *res);

int http_cookie(const char *name, char *out, int size);
void http_set_cookie(const char *name, const char *value);
void http_save_cookies(void);
int http_has_login(void);

const char *http_buvid(void);
int http_url_encode(const char *in, char *out, int size);

#endif
