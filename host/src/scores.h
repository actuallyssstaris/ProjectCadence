#ifndef OSU5_SCORES_H
#define OSU5_SCORES_H

#include "../include/osu5.h"
#include <stdbool.h>
#include <stdint.h>

/* Local scores, one line each in /data/cadence/scores.txt, keyed by the .osu path
 * relative to Songs/. */

typedef struct {
    int64_t score;
    float accuracy;
    int max_combo;
    int rank;
    int mods;
    int great, ok, meh, miss;
    int64_t when; /* unix time, also names the replay */
    float pp;
} score_rec;

void scores_load(void);

/* returns the new score's place for this map, 0 = best; *when gets its timestamp */
int scores_add(const char *osu_path, const osu5_hud *hud, int mods, float pp, int64_t *when);

/* best first; returns how many were written */
int scores_for(const char *osu_path, score_rec *out, int max);

#endif
