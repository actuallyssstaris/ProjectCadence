#ifndef SONGS_H
#define SONGS_H

#include "../include/osu5.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char dir[512];    /* set folders are often "id artist - title", long ones aren't rare */
    char file[768];   /* full path to the .osu */
    osu5_peek_info info;
    long added;       /* folder mtime */
    float key;        /* scratch for sorting */
    bool video;       /* the set folder has a video file */
} song_entry;

typedef struct {
    song_entry *items;
    int count;
} song_list;

enum { SORT_ARTIST, SORT_TITLE, SORT_CREATOR, SORT_STARS, SORT_ADDED, SORT_COUNT };

/* unsorted; follow with songs_sort */
int songs_scan(const char *root, song_list *out);
void songs_free(song_list *l);
void songs_sort(song_list *l, int mode);
const char *songs_sort_name(int mode);

/* copies the entries of all matching every word of query into out */
int songs_filter(const song_list *all, song_list *out, const char *query);

/* dir/name, matched case-insensitively if the exact name isn't there */
bool songs_file(const char *dir, const char *name, char *out, size_t n);
int songs_find(const song_list *l, const char *file); /* index or -1 */
/* re-sorts the difficulties in [from, to), one set, after new star ratings */
void songs_sort_range(song_list *l, int from, int to);

#endif
