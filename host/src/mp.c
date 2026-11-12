/* Multiplayer: one worker thread talks to either ProjectCadence's room server
 * (JSON, server/cloudflare) or a bancho server (osu!'s binary packets, as
 * bancho.py speaks them). Both end up in the same mp_state the game draws. */
#include "mp.h"
#include "json.h"
#include "log.h"
#include "md5.h"
#include "net.h"
#include "online.h"
#include "account.h"
#include "bweb.h"
#include "../include/osu5.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static bool started, want_on;
static mp_state st;
static int kind;
static char server[160], user[32], pass[64];

enum { OP_REFRESH, OP_CREATE, OP_JOIN, OP_LEAVE, OP_MAP, OP_READY, OP_START, OP_SCORE, OP_SETTINGS, OP_TEAM, OP_MODS, OP_CHAT, OP_LOBBY_CHAT,
       OP_SAY, OP_CHAN_JOIN, OP_CHAN_PART, OP_STATS, OP_FRIEND, OP_HOST, OP_LOCK, OP_SLOT, OP_WIN, OP_STATUS,
       OP_SPECTATE, OP_LOOKUP };

typedef struct {
    int op;
    int a, b, mods, mode;
    char s1[128], s2[40], md5[33];
    bool f1, f2;
    long long score;
    int combo, max_combo, n300, n100, n50, miss;
    float acc;
} cmd;

#define QMAX 24
static cmd queue[QMAX];
static int queued;

static double wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void push(const cmd *c)
{
    pthread_mutex_lock(&lock);
    /* only the newest running score matters; finished / failed ones always go */
    if (c->op == OP_SCORE && !c->f1 && !c->f2)
        for (int i = 0; i < queued; i++)
            if (queue[i].op == OP_SCORE && !queue[i].f1 && !queue[i].f2) {
                queue[i] = *c;
                pthread_mutex_unlock(&lock);
                return;
            }
    if (queued < QMAX)
        queue[queued++] = *c;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
}

static void fail(const char *msg)
{
    pthread_mutex_lock(&lock);
    snprintf(st.error, sizeof(st.error), "%s", msg);
    pthread_mutex_unlock(&lock);
    LOG("mp: %s", msg);
}

/* spectating, either server: their frames until the game takes them, ours until sent */
static mp_spec_frame spec[4096];
static int nspec, spec_action, watching;
static mp_spec_frame outgoing[4096];
static int noutgoing;
static long spec_seq;

static void spec_add(double t, float x, float y, int keys, int raw)
{
    if (nspec == (int)(sizeof(spec) / sizeof(spec[0]))) {
        memmove(spec, spec + 1024, sizeof(spec[0]) * (size_t)(nspec - 1024));
        nspec -= 1024;
    }
    spec[nspec++] = (mp_spec_frame){t, x, y, keys, raw};
}

/* ================= ProjectCadence room server ================= */

static char cd_token[64];
static int cd_me;
static double cd_offset; /* server clock minus ours, ms */

typedef struct {
    bool has;
    int id, host, mods, round;
    bool teams, freemod;
    int nchat;
    mp_line chat[40];
    int win;
    double start_at;
    char name[48], state[16];
    int map_id, map_set;
    char md5[33], title[128];
    int n;
    mp_member m[MP_MAX_MEMBERS];
} cd_room;

typedef struct {
    double now;
    int online;
    char token[64], error[128];
    int id;
    bool got_rooms;
    int nrooms;
    mp_room_brief rooms[MP_MAX_ROOMS];
    bool got_room;
    cd_room room;
    bool got_lobby;
    int nlobby;
    mp_line lobby[40];
    int watchers;
} cd_reply;

static bool jbool(js *j)
{
    bool v = !strncmp(j->p, "true", 4);
    json_skip(j);
    return v;
}

static bool member_key(js *j, const char *key, void *ctx)
{
    mp_member *m = ctx;
    if (!strcmp(key, "id")) m->id = (int)json_num(j);
    else if (!strcmp(key, "name")) return json_str(j, m->name, sizeof(m->name));
    else if (!strcmp(key, "ready")) m->ready = jbool(j);
    else if (!strcmp(key, "has_map")) m->has_map = jbool(j);
    else if (!strcmp(key, "failed")) m->failed = jbool(j);
    else if (!strcmp(key, "done")) m->done = jbool(j);
    else if (!strcmp(key, "score")) m->score = (int)json_num(j);
    else if (!strcmp(key, "combo")) m->combo = (int)json_num(j);
    else if (!strcmp(key, "max_combo")) m->max_combo = (int)json_num(j);
    else if (!strcmp(key, "n300")) m->n300 = (int)json_num(j);
    else if (!strcmp(key, "n100")) m->n100 = (int)json_num(j);
    else if (!strcmp(key, "n50")) m->n50 = (int)json_num(j);
    else if (!strcmp(key, "miss")) m->miss = (int)json_num(j);
    else if (!strcmp(key, "acc")) m->acc = (float)json_num(j);
    else if (!strcmp(key, "team")) m->team = (int)json_num(j);
    else if (!strcmp(key, "mods")) m->mods = (int)json_num(j);
    else if (!strcmp(key, "account")) m->account = jbool(j);
    else return false;
    return true;
}

static bool map_key(js *j, const char *key, void *ctx)
{
    cd_room *r = ctx;
    if (!strcmp(key, "beatmap_id")) r->map_id = (int)json_num(j);
    else if (!strcmp(key, "set_id")) r->map_set = (int)json_num(j);
    else if (!strcmp(key, "md5")) return json_str(j, r->md5, sizeof(r->md5));
    else if (!strcmp(key, "title")) return json_str(j, r->title, sizeof(r->title));
    else return false;
    return true;
}

static bool chat_key(js *j, const char *key, void *ctx)
{
    mp_line *c = ctx;
    if (!strcmp(key, "id")) c->id = (int)json_num(j);
    else if (!strcmp(key, "from")) return json_str(j, c->from, sizeof(c->from));
    else if (!strcmp(key, "text")) return json_str(j, c->text, sizeof(c->text));
    else return false;
    return true;
}

static bool read_chat(js *j, mp_line *out, int *n)
{
    j->p++;
    for (;;) {
        json_ws(j);
        if (*j->p == ']') {
            j->p++;
            return true;
        }
        mp_line c;
        memset(&c, 0, sizeof(c));
        if (!json_object(j, chat_key, &c))
            return false;
        if (*n < 40)
            out[(*n)++] = c;
        json_ws(j);
        if (*j->p == ',')
            j->p++;
    }
}

static bool room_key(js *j, const char *key, void *ctx)
{
    cd_room *r = ctx;
    if (!strcmp(key, "id")) r->id = (int)json_num(j);
    else if (!strcmp(key, "host")) r->host = (int)json_num(j);
    else if (!strcmp(key, "mods")) r->mods = (int)json_num(j);
    else if (!strcmp(key, "round")) r->round = (int)json_num(j);
    else if (!strcmp(key, "teams")) r->teams = jbool(j);
    else if (!strcmp(key, "freemod")) r->freemod = jbool(j);
    else if (!strcmp(key, "chat") && *j->p == '[') return read_chat(j, r->chat, &r->nchat);
    else if (!strcmp(key, "win")) r->win = (int)json_num(j);
    else if (!strcmp(key, "start_at")) r->start_at = json_num(j);
    else if (!strcmp(key, "name")) return json_str(j, r->name, sizeof(r->name));
    else if (!strcmp(key, "state")) return json_str(j, r->state, sizeof(r->state));
    else if (!strcmp(key, "map") && *j->p == '{') return json_object(j, map_key, r);
    else if (!strcmp(key, "members") && *j->p == '[') {
        j->p++;
        for (;;) {
            json_ws(j);
            if (*j->p == ']') {
                j->p++;
                return true;
            }
            mp_member m;
            memset(&m, 0, sizeof(m));
            if (!json_object(j, member_key, &m))
                return false;
            if (r->n < MP_MAX_MEMBERS)
                r->m[r->n++] = m;
            json_ws(j);
            if (*j->p == ',')
                j->p++;
        }
    } else
        return false;
    return true;
}

static bool brief_key(js *j, const char *key, void *ctx)
{
    mp_room_brief *b = ctx;
    if (!strcmp(key, "id")) b->id = (int)json_num(j);
    else if (!strcmp(key, "players")) b->players = (int)json_num(j);
    else if (!strcmp(key, "name")) return json_str(j, b->name, sizeof(b->name));
    else if (!strcmp(key, "map") && *j->p == '"') return json_str(j, b->map, sizeof(b->map));
    else if (!strcmp(key, "state")) {
        char s[16];
        if (!json_str(j, s, sizeof(s))) return false;
        b->playing = !strcmp(s, "playing");
    } else if (!strcmp(key, "locked")) {
        b->locked = jbool(j);
    } else
        return false;
    return true;
}

