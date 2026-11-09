#include "mapdl.h"
#include "json.h"
#include "log.h"
#include "net.h"
#include "unzip.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SONGS_DIR "/data/cadence/Songs"
#define QUEUE_MAX 32
#define KNOWN_MAX 8192

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;

static mapdl_search search;
static int search_gen;

static mapdl_set queue[QUEUE_MAX];
static int queued;
static int current;
static volatile uint64_t current_bytes;
static int installed_count;
static char status_msg[128];
static bool worker_up;

/* set ids already in Songs/, from folder names that start with the id */
static int known[KNOWN_MAX];
static int nknown = -1;


typedef struct {
    int mode;
    float stars;
} diff_info;

static bool diff_key(js *j, const char *key, void *ctx)
{
    diff_info *d = ctx;
    if (!strcmp(key, "mode_int"))
        d->mode = (int)json_num(j);
    else if (!strcmp(key, "difficulty_rating"))
        d->stars = (float)json_num(j);
    else
        return false;
    return true;
}

static bool set_key(js *j, const char *key, void *ctx)
{
    mapdl_set *s = ctx;
    if (!strcmp(key, "id"))
        s->id = (int)json_num(j);
    else if (!strcmp(key, "artist"))
        json_str(j, s->artist, sizeof(s->artist));
    else if (!strcmp(key, "title"))
        json_str(j, s->title, sizeof(s->title));
    else if (!strcmp(key, "creator"))
        json_str(j, s->creator, sizeof(s->creator));
    else if (!strcmp(key, "status") && *j->p == '"')
        json_str(j, s->status, sizeof(s->status));
    else if (!strcmp(key, "video")) {
        s->video = !strncmp(j->p, "true", 4);
        json_skip(j);
    } else if (!strcmp(key, "beatmaps") && *j->p == '[') {
        j->p++;
        for (;;) {
            json_ws(j);
            if (*j->p == ']') {
                j->p++;
                break;
            }
            diff_info d = {-1, 0};
            if (!json_object(j, diff_key, &d))
                return true; /* garbage; the outer walk will notice */
            if (d.mode == 0) {
                s->diffs++;
                if (d.stars > s->stars)
                    s->stars = d.stars;
            }
            json_ws(j);
            if (*j->p == ',')
                j->p++;
        }
    } else
        return false;
    return true;
}

int mapdl_parse(const char *json, mapdl_set *out, int max)
{
    js j = {json};
    json_ws(&j);
    if (*j.p != '[')
        return -1;
    j.p++;
    int n = 0;
    for (;;) {
        json_ws(&j);
        if (*j.p == ']' || !*j.p)
            break;
        mapdl_set s;
        memset(&s, 0, sizeof(s));
        if (!json_object(&j, set_key, &s))
            return n > 0 ? n : -1;
        if (s.id > 0 && s.diffs > 0 && n < max)
            out[n++] = s;
        json_ws(&j);
        if (*j.p == ',')
            j.p++;
    }
    return n;
}

/* ---- what's on disk ---- */

static void scan_known(void)
{
    nknown = 0;
    DIR *d = opendir(SONGS_DIR);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) && nknown < KNOWN_MAX) {
        int id = atoi(e->d_name);
        if (id > 0 && isdigit((unsigned char)e->d_name[0]))
            known[nknown++] = id;
    }
    closedir(d);
}

static bool is_known(int id)
{
    if (nknown < 0)
        scan_known();
    for (int i = 0; i < nknown; i++)
        if (known[i] == id)
            return true;
    return false;
}

int mapdl_state(int id)
{
    pthread_mutex_lock(&lock);
    int st = is_known(id) ? 2 : current == id ? 1 : 0;
    for (int i = 0; i < queued && !st; i++)
        if (queue[i].id == id)
            st = 1;
    pthread_mutex_unlock(&lock);
    return st;
}

/* ---- search ---- */

typedef struct {
    int gen;
    char url[512];
} search_req;

static void *search_thread(void *arg)
{
    search_req *r = arg;
    char *body = NULL;
    size_t len = 0;
    int st = net_get(r->url, &body, &len);

    static mapdl_set found[MAPDL_RESULTS];
    int n = st == 200 ? mapdl_parse(body, found, MAPDL_RESULTS) : -1;
    free(body);

    pthread_mutex_lock(&lock);
    if (r->gen == search_gen) {
        if (n < 0) {
            search.state = 3;
            snprintf(search.msg, sizeof(search.msg), st == 200 ? "the mirror sent something odd" : "search failed (%d)", st);
            LOG("mapdl: %s, %s", search.msg, r->url);
        } else {
            search.state = 2;
            search.count = n;
            memcpy(search.sets, found, sizeof(mapdl_set) * (size_t)n);
            snprintf(search.msg, sizeof(search.msg), n ? "" : "nothing found");
        }
    }
    pthread_mutex_unlock(&lock);
    free(r);
    return NULL;
}

