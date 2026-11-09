#ifndef OSU5_PREFS_H
#define OSU5_PREFS_H

#include <stdbool.h>

bool fav_is(const char *set_dir);
bool fav_toggle(const char *set_dir); /* true if it's a favourite now */

int offsets_get(const char *osu_file);
void offsets_set(const char *osu_file, int ms);

#endif
