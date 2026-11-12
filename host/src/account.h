#ifndef OSU5_ACCOUNT_H
#define OSU5_ACCOUNT_H

#include "replay.h"

#include <stdbool.h>
#include <stdint.h>

/* A ProjectCadence account on our server: global scores and rankings, replays,
 * friends, clans, and your name in multiplayer. The session is kept in
 * /data/cadence/account.txt. */

#define GLOBAL_MAX 50

typedef struct {
    char name[24];
    long long score;
    float acc, pp;
    int combo, mods, rank;
    bool replay;
} global_score;

typedef struct {
    char md5[33];
    int count;
    int status; /* osu!'s ranked status as the server knows it, -99 unknown */
    global_score s[GLOBAL_MAX];
} global_board;

typedef struct {
    char name[24], clan[8];
    int rank, pp, plays;
    float acc;
} cadence_rank;

typedef struct {
    int state; /* ONLINE_* */
    int mode, page, count, total;
    cadence_rank r[GLOBAL_MAX];
} cadence_rankings;

typedef struct {
    int state, mode;
    char name[24], clan[8], clan_name[32];
    int rank, pp, plays, max_combo;
    float acc;
    long long ranked_score, total_score, joined_ms;
    bool friend_, restricted;
    int ntop;
    struct {
        char title[96];
        float acc, pp;
        int combo, mods, rank;
    } top[10];
} cadence_user;

enum { ACC_IDLE, ACC_BUSY, ACC_OK, ACC_FAILED };

void account_init(const char *server);
bool account_signed_in(void);
const char *account_name(void);
const char *account_session(void);
const char *account_clan(void); /* your clan's tag, "" none */

/* background; account_state() says how it went, with a message when it failed */
void account_sign_in(const char *name, const char *password, bool create);
void account_sign_out(void);
int account_state(char *msg, int n);

/* sends a finished play with its replay; the place it got comes back through account_last_place */
void account_submit(const char *md5, int beatmap_id, int mode, long long score, float acc, int max_combo, int mods,
                    float pp, int rank, int n300, int n100, int n50, int miss, const replay *rp);
int account_last_place(void); /* 0 none yet / not a best */

/* the top 50 for a map: ONLINE_* states from online.h */
int account_board(const char *md5, global_board *out);

/* someone's replay on a map; poll take until it stops saying ONLINE_LOADING */
void account_replay_get(const char *md5, const char *player, int mode, const char *osu_path);
int account_replay_take(replay *out);

void account_rankings_get(int mode, int page);
void account_rankings_read(cadence_rankings *out);
void account_profile_get(const char *name, int mode);
void account_profile_read(cadence_user *out);

/* account changes, one at a time; account_action() says how the last went */
void account_friend(const char *name, bool add);
void account_clan_join(const char *tag);
void account_clan_create(const char *tag, const char *name);
void account_clan_leave(void);
void account_password(const char *old_pw, const char *new_pw);
void account_delete(const char *password);
int account_action(char *msg, int n); /* ACC_* */

#endif