static bool reply_key(js *j, const char *key, void *ctx)
{
    cd_reply *r = ctx;
    if (!strcmp(key, "now")) r->now = json_num(j);
    else if (!strcmp(key, "watchers")) r->watchers = (int)json_num(j);
    else if (!strcmp(key, "online")) r->online = (int)json_num(j);
    else if (!strcmp(key, "id")) r->id = (int)json_num(j);
    else if (!strcmp(key, "token")) return json_str(j, r->token, sizeof(r->token));
    else if (!strcmp(key, "error")) return json_str(j, r->error, sizeof(r->error));
    else if (!strcmp(key, "room")) {
        r->got_room = true;
        if (*j->p != '{')
            return false; /* null: not in a room */
        r->room.has = true;
        return json_object(j, room_key, &r->room);
    } else if (!strcmp(key, "lobby") && *j->p == '[') {
        r->got_lobby = true;
        return read_chat(j, r->lobby, &r->nlobby);
    } else if (!strcmp(key, "rooms") && *j->p == '[') {
        r->got_rooms = true;
        j->p++;
        for (;;) {
            json_ws(j);
            if (*j->p == ']') {
                j->p++;
                return true;
            }
            mp_room_brief b;
            memset(&b, 0, sizeof(b));
            if (!json_object(j, brief_key, &b))
                return false;
            if (r->nrooms < MP_MAX_ROOMS)
                r->rooms[r->nrooms++] = b;
            json_ws(j);
            if (*j->p == ',')
                j->p++;
        }
    } else
        return false;
    return true;
}

/* one call; the reply is folded into st */
static int cd_call(const char *method, const char *path, const char *body)
{
    char url[300];
    snprintf(url, sizeof(url), "%s%s", server, path);
    static const char *const hdr[] = {"Content-Type", "application/json", NULL};
    char *out;
    size_t len;
    double sent = wall_ms();
    int status = net_request(method, url, hdr, body, &out, &len);
    if (status != 200) {
        /* the server answers errors with a JSON reason we'd like to show, but our
         * HTTP layer drops non-200 bodies; the status will do */
        char msg[96];
        snprintf(msg, sizeof(msg), status == 401 ? "the room server forgot us, reconnecting" : "room server: %d", status);
        fail(msg);
        if (status == 401)
            cd_token[0] = 0;
        return status;
    }
    static cd_reply r;
    memset(&r, 0, sizeof(r));
    js j = {out};
    json_object(&j, reply_key, &r);
    free(out);

    if (r.now > 0)
        cd_offset = r.now - (sent + wall_ms()) / 2;
    if (r.token[0]) {
        snprintf(cd_token, sizeof(cd_token), "%s", r.token);
        cd_me = r.id;
    }

    pthread_mutex_lock(&lock);
    if (r.online > 0)
        st.online = r.online;
    st.spectators = r.watchers;
    if (r.error[0])
        snprintf(st.error, sizeof(st.error), "%s", r.error);
    if (r.got_rooms) {
        st.nrooms = r.nrooms;
        if (r.online > 0)
            st.online = r.online;
        memcpy(st.rooms, r.rooms, sizeof(r.rooms[0]) * (size_t)r.nrooms);
    }
    if (r.got_lobby) {
        st.nlobby = r.nlobby;
        memcpy(st.lobby, r.lobby, sizeof(r.lobby[0]) * (size_t)r.nlobby);
    }
    if (r.got_room) {
        const cd_room *rm = &r.room;
        st.in_room = rm->has;
        if (rm->has) {
            st.room_id = rm->id;
            snprintf(st.room_name, sizeof(st.room_name), "%s", rm->name);
            st.i_host = rm->host == cd_me;
            int state = !strcmp(rm->state, "playing") ? MP_PLAYING : !strcmp(rm->state, "results") ? MP_RESULTS : MP_LOBBY;
            if (state == MP_PLAYING && rm->round != st.round) {
                st.round = rm->round;
                st.go_at = rm->start_at - cd_offset;
            }
            st.state = state;
            st.map_id = rm->map_id;
            st.map_set = rm->map_set;
            snprintf(st.map_md5, sizeof(st.map_md5), "%s", rm->md5);
            snprintf(st.map_title, sizeof(st.map_title), "%s", rm->title);
            st.mods = rm->mods;
            st.teams = rm->teams;
            st.freemod = rm->freemod;
            st.win = rm->win;
            st.nchat = rm->nchat;
            memcpy(st.chat, rm->chat, sizeof(rm->chat[0]) * (size_t)rm->nchat);
            st.nmembers = rm->n;
            for (int i = 0; i < rm->n; i++) {
                st.members[i] = rm->m[i];
                st.members[i].host = rm->m[i].id == rm->host;
                st.members[i].me = rm->m[i].id == cd_me;
                st.members[i].slot = rm->m[i].id; /* host tools name members by id here */
            }
        }
    }
    pthread_mutex_unlock(&lock);
    return 200;
}

/* the list, with our token so the server knows we're still here */
static void cd_rooms(void)
{
    char path[128];
    snprintf(path, sizeof(path), "/api/rooms?token=%s", cd_token);
    cd_call("GET", path, NULL);
}

static bool cd_login(void)
{
    char body[256];
    /* signed in: the server puts our account name on us instead */
    snprintf(body, sizeof(body), "{\"name\":\"%s\",\"session\":\"%s\"}", user, account_session());
    if (cd_call("POST", "/api/hello", body) != 200 || !cd_token[0])
        return false;
    cd_rooms();
    return true;
}

static void json_escape(const char *in, char *out, size_t n)
{
    size_t k = 0;
    for (; *in && k + 2 < n; in++) {
        if (*in == '"' || *in == '\\')
            out[k++] = '\\';
        out[k++] = (unsigned char)*in < 32 ? ' ' : *in;
    }
    out[k] = 0;
}

static void cd_online(void);
static void cd_do(const cmd *c)
{
    char body[512], esc[256];
    switch (c->op) {
    case OP_REFRESH:
        cd_rooms();
        return;
    case OP_CREATE: {
        char pw[80];
        json_escape(c->s1, esc, sizeof(esc));
        json_escape(c->s2, pw, sizeof(pw));
        snprintf(body, sizeof(body), "{\"token\":\"%s\",\"name\":\"%s\",\"password\":\"%s\"}", cd_token, esc, pw);
        cd_call("POST", "/api/rooms", body);
        return;
    }
    case OP_JOIN: {
        char pw[80];
        json_escape(c->s2, pw, sizeof(pw));
        snprintf(body, sizeof(body), "{\"token\":\"%s\",\"room\":%d,\"password\":\"%s\"}", cd_token, c->a, pw);
        cd_call("POST", "/api/join", body);
        return;
    }
    case OP_SETTINGS:
        snprintf(body, sizeof(body), "{\"token\":\"%s\",\"teams\":%s,\"freemod\":%s}", cd_token, c->f1 ? "true" : "false",
                 c->f2 ? "true" : "false");
        cd_call("POST", "/api/settings", body);
        return;
    case OP_TEAM:
        snprintf(body, sizeof(body), "{\"token\":\"%s\",\"team\":%d}", cd_token, c->a);
        cd_call("POST", "/api/team", body);
        return;
    case OP_MODS:
        snprintf(body, sizeof(body), "{\"token\":\"%s\",\"mods\":%d}", cd_token, c->mods);
        cd_call("POST", "/api/mods", body);
        return;
    case OP_CHAT:
        json_escape(c->s1, esc, sizeof(esc));
        snprintf(body, sizeof(body), "{\"token\":\"%s\",\"text\":\"%s\"}", cd_token, esc);
        cd_call("POST", "/api/chat", body);
        return;
    case OP_STATUS: {
        json_escape(c->s1, esc, sizeof(esc));
        snprintf(body, sizeof(body),
                 "{\"token\":\"%s\",\"action\":%d,\"text\":\"%s\",\"md5\":\"%s\",\"mode\":%d,\"mods\":%d,\"beatmap_id\":%d}",
                 cd_token, c->a, esc, c->md5, c->mode, c->mods, c->b);
        cd_call("POST", "/api/status", body);
        return;
    }
    case OP_STATS:
    case OP_LOOKUP:
        cd_online();
        return;
    case OP_HOST:
    case OP_LOCK:
        snprintf(body, sizeof(body), "{\"token\":\"%s\",\"member\":%d}", cd_token, c->a);
        cd_call("POST", c->op == OP_HOST ? "/api/host" : "/api/kick", body);
        return;
    case OP_WIN: {
        pthread_mutex_lock(&lock);
        bool teams = st.teams, freemod = st.freemod;
        pthread_mutex_unlock(&lock);
        snprintf(body, sizeof(body), "{\"token\":\"%s\",\"teams\":%s,\"freemod\":%s,\"win\":%d}", cd_token,
                 teams ? "true" : "false", freemod ? "true" : "false", c->a);
        cd_call("POST", "/api/settings", body);
        return;
    }
    case OP_SPECTATE:
        snprintf(body, sizeof(body), "{\"token\":\"%s\",\"id\":%d}", cd_token, c->a);
        cd_call("POST", "/api/spectate", body);
        pthread_mutex_lock(&lock);
        watching = c->a;
        st.spectating = c->a;
        nspec = spec_action = 0;
        spec_seq = 0;
        pthread_mutex_unlock(&lock);
        return;
    case OP_LOBBY_CHAT:
        json_escape(c->s1, esc, sizeof(esc));
        snprintf(body, sizeof(body), "{\"token\":\"%s\",\"text\":\"%s\"}", cd_token, esc);
        cd_call("POST", "/api/lobby", body);
        return;
    case OP_LEAVE:
        snprintf(body, sizeof(body), "{\"token\":\"%s\"}", cd_token);
        cd_call("POST", "/api/leave", body);
        pthread_mutex_lock(&lock);
        st.in_room = false;
        pthread_mutex_unlock(&lock);
        cd_rooms();
        return;
    case OP_MAP:
        json_escape(c->s1, esc, sizeof(esc));
        snprintf(body, sizeof(body), "{\"token\":\"%s\",\"beatmap_id\":%d,\"set_id\":%d,\"md5\":\"%s\",\"title\":\"%s\",\"mods\":%d}",
                 cd_token, c->a, c->b, c->md5, esc, c->mods);
        cd_call("POST", "/api/map", body);
        return;
    case OP_READY:
        snprintf(body, sizeof(body), "{\"token\":\"%s\",\"ready\":%s,\"has_map\":%s}", cd_token, c->f1 ? "true" : "false",
                 c->f2 ? "true" : "false");
        cd_call("POST", "/api/ready", body);
        return;
    case OP_START:
        snprintf(body, sizeof(body), "{\"token\":\"%s\"}", cd_token);
        cd_call("POST", "/api/start", body);
        return;
    case OP_SCORE:
        snprintf(body, sizeof(body),
                 "{\"token\":\"%s\",\"score\":%lld,\"combo\":%d,\"max_combo\":%d,\"acc\":%.4f,\"n300\":%d,\"n100\":%d,"
                 "\"n50\":%d,\"miss\":%d,\"failed\":%s,\"done\":%s}",
                 cd_token, c->score, c->combo, c->max_combo, c->acc, c->n300, c->n100, c->n50, c->miss,
                 c->f1 ? "true" : "false", c->f2 ? "true" : "false");
        cd_call("POST", "/api/score", body);
        return;
    }
}

