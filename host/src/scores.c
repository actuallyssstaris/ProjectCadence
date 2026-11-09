#include "scores.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SCORES_FILE "/data/osu5/scores.txt"
#define SONGS_PREFIX "/data/osu5/Songs/"

typedef struct {
    char key[400];
    score_rec r;
} entry;

static entry *all;
static int count, cap;

static const char *key_of(const char *path)
{
    size_t n = strlen(SONGS_PREFIX);
    return strncmp(path, SONGS_PREFIX, n) == 0 ? path + n : path;
}

static void push(const char *key, const score_rec *r)
{
    if (count == cap) {
        int ncap = cap ? cap * 2 : 256;
        entry *grown = realloc(all, sizeof(entry) * (size_t)ncap);
        if (!grown)
            return;
        all = grown;
        cap = ncap;
    }
    snprintf(all[count].key, sizeof(all[count].key), "%s", key);
    all[count++].r = *r;
}

/* tab separated: key score acc combo rank mods 300 100 50 miss time */
void scores_load(void)
{
    count = 0;
    FILE *f = fopen(SCORES_FILE, "r");
    if (!f)
        return;
    char line[600];
    while (fgets(line, sizeof(line), f)) {
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = 0;
        score_rec r;
        long long sc, when;
        if (sscanf(tab + 1, "%lld %f %d %d %d %d %d %d %d %lld", &sc, &r.accuracy, &r.max_combo, &r.rank, &r.mods, &r.great,
                   &r.ok, &r.meh, &r.miss, &when) != 10)
            continue;
        r.score = sc;
        r.when = when;
        push(line, &r);
    }
    fclose(f);
    LOG("scores: %d loaded", count);
}

static int better(const void *a, const void *b)
{
    const score_rec *x = a, *y = b;
    return x->score < y->score ? 1 : x->score > y->score ? -1 : 0;
}

int scores_for(const char *osu_path, score_rec *out, int max)
{
    const char *key = key_of(osu_path);
    int n = 0;
    static score_rec tmp[256];
    for (int i = 0; i < count && n < 256; i++)
        if (!strcmp(all[i].key, key))
            tmp[n++] = all[i].r;
    qsort(tmp, (size_t)n, sizeof(score_rec), better);
    if (n > max)
        n = max;
    memcpy(out, tmp, sizeof(score_rec) * (size_t)n);
    return n;
}

int scores_add(const char *osu_path, const osu5_hud *h, int mods)
{
    score_rec r = {
        .score = h->score, .accuracy = h->accuracy, .max_combo = h->max_combo, .rank = h->rank, .mods = mods,
        .great = h->great, .ok = h->ok, .meh = h->meh, .miss = h->miss, .when = (int64_t)time(NULL),
    };
    const char *key = key_of(osu_path);

    int place = 0;
    for (int i = 0; i < count; i++)
        if (!strcmp(all[i].key, key) && all[i].r.score > r.score)
            place++;

    push(key, &r);

    FILE *f = fopen(SCORES_FILE, "a");
    if (f) {
        fprintf(f, "%s\t%lld %.6f %d %d %d %d %d %d %d %lld\n", key, (long long)r.score, r.accuracy, r.max_combo, r.rank, r.mods,
                r.great, r.ok, r.meh, r.miss, (long long)r.when);
        fclose(f);
    } else {
        LOG("scores: couldn't write " SCORES_FILE);
    }
    return place;
}
