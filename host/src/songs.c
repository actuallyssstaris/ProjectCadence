#include "songs.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

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
    /* keeps each set's difficulties together, song select groups on the folder */
    c = strcmp(x->dir, y->dir);
    if (c) return c;
    /* same set: rough difficulty order */
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