static bool online_key(js *j, const char *key, void *ctx)
{
    mp_user *u = ctx;
    if (!strcmp(key, "id")) u->id = (int)json_num(j);
    else if (!strcmp(key, "name")) return json_str(j, u->name, sizeof(u->name));
    else if (!strcmp(key, "text")) return json_str(j, u->doing, sizeof(u->doing));
    else if (!strcmp(key, "md5")) return json_str(j, u->md5, sizeof(u->md5));
    else if (!strcmp(key, "action")) u->action = (int)json_num(j);
    else if (!strcmp(key, "mode")) u->mode = (int)json_num(j);
    else if (!strcmp(key, "mods")) u->mods = (int)json_num(j);
    else if (!strcmp(key, "beatmap_id")) u->map_id = (int)json_num(j);
    else return false;
    return true;
}

static bool players_key(js *j, const char *key, void *ctx)
{
    (void)ctx;
    if (strcmp(key, "players") || *j->p != '[')
        return false;
    static mp_user got[MP_MAX_USERS];
    int n = 0;
    j->p++;
    for (;;) {
        json_ws(j);
        if (*j->p == ']') {
            j->p++;
            break;
        }
        mp_user u;
        memset(&u, 0, sizeof(u));
        if (!json_object(j, online_key, &u))
            return false;
        u.online = true;
        if (n < MP_MAX_USERS)
            got[n++] = u;
        json_ws(j);
        if (*j->p == ',')
            j->p++;
    }
    pthread_mutex_lock(&lock);
    st.nusers = n;
    memcpy(st.users, got, sizeof(got[0]) * (size_t)n);
    pthread_mutex_unlock(&lock);
    return true;
}

static void cd_online(void)
{
    char url[300], *out = NULL;
    size_t len;
    snprintf(url, sizeof(url), "%s/api/online?token=%s", server, cd_token);
    if (net_get(url, &out, &len) == 200 && out) {
        js j = {out};
        json_object(&j, players_key, NULL);
    }
    free(out);
}

/* [[seq, t, x, y, keys], ...] and the player's last action */
static void cd_watch(void)
{
    char url[300], *out = NULL;
    size_t len;
    snprintf(url, sizeof(url), "%s/api/watch?token=%s&after=%ld", server, cd_token, spec_seq);
    if (net_get(url, &out, &len) != 200 || !out) {
        free(out);
        return;
    }
    const char *f = strstr(out, "\"frames\":[");
    const char *a = strstr(out, "\"action\":");
    pthread_mutex_lock(&lock);
    if (f) {
        const char *p = f + 10;
        while ((p = strchr(p, '[')) != NULL) {
            char *e;
            double v[5];
            int k = 0;
            p++;
            for (; k < 5; k++) {
                v[k] = strtod(p, &e);
                if (e == p)
                    break;
                p = e;
                while (*p == ',' || *p == ' ')
                    p++;
            }
            if (k < 5)
                break;
            spec_seq = (long)v[0];
            spec_add(v[1], (float)v[2], (float)v[3], (int)v[4], 1);
        }
    }
    if (a) {
        int act = atoi(a + 9);
        if (act)
            spec_action = act;
    }
    pthread_mutex_unlock(&lock);
    free(out);
}

static void cd_send_frames(void)
{
    pthread_mutex_lock(&lock);
    int n = noutgoing;
    static mp_spec_frame copy[4096];
    memcpy(copy, outgoing, sizeof(copy[0]) * (size_t)n);
    noutgoing = 0;
    pthread_mutex_unlock(&lock);
    if (!n)
        return;
    size_t cap = 128 + (size_t)n * 48, k = 0;
    char *body = malloc(cap);
    if (!body)
        return;
    k += (size_t)snprintf(body, cap, "{\"token\":\"%s\",\"frames\":[", cd_token);
    for (int i = 0; i < n && k + 64 < cap; i++)
        k += (size_t)snprintf(body + k, cap - k, "%s[%.0f,%.1f,%.1f,%d]", i ? "," : "", copy[i].t, copy[i].x, copy[i].y, copy[i].keys);
    snprintf(body + k, cap - k, "]}");
    cd_call("POST", "/api/frames", body);
    free(body);
}

static void cd_poll(void)
{
    static double online_at;
    if (wall_ms() - online_at > 10000) {
        cd_online();
        online_at = wall_ms();
    }
    if (watching)
        cd_watch();
    cd_send_frames();
    char path[128];
    bool in;
    pthread_mutex_lock(&lock);
    in = st.in_room;
    pthread_mutex_unlock(&lock);
    if (in) {
        snprintf(path, sizeof(path), "/api/room?token=%s", cd_token);
        cd_call("GET", path, NULL);
    } else {
        cd_rooms();
    }
}

/* ================= bancho ================= */

/* the ids we use, as osu! stable numbers them */
enum {
    C_PART_LOBBY = 29, C_JOIN_LOBBY = 30, C_CREATE_MATCH = 31, C_JOIN_MATCH = 32, C_PART_MATCH = 33,
    C_MATCH_READY = 39, C_MATCH_CHANGE_SETTINGS = 41, C_MATCH_START = 44, C_MATCH_SCORE_UPDATE = 47,
    C_MATCH_COMPLETE = 49, C_MATCH_CHANGE_MODS = 51, C_MATCH_LOAD_COMPLETE = 52, C_MATCH_NO_BEATMAP = 54,
    C_MATCH_NOT_READY = 55, C_MATCH_FAILED = 56, C_MATCH_HAS_BEATMAP = 59, C_SEND_PUBLIC_MESSAGE = 1,
    C_MATCH_CHANGE_TEAM = 77, C_SEND_PRIVATE_MESSAGE = 25, C_CHANNEL_JOIN = 63, C_CHANNEL_PART = 78,
    C_USER_STATS_REQUEST = 85, C_FRIEND_ADD = 73, C_FRIEND_REMOVE = 74, C_MATCH_TRANSFER_HOST = 70,
    C_MATCH_LOCK = 40, C_MATCH_CHANGE_SLOT = 38, C_START_SPECTATING = 16, C_STOP_SPECTATING = 17,
    C_CHANGE_ACTION = 0,

