#include "songs.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n > e && strcasecmp(name + n - e, ext) == 0;
}

static int cmp(const void *a, const void *b)
{
    const song_entry *x = a, *y = b;
    int c = strcasecmp(x->info.artist, y->info.artist);
    if (c) return c;
    c = strcasecmp(x->info.title, y->info.title);
    if (c) return c;
    /* same song: rough difficulty order */
    float dx = x->info.od + x->info.ar + x->info.cs, dy = y->info.od + y->info.ar + y->info.cs;
    return dx < dy ? -1 : dx > dy;
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
            if (snprintf(e->file, sizeof(e->file), "%s/%s", dir, f->d_name) >= (int)sizeof(e->file))
                continue;

            if (osu5_peek(e->file, &e->info) == 0 && e->info.mode == 0)
                out->count++;
        }
        closedir(d);
    }
    closedir(top);

    if (out->count > 1)
        qsort(out->items, out->count, sizeof(song_entry), cmp);
    return out->count;
}

void songs_free(song_list *l)
{
    free(l->items);
    l->items = NULL;
    l->count = 0;
}
