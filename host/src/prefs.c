/* little per-map things: favourite sets and per-difficulty offsets, one line each */
#include "prefs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAV_FILE "/data/cadence/favourites.txt"
#define OFFSET_FILE "/data/cadence/offsets.txt"
#define MAX_ITEMS 4096

typedef struct {
    char key[768];
    int value;
} item;

static item *favs, *offs;
static int nfavs, noffs;
static bool loaded;

static int load(const char *path, item **out)
{
    *out = calloc(MAX_ITEMS, sizeof(item));
    FILE *f = fopen(path, "r");
    if (!f || !*out) {
        if (f) fclose(f);
        return 0;
    }
    int n = 0;
    char line[800];
    while (n < MAX_ITEMS && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *tab = strchr(line, '\t');
        if (tab) {
            *tab = 0;
            (*out)[n].value = atoi(tab + 1);
        }
        if (line[0])
            snprintf((*out)[n++].key, sizeof((*out)[0].key), "%s", line);
    }
    fclose(f);
    return n;
}

static void save(const char *path, const item *it, int n, bool values)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    for (int i = 0; i < n; i++)
        values ? fprintf(f, "%s\t%d\n", it[i].key, it[i].value) : fprintf(f, "%s\n", it[i].key);
    fclose(f);
}

static void ensure(void)
{
    if (loaded)
        return;
    loaded = true;
    nfavs = load(FAV_FILE, &favs);
    noffs = load(OFFSET_FILE, &offs);
}

static int find(const item *it, int n, const char *key)
{
    for (int i = 0; it && i < n; i++)
        if (!strcmp(it[i].key, key))
            return i;
    return -1;
}

bool fav_is(const char *set_dir)
{
    ensure();
    return find(favs, nfavs, set_dir) >= 0;
}

bool fav_toggle(const char *set_dir)
{
    ensure();
    if (!favs)
        return false;
    int i = find(favs, nfavs, set_dir);
    if (i >= 0)
        favs[i] = favs[--nfavs];
    else if (nfavs < MAX_ITEMS)
        snprintf(favs[nfavs++].key, sizeof(favs[0].key), "%s", set_dir);
    save(FAV_FILE, favs, nfavs, false);
    return i < 0;
}

int offsets_get(const char *file)
{
    ensure();
    int i = find(offs, noffs, file);
    return i >= 0 ? offs[i].value : 0;
}

void offsets_set(const char *file, int ms)
{
    ensure();
    if (!offs)
        return;
    int i = find(offs, noffs, file);
    if (i < 0) {
        if (ms == 0 || noffs >= MAX_ITEMS)
            return;
        i = noffs++;
        snprintf(offs[i].key, sizeof(offs[0].key), "%s", file);
    }
    offs[i].value = ms;
    if (ms == 0)
        offs[i] = offs[--noffs];
    save(OFFSET_FILE, offs, noffs, true);
}