    S_USER_ID = 5, S_NOTIFICATION = 24, S_UPDATE_MATCH = 26, S_NEW_MATCH = 27, S_DISPOSE_MATCH = 28,
    S_MATCH_JOIN_SUCCESS = 36, S_MATCH_JOIN_FAIL = 37, S_MATCH_START = 46, S_MATCH_SCORE_UPDATE = 48,
    S_MATCH_ALL_LOADED = 53, S_MATCH_COMPLETE = 58, S_USER_PRESENCE = 83, S_SEND_MESSAGE = 7,
    S_USER_STATS = 11, S_USER_LOGOUT = 12, S_SPECTATOR_JOINED = 13, S_SPECTATOR_LEFT = 14, S_SPECTATE_FRAMES = 15,
    S_CHANNEL_JOIN_SUCCESS = 64, S_CHANNEL_INFO = 65, S_CHANNEL_KICK = 66, S_CHANNEL_AUTO_JOIN = 67,
    S_FRIENDS_LIST = 72,
};

enum { SLOT_OPEN = 1, SLOT_LOCKED = 2, SLOT_NOT_READY = 4, SLOT_READY = 8, SLOT_NO_MAP = 16, SLOT_PLAYING = 32,
       SLOT_COMPLETE = 64, SLOT_HAS_PLAYER = 124 };

typedef struct {
    int id;
    bool in_progress;
    uint32_t mods;
    char name[48], password[32], map_name[128];
    int map_id;
    char md5[33];
    uint8_t status[16], team[16];
    int uid[16];
    int host;
    uint8_t mode, win, team_type, freemods;
    uint32_t slot_mods[16];
    int seed;
} bmatch;

typedef struct {
    int score, combo, max_combo, n300, n100, n50, miss;
    bool failed;
} bframe;

static char bc_token[128];
static int bc_me;
static uint8_t outbuf[16384];
static size_t outn;
static bmatch lobby[MP_MAX_ROOMS];
static int nlobby;
static bmatch cur;
static bool in_match, results;
static bframe frames[16];
static int round_no;
static double go_at;
static struct { int id; char name[32]; } names[512];
static int nnames;
/* the match's chat channel, newest last */
static mp_line bc_chat[40];
static int bc_nchat, bc_chat_id;
/* everything else said in channels and DMs */
static mp_channel chans[MP_MAX_CHANNELS];
static int nchans;
static mp_line msgs[120];
static int nmsgs;
static mp_user users[MP_MAX_USERS];
static int nusers;
static int friends[256], nfriends;
static int watchers;

/* ---- writing ---- */

static void w_bytes(const void *p, size_t n)
{
    if (outn + n <= sizeof(outbuf)) {
        memcpy(outbuf + outn, p, n);
        outn += n;
    }
}
static void w_u8(uint8_t v) { w_bytes(&v, 1); }
static void w_u16(uint16_t v) { w_bytes(&v, 2); } /* the console is little endian, like the protocol */
static void w_u32(uint32_t v) { w_bytes(&v, 4); }
static void w_i32(int32_t v) { w_bytes(&v, 4); }
static void w_str(const char *s)
{
    size_t n = strlen(s);
    if (!n) {
        w_u8(0);
        return;
    }
    w_u8(0x0b);
    for (size_t v = n;;) { /* uleb128 length */
        uint8_t b = v & 0x7f;
        v >>= 7;
        w_u8(v ? b | 0x80 : b);
        if (!v) break;
    }
    w_bytes(s, n);
}
static size_t begin(uint16_t id)
{
    w_u16(id);
    w_u8(0);
    w_u32(0);
    return outn;
}
static void end(size_t at)
{
    uint32_t len = (uint32_t)(outn - at);
    memcpy(outbuf + at - 4, &len, 4);
}
static void packet(uint16_t id) { end(begin(id)); }

static void w_match(const bmatch *m)
{
    w_u16((uint16_t)m->id);
    w_u8(m->in_progress);
    w_u8(0);
    w_u32(m->mods);
    w_str(m->name);
    w_str(m->password);
    w_str(m->map_name);
    w_i32(m->map_id);
    w_str(m->md5);
    for (int i = 0; i < 16; i++) w_u8(m->status[i]);
    for (int i = 0; i < 16; i++) w_u8(m->team[i]);
    for (int i = 0; i < 16; i++)
        if (m->status[i] & SLOT_HAS_PLAYER) w_i32(m->uid[i]);
    w_i32(m->host);
    w_u8(m->mode);
    w_u8(m->win);
    w_u8(m->team_type);
    w_u8(m->freemods);
    if (m->freemods)
        for (int i = 0; i < 16; i++) w_u32(m->slot_mods[i]);
    w_i32(m->seed);
}

/* ---- reading ---- */

typedef struct {
    const uint8_t *p, *end;
} rd;

static bool r_ok(rd *r, size_t n) { return (size_t)(r->end - r->p) >= n; }
static uint8_t r_u8(rd *r) { return r_ok(r, 1) ? *r->p++ : 0; }
static uint16_t r_u16(rd *r)
{
    uint16_t v = 0;
    if (r_ok(r, 2)) memcpy(&v, r->p, 2), r->p += 2;
    return v;
}
static uint32_t r_u32(rd *r)
{
    uint32_t v = 0;
    if (r_ok(r, 4)) memcpy(&v, r->p, 4), r->p += 4;
    return v;
}
static void r_str(rd *r, char *out, size_t cap)
{
    out[0] = 0;
    if (r_u8(r) != 0x0b)
        return;
    size_t n = 0;
    for (int shift = 0; r_ok(r, 1) && shift < 35; shift += 7) {
        uint8_t b = r_u8(r);
        n |= (size_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) break;
    }
    if (!r_ok(r, n))
        n = (size_t)(r->end - r->p);
    size_t k = n < cap - 1 ? n : cap - 1;
    memcpy(out, r->p, k);
    out[k] = 0;
    r->p += n;
}

static void r_match(rd *r, bmatch *m)
{
    memset(m, 0, sizeof(*m));
    m->id = r_u16(r);
    m->in_progress = r_u8(r);
    r_u8(r);
    m->mods = r_u32(r);
    r_str(r, m->name, sizeof(m->name));
    r_str(r, m->password, sizeof(m->password));
    r_str(r, m->map_name, sizeof(m->map_name));
    m->map_id = (int32_t)r_u32(r);
    r_str(r, m->md5, sizeof(m->md5));
    for (int i = 0; i < 16; i++) m->status[i] = r_u8(r);
    for (int i = 0; i < 16; i++) m->team[i] = r_u8(r);
    for (int i = 0; i < 16; i++) m->uid[i] = m->status[i] & SLOT_HAS_PLAYER ? (int32_t)r_u32(r) : -1;
    m->host = (int32_t)r_u32(r);
    m->mode = r_u8(r);
    m->win = r_u8(r);
    m->team_type = r_u8(r);
    m->freemods = r_u8(r);
    if (m->freemods)
        for (int i = 0; i < 16; i++) m->slot_mods[i] = r_u32(r);
    m->seed = (int32_t)r_u32(r);
}

static const char *name_of(int id)
{
    for (int i = 0; i < nnames; i++)
        if (names[i].id == id) return names[i].name;
    return "?";
}

/* ---- our mods <-> osu!'s ---- */

static const struct { int ours; uint32_t theirs; } mod_map[] = {
    {OSU5_MOD_NOFAIL, 1}, {OSU5_MOD_EASY, 2}, {OSU5_MOD_HIDDEN, 8}, {OSU5_MOD_HARDROCK, 16},
    {OSU5_MOD_SUDDENDEATH, 32}, {OSU5_MOD_DOUBLETIME, 64}, {OSU5_MOD_RELAX, 128}, {OSU5_MOD_HALFTIME, 256},
    {OSU5_MOD_NIGHTCORE, 512 | 64}, {OSU5_MOD_FLASHLIGHT, 1024}, {OSU5_MOD_AUTO, 2048}, {OSU5_MOD_SPUNOUT, 4096},
    {OSU5_MOD_AUTOPILOT, 8192}, {OSU5_MOD_PERFECT, 16384 | 32}, {OSU5_MOD_DAYCORE, 256}, {OSU5_MOD_MIRROR, 1u << 30},
};

uint32_t mp_mods_to_osu(int mods)
{
    uint32_t m = 0;
    for (size_t i = 0; i < sizeof(mod_map) / sizeof(mod_map[0]); i++)
        if (mods & mod_map[i].ours) m |= mod_map[i].theirs;
    return m;
}

int mp_mods_from_osu(uint32_t m)
{
    int mods = 0;
    if (m & 512) m &= ~64u;      /* nightcore carries DT's bit too */
    if (m & 16384) m &= ~32u;    /* and perfect SD's */
    for (size_t i = 0; i < sizeof(mod_map) / sizeof(mod_map[0]); i++)
        if ((m & mod_map[i].theirs) == mod_map[i].theirs && mod_map[i].ours != OSU5_MOD_DAYCORE)
            mods |= mod_map[i].ours;
    return mods;
}

/* ---- state for the game ---- */

