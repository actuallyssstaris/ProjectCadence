/* Looks at the latest GitHub release once at start. On request it downloads and
 * unpacks it, and hands the copying to a small helper run by the ELF loader
 * (tools/updater), which finishes once the game is closed. */
#include "update.h"
#include "json.h"
#include "log.h"
#include "net.h"
#include "unzip.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LATEST "https://api.github.com/repos/actuallyssstaris/ProjectCadence/releases/latest"
#define ZIP "/data/cadence/update.zip"
#define STAGE "/data/cadence/.update"
#define APP_DIR "/data/homebrew/PPSA70727"
#define JOB "/data/cadence/update-job.txt"
#define HELPER "/app0/assets/updater.elf"
#define LOADER_PORT 9021

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static update_info info;
static char zip_url[512];

/* "v1.10.2" -> comparable number; anything odd reads as 0 */
static long version_num(const char *v)
{
    int a = 0, b = 0, c = 0;
    if (*v == 'v')
        v++;
    sscanf(v, "%d.%d.%d", &a, &b, &c);
    return a * 1000000L + b * 1000L + c;
}

static bool asset_key(js *j, const char *key, void *ctx)
{
    if (strcmp(key, "browser_download_url"))
        return false;
    char url[512];
    if (!json_str(j, url, sizeof(url)))
        return false;
    size_t n = strlen(url);
    if (!zip_url[0] && n > 4 && !strcmp(url + n - 4, ".zip"))
        snprintf(zip_url, sizeof(zip_url), "%s", url);
    return true;
}

static bool release_key(js *j, const char *key, void *ctx)
{
    if (!strcmp(key, "tag_name"))
        return json_str(j, info.latest, sizeof(info.latest));
    if (strcmp(key, "assets") || *j->p != '[')
        return false;
    j->p++;
    for (;;) {
        json_ws(j);
        if (*j->p == ']') {
            j->p++;
            return true;
        }
        if (!json_object(j, asset_key, NULL))
            return false;
        json_ws(j);
        if (*j->p == ',')
            j->p++;
    }
}

static void *check_thread(void *arg)
{
    (void)arg;
    char *body;
    size_t len;
    int st = net_get(LATEST, &body, &len);
    pthread_mutex_lock(&lock);
    if (st == 200) {
        js j = {body};
        json_object(&j, release_key, NULL);
        free(body);
        if (zip_url[0] && version_num(info.latest) > version_num(CADENCE_VERSION))
            info.state = UPDATE_AVAILABLE;
        else
            info.state = UPDATE_NONE;
    } else {
        info.state = UPDATE_NONE; /* offline or rate limited: say nothing */
    }
    LOG("update: latest %s, running %s (%d)", info.latest[0] ? info.latest : "?", CADENCE_VERSION, st);
    pthread_mutex_unlock(&lock);
    return NULL;
}

void update_check(void)
{
    pthread_mutex_lock(&lock);
    info.state = UPDATE_CHECKING;
    pthread_mutex_unlock(&lock);
    pthread_t t;
    if (pthread_create(&t, NULL, check_thread, NULL) == 0)
        pthread_detach(t);
}

static void remove_tree(const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        remove(path);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char sub[1024];
        snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
        remove_tree(sub);
    }
    closedir(d);
    rmdir(path);
}

int sceNetSocket(const char *name, int family, int type, int protocol);
int sceNetConnect(int s, const void *addr, uint32_t len);
int sceNetSend(int s, const void *buf, size_t len, int flags);
int sceNetSocketClose(int s);

/* the helper goes to the console's ELF loader, which runs it outside our sandbox */
static bool send_helper(void)
{
    FILE *f = fopen(HELPER, "rb");
    if (!f) {
        LOG("update: no %s", HELPER);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *elf = n > 0 ? malloc((size_t)n) : NULL;
    bool read_ok = elf && fread(elf, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!read_ok) {
        free(elf);
        return false;
    }

    struct {
        uint8_t len, family;
        uint16_t port;
        uint32_t addr;
        uint8_t zero[8];
    } sa = {16, 2, (uint16_t)((LOADER_PORT >> 8) | (LOADER_PORT << 8 & 0xFF00)), 0x0100007F, {0}};
    int s = sceNetSocket("cadence-update", 2, 1, 0);
    bool ok = s >= 0 && sceNetConnect(s, &sa, sizeof(sa)) >= 0;
    for (long sent = 0; ok && sent < n;) {
        int k = sceNetSend(s, elf + sent, (size_t)(n - sent), 0);
        if (k <= 0)
            ok = false;
        else
            sent += k;
    }
    if (s >= 0)
        sceNetSocketClose(s);
    free(elf);
    LOG("update: helper %s", ok ? "sent" : "not sent");
    return ok;
}

static void finish(int state, const char *msg)
{
    pthread_mutex_lock(&lock);
    info.state = state;
    snprintf(info.msg, sizeof(info.msg), "%s", msg);
    pthread_mutex_unlock(&lock);
    LOG("update: %s", msg);
}

static void *install_thread(void *arg)
{
    (void)arg;
    int st = net_fetch(zip_url, ZIP, &info.bytes);
    if (st != 200) {
        finish(UPDATE_FAILED, "couldn't download the update");
        return NULL;
    }
    remove_tree(STAGE);
    int files = unzip_to(ZIP, STAGE, false);
    remove(ZIP);
    if (files <= 0) {
        finish(UPDATE_FAILED, "the update archive was broken");
        return NULL;
    }
    /* the zip has one top folder, projectcadence-<version>/ */
    char src[600] = "";
    DIR *d = opendir(STAGE);
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (e->d_name[0] != '.')
            snprintf(src, sizeof(src), STAGE "/%s/homebrew/PPSA70727", e->d_name);
    if (d)
        closedir(d);
    if (!src[0]) {
        remove_tree(STAGE);
        finish(UPDATE_FAILED, "the update archive had no app folder in it");
        return NULL;
    }

    /* our sandbox can't write the app folder: the helper does it once we're closed */
    FILE *job = fopen(JOB, "w");
    if (job) {
        fprintf(job, "%s\n%s\n%d\n%s\n", src, APP_DIR, (int)getpid(), info.latest);
        fclose(job);
    }
    if (job && send_helper())
        finish(UPDATE_DONE, "ready: close ProjectCadence (PS button) and it finishes by itself");
    else
        finish(UPDATE_FAILED, "the ELF loader (port 9021) isn't running; copy the release over FTP instead");
    return NULL;
}

void update_install(void)
{
    pthread_mutex_lock(&lock);
    bool go = info.state == UPDATE_AVAILABLE || info.state == UPDATE_FAILED;
    if (go) {
        info.state = UPDATE_INSTALLING;
        info.bytes = 0;
    }
    pthread_mutex_unlock(&lock);
    if (!go)
        return;
    pthread_t t;
    if (pthread_create(&t, NULL, install_thread, NULL) == 0)
        pthread_detach(t);
    else
        finish(UPDATE_FAILED, "couldn't start the update");
}

void update_status(update_info *out)
{
    pthread_mutex_lock(&lock);
    *out = info;
    pthread_mutex_unlock(&lock);
}
