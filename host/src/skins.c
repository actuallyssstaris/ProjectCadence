#include "skins.h"
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
#include <sys/stat.h>
#include <unistd.h>

#define SKINS_DIR "/data/cadence/skins"
#define ACTIVE_FILE "/data/cadence/skin.txt"

/* Only skins their authors publish with a direct download. The big skin sites sit
 * behind bot checks a console can't pass, so those are left to the player. */
const skin_entry skin_catalog[] = {
    {"default", "osu! classic (default)", "ppy", "CC BY-NC 4.0", NULL},
    {"monomal", "monomal", "UlyssesZh", "MIT",
     "https://github.com/UlyssesZh/monomal/releases/download/v1.0/monomal.osk"},
    {"project-minimalist", "Project Minimalist", "popcat19", "not stated",
     "https://github.com/popcat19/Project-Minimalist/releases/download/v1.2.1/InstaNM.Project.Minimalist.osk"},
    {"sakurada-shiro", "Sakurada Shiro (dark)", "Maseshi", "CC0",
     "https://github.com/Maseshi/Sakurada-Shiro/releases/download/1.1.0/sakurada-shiro.dark.osk"},
    {"osu-mg-solitary", "osu!mg Solitary", "alphamg2003", "MIT",
     "https://github.com/alphamg2003/osu_mg_solitary/releases/download/v.7.3.1-release/osu.Mg.7.3.1.osk"},
};
const int skin_catalog_count = sizeof(skin_catalog) / sizeof(skin_catalog[0]);

/* the classic skin as lazer ships it, osu!standard parts only */
#define DEFAULT_BASE "https://raw.githubusercontent.com/ppy/osu-resources/master/osu.Game.Resources/Skins/Legacy/"
static const char *default_files[] = {
    "approachcircle@2x.png", "cursor@2x.png", "followpoint@2x.png", "hitcircle@2x.png",
    "hitcircleoverlay@2x.png", "reversearrow@2x.png", "sliderb0@2x.png", "sliderfollowcircle@2x.png",
    "sliderscorepoint@2x.png", "hit0@2x.png", "hit50@2x.png", "hit100@2x.png", "hit300@2x.png",
    "default-0@2x.png", "default-1@2x.png", "default-2@2x.png", "default-3@2x.png", "default-4@2x.png",
    "default-5@2x.png", "default-6@2x.png", "default-7@2x.png", "default-8@2x.png", "default-9@2x.png",
    "normal-hitnormal.wav", "normal-hitwhistle.wav", "normal-hitfinish.wav", "normal-hitclap.wav",
    "normal-slidertick.wav", "soft-hitnormal.wav", "soft-hitwhistle.wav", "soft-hitfinish.wav",
    "soft-hitclap.wav", "soft-slidertick.wav", "drum-hitnormal.wav", "drum-hitwhistle.wav",
    "drum-hitfinish.wav", "drum-hitclap.wav", "drum-slidertick.wav", "combobreak.mp3", "spinnerbonus.wav",
    "score-0@2x.png", "score-1@2x.png", "score-2@2x.png", "score-3@2x.png", "score-4@2x.png",
    "score-5@2x.png", "score-6@2x.png", "score-7@2x.png", "score-8@2x.png", "score-9@2x.png",
    "score-comma@2x.png", "score-dot@2x.png", "score-percent@2x.png", "score-x@2x.png",
    "scorebar-bg@2x.png", "scorebar-colour@2x.png",
    "spinner-approachcircle@2x.png", "spinner-bottom@2x.png", "spinner-clear@2x.png", "spinner-glow@2x.png",
    "spinner-middle@2x.png", "spinner-middle2@2x.png", "spinner-top@2x.png",
    "ranking-XH@2x.png", "ranking-X@2x.png", "ranking-SH@2x.png", "ranking-S@2x.png", "ranking-A@2x.png",
    "ranking-B@2x.png", "ranking-C@2x.png", "ranking-D@2x.png",
    "ranking-XH-small@2x.png", "ranking-X-small@2x.png", "ranking-SH-small@2x.png", "ranking-S-small@2x.png",
    "ranking-A-small@2x.png", "ranking-B-small@2x.png", "ranking-C-small@2x.png", "ranking-D-small@2x.png",
};

static char active[128];

