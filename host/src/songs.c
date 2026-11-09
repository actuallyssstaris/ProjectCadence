#include "songs.h"
#include "stars.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static int has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n > e && strcasecmp(name + n - e, ext) == 0;
}

/* inside one set: by stars once they're known, a rough guess until then */
static int cmp_diff(const void *a, const void *b)
{
    const song_entry *x = a, *y = b;
    float sx = stars_get(x->file, 0), sy = stars_get(y->file, 0);
    if (sx >= 0 && sy >= 0)
        return sx < sy ? -1 : sx > sy;
    float dx = x->info.od + x->info.ar + x->info.cs, dy = y->info.od + y->info.ar + y->info.cs;
    return dx < dy ? -1 : dx > dy;
}

static int sort_mode;

/* sets stay together whatever the order: the folder breaks every tie, song
 * select groups on it */
static int cmp(const void *a, const void *b)
{
    const song_entry *x = a, *y = b;
    int c = 0;
    switch (sort_mode) {
    case SORT_TITLE: c = strcasecmp(x->info.title, y->info.title); break;
    case SORT_CREATOR: c = strcasecmp(x->info.creator, y->info.creator); break;
    case SORT_STARS:
    case SORT_ADDED:
        /* key holds the set's hardest rating / newest first */
        c = x->key < y->key ? (sort_mode == SORT_ADDED ? 1 : -1) : x->key > y->key ? (sort_mode == SORT_ADDED ? -1 : 1) : 0;
        break;
    default: break;
    }
    if (c) return c;
    c = strcasecmp(x->info.artist, y->info.artist);
    if (c) return c;
    c = strcasecmp(x->info.title, y->info.title);
    if (c) return c;
    c = strcmp(x->dir, y->dir);
    if (c) return c;
    return cmp_diff(a, b);
}

static int by_dir(const void *a, const void *b)
{
    return strcmp(((const song_entry *)a)->dir, ((const song_entry *)b)->dir);
}

const char *songs_sort_name(int mode)
{
    static const char *names[SORT_COUNT] = {"artist", "title", "mapper", "stars", "date added"};
    return mode >= 0 && mode < SORT_COUNT ? names[mode] : "?";
}

void songs_sort(song_list *l, int mode)
{
    if (l->count < 2)
        return;
    sort_mode = mode;
    if (mode == SORT_STARS || mode == SORT_ADDED) {
        /* group first so each set can share one key */
        qsort(l->items, (size_t)l->count, sizeof(song_entry), by_dir);
        for (int s = 0, e; s < l->count; s = e) {
            float key = mode == SORT_ADDED ? (float)l->items[s].added : 0;
            for (e = s; e < l->count && !strcmp(l->items[e].dir, l->items[s].dir); e++)
                if (mode == SORT_STARS)
                    key = fmaxf(key, stars_get(l->items[e].file, 0));
            for (int i = s; i < e; i++)
                l->items[i].key = key;
        }
    }
    qsort(l->items, (size_t)l->count, sizeof(song_entry), cmp);
}

/* case-insensitive substring; the console's libc has no strcasestr */
static bool has_word(const char *s, const char *w)
{
    size_t n = strlen(w);
    for (; *s; s++)
        if (!strncasecmp(s, w, n))
            return true;
    return false;
}

/* every word of the query has to show up somewhere in the map's names */
static bool matches(const song_entry *e, const char *query)
{
    char word[64];
    const char *q = query;
    while (*q) {
        while (*q == ' ')
            q++;
        size_t n = 0;
        while (*q && *q != ' ' && n + 1 < sizeof(word))
            word[n++] = *q++;
        word[n] = 0;
        if (!n)
            break;
        const osu5_peek_info *in = &e->info;
        if (!has_word(in->title, word) && !has_word(in->artist, word) && !has_word(in->creator, word) &&
            !has_word(in->version, word))
            return false;
    }
    return true;
}

int songs_filter(const song_list *all, song_list *out, const char *query)
{
    songs_free(out);
    out->items = malloc(sizeof(song_entry) * (size_t)(all->count ? all->count : 1));
    if (!out->items)
        return 0;
    for (int i = 0; i < all->count; i++)
        if (matches(&all->items[i], query))
            out->items[out->count++] = all->items[i];
    return out->count;
}

int songs_scan(const char *root, song_list *out)
{
    songs_free(out);
    int cap = 0;

    DIR *top = opendir(root);
    if (!top)
        return 0;

    struct dirent *set;
    while ((set = readdir(top))) {
        if (set->d_name[0] == '.')
            continue;

        char dir[512];
        if (snprintf(dir, sizeof(dir), "%s/%s", root, set->d_name) >= (int)sizeof(dir))
            continue;
        DIR *d = opendir(dir);
        if (!d)
            continue;
        struct stat st;
        long added = stat(dir, &st) == 0 ? (long)st.st_mtime : 0;

        struct dirent *f;
        while ((f = readdir(d))) {
            if (!has_ext(f->d_name, ".osu"))
                continue;

            if (out->count == cap) {
                cap = cap ? cap * 2 : 64;
                song_entry *grown = realloc(out->items, cap * sizeof(song_entry));
                if (!grown)
                    break;
                out->items = grown;
            }

            song_entry *e = &out->items[out->count];
            memset(e, 0, sizeof(*e));
            snprintf(e->dir, sizeof(e->dir), "%s", dir);
            e->added = added;
            if (snprintf(e->file, sizeof(e->file), "%s/%s", dir, f->d_name) >= (int)sizeof(e->file))
                continue;

            if (osu5_peek(e->file, &e->info) == 0 && e->info.mode == 0)
                out->count++;
        }
        closedir(d);
    }
    closedir(top);

    return out->count;
}

/* .osu files name their audio and background the way Windows sees them, so
 * "Audio.MP3" has to find audio.mp3 here */
bool songs_file(const char *dir, const char *name, char *out, size_t n)
{
    if (!name[0])
        return false;
    char rel[256];
    snprintf(rel, sizeof(rel), "%s", name);
    for (char *c = rel; *c; c++)
        if (*c == '\\')
            *c = '/';
    snprintf(out, n, "%s/%s", dir, rel);
    if (access(out, F_OK) == 0)
        return true;
    if (strchr(rel, '/'))
        return false; /* subfolders: not worth a deeper search */

    DIR *d = opendir(dir);
    if (!d)
        return false;
    struct dirent *e;
    bool found = false;
    while ((e = readdir(d))) {
        if (!strcasecmp(e->d_name, rel)) {
            snprintf(out, n, "%s/%s", dir, e->d_name);
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

/* re-sorts items [from, to) after new star ratings came in */
void songs_sort_range(song_list *l, int from, int to)
{
    if (to - from > 1)
        qsort(l->items + from, (size_t)(to - from), sizeof(song_entry), cmp_diff);
}

int songs_find(const song_list *l, const char *file)
{
    for (int i = 0; i < l->count; i++)
        if (!strcmp(l->items[i].file, file))
            return i;
    return -1;
}

void songs_free(song_list *l)
{
    free(l->items);
    l->items = NULL;
    l->count = 0;
}