static void bc_publish(void)
{
    pthread_mutex_lock(&lock);
    st.nrooms = 0;
    st.online = nnames;
    for (int i = 0; i < nlobby && st.nrooms < MP_MAX_ROOMS; i++) {
        mp_room_brief *b = &st.rooms[st.nrooms++];
        memset(b, 0, sizeof(*b));
        b->id = lobby[i].id;
        snprintf(b->name, sizeof(b->name), "%s", lobby[i].name);
        snprintf(b->map, sizeof(b->map), "%s", lobby[i].map_name);
        b->playing = lobby[i].in_progress;
        for (int s = 0; s < 16; s++)
            if (lobby[i].status[s] & SLOT_HAS_PLAYER) b->players++;
    }
    st.nchannels = nchans;
    memcpy(st.channels, chans, sizeof(chans[0]) * (size_t)nchans);
    st.nmsgs = nmsgs;
    memcpy(st.msgs, msgs, sizeof(msgs[0]) * (size_t)nmsgs);
    st.nusers = nusers;
    memcpy(st.users, users, sizeof(users[0]) * (size_t)nusers);
    st.spectating = watching;
    st.spectators = watchers;
    st.my_slot = -1;
    for (int s = 0; s < 16 && in_match; s++)
        if (cur.uid[s] == bc_me)
            st.my_slot = s;
    st.in_room = in_match;
    st.nchat = bc_nchat;
    memcpy(st.chat, bc_chat, sizeof(bc_chat[0]) * (size_t)bc_nchat);
    if (in_match) {
        st.teams = cur.team_type == 2 || cur.team_type == 3;
        st.freemod = cur.freemods != 0;
        st.win = cur.win;
        st.room_id = cur.id;
        snprintf(st.room_name, sizeof(st.room_name), "%s", cur.name);
        st.i_host = cur.host == bc_me;
        st.state = cur.in_progress ? MP_PLAYING : results ? MP_RESULTS : MP_LOBBY;
        st.round = round_no;
        st.go_at = go_at;
        st.map_id = cur.map_id;
        st.map_set = 0;
        snprintf(st.map_md5, sizeof(st.map_md5), "%s", cur.md5);
        snprintf(st.map_title, sizeof(st.map_title), "%s", cur.map_name);
        st.mods = mp_mods_from_osu(cur.mods);
        st.nmembers = 0;
        for (int s = 0; s < 16 && st.nmembers < MP_MAX_MEMBERS; s++) {
            if (!(cur.status[s] & SLOT_HAS_PLAYER)) continue;
            mp_member *m = &st.members[st.nmembers++];
            memset(m, 0, sizeof(*m));
            m->id = cur.uid[s];
            m->slot = s;
            snprintf(m->name, sizeof(m->name), "%s", name_of(cur.uid[s]));
            m->ready = cur.status[s] == SLOT_READY;
            m->has_map = cur.status[s] != SLOT_NO_MAP;
            m->done = cur.status[s] == SLOT_COMPLETE;
            m->host = cur.uid[s] == cur.host;
            m->me = cur.uid[s] == bc_me;
            m->score = frames[s].score;
            m->combo = frames[s].combo;
            m->max_combo = frames[s].max_combo;
            m->n300 = frames[s].n300;
            m->n100 = frames[s].n100;
            m->n50 = frames[s].n50;
            m->miss = frames[s].miss;
            m->failed = frames[s].failed;
            int total = m->n300 + m->n100 + m->n50 + m->miss;
            m->acc = total ? (300.0f * m->n300 + 100.0f * m->n100 + 50.0f * m->n50) / (300.0f * total) : 1;
        }
    }
    pthread_mutex_unlock(&lock);
}

static void lobby_put(const bmatch *m)
{
    for (int i = 0; i < nlobby; i++)
        if (lobby[i].id == m->id) {
            lobby[i] = *m;
            return;
        }
    if (nlobby < MP_MAX_ROOMS)
        lobby[nlobby++] = *m;
}

static mp_channel *chan_find(const char *name, bool add)
{
    for (int i = 0; i < nchans; i++)
        if (!strcasecmp(chans[i].name, name))
            return &chans[i];
    if (!add || nchans >= MP_MAX_CHANNELS)
        return NULL;
    mp_channel *c = &chans[nchans++];
    memset(c, 0, sizeof(*c));
    snprintf(c->name, sizeof(c->name), "%s", name);
    return c;
}

static void msg_add(const char *to, const char *from, const char *text)
{
    if (nmsgs == 120) {
        memmove(msgs, msgs + 1, sizeof(msgs[0]) * 119);
        nmsgs--;
    }
    mp_line *m = &msgs[nmsgs++];
    memset(m, 0, sizeof(*m));
    m->id = ++bc_chat_id;
    snprintf(m->to, sizeof(m->to), "%s", to);
    snprintf(m->from, sizeof(m->from), "%s", from);
    snprintf(m->text, sizeof(m->text), "%s", text);
    mp_channel *c = chan_find(to, true);
    if (c && strcmp(from, user))
        c->unread++;
}

static mp_user *user_slot(int uid)
{
    for (int i = 0; i < nusers; i++)
        if (users[i].id == uid)
            return &users[i];
    if (nusers < MP_MAX_USERS) {
        memset(&users[nusers], 0, sizeof(users[0]));
        users[nusers].id = uid;
        return &users[nusers++];
    }
    /* full: the oldest one makes room */
    memmove(users, users + 1, sizeof(users[0]) * (MP_MAX_USERS - 1));
    memset(&users[MP_MAX_USERS - 1], 0, sizeof(users[0]));
    users[MP_MAX_USERS - 1].id = uid;
    return &users[MP_MAX_USERS - 1];
}

