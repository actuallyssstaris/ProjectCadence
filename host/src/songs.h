#ifndef SONGS_H
#define SONGS_H

#include "../include/osu5.h"

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

#endif