bool skins_chosen(void)
{
    return access(ACTIVE_FILE, F_OK) == 0;
}

const char *skins_active(void)
{
    if (!active[0]) {
        snprintf(active, sizeof(active), "builtin");
        FILE *f = fopen(ACTIVE_FILE, "r");
        if (f) {
            if (fgets(active, sizeof(active), f))
                active[strcspn(active, "\r\n")] = 0;
            fclose(f);
        }
    }
    return active;
}

void skins_set_active(const char *id)
{
    snprintf(active, sizeof(active), "%s", id);
    FILE *f = fopen(ACTIVE_FILE, "w");
    if (f) {
        fprintf(f, "%s\n", id);
        fclose(f);
    }
}

void skins_path(const char *id, char *out, size_t n)
{
    snprintf(out, n, SKINS_DIR "/%s", id);
}

bool skins_installed(const char *id)
{
    char p[256];
    skins_path(id, p, sizeof(p));
    DIR *d = opendir(p);
    if (!d)
        return false;
    int files = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        files += e->d_name[0] != '.';
    closedir(d);
    return files > 0;
}

int skins_local(char ids[][64], int max)
{
    int n = 0;
    DIR *d = opendir(SKINS_DIR);
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] != '.' && skins_installed(e->d_name))
            snprintf(ids[n++], 64, "%s", e->d_name);
    }
    closedir(d);
    return n;
}

typedef struct {
    char id[96];
    char name[96];
    char url[256]; /* empty: the default skin's loose files */
    skin_job *job;
} work;

static work pending;

static void *install_thread(void *arg)
{
    work *w = arg;
    skin_job *job = w->job;
    char dir[256], path[512];

    mkdir(SKINS_DIR, 0777);
    skins_path(w->id, dir, sizeof(dir));
    mkdir(dir, 0777);

    if (!w->url[0]) {
        int n = (int)(sizeof(default_files) / sizeof(default_files[0]));
        job->total = n;
        for (int i = 0; i < n; i++) {
            char url[256], name[64];
            snprintf(url, sizeof(url), DEFAULT_BASE "%s", default_files[i]);
            /* stored lowercase like an unpacked .osk, that's how they're looked up */
            snprintf(name, sizeof(name), "%s", default_files[i]);
            for (char *c = name; *c; c++)
                *c = (char)tolower((unsigned char)*c);
            snprintf(path, sizeof(path), "%s/%s", dir, name);
            job->done = i;
            if (!job->force && access(path, F_OK) == 0)
                continue; /* topping up an older download */
            int st = net_fetch(url, path, &job->bytes);
            if (st != 200) {
                snprintf(job->msg, sizeof(job->msg), "%.60s: %.40s 0x%x", w->name, default_files[i], (unsigned)st);
                LOG("skins: %s", job->msg);
                job->state = 3;
                return NULL;
            }
            job->done = i + 1;
        }
    } else {
        job->total = 1;
        snprintf(path, sizeof(path), SKINS_DIR "/%s.osk", w->id);
        int st = net_fetch(w->url, path, &job->bytes);
        if (st != 200) {
            snprintf(job->msg, sizeof(job->msg), "download failed (0x%x)", (unsigned)st);
            LOG("skins: %s %s", w->id, job->msg);
            job->state = 3;
            return NULL;
        }
        snprintf(job->msg, sizeof(job->msg), "unpacking");
        int files = unzip_to(path, dir, true);
        remove(path);
        if (files <= 0) {
            snprintf(job->msg, sizeof(job->msg), "couldn't unpack the .osk");
            LOG("skins: %s %s", w->id, job->msg);
            job->state = 3;
            return NULL;
        }
        job->done = 1;
        LOG("skins: %s unpacked, %d files", w->id, files);
    }
    snprintf(job->msg, sizeof(job->msg), "done");
    job->state = 2;
    return NULL;
}

bool skins_default_outdated(void)
{
    return skins_installed("default") && access(SKINS_DIR "/default/spinner-top@2x.png", F_OK) != 0;
}