static void handle(uint16_t id, rd *r)
{
    switch (id) {
    case S_USER_ID:
        bc_me = (int32_t)r_u32(r);
        break;
    case S_NOTIFICATION: {
        char msg[160];
        r_str(r, msg, sizeof(msg));
        fail(msg);
        break;
    }
    case S_SEND_MESSAGE: {
        char from[24], text[124], target[40];
        r_str(r, from, sizeof(from));
        r_str(r, text, sizeof(text));
        r_str(r, target, sizeof(target));
        if (strcmp(target, "#multiplayer") && strncmp(target, "#mp_", 4)) {
            /* a DM's target is us: file it under the sender */
            msg_add(target[0] == '#' ? target : from, from, text);
            break;
        }
        if (bc_nchat == 40) {
            memmove(bc_chat, bc_chat + 1, sizeof(bc_chat[0]) * 39);
            bc_nchat--;
        }
        bc_chat[bc_nchat].id = ++bc_chat_id;
        snprintf(bc_chat[bc_nchat].from, sizeof(bc_chat[0].from), "%s", from);
        snprintf(bc_chat[bc_nchat].text, sizeof(bc_chat[0].text), "%s", text);
        bc_nchat++;
        break;
    }
    case S_USER_PRESENCE: {
        int uid = (int32_t)r_u32(r);
        char name[32];
        r_str(r, name, sizeof(name));
        for (int i = 0; i < nnames; i++)
            if (names[i].id == uid) {
                snprintf(names[i].name, sizeof(names[i].name), "%s", name);
                return;
            }
        if (nnames < 512) {
            names[nnames].id = uid;
            snprintf(names[nnames++].name, sizeof(names[0].name), "%s", name);
        }
        break;
    }
    case S_USER_STATS: {
        int uid = (int32_t)r_u32(r);
        mp_user *u = user_slot(uid);
        u->action = r_u8(r);
        r_str(r, u->doing, sizeof(u->doing));
        r_str(r, u->md5, sizeof(u->md5));
        u->mods = mp_mods_from_osu(r_u32(r));
        u->mode = r_u8(r);
        u->map_id = (int32_t)r_u32(r);
        uint32_t lo = r_u32(r), hi = r_u32(r);
        u->ranked_score = (long long)((uint64_t)hi << 32 | lo);
        uint32_t accb = r_u32(r);
        memcpy(&u->acc, &accb, 4);
        u->plays = (int32_t)r_u32(r);
        lo = r_u32(r);
        hi = r_u32(r);
        u->total_score = (long long)((uint64_t)hi << 32 | lo);
        u->rank = (int32_t)r_u32(r);
        u->pp = r_u16(r);
        u->online = true;
        snprintf(u->name, sizeof(u->name), "%s", name_of(uid));
        for (int i = 0; i < nfriends; i++)
            if (friends[i] == uid)
                u->friend_ = true;
        break;
    }
    case S_USER_LOGOUT: {
        int uid = (int32_t)r_u32(r);
        for (int i = 0; i < nusers; i++)
            if (users[i].id == uid)
                users[i].online = false;
        for (int i = 0; i < nnames; i++)
            if (names[i].id == uid)
                names[i--] = names[--nnames];
        if (uid == watching)
            watching = 0;
        break;
    }
    case S_CHANNEL_INFO:
    case S_CHANNEL_AUTO_JOIN: {
        char name[32], topic[64];
        r_str(r, name, sizeof(name));
        r_str(r, topic, sizeof(topic));
        int n = r_u16(r);
        /* info is about every channel; only remember ones we're in, or auto-joined */
        mp_channel *c = chan_find(name, id == S_CHANNEL_AUTO_JOIN);
        if (c) {
            snprintf(c->topic, sizeof(c->topic), "%s", topic);
            c->users = n;
        }
        break;
    }
    case S_CHANNEL_JOIN_SUCCESS: {
        char name[32];
        r_str(r, name, sizeof(name));
        if (strcmp(name, "#multiplayer") && strncmp(name, "#mp_", 4) && strcmp(name, "#lobby") && strcmp(name, "#spectator"))
            chan_find(name, true);
        break;
    }
    case S_CHANNEL_KICK: {
        char name[32];
        r_str(r, name, sizeof(name));
        for (int i = 0; i < nchans; i++)
            if (!strcasecmp(chans[i].name, name))
                chans[i--] = chans[--nchans];
        break;
    }
    case S_FRIENDS_LIST: {
        int n = r_u16(r);
        nfriends = 0;
        for (int i = 0; i < n && nfriends < 256; i++)
            friends[nfriends++] = (int32_t)r_u32(r);
        for (int i = 0; i < nusers; i++) {
            users[i].friend_ = false;
            for (int k = 0; k < nfriends; k++)
                if (friends[k] == users[i].id)
                    users[i].friend_ = true;
        }
        break;
    }
    case S_SPECTATOR_JOINED:
        watchers++;
        break;
    case S_SPECTATOR_LEFT:
        if (watchers > 0)
            watchers--;
        break;
    case S_SPECTATE_FRAMES: {
        r_u32(r); /* extra */
        int n = r_u16(r);
        for (int i = 0; i < n && r_ok(r, 14); i++) {
            int keys = r_u8(r);
            r_u8(r); /* taiko's old byte */
            uint32_t xb = r_u32(r), yb = r_u32(r);
            int t = (int32_t)r_u32(r);
            float x, y;
            memcpy(&x, &xb, 4);
            memcpy(&y, &yb, 4);
            pthread_mutex_lock(&lock);
            spec_add(t, x, y, keys, 0);
            pthread_mutex_unlock(&lock);
        }
        int action = r_u8(r);
        if (action)
            spec_action = action;
        break;
    }
    case S_NEW_MATCH:
    case S_UPDATE_MATCH: {
        bmatch m;
        r_match(r, &m);
        lobby_put(&m);
        if (in_match && m.id == cur.id)
            cur = m;
        break;
    }
    case S_DISPOSE_MATCH: {
        int mid = (int32_t)r_u32(r);
        for (int i = 0; i < nlobby; i++)
            if (lobby[i].id == mid) lobby[i--] = lobby[--nlobby];
        if (in_match && cur.id == mid) in_match = false;
        break;
    }
    case S_MATCH_JOIN_SUCCESS:
        r_match(r, &cur);
        in_match = true;
        results = false;
        memset(frames, 0, sizeof(frames));
        break;
    case S_MATCH_JOIN_FAIL:
        fail("couldn't join that room");
        break;
    case S_MATCH_START:
        r_match(r, &cur);
        results = false;
        round_no++;
        go_at = 0;
        memset(frames, 0, sizeof(frames));
        /* we load fast enough to say so straight away */
        packet(C_MATCH_LOAD_COMPLETE);
        break;
    case S_MATCH_ALL_LOADED:
        go_at = wall_ms();
        break;
    case S_MATCH_SCORE_UPDATE: {
        r_u32(r); /* time */
        int slot = r_u8(r);
        if (slot >= 16) break;
        bframe *f = &frames[slot];
        f->n300 = r_u16(r);
        f->n100 = r_u16(r);
        f->n50 = r_u16(r);
        r_u16(r);
        r_u16(r);
        f->miss = r_u16(r);
        f->score = (int32_t)r_u32(r);
        f->max_combo = r_u16(r);
        f->combo = r_u16(r);
        r_u8(r);
        f->failed = r_u8(r) == 0;
        break;
    }
    case S_MATCH_COMPLETE:
        cur.in_progress = false;
        results = true;
        break;
    }
}

static int bc_post(bool login, const void *body, size_t blen)
{
    char url[220], headers[2048] = "";
    snprintf(url, sizeof(url), "https://c.%s/", server);
    const char *hdr_login[] = {"User-Agent", "osu!", NULL};
    const char *hdr[] = {"User-Agent", "osu!", "osu-token", bc_token, NULL};
    char *out;
    size_t len;
    int status = net_request_raw("POST", url, login ? hdr_login : hdr, body, blen, &out, &len, headers, sizeof(headers));
    if (status != 200) {
        char msg[96];
        snprintf(msg, sizeof(msg), "bancho: %d", status);
        fail(msg);
        return status;
    }
    if (login) {
        char *t = strstr(headers, "cho-token:");
        bc_token[0] = 0;
        if (t) {
            t += 10;
            while (*t == ' ') t++;
            size_t n = strcspn(t, "\r\n");
            snprintf(bc_token, sizeof(bc_token), "%.*s", (int)n, t);
        }
    }
    rd r = {(const uint8_t *)out, (const uint8_t *)out + len};
    while (r_ok(&r, 7)) {
        uint16_t id = r_u16(&r);
        r_u8(&r);
        uint32_t plen = r_u32(&r);
        if (!r_ok(&r, plen)) break;
        rd p = {r.p, r.p + plen};
        handle(id, &p);
        r.p += plen;
    }
    free(out);
    bc_publish();
    return 200;
}

static bool bc_login(void)
{
    char version[32] = "b20260924";
    online_stable_version(version, sizeof(version));
    char md5[33];
    md5_text(pass, md5);
    /* machine hashes. Score submission sends the same client string and the two
     * ids behind the last hashes, so they have to agree with each other */
    char uid1[40], uid2[40], path_md5[33], adapters_md5[33], u1[33], u2[33], client[256];
    snprintf(uid1, sizeof(uid1), "cadence-%.20s", user);
    snprintf(uid2, sizeof(uid2), "ps5-%.20s", user);
    md5_text("/data/cadence", path_md5);
    md5_text("00-11-22-33-44-55.", adapters_md5);
    md5_text(uid1, u1);
    md5_text(uid2, u2);
    snprintf(client, sizeof(client), "%s:00-11-22-33-44-55.:%s:%s:%s:", path_md5, adapters_md5, u1, u2);
    char body[512];
    snprintf(body, sizeof(body), "%s\n%s\n%s|0|0|%s|0\n", user, md5, version, client);
    bc_me = 0;
    nlobby = 0;
    in_match = false;
    nchans = nmsgs = nusers = nfriends = 0;
    watching = watchers = nspec = spec_action = 0;
    if (bc_post(true, body, strlen(body)) != 200)
        return false;
    if (bc_me <= 0 || !bc_token[0] || !strcmp(bc_token, "no")) {
        pthread_mutex_lock(&lock);
        if (!st.error[0])
            snprintf(st.error, sizeof(st.error), bc_me == -1 ? "wrong username or password" : "the server said no (%d)", bc_me);
        pthread_mutex_unlock(&lock);
        return false;
    }
    LOG("mp: bancho %s as %s (%d), client %s", server, user, bc_me, version);
    bweb_session(server, user, md5, version, client, uid1, uid2, bc_token);
    outn = 0;
    packet(C_JOIN_LOBBY);
    return true;
}

