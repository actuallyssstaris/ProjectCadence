#ifndef OSU5_ACCOUNT_H
#define OSU5_ACCOUNT_H

#include <stdbool.h>
#include <stdint.h>

/* A ProjectCadence account on our server: global scores per map, and your name
 * in multiplayer. The session is kept in /data/cadence/account.txt. */

#define GLOBAL_MAX 50

typedef struct {
    char name[24];
    long long score;
    float acc, pp;
    int combo, mods, rank;
} global_score;

typedef struct {
    char md5[33];
    int count;
    global_score s[GLOBAL_MAX];
} global_board;

enum { ACC_IDLE, ACC_BUSY, ACC_OK, ACC_FAILED };

void account_init(const char *server);
bool account_signed_in(void);
const char *account_name(void);
const char *account_session(void);

/* background; account_state() says how it went, with a message when it failed */
void account_sign_in(const char *name, const char *password, bool create);
void account_sign_out(void);
int account_state(char *msg, int n);

/* sends a finished play; the place it got comes back through account_last_place */
void account_submit(const char *md5, int beatmap_id, int mode, long long score, float acc, int max_combo, int mods,
                    float pp, int rank, int n300, int n100, int n50, int miss);
int account_last_place(void); /* 0 none yet / not a best */

/* the top 50 for a map: ONLINE_* states from online.h */
int account_board(const char *md5, global_board *out);

#endif
