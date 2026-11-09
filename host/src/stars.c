#include "stars.h"
#include "../include/osu5.h"
#include "log.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STARS_FILE "/data/cadence/stars.txt"
#define SONGS_PREFIX "/data/cadence/Songs/"
/* bump with the core's StarRating.Version so old numbers get thrown away */
#define STARS_HEADER "osu5 stars 20260706"

#define SLOTS 32768 /* power of two */
#define QUEUE SLOTS

enum { EMPTY, PENDING, DONE };

typedef struct {
    char *key; /* path under Songs/ */
    int mods;
    int state;
    float stars;
} slot;

static slot table[SLOTS];
static int used;

/* deque of slot indices; urgent requests go in at the front */
static int queue[QUEUE];
static int qhead, qcount;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static int generation;
static bool started;

static const char *key_of(const char *path)
{
    size_t n = strlen(SONGS_PREFIX);
    return strncmp(path, SONGS_PREFIX, n) == 0 ? path + n : path;
}

static uint32_t hash(const char *s, int mods)
{
    uint32_t h = 2166136261u ^ (uint32_t)mods;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

/* finds or makes the slot; NULL once the table is nearly full */
static slot *find(const char *key, int mods, bool create)
{
    uint32_t i = hash(key, mods) & (SLOTS - 1);
    for (int probe = 0; probe < SLOTS; probe++, i = (i + 1) & (SLOTS - 1)) {
        slot *s = &table[i];
        if (!s->key) {
            if (!create || used > SLOTS * 3 / 4)
                return NULL;
            s->key = strdup(key);
            if (!s->key)
                return NULL;
            s->mods = mods;
            s->state = EMPTY;
            used++;
            return s;
        }
        if (s->mods == mods && !strcmp(s->key, key))
            return s;
    }
    return NULL;
}

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&lock);
        while (qcount == 0)
            pthread_cond_wait(&wake, &lock);
        slot *s = &table[queue[qhead]];
        qhead = (qhead + 1) % QUEUE;
        qcount--;
        char path[768];
        snprintf(path, sizeof(path), SONGS_PREFIX "%s", s->key);
        int mods = s->mods;
        pthread_mutex_unlock(&lock);

        float v = osu5_stars(path, mods);

        pthread_mutex_lock(&lock);
        s->stars = v < 0 ? -2 : v;
        s->state = DONE;
        generation++;
        FILE *f = fopen(STARS_FILE, "a");
        if (f) {
            fprintf(f, "%d\t%.4f\t%s\n", mods, s->stars, s->key);
            fclose(f);
        }
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

void stars_init(void)
{
    FILE *f = fopen(STARS_FILE, "r");
    char line[900];
    bool current = f && fgets(line, sizeof(line), f) && !strncmp(line, STARS_HEADER, strlen(STARS_HEADER));
    int n = 0;
    while (current && fgets(line, sizeof(line), f)) {
        char *a = strchr(line, '\t'), *b = a ? strchr(a + 1, '\t') : NULL;
        if (!b)
            continue;
        *a = *b = 0;
        b[1 + strcspn(b + 1, "\r\n")] = 0;
        slot *s = find(b + 1, atoi(line), true);
        if (s) {
            s->stars = (float)atof(a + 1);
            s->state = DONE;
            n++;
        }
    }
    if (f)
        fclose(f);
    if (!current) {
        /* first run, or numbers from another calculator version */
        f = fopen(STARS_FILE, "w");
        if (f) {
            fprintf(f, STARS_HEADER "\n");
            fclose(f);
        }
    }
    LOG("stars: %d cached", n);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024 * 1024); /* managed code runs on it */
    pthread_t t;
    started = pthread_create(&t, &attr, worker, NULL) == 0;
    if (started)
        pthread_detach(t);
    pthread_attr_destroy(&attr);
}

float stars_get(const char *osu_path, int mods)
{
    pthread_mutex_lock(&lock);
    slot *s = find(key_of(osu_path), mods, false);
    float v = s && s->state == DONE ? s->stars : -1;
    pthread_mutex_unlock(&lock);
    return v;
}

void stars_want(const char *osu_path, int mods, bool urgent)
{
    if (!started)
        return;
    pthread_mutex_lock(&lock);
    slot *s = find(key_of(osu_path), mods, true);
    if (s && s->state == EMPTY && qcount < QUEUE) {
        int idx = (int)(s - table);
        if (urgent) {
            qhead = (qhead + QUEUE - 1) % QUEUE;
            queue[qhead] = idx;
        } else {
            queue[(qhead + qcount) % QUEUE] = idx;
        }
        qcount++;
        s->state = PENDING;
        pthread_cond_signal(&wake);
    } else if (s && s->state == PENDING && urgent && qcount > 1) {
        /* already waiting somewhere in the back: pull it to the front */
        int idx = (int)(s - table);
        for (int i = 1; i < qcount; i++) {
            int at = (qhead + i) % QUEUE;
            if (queue[at] == idx) {
                for (int j = i; j > 0; j--)
                    queue[(qhead + j) % QUEUE] = queue[(qhead + j - 1) % QUEUE];
                queue[qhead] = idx;
                break;
            }
        }
    }
    pthread_mutex_unlock(&lock);
}

int stars_generation(void)
{
    pthread_mutex_lock(&lock);
    int g = generation;
    pthread_mutex_unlock(&lock);
    return g;
}

int stars_mods(int mods)
{
    return mods & (OSU5_MOD_EASY | OSU5_MOD_HARDROCK | OSU5_MOD_HIDDEN | OSU5_MOD_DOUBLETIME | OSU5_MOD_HALFTIME);
}
