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
    int slot;           /* bancho: which of the 16 slots */
} mp_member;

typedef struct {
    int id;
    char name[48];
    int players;
    bool playing, locked;
    char map[96];
} mp_room_brief;

/* one chat line, oldest first in the arrays below */
typedef struct {
    int id;
    char from[24];
    char text[124];
    char to[32];        /* bancho: the channel, or the other person in a DM */
} mp_line;

#define MP_MAX_CHANNELS 16
#define MP_MAX_USERS 64

typedef struct {
    char name[32];      /* "#osu", or a player's name for a DM */
    char topic[64];
    int users;
    int unread;
} mp_channel;

/* what bancho says about a player once we've asked */
typedef struct {
    int id;
    char name[32];
    int action;         /* 0 idle, 2 playing, ... as osu! numbers them */
    char doing[80];     /* "Artist - Title [Diff]" while playing */
    char md5[33];
    int mods, mode, map_id;
    long long ranked_score, total_score;
    float acc;          /* 0-1 */
    int plays, rank, pp;
    bool friend_, online;
} mp_user;

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
    int win;            /* bancho: 0 score, 1 accuracy, 2 combo, 3 score v2 */
    int nchat;
    mp_line chat[40];
    int nlobby;         /* the server-wide lobby channel, ProjectCadence server only */
    mp_line lobby[40];

    /* bancho only: channels and DMs, their messages (newest last), players */
    int nchannels;
    mp_channel channels[MP_MAX_CHANNELS];
    int nmsgs;
    mp_line msgs[120];
    int nusers;
    mp_user users[MP_MAX_USERS];
    int spectating;     /* the user id we're watching, 0 none */
    int spectators;     /* people watching us */
    int my_slot;        /* our slot in the match */
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
void mp_lobby_chat(const char *text);

/* bancho: say something in a channel ("#osu") or to a player (their name) */
void mp_say(const char *to, const char *text);
void mp_channel_join(const char *name);
void mp_channel_leave(const char *name);
void mp_channel_seen(const char *name);
/* asks for a player's stats; they show up in mp_state.users */
void mp_user_stats(int user_id);
void mp_user_lookup(const char *name); /* by name, from the presence list */
void mp_friend(int user_id, bool add);
/* host tools for the match */
void mp_host_to(int slot);
void mp_lock_slot(int slot);
void mp_move_slot(int slot);
void mp_win_condition(int win); /* 0 score, 1 accuracy, 2 combo, 3 score v2 */
/* what we're up to, for everyone's player list */
void mp_status(int action, const char *text, const char *md5, int mods, int mode, int map_id);

/* spectating someone: their frames pile up until taken. action is osu!'s
 * replay action from the last batch (1 new song, 3 done, 4 failed, 7 back in song select) */
void mp_spectate(int user_id); /* 0 stops */
typedef struct { double t; float x, y; int keys; int raw; } mp_spec_frame; /* raw: keys already ours */
int mp_spec_take(mp_spec_frame *out, int max, int *action);
/* our own play, for whoever spectates us on the ProjectCadence server */
void mp_frames_push(double t, float x, float y, int keys);

/* our mods to osu!'s bits and back */
uint32_t mp_mods_to_osu(int mods);
int mp_mods_from_osu(uint32_t m);
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
