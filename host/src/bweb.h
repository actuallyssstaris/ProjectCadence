#ifndef OSU5_BWEB_H
#define OSU5_BWEB_H

#include "replay.h"

#include <stdbool.h>
#include <stdint.h>

/* The web half of a bancho server (bancho.py and the like): its leaderboards,
 * score submission and replays, the calls osu! stable makes to osu.<domain>/web/.
 * Only works while mp.c holds a bancho session, since the server checks we're online. */

#define BWEB_MAX 50

typedef struct {
    int id;
    char user[32];
    long long score;
    int combo, n300, n100, n50, miss;
    int mods; /* OSU5_MOD_* */
    int rank; /* OSU5_RANK_* */
    float acc;
} bweb_score;

typedef struct {
    char md5[33];
    int status; /* the server's ranked status, -1 not submitted */
    int count;
    bweb_score s[BWEB_MAX];
    bool has_mine;
    bweb_score mine;
} bweb_board;

typedef struct {
    char md5[33];
    char osu_name[256]; /* the .osu file's name */
    int mode;           /* 0-3 */
    int n300, n100, n50, geki, katu, miss;
    long long score;
    int max_combo;
    bool perfect, passed;
    int rank;           /* OSU5_RANK_* */
    int mods;           /* OSU5_MOD_* */
    int play_ms, fail_ms;
    const replay *rp;   /* copied into the request */
} bweb_play;

/* from mp.c's login and logout */
void bweb_session(const char *domain, const char *user, const char *pass_md5, const char *version,
                  const char *client_hash, const char *uid1, const char *uid2, const char *token);
void bweb_end(void);
bool bweb_on(void);

/* ONLINE_* states from online.h; fetches in the background */
int bweb_board_for(const char *md5, const char *osu_name, int set_id, int mode, bweb_board *out);
void bweb_forget(const char *md5); /* after a submit, so the board refreshes */

void bweb_submit(const bweb_play *p);
/* 0 nothing yet, 1 sending, 2 done, 3 failed; msg says what the server answered */
int bweb_submit_state(char *msg, int n);

/* downloads a score's replay; poll take until it stops saying ONLINE_LOADING */
void bweb_replay_get(int score_id, int mode, const char *osu_path);
int bweb_replay_take(replay *out);

#endif
