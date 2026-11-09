#ifndef OSU5_ONLINE_H
#define OSU5_ONLINE_H

#include <stdbool.h>

/* osu.ppy.sh leaderboards, read only. Needs /data/cadence/osuapi.txt with an
 * OAuth client id on the first line and its secret on the second. */

#define ONLINE_MAX 50

typedef struct {
    char user[32];
    int rank; /* OSU5_RANK_* */
    char mods[32];
    long long score;
    float pp, accuracy;
    int combo;
} online_score;

typedef struct {
    int beatmap_id;
    int count;
    online_score s[ONLINE_MAX];
} online_board;

enum { ONLINE_OFF, ONLINE_LOADING, ONLINE_READY, ONLINE_FAILED };

void online_init(void);
bool online_enabled(void);

/* fetches in the background if it isn't cached; returns one of the above and
 * fills out once it's READY */
int online_board_for(int beatmap_id, online_board *out);

#endif
