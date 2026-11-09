/* HTTPS downloads through the console's own HTTP/2 stack (same calls as the
 * payload SDK's http2_get sample). */
#include "net.h"
#include "log.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int sceNetInit(void);
int sceNetPoolCreate(const char *name, int size, int flags);
int sceSslInit(size_t pool);
int sceHttp2Init(int net_pool, int ssl_ctx, size_t pool, int max_requests);
int sceHttp2CreateTemplate(int ctx, const char *agent, int http_ver, int auto_proxy);
int sceHttp2SetAutoRedirect(int id, int enable);
int sceHttp2SetTimeOut(int id, uint32_t usec);
int sceHttp2CreateRequestWithURL(int tmpl, const char *method, const char *url, uint64_t content_length);
int sceHttp2DeleteRequest(int req);
int sceHttp2AddRequestHeader(int req, const char *name, const char *value, uint32_t mode);
int sceHttp2GetAllResponseHeaders(int req, char **headers, size_t *size);
int sceHttp2SendRequest(int req, const void *data, size_t size);
int sceHttp2GetStatusCode(int req, int *status);
int sceHttp2ReadData(int req, void *buf, size_t size);

static int tmpl = -1;

bool net_init(void)
{
    if (tmpl >= 0)
        return true;

    int rc = sceNetInit();
    int pool = sceNetPoolCreate("cadence", 64 * 1024, 0);
    int ssl = sceSslInit(512 * 1024);
    int http = sceHttp2Init(pool, ssl, 512 * 1024, 4);
    LOG("net: init 0x%x pool %d ssl %d http %d", (unsigned)rc, pool, ssl, http);
    if (pool < 0 || ssl < 0 || http < 0)
        return false;

    tmpl = sceHttp2CreateTemplate(http, "ProjectCadence", 3, 1);
    if (tmpl < 0) {
        LOG("net: template 0x%08x", (unsigned)tmpl);
        return false;
    }
    sceHttp2SetAutoRedirect(tmpl, 1);
    sceHttp2SetTimeOut(tmpl, 30 * 1000 * 1000);
    return true;
}

/* sends the request; > 0 is a request id with a 200 behind it, otherwise the
 * status or error to hand back */
static int open_req(const char *method, const char *url, const char *const *headers, const char *body, size_t blen, int *req_out)
{
    if (tmpl < 0 && !net_init())
        return -1;


    int req = sceHttp2CreateRequestWithURL(tmpl, method, url, blen);
    if (req < 0)
        return req;
    for (int i = 0; headers && headers[i]; i += 2)
        sceHttp2AddRequestHeader(req, headers[i], headers[i + 1], 0);

    int status = -1;
    int rc = sceHttp2SendRequest(req, body, blen);
    if (rc == 0)
        rc = sceHttp2GetStatusCode(req, &status);
    if (rc != 0) {
        sceHttp2DeleteRequest(req);
        return rc < 0 ? rc : -rc;
    }
    if (status != 200) {
        sceHttp2DeleteRequest(req);
        return status;
    }
    *req_out = req;
    return 200;
}

static int open_get(const char *url, int *req_out)
{
    return open_req("GET", url, NULL, NULL, 0, req_out);
}

int net_fetch(const char *url, const char *path, volatile uint64_t *got)
{
    int req;
    int st = open_get(url, &req);
    if (st != 200)
        return st;

    FILE *f = fopen(path, "wb");
    char *buf = malloc(64 * 1024);
    if (!f || !buf) {
        if (f) fclose(f);
        free(buf);
        sceHttp2DeleteRequest(req);
        return -2;
    }
    int n;
    while ((n = sceHttp2ReadData(req, buf, 64 * 1024)) > 0) {
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) {
            n = -3; /* disk full, most likely */
            break;
        }
        if (got)
            *got += (uint64_t)n;
    }
    fclose(f);
    free(buf);
    sceHttp2DeleteRequest(req);
    if (n < 0)
        remove(path);
    return n < 0 ? n : 200;
}

int net_get(const char *url, char **out, size_t *len)
{
    return net_request("GET", url, NULL, NULL, out, len);
}

int net_request_raw(const char *method, const char *url, const char *const *headers, const void *body, size_t blen,
                    char **out, size_t *len, char *resp_headers, size_t hcap)
{
    *out = NULL;
    *len = 0;
    int req;
    int st = open_req(method, url, headers, body, blen, &req);
    if (st == 200 && resp_headers && hcap) {
        char *h = NULL;
        size_t hn = 0;
        resp_headers[0] = 0;
        if (sceHttp2GetAllResponseHeaders(req, &h, &hn) == 0 && h)
            snprintf(resp_headers, hcap, "%.*s", (int)hn, h);
    }
    if (st != 200)
        return st;

    size_t cap = 256 * 1024, n = 0;
    char *buf = malloc(cap + 1);
    int got = 0;
    while (buf && (got = sceHttp2ReadData(req, buf + n, cap - n)) > 0) {
        n += (size_t)got;
        if (n == cap) {
            if (cap >= 16 * 1024 * 1024)
                break; /* nothing we ask for is this big */
            char *grown = realloc(buf, cap * 2 + 1);
            if (!grown)
                break;
            buf = grown;
            cap *= 2;
        }
    }
    sceHttp2DeleteRequest(req);
    if (!buf)
        return -2;
    if (got < 0) {
        free(buf);
        return got;
    }
    buf[n] = 0;
    *out = buf;
    *len = n;
    return 200;
}

int net_request(const char *method, const char *url, const char *const *headers, const char *body, char **out, size_t *len)
{
    return net_request_raw(method, url, headers, body, body ? strlen(body) : 0, out, len, NULL, 0);
}
