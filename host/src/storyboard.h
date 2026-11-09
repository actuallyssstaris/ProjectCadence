#ifndef OSU5_STORYBOARD_H
#define OSU5_STORYBOARD_H

#include <stdbool.h>
#include "../include/osu5.h"

/* osu! storyboards: sprites and animations from the map's .osb and its [Events],
 * with F/M/MX/MY/S/V/R/C/P commands, loops, triggers and the pass/fail layers.
 * Samples and videos are skipped. */

typedef struct storyboard storyboard;

enum { SB_UNDER, SB_OVER }; /* below the hit objects, or the overlay layer above */

/* NULL when the map has no storyboard (or nothing in it loads) */
storyboard *sb_load(const char *dir, const char *osu_path, const char *background);
void sb_draw(storyboard *sb, double time, int which, float dim);
/* the storyboard draws the map's background itself, so the plain one stays off */
bool sb_replaces_bg(const storyboard *sb);
/* gameplay events for T groups */
void sb_hitsound(storyboard *sb, const osu5_sound *ev, double time);
void sb_set_failing(storyboard *sb, bool failing, double time);
void sb_reset(storyboard *sb); /* restart / retry */
void sb_free(storyboard *sb);

#endif
