/* Looks at the latest GitHub release once at start, and on request puts its
 * homebrew/PPSA70727 over the installed one. The new build runs next launch. */
#include "update.h"
#include "json.h"
#include "log.h"
#include "net.h"
#include "unzip.h"

#include <dirent.h>
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

static bool copy_file(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : NULL;
    bool ok = in && out;
    char buf[64 * 1024];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0)
        ok = fwrite(buf, 1, n, out) == n;
    if (in) fclose(in);
    if (out) ok &= fclose(out) == 0;
    return ok;
}

static bool copy_tree(const char *from, const char *to)
{
    DIR *d = opendir(from);
    if (!d)
        return false;
    mkdir(to, 0777);
    bool ok = true;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char a[1024], b[1024];
        snprintf(a, sizeof(a), "%s/%s", from, e->d_name);
        snprintf(b, sizeof(b), "%s/%s", to, e->d_name);
        struct stat st;
        if (stat(a, &st) != 0)
            continue;
        ok &= S_ISDIR(st.st_mode) ? copy_tree(a, b) : copy_file(a, b);
    }
    closedir(d);
    return ok;
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
    bool ok = src[0] && copy_tree(src, APP_DIR);
    remove_tree(STAGE);
    if (ok)
        finish(UPDATE_DONE, "updated: close ProjectCadence and open it again");
    else
        finish(UPDATE_FAILED, "couldn't write the new files; copy the release over FTP instead");
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