static void bc_do(const cmd *c)
{
    switch (c->op) {
    case OP_REFRESH:
        break; /* the lobby keeps us up to date */
    case OP_CREATE: {
        bmatch m;
        memset(&m, 0, sizeof(m));
        snprintf(m.name, sizeof(m.name), "%s", c->s1);
        m.map_id = -1;
        for (int i = 0; i < 16; i++) m.status[i] = SLOT_OPEN;
        m.host = bc_me;
        size_t at = begin(C_CREATE_MATCH);
        w_match(&m);
        end(at);
        break;
    }
    case OP_JOIN: {
        size_t at = begin(C_JOIN_MATCH);
        w_i32(c->a);
        w_str("");
        end(at);
        break;
    }
    case OP_LEAVE:
        packet(C_PART_MATCH);
        in_match = false;
        packet(C_JOIN_LOBBY);
        break;
    case OP_MAP: {
        if (!in_match) break;
        bmatch m = cur;
        m.map_id = c->a;
        snprintf(m.md5, sizeof(m.md5), "%s", c->md5);
        snprintf(m.map_name, sizeof(m.map_name), "%s", c->s1);
        m.mode = (uint8_t)c->mode;
        size_t at = begin(C_MATCH_CHANGE_SETTINGS);
        w_match(&m);
        end(at);
        at = begin(C_MATCH_CHANGE_MODS);
        w_u32(mp_mods_to_osu(c->mods));
        end(at);
        break;
    }
    case OP_READY:
        packet(c->f2 ? C_MATCH_HAS_BEATMAP : C_MATCH_NO_BEATMAP);
        packet(c->f1 ? C_MATCH_READY : C_MATCH_NOT_READY);
        break;
    case OP_CHAT: {
        size_t at = begin(C_SEND_PUBLIC_MESSAGE);
        w_str("");
        w_str(c->s1);
        w_str("#multiplayer");
        w_i32(0);
        end(at);
        /* the server doesn't echo our own lines back */
        if (bc_nchat == 40) {
            memmove(bc_chat, bc_chat + 1, sizeof(bc_chat[0]) * 39);
            bc_nchat--;
        }
        bc_chat[bc_nchat].id = ++bc_chat_id;
        snprintf(bc_chat[bc_nchat].from, sizeof(bc_chat[0].from), "%s", user);
        snprintf(bc_chat[bc_nchat].text, sizeof(bc_chat[0].text), "%s", c->s1);
        bc_nchat++;
        break;
    }
    case OP_TEAM:
        packet(C_MATCH_CHANGE_TEAM); /* bancho only flips it */
        break;
    case OP_SAY: {
        bool dm = c->s2[0] != '#';
        size_t at = begin(dm ? C_SEND_PRIVATE_MESSAGE : C_SEND_PUBLIC_MESSAGE);
        w_str(user);
        w_str(c->s1);
        w_str(c->s2);
        w_i32(bc_me);
        end(at);
        msg_add(c->s2, user, c->s1); /* no echo from the server */
        break;
    }
    case OP_CHAN_JOIN: {
        size_t at = begin(C_CHANNEL_JOIN);
        w_str(c->s2);
        end(at);
        break;
    }
    case OP_CHAN_PART: {
        if (c->s2[0] == '#') {
            size_t at = begin(C_CHANNEL_PART);
            w_str(c->s2);
            end(at);
        }
        for (int i = 0; i < nchans; i++)
            if (!strcasecmp(chans[i].name, c->s2))
                chans[i--] = chans[--nchans];
        break;
    }
    case OP_STATS: {
        size_t at = begin(C_USER_STATS_REQUEST);
        w_u16(1);
        w_i32(c->a);
        end(at);
        break;
    }
    case OP_LOOKUP:
        for (int i = 0; i < nnames; i++)
            if (!strcasecmp(names[i].name, c->s1)) {
                size_t at = begin(C_USER_STATS_REQUEST);
                w_u16(1);
                w_i32(names[i].id);
                end(at);
                pthread_mutex_lock(&lock);
                snprintf(user_slot(names[i].id)->name, sizeof(users[0].name), "%s", names[i].name);
                pthread_mutex_unlock(&lock);
                break;
            }
        break;
    case OP_FRIEND: {
        size_t at = begin(c->f1 ? C_FRIEND_ADD : C_FRIEND_REMOVE);
        w_i32(c->a);
        end(at);
        if (c->f1 && nfriends < 256)
            friends[nfriends++] = c->a;
        for (int i = 0; i < nfriends && !c->f1; i++)
            if (friends[i] == c->a)
                friends[i--] = friends[--nfriends];
        for (int i = 0; i < nusers; i++)
            if (users[i].id == c->a)
                users[i].friend_ = c->f1;
        break;
    }
    case OP_HOST:
    case OP_LOCK:
    case OP_SLOT: {
        size_t at = begin(c->op == OP_HOST ? C_MATCH_TRANSFER_HOST : c->op == OP_LOCK ? C_MATCH_LOCK : C_MATCH_CHANGE_SLOT);
        w_i32(c->a);
        end(at);
        break;
    }
    case OP_WIN: {
        if (!in_match)
            break;
        bmatch m = cur;
        m.win = (uint8_t)c->a;
        size_t at = begin(C_MATCH_CHANGE_SETTINGS);
        w_match(&m);
        end(at);
        break;
    }
    case OP_STATUS: {
        size_t at = begin(C_CHANGE_ACTION);
        w_u8((uint8_t)c->a);
        w_str(c->s1);
        w_str(c->md5);
        w_u32(mp_mods_to_osu(c->mods));
        w_u8((uint8_t)c->mode);
        w_i32(c->b);
        end(at);
        break;
    }
    case OP_SPECTATE:
        if (watching)
            packet(C_STOP_SPECTATING);
        pthread_mutex_lock(&lock);
        watching = c->a;
        nspec = spec_action = 0;
        pthread_mutex_unlock(&lock);
        if (watching) {
            size_t at = begin(C_START_SPECTATING);
            w_i32(watching);
            end(at);
        }
        break;
    case OP_MODS: {
        size_t at = begin(C_MATCH_CHANGE_MODS);
        w_u32(mp_mods_to_osu(c->mods));
        end(at);
        break;
    }
    case OP_SETTINGS: {
        if (!in_match)
            break;
        bmatch m = cur;
        m.team_type = c->f1 ? 2 : 0;
        m.freemods = c->f2;
        size_t at = begin(C_MATCH_CHANGE_SETTINGS);
        w_match(&m);
        end(at);
        break;
    }
    case OP_START:
        packet(C_MATCH_START);
        break;
    case OP_SCORE: {
        int slot = 0;
        for (int i = 0; i < 16; i++)
            if (cur.uid[i] == bc_me) slot = i;
        size_t at = begin(C_MATCH_SCORE_UPDATE);
        w_i32(0);
        w_u8((uint8_t)slot);
        w_u16((uint16_t)c->n300);
        w_u16((uint16_t)c->n100);
        w_u16((uint16_t)c->n50);
        w_u16(0);
        w_u16(0);
        w_u16((uint16_t)c->miss);
        w_i32((int32_t)c->score);
        w_u16((uint16_t)c->max_combo);
        w_u16((uint16_t)c->combo);
        w_u8(c->miss == 0);
        w_u8(c->f1 ? 0 : 200);
        w_u8(0);
        w_u8(0);
        end(at);
        if (c->f1) packet(C_MATCH_FAILED);
        if (c->f2) packet(C_MATCH_COMPLETE);
        break;
    }
    }
}

static void bc_poll(void)
{
    size_t n = outn;
    static uint8_t send[sizeof(outbuf)];
    memcpy(send, outbuf, n);
    outn = 0;
    bc_post(false, send, n);
}

/* ================= worker ================= */

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&lock);
        while (!want_on)
            pthread_cond_wait(&wake, &lock);
        bool need_login = st.conn != MP_ONLINE;
        if (need_login) {
            st.conn = MP_CONNECTING;
            st.error[0] = 0;
        }
        pthread_mutex_unlock(&lock);

        if (need_login) {
            bool ok = kind == MP_CADENCE ? cd_login() : bc_login();
            pthread_mutex_lock(&lock);
            st.conn = ok ? MP_ONLINE : MP_FAILED;
            if (!ok)
                want_on = false;
            pthread_mutex_unlock(&lock);
            if (!ok)
                continue;
        }

        pthread_mutex_lock(&lock);
        cmd todo[QMAX];
        int n = queued;
        memcpy(todo, queue, sizeof(cmd) * (size_t)n);
        queued = 0;
        pthread_mutex_unlock(&lock);
        for (int i = 0; i < n; i++)
            kind == MP_CADENCE ? cd_do(&todo[i]) : bc_do(&todo[i]);

        if (kind == MP_CADENCE) {
            if (!cd_token[0]) { /* the server restarted under us */
                pthread_mutex_lock(&lock);
                st.conn = MP_CONNECTING;
                pthread_mutex_unlock(&lock);
                cd_login();
                pthread_mutex_lock(&lock);
                st.conn = cd_token[0] ? MP_ONLINE : MP_FAILED;
                pthread_mutex_unlock(&lock);
            }
            cd_poll();
        } else {
            bc_poll();
        }

        /* about once a second, sooner when the game asks for something */
        pthread_mutex_lock(&lock);
        if (want_on && queued == 0) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            /* every 0.8s while something's happening, every 4s sat in the lobby */
            bool busy = kind == MP_BANCHO || st.in_room || watching || st.spectators > 0;
            long ms = busy ? 800 : 4000;
            until.tv_sec += ms / 1000;
            until.tv_nsec += (ms % 1000) * 1000000L;
            if (until.tv_nsec >= 1000000000L) {
                until.tv_sec++;
                until.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&wake, &lock, &until);
        }
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

void mp_connect(int k, const char *srv, const char *u, const char *p)
{
    pthread_mutex_lock(&lock);
    if (!started) {
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 256 * 1024);
        started = pthread_create(&t, &attr, worker, NULL) == 0;
        if (started)
            pthread_detach(t);
        pthread_attr_destroy(&attr);
    }
    kind = k;
    snprintf(server, sizeof(server), "%s", srv);
    size_t l = strlen(server);
    while (l && server[l - 1] == '/')
        server[--l] = 0;
    snprintf(user, sizeof(user), "%s", u);
    snprintf(pass, sizeof(pass), "%s", p ? p : "");
    memset(&st, 0, sizeof(st));
    snprintf(st.me, sizeof(st.me), "%s", u);
    st.conn = MP_CONNECTING;
    cd_token[0] = bc_token[0] = 0;
    queued = 0;
    want_on = true;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
}

/* saying goodbye on the way out, so the server drops us at once instead of
 * counting us until we time out */
