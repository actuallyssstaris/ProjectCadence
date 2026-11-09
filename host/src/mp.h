#ifndef OSU5_MP_H
#define OSU5_MP_H

#include <stdbool.h>
#include <stdint.h>

/* Multiplayer rooms, from either ProjectCadence's own room server or a bancho
 * (osu! private server, e.g. bancho.py). Network work runs on a worker; the
 * game reads a snapshot each frame and queues commands. */

enum { MP_CADENCE, MP_BANCHO };
enum { MP_OFF, MP_CONNECTING, MP_ONLINE, MP_FAILED };
enum { MP_LOBBY, MP_PLAYING, MP_RESULTS };

#define MP_MAX_ROOMS 40
#define MP_MAX_MEMBERS 16

typedef struct {
    int id;
    char name[32];
    bool ready, has_map, done, failed, host, me;
    int score, combo, max_combo, n300, n100, n50, miss;
    float acc;
    int team;           /* 0 red, 1 blue */
    int mods;           /* their own, when the room has free mods */
    bool account;       /* a signed-in name */
} mp_member;

typedef struct {
    int id;
    char name[48];
    int players;
    bool playing, locked;
    char map[96];
} mp_room_brief;

typedef struct {
    int conn;
    int online;         /* players connected to the server, -1 unknown */
    char error[160];
    char me[32];

    int nrooms;
    mp_room_brief rooms[MP_MAX_ROOMS];

    bool in_room;
    int room_id;
    char room_name[48];
    bool i_host;
    int state;
    int round;          /* goes up with every start */
    double go_at;       /* wall ms the round's music should start, 0 not yet */
    int map_id, map_set;
    char map_md5[33];
    char map_title[128];
    int mods;           /* ours (OSU5_MOD_*) */
    bool teams, freemod;
    int nchat;
    struct { int id; char from[24]; char text[124]; } chat[40];
    int nmembers;
    mp_member members[MP_MAX_MEMBERS];
} mp_state;

/* server: the room server's base URL for MP_CADENCE, the domain for MP_BANCHO */
void mp_connect(int kind, const char *server, const char *user, const char *password);
void mp_disconnect(void);
void mp_snapshot(mp_state *out);

void mp_refresh(void);
void mp_create(const char *name, const char *password);
void mp_join(int room_id, const char *password);
void mp_settings(bool teams, bool freemod); /* host */
void mp_team(int team);
void mp_my_mods(int mods);
void mp_chat(const char *text);
void mp_leave(void);
void mp_set_map(int beatmap_id, int set_id, int mode, const char *md5, const char *title, int mods);
void mp_ready(bool ready, bool has_map);
void mp_start(void);
/* during a round, every second or so; done / failed at the end */
void mp_score(long long score, int combo, int max_combo, float acc, int n300, int n100, int n50, int miss,
              bool failed, bool done);

/* the room server's player count, refreshed now and then in the background; -1 unknown */
int mp_peek_online(int kind, const char *address);

#endif
