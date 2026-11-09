#include "skins.h"
#include "log.h"
#include "net.h"
#include "unzip.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SKINS_DIR "/data/osu5/skins"
#define ACTIVE_FILE "/data/osu5/skin.txt"

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
};

static char active[64];

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
    int index;
    skin_job *job;
} work;

static work pending;

static void *install_thread(void *arg)
{
    work *w = arg;
    skin_job *job = w->job;
    const skin_entry *e = &skin_catalog[w->index];
    char dir[256], path[512];

    mkdir(SKINS_DIR, 0777);
    skins_path(e->id, dir, sizeof(dir));
    mkdir(dir, 0777);

    if (!e->url) {
        int n = (int)(sizeof(default_files) / sizeof(default_files[0]));
        job->total = n;
        for (int i = 0; i < n; i++) {
            char url[256];
            snprintf(url, sizeof(url), DEFAULT_BASE "%s", default_files[i]);
            snprintf(path, sizeof(path), "%s/%s", dir, default_files[i]);
            int st = net_fetch(url, path, &job->bytes);
            if (st != 200) {
                snprintf(job->msg, sizeof(job->msg), "%s: %s 0x%x", e->name, default_files[i], (unsigned)st);
                LOG("skins: %s", job->msg);
                job->state = 3;
                return NULL;
            }
            job->done = i + 1;
        }
    } else {
        job->total = 1;
        snprintf(path, sizeof(path), SKINS_DIR "/%s.osk", e->id);
        int st = net_fetch(e->url, path, &job->bytes);
        if (st != 200) {
            snprintf(job->msg, sizeof(job->msg), "download failed (0x%x)", (unsigned)st);
            LOG("skins: %s %s", e->id, job->msg);
            job->state = 3;
            return NULL;
        }
        snprintf(job->msg, sizeof(job->msg), "unpacking");
        int files = unzip_to(path, dir);
        remove(path);
        if (files <= 0) {
            snprintf(job->msg, sizeof(job->msg), "couldn't unpack the .osk");
            LOG("skins: %s %s", e->id, job->msg);
            job->state = 3;
            return NULL;
        }
        job->done = 1;
        LOG("skins: %s unpacked, %d files", e->id, files);
    }
    snprintf(job->msg, sizeof(job->msg), "done");
    job->state = 2;
    return NULL;
}

bool skins_install(int i, skin_job *job)
{
    if (i < 0 || i >= skin_catalog_count || job->state == 1)
        return false;
    memset(job, 0, sizeof(*job));
    job->state = 1;
    snprintf(job->msg, sizeof(job->msg), "downloading");
    if (!net_init()) {
        snprintf(job->msg, sizeof(job->msg), "no network");
        job->state = 3;
        return false;
    }
    pending = (work){i, job};
    pthread_t t;
    if (pthread_create(&t, NULL, install_thread, &pending) != 0) {
        job->state = 3;
        return false;
    }
    pthread_detach(t);
    return true;
}
