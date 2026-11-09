#ifndef SONGS_H
#define SONGS_H

#include "../include/osu5.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char dir[512];    /* set folders are often "id artist - title", long ones aren't rare */
    char file[768];   /* full path to the .osu */
    osu5_peek_info info;
} song_entry;

typedef struct {
    song_entry *items;
    int count;
} song_list;

int songs_scan(const char *root, song_list *out);
void songs_free(song_list *l);

/* dir/name, matched case-insensitively if the exact name isn't there */
bool songs_file(const char *dir, const char *name, char *out, size_t n);
int songs_find(const song_list *l, const char *file); /* index or -1 */

#endif
