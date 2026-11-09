#ifndef OSU5_STARS_H
#define OSU5_STARS_H

#include <stdbool.h>

/* Star ratings, worked out by the core on a background thread and kept in
 * /data/osu5/stars.txt so each map is only ever calculated once per mod set. */

void stars_init(void);

/* -1 not known yet, -2 couldn't be calculated */
float stars_get(const char *osu_path, int mods);

/* ask for it; urgent ones jump the queue (what's on screen) */
void stars_want(const char *osu_path, int mods, bool urgent);

/* goes up whenever new ratings arrive */
int stars_generation(void);

/* the mods that change star rating, out of a full mod set */
int stars_mods(int mods);

#endif