static struct {
    int kind;
    char server[160], token[128];
} bye;

static void *bye_thread(void *arg)
{
    (void)arg;
    char *out = NULL;
    size_t len;
    if (bye.kind == MP_CADENCE) {
        char url[220], body[160];
        snprintf(url, sizeof(url), "%s/api/bye", bye.server);
        snprintf(body, sizeof(body), "{\"token\":\"%s\"}", bye.token);
        static const char *const hdr[] = {"Content-Type", "application/json", NULL};
        net_request("POST", url, hdr, body, &out, &len);
    } else {
        char url[220];
        snprintf(url, sizeof(url), "https://c.%s/", bye.server);
        const char *hdr[] = {"User-Agent", "osu!", "osu-token", bye.token, NULL};
        static const uint8_t logout[] = {2, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0};
        net_request_raw("POST", url, hdr, logout, sizeof(logout), &out, &len, NULL, 0);
    }
    free(out);
    return NULL;
}

void mp_disconnect(void)
{
    pthread_mutex_lock(&lock);
    bool online = st.conn == MP_ONLINE;
    want_on = false;
    st.conn = MP_OFF;
    queued = 0;
    bweb_end();
    bye.kind = kind;
    snprintf(bye.server, sizeof(bye.server), "%s", server);
    snprintf(bye.token, sizeof(bye.token), "%s", kind == MP_CADENCE ? cd_token : bc_token);
    pthread_mutex_unlock(&lock);
    if (online && bye.token[0]) {
        pthread_t t;
        if (pthread_create(&t, NULL, bye_thread, NULL) == 0)
            pthread_detach(t);
    }
}

void mp_snapshot(mp_state *out)
{
    pthread_mutex_lock(&lock);
    *out = st;
    pthread_mutex_unlock(&lock);
}

void mp_refresh(void) { push(&(cmd){.op = OP_REFRESH}); }

void mp_create(const char *name, const char *password)
{
    cmd c = {.op = OP_CREATE};
    snprintf(c.s1, sizeof(c.s1), "%s", name);
    snprintf(c.s2, sizeof(c.s2), "%s", password ? password : "");
    push(&c);
}

void mp_join(int id, const char *password)
{
    cmd c = {.op = OP_JOIN, .a = id};
    snprintf(c.s2, sizeof(c.s2), "%s", password ? password : "");
    push(&c);
}

void mp_leave(void) { push(&(cmd){.op = OP_LEAVE}); }
void mp_settings(bool teams, bool freemod) { push(&(cmd){.op = OP_SETTINGS, .f1 = teams, .f2 = freemod}); }
void mp_team(int team) { push(&(cmd){.op = OP_TEAM, .a = team}); }
void mp_my_mods(int mods) { push(&(cmd){.op = OP_MODS, .mods = mods}); }

void mp_chat(const char *text)
{
    cmd c = {.op = OP_CHAT};
    snprintf(c.s1, sizeof(c.s1), "%s", text);
    push(&c);
}

void mp_say(const char *to, const char *text)
{
    cmd c = {.op = OP_SAY};
    snprintf(c.s1, sizeof(c.s1), "%s", text);
    snprintf(c.s2, sizeof(c.s2), "%s", to);
    push(&c);
}

void mp_channel_join(const char *name)
{
    cmd c = {.op = OP_CHAN_JOIN};
    snprintf(c.s2, sizeof(c.s2), "%s", name);
    push(&c);
}

void mp_channel_leave(const char *name)
{
    cmd c = {.op = OP_CHAN_PART};
    snprintf(c.s2, sizeof(c.s2), "%s", name);
    push(&c);
}

void mp_channel_seen(const char *name)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < nchans; i++)
        if (!strcasecmp(chans[i].name, name))
            chans[i].unread = 0;
    pthread_mutex_unlock(&lock);
}

void mp_user_stats(int user_id) { push(&(cmd){.op = OP_STATS, .a = user_id}); }

void mp_user_lookup(const char *name)
{
    cmd c = {.op = OP_LOOKUP};
    snprintf(c.s1, sizeof(c.s1), "%s", name);
    push(&c);
}

void mp_friend(int user_id, bool add) { push(&(cmd){.op = OP_FRIEND, .a = user_id, .f1 = add}); }
void mp_host_to(int slot) { push(&(cmd){.op = OP_HOST, .a = slot}); }
void mp_lock_slot(int slot) { push(&(cmd){.op = OP_LOCK, .a = slot}); }
void mp_move_slot(int slot) { push(&(cmd){.op = OP_SLOT, .a = slot}); }
void mp_win_condition(int win) { push(&(cmd){.op = OP_WIN, .a = win}); }

void mp_status(int action, const char *text, const char *md5, int mods, int mode, int map_id)
{
    cmd c = {.op = OP_STATUS, .a = action, .b = map_id, .mods = mods, .mode = mode};
    snprintf(c.s1, sizeof(c.s1), "%s", text ? text : "");
    snprintf(c.md5, sizeof(c.md5), "%s", md5 ? md5 : "");
    push(&c);
}

void mp_spectate(int user_id) { push(&(cmd){.op = OP_SPECTATE, .a = user_id}); }

int mp_spec_take(mp_spec_frame *out, int max, int *action)
{
    pthread_mutex_lock(&lock);
    int n = nspec < max ? nspec : max;
    memcpy(out, spec, sizeof(spec[0]) * (size_t)n);
    memmove(spec, spec + n, sizeof(spec[0]) * (size_t)(nspec - n));
    nspec -= n;
    if (action) {
        *action = spec_action;
        spec_action = 0;
    }
    pthread_mutex_unlock(&lock);
    return n;
}

void mp_frames_push(double t, float x, float y, int keys)
{
    pthread_mutex_lock(&lock);
    if (noutgoing < (int)(sizeof(outgoing) / sizeof(outgoing[0])))
        outgoing[noutgoing++] = (mp_spec_frame){t, x, y, keys, 1};
    pthread_mutex_unlock(&lock);
}

void mp_lobby_chat(const char *text)
{
    cmd c = {.op = OP_LOBBY_CHAT};
    snprintf(c.s1, sizeof(c.s1), "%s", text);
    push(&c);
}

void mp_set_map(int beatmap_id, int set_id, int mode, const char *md5, const char *title, int mods)
{
    cmd c = {.op = OP_MAP, .a = beatmap_id, .b = set_id, .mods = mods, .mode = mode};
    snprintf(c.md5, sizeof(c.md5), "%s", md5);
    snprintf(c.s1, sizeof(c.s1), "%s", title);
    push(&c);
}

void mp_ready(bool ready, bool has_map) { push(&(cmd){.op = OP_READY, .f1 = ready, .f2 = has_map}); }
void mp_start(void) { push(&(cmd){.op = OP_START}); }

void mp_score(long long score, int combo, int max_combo, float acc, int n300, int n100, int n50, int miss,
              bool failed, bool done)
{
    cmd c = {.op = OP_SCORE, .score = score, .combo = combo, .max_combo = max_combo, .acc = acc, .n300 = n300,
             .n100 = n100, .n50 = n50, .miss = miss, .f1 = failed, .f2 = done};
    push(&c);
}

/* ---- how many are on the room server, for the main menu, without joining ---- */

static pthread_mutex_t peek_lock = PTHREAD_MUTEX_INITIALIZER;
static int peek_count = -1;
static double peek_at = -1e9;
static bool peek_busy;
static char peek_url[200];

static void *peek_thread(void *arg)
{
    (void)arg;
    char *out;
    size_t len;
    int n = -1;
    int st_ = net_get(peek_url, &out, &len);
    if (st_ == 200) {
        const char *k = strstr(out, "\"online\":");
        if (k)
            n = atoi(k + 9);
        free(out);
    }
    pthread_mutex_lock(&peek_lock);
    if (n >= 0)
        peek_count = n;
    peek_busy = false;
    LOG("mp: %s -> %d, %d online", peek_url, st_, n);
    pthread_mutex_unlock(&peek_lock);
    return NULL;
}

int mp_peek_online(int kind_, const char *address)
{
    pthread_mutex_lock(&peek_lock);
    int n = peek_count;
    char want[200];
    /* bancho.py publishes its count on the api. subdomain; our rooms list carries it too */
    if (kind_ == MP_BANCHO)
        snprintf(want, sizeof(want), "https://api.%s/v1/get_player_count", address);
    else
        snprintf(want, sizeof(want), "%s/api/rooms", address);
    if (strcmp(want, peek_url))
        peek_count = -1, n = -1, peek_at = -1e9; /* another server: forget the old number */
    if (!peek_busy && address[0] && wall_ms() - peek_at > 5000) {
        peek_at = wall_ms();
        peek_busy = true;
        snprintf(peek_url, sizeof(peek_url), "%s", want);
        pthread_t t;
        if (pthread_create(&t, NULL, peek_thread, NULL) == 0)
            pthread_detach(t);
        else
            peek_busy = false;
    }
    pthread_mutex_unlock(&peek_lock);
    return n;
}