static bool start(const char *id, const char *name, const char *url, skin_job *job, bool force)
{
    if (job->state == 1)
        return false;
    memset(job, 0, sizeof(*job));
    job->force = force;
    job->state = 1;
    snprintf(job->msg, sizeof(job->msg), "downloading");
    if (!net_init()) {
        snprintf(job->msg, sizeof(job->msg), "no network");
        job->state = 3;
        return false;
    }
    memset(&pending, 0, sizeof(pending));
    snprintf(pending.id, sizeof(pending.id), "%s", id);
    snprintf(pending.name, sizeof(pending.name), "%s", name);
    snprintf(pending.url, sizeof(pending.url), "%s", url ? url : "");
    pending.job = job;
    pthread_t t;
    if (pthread_create(&t, NULL, install_thread, &pending) != 0) {
        job->state = 3;
        return false;
    }
    pthread_detach(t);
    return true;
}

bool skins_install(int i, skin_job *job, bool force)
{
    if (i < 0 || i >= skin_catalog_count)
        return false;
    const skin_entry *e = &skin_catalog[i];
    return start(e->id, e->name, e->url, job, force);
}

bool skins_install_remote(const skin_remote *r, skin_job *job)
{
    return start(r->id, r->name, r->url, job, false);
}

/* ---- o!rdr's list ---- */

#define ORDR_LIST "https://apis.issou.best/ordr/skins?pageSize=1000&page=1"
#define REMOTE_MAX 1000

static skin_remote *remote;
static int nremote;
static volatile int remote_state;

static bool remote_key(js *j, const char *key, void *ctx)
{
    skin_remote *r = ctx;
    if (!strcmp(key, "skin"))
        json_str(j, r->id, sizeof(r->id));
    else if (!strcmp(key, "presentationName"))
        json_str(j, r->name, sizeof(r->name));
    else if (!strcmp(key, "author"))
        json_str(j, r->author, sizeof(r->author));
    else if (!strcmp(key, "url"))
        json_str(j, r->url, sizeof(r->url));
    else if (!strcmp(key, "timesUsed"))
        r->used = (int)json_num(j);
    else
        return false;
    return true;
}

typedef struct {
    skin_remote *list;
    int n;
} remote_parse;

static bool reply_key(js *j, const char *key, void *ctx)
{
    remote_parse *p = ctx;
    if (strcmp(key, "skins") || *j->p != '[')
        return false;
    j->p++;
    for (;;) {
        json_ws(j);
        if (*j->p == ']') {
            j->p++;
            break;
        }
        skin_remote r;
        memset(&r, 0, sizeof(r));
        if (!json_object(j, remote_key, &r))
            break;
        /* the id becomes a folder name: nothing that could climb out of skins/ */
        bool safe = r.id[0] && r.id[0] != '.' && !strchr(r.id, '/') && !strchr(r.id, '\\');
        if (safe && !strncmp(r.url, "https://", 8) && p->n < REMOTE_MAX)
            p->list[p->n++] = r;
        json_ws(j);
        if (*j->p == ',')
            j->p++;
    }
    return true;
}

static int by_use(const void *a, const void *b)
{
    const skin_remote *x = a, *y = b;
    return y->used - x->used;
}

static void *remote_thread(void *arg)
{
    (void)arg;
    char *body = NULL;
    size_t len = 0;
    int st = net_get(ORDR_LIST, &body, &len);
    remote_parse p = {calloc(REMOTE_MAX, sizeof(skin_remote)), 0};
    if (st == 200 && p.list) {
        js j = {body};
        json_object(&j, reply_key, &p);
    }
    free(body);
    if (p.n > 0) {
        qsort(p.list, (size_t)p.n, sizeof(skin_remote), by_use);
        remote = p.list;
        nremote = p.n;
        remote_state = 2;
    } else {
        free(p.list);
        remote_state = 3;
    }
    LOG("skins: o!rdr list -> %d, %d skins", st, p.n);
    return NULL;
}

void skins_remote_load(void)
{
    if (remote_state == 1 || remote_state == 2)
        return;
    if (!net_init()) {
        remote_state = 3;
        return;
    }
    remote_state = 1;
    pthread_t t;
    if (pthread_create(&t, NULL, remote_thread, NULL) != 0) {
        remote_state = 3;
        return;
    }
    pthread_detach(t);
}

int skins_remote_state(void) { return remote_state; }

int skins_remote(const skin_remote **out)
{
    *out = remote;
    return remote_state == 2 ? nremote : 0;
}
