#ifndef OSU5_WEB_H
#define OSU5_WEB_H

#include <stdbool.h>

/* osu.ppy.sh rankings and player profiles, through online.c's API keys.
 * Each fetch runs on its own thread; poll the getters every frame. */

enum { WEB_IDLE, WEB_LOADING, WEB_READY, WEB_FAILED };

#define WEB_RANKS 50

typedef struct {
    int id;
    char user[32];
    char country[4];
    int rank, plays;
    float pp, acc;
} web_rank;

typedef struct {
    int state, mode, page;
    int count;
    web_rank r[WEB_RANKS];
} web_rankings;

typedef struct {
    int state, mode;
    int id;
    char user[32], country[4], joined[12];
    int rank, country_rank, plays, max_combo, level, level_pct;
    int hours;
    float pp, acc;
    long long ranked_score, total_score;
    int ss, s, a;
    bool supporter;
} web_user;

void web_rankings_get(int mode, int page);
void web_rankings_read(web_rankings *out);

/* who: a user id, or a name */
void web_user_get(const char *who, int mode);
void web_user_read(web_user *out);

#endif
