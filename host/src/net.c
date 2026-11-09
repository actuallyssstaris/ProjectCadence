/* HTTPS downloads through the console's own HTTP/2 stack (same calls as the
 * payload SDK's http2_get sample). */
#include "net.h"
#include "log.h"

#include <stddef.h>
#include <stdio.h>

int sceNetInit(void);
int sceNetPoolCreate(const char *name, int size, int flags);
int sceSslInit(size_t pool);
int sceHttp2Init(int net_pool, int ssl_ctx, size_t pool, int max_requests);
int sceHttp2CreateTemplate(int ctx, const char *agent, int http_ver, int auto_proxy);
int sceHttp2SetAutoRedirect(int id, int enable);
int sceHttp2SetTimeOut(int id, uint32_t usec);
int sceHttp2CreateRequestWithURL(int tmpl, const char *method, const char *url, uint64_t content_length);
int sceHttp2DeleteRequest(int req);
int sceHttp2SendRequest(int req, const void *data, size_t size);
int sceHttp2GetStatusCode(int req, int *status);
int sceHttp2ReadData(int req, void *buf, size_t size);

static int tmpl = -1;

bool net_init(void)
{
    if (tmpl >= 0)
        return true;

    int rc = sceNetInit();
    int pool = sceNetPoolCreate("osu5", 64 * 1024, 0);
    int ssl = sceSslInit(512 * 1024);
    int http = sceHttp2Init(pool, ssl, 512 * 1024, 4);
    LOG("net: init 0x%x pool %d ssl %d http %d", (unsigned)rc, pool, ssl, http);
    if (pool < 0 || ssl < 0 || http < 0)
        return false;

    tmpl = sceHttp2CreateTemplate(http, "osu-ps5/0.1", 3, 1);
    if (tmpl < 0) {
        LOG("net: template 0x%08x", (unsigned)tmpl);
        return false;
    }
    sceHttp2SetAutoRedirect(tmpl, 1);
    sceHttp2SetTimeOut(tmpl, 30 * 1000 * 1000);
    return true;
}

int net_fetch(const char *url, const char *path, volatile uint64_t *got)
{
    if (tmpl < 0 && !net_init())
        return -1;

    int req = sceHttp2CreateRequestWithURL(tmpl, "GET", url, 0);
    if (req < 0)
        return req;

    int status = -1;
    int rc = sceHttp2SendRequest(req, NULL, 0);
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

    FILE *f = fopen(path, "wb");
    if (!f) {
        sceHttp2DeleteRequest(req);
        return -2;
    }
    static char buf[64 * 1024];
    int n;
    while ((n = sceHttp2ReadData(req, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, (size_t)n, f);
        if (got)
            *got += (uint64_t)n;
    }
    fclose(f);
    sceHttp2DeleteRequest(req);
    return n < 0 ? n : 200;
}