static void url_encode(const char *in, char *out, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t k = 0;
    for (; *in && k + 4 < n; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[k++] = (char)c;
        } else {
            out[k++] = '%';
            out[k++] = hex[c >> 4];
            out[k++] = hex[c & 15];
        }
    }
    out[k] = 0;
}

bool mapdl_find(const char *query, int page)
{
    if (!net_init())
        return false;
    search_req *r = malloc(sizeof(*r));
    if (!r)
        return false;
    char q[256];
    url_encode(query, q, sizeof(q));
    snprintf(r->url, sizeof(r->url), "https://catboy.best/api/v2/search?query=%s&mode=0&limit=%d&offset=%d", q,
             MAPDL_RESULTS, page * MAPDL_RESULTS);

    pthread_mutex_lock(&lock);
    r->gen = ++search_gen;
    search.state = 1;
    search.count = 0;
    search.msg[0] = 0;
    pthread_mutex_unlock(&lock);

    pthread_t t;
    if (pthread_create(&t, NULL, search_thread, r) != 0) {
        free(r);
        pthread_mutex_lock(&lock);
        search.state = 3;
        snprintf(search.msg, sizeof(search.msg), "couldn't start the search");
        pthread_mutex_unlock(&lock);
        return false;
    }
    pthread_detach(t);
    return true;
}

void mapdl_results(mapdl_search *out)
{
    pthread_mutex_lock(&lock);
    *out = search;
    pthread_mutex_unlock(&lock);
}

/* ---- downloads ---- */

static void folder_name(const mapdl_set *s, char *out, size_t n)
{
    char name[200];
    snprintf(name, sizeof(name), "%d %.80s - %.100s", s->id, s->artist, s->title);
    for (char *c = name; *c; c++) {
        if (strchr("/\\:*?\"<>|", *c) || (unsigned char)*c < 32)
            *c = '_';
    }
    /* no trailing dots or spaces, Windows tools choke on those when copying back */
    size_t l = strlen(name);
    while (l > 0 && (name[l - 1] == '.' || name[l - 1] == ' '))
        name[--l] = 0;
    snprintf(out, n, SONGS_DIR "/%s", name);
}

static void *download_thread(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&lock);
        while (queued == 0)
            pthread_cond_wait(&wake, &lock);
        mapdl_set s = queue[0];
        memmove(queue, queue + 1, sizeof(mapdl_set) * (size_t)(queued - 1));
        queued--;
        current = s.id;
        current_bytes = 0;
        pthread_mutex_unlock(&lock);

        char zip[128], url[160], dir[512];
        snprintf(zip, sizeof(zip), SONGS_DIR "/.dl-%d.osz", s.id);

        /* with the video when there is one: they play behind the map now */
        snprintf(url, sizeof(url), "https://catboy.best/d/%d", s.id);
        int st = net_fetch(url, zip, &current_bytes);
        if (st != 200) {
            LOG("mapdl: %d from catboy -> %d, trying nerinyan", s.id, st);
            current_bytes = 0;
            snprintf(url, sizeof(url), "https://api.nerinyan.moe/d/%d", s.id);
            st = net_fetch(url, zip, &current_bytes);
        }

        int files = -1;
        if (st == 200) {
            folder_name(&s, dir, sizeof(dir));
            files = unzip_to(zip, dir, false);
        }
        remove(zip);

        pthread_mutex_lock(&lock);
        current = 0;
        if (files > 0) {
            installed_count++;
            if (nknown >= 0 && nknown < KNOWN_MAX)
                known[nknown++] = s.id;
            snprintf(status_msg, sizeof(status_msg), "got %.50s - %.60s", s.artist, s.title);
        } else if (st == 200) {
            snprintf(status_msg, sizeof(status_msg), "%.90s: broken archive", s.title);
        } else {
            snprintf(status_msg, sizeof(status_msg), "%.80s: download failed (%d)", s.title, st);
        }
        LOG("mapdl: %d %s", s.id, status_msg);
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

bool mapdl_queue(const mapdl_set *s)
{
    if (!net_init())
        return false;

    pthread_mutex_lock(&lock);
    bool ok = queued < QUEUE_MAX && current != s->id && !is_known(s->id);
    for (int i = 0; i < queued && ok; i++)
        ok = queue[i].id != s->id;
    if (ok) {
        queue[queued++] = *s;
        pthread_cond_signal(&wake);
    }
    if (ok && !worker_up) {
        pthread_t t;
        worker_up = pthread_create(&t, NULL, download_thread, NULL) == 0;
        if (worker_up)
            pthread_detach(t);
        else {
            queued--;
            ok = false;
        }
    }
    pthread_mutex_unlock(&lock);
    return ok;
}

void mapdl_progress(mapdl_status *out)
{
    pthread_mutex_lock(&lock);
    out->queued = queued;
    out->current = current;
    out->bytes = current_bytes;
    out->installed = installed_count;
    snprintf(out->msg, sizeof(out->msg), "%s", status_msg);
    pthread_mutex_unlock(&lock);
}
