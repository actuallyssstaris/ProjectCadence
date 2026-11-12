/* ProjectCadence accounts, scores, rankings, replays, friends and clans
 * (server/cloudflare, the /api calls outside the rooms). Every call runs on a
 * short-lived thread of its own. */
#include "account.h"
#include "bweb.h"
#include "json.h"
#include "log.h"
#include "net.h"
#include "online.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ACCOUNT_FILE "/data/cadence/account.txt"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char base[160];
static char name[24], session[80], clan[8];
static int state = ACC_IDLE;
static char message[128];
static int last_place;

static void spawn(void *(*fn)(void *), void *arg)
{
    pthread_t t;
    if (pthread_create(&t, NULL, fn, arg) == 0)
        pthread_detach(t);
    else
        free(arg);
}

static void escape(const char *in, char *out, size_t n)
{
    size_t k = 0;
    for (; *in && k + 2 < n; in++) {
        if (*in == '"' || *in == '\\')
            out[k++] = '\\';
        out[k++] = (unsigned char)*in < 32 ? ' ' : *in;
    }
    out[k] = 0;
}

static void url_encode(const char *in, char *out, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t k = 0;
    for (; *in && k + 4 < n; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || c == '-' || c == '_' || c == '.') {
            out[k++] = (char)c;
        } else {
            out[k++] = '%';
            out[k++] = hex[c >> 4];
            out[k++] = hex[c & 15];
        }
    }
    out[k] = 0;
}

static int post(const char *path, const char *body, char **out)
{
    char url[256];
    snprintf(url, sizeof(url), "%s%s", base, path);
    static const char *const hdr[] = {"Content-Type", "application/json", NULL};
    size_t len;
    return net_request("POST", url, hdr, body, out, &len);
}

static int get(const char *path, char **out, size_t *len)
{
    char url[512];
    snprintf(url, sizeof(url), "%s%s", base, path);
    return net_get(url, out, len);
}

static void save(void)
{
    FILE *f = fopen(ACCOUNT_FILE, "w");
    if (f) {
        fprintf(f, "%s\n%s\n%s\n", name, session, clan);
        fclose(f);
    }
}

void account_init(const char *server)
{
    snprintf(base, sizeof(base), "%s", server);
    FILE *f = fopen(ACCOUNT_FILE, "r");
    if (!f)
        return;
    if (fgets(name, sizeof(name), f) && fgets(session, sizeof(session), f)) {
        name[strcspn(name, "\r\n")] = 0;
        session[strcspn(session, "\r\n")] = 0;
        if (fgets(clan, sizeof(clan), f))
            clan[strcspn(clan, "\r\n")] = 0;
    } else {
        name[0] = session[0] = 0;
    }
    fclose(f);
}

bool account_signed_in(void) { return session[0] != 0; }
const char *account_name(void) { return name; }
const char *account_session(void) { return session; }
const char *account_clan(void) { return clan; }

/* ---- signing in ---- */

typedef struct {
    char name[24], password[64];
    bool create;
} sign_job;

typedef struct {
    char session[80], name[24], clan[8], error[128];
} sign_reply;

static bool sign_key(js *j, const char *key, void *ctx)
{
    sign_reply *r = ctx;
    if (!strcmp(key, "session")) return json_str(j, r->session, sizeof(r->session));
    if (!strcmp(key, "name")) return json_str(j, r->name, sizeof(r->name));
    if (!strcmp(key, "clan")) return json_str(j, r->clan, sizeof(r->clan));
    if (!strcmp(key, "error")) return json_str(j, r->error, sizeof(r->error));
    return false;
}

static void *sign_thread(void *arg)
{
    sign_job *job = arg;
    char en[64], ep[160], body[300];
    escape(job->name, en, sizeof(en));
    escape(job->password, ep, sizeof(ep));
    snprintf(body, sizeof(body), "{\"name\":\"%s\",\"password\":\"%s\"}", en, ep);
    char *out = NULL;
    int st = post(job->create ? "/api/account/register" : "/api/account/login", body, &out);

    sign_reply r;
    memset(&r, 0, sizeof(r));
    if (out) {
        js j = {out};
        json_object(&j, sign_key, &r);
        free(out);
    }
    pthread_mutex_lock(&lock);
    if (st == 200 && r.session[0]) {
        snprintf(session, sizeof(session), "%s", r.session);
        snprintf(name, sizeof(name), "%s", r.name);
        snprintf(clan, sizeof(clan), "%s", r.clan);
        save();
        state = ACC_OK;
        message[0] = 0;
    } else {
        state = ACC_FAILED;
        /* the server's reason doesn't survive a non-200 here, so say it ourselves */
        snprintf(message, sizeof(message), "%s", r.error[0] ? r.error
                 : st == 401 ? "wrong name or password"
                 : st == 409 ? "that name is taken"
                 : st == 400 ? "names are 3-20 letters, digits, spaces, _ or -; passwords 6+ characters"
                 : "couldn't reach the server");
    }
    pthread_mutex_unlock(&lock);
    LOG("account: %s %s -> %d", job->create ? "register" : "login", job->name, st);
    free(job);
    return NULL;
}

void account_sign_in(const char *n, const char *password, bool create)
{
    sign_job *job = calloc(1, sizeof(*job));
    if (!job)
        return;
    snprintf(job->name, sizeof(job->name), "%s", n);
    snprintf(job->password, sizeof(job->password), "%s", password);
    job->create = create;
    pthread_mutex_lock(&lock);
    state = ACC_BUSY;
    message[0] = 0;
    pthread_mutex_unlock(&lock);
    spawn(sign_thread, job);
}

void account_sign_out(void)
{
    pthread_mutex_lock(&lock);
    name[0] = session[0] = clan[0] = 0;
    state = ACC_IDLE;
    remove(ACCOUNT_FILE);
    pthread_mutex_unlock(&lock);
}

int account_state(char *msg, int n)
{
    pthread_mutex_lock(&lock);
    int s = state;
    if (msg)
        snprintf(msg, (size_t)n, "%s", message);
    pthread_mutex_unlock(&lock);
    return s;
}

/* ---- submitting ---- */

static void *submit_thread(void *arg)
{
    char *body = arg, *out = NULL;
    int st = post("/api/scores/submit", body, &out);
    int place = 0;
    if (st == 200 && out) {
        if (strstr(out, "\"best\":true")) {
            const char *p = strstr(out, "\"place\":");
            if (p)
                place = atoi(p + 8);
        }
    }
    free(out);
    pthread_mutex_lock(&lock);
    last_place = place;
    pthread_mutex_unlock(&lock);
    LOG("account: score sent -> %d, place %d", st, place);
    free(body);
    return NULL;
}

void account_submit(const char *md5, int beatmap_id, int mode, long long score, float acc, int max_combo, int mods,
                    float pp, int rank, int n300, int n100, int n50, int miss, const replay *rp)
{
    if (!session[0] || !md5[0])
        return;
    /* the replay rides along, LZMA'd and base64'd, so the board can show it */
    size_t packed_n = 0;
    uint8_t *packed = rp && rp->count ? osr_pack(rp, mode, &packed_n) : NULL;
    size_t cap = 700 + (packed_n + 2) / 3 * 4;
    char *body = malloc(cap);
    if (!body) {
        free(packed);
        return;
    }
    int k = snprintf(body, cap,
                     "{\"session\":\"%s\",\"md5\":\"%s\",\"beatmap_id\":%d,\"mode\":%d,\"score\":%lld,\"acc\":%.5f,\"max_combo\":%d,"
                     "\"mods\":%d,\"pp\":%.2f,\"rank\":%d,\"n300\":%d,\"n100\":%d,\"n50\":%d,\"miss\":%d,\"replay\":\"",
                     session, md5, beatmap_id, mode, score, acc, max_combo, mods, pp, rank, n300, n100, n50, miss);
    if (packed)
        k += (int)b64_encode(packed, packed_n, body + k);
    snprintf(body + k, cap - (size_t)k, "\"}");
    free(packed);
    pthread_mutex_lock(&lock);
    last_place = 0;
    pthread_mutex_unlock(&lock);
    spawn(submit_thread, body);
}

int account_last_place(void)
{
    pthread_mutex_lock(&lock);
    int p = last_place;
    pthread_mutex_unlock(&lock);
    return p;
}

/* ---- the global board ---- */

static global_board board;
static int board_state = ONLINE_OFF;
static time_t board_when;
static bool board_busy;

static bool score_key(js *j, const char *key, void *ctx)
{
    global_score *s = ctx;
    if (!strcmp(key, "name")) return json_str(j, s->name, sizeof(s->name));
    if (!strcmp(key, "score")) s->score = (long long)json_num(j);
    else if (!strcmp(key, "acc")) s->acc = (float)json_num(j);
    else if (!strcmp(key, "pp")) s->pp = (float)json_num(j);
    else if (!strcmp(key, "max_combo")) s->combo = (int)json_num(j);
    else if (!strcmp(key, "mods")) s->mods = (int)json_num(j);
    else if (!strcmp(key, "rank")) s->rank = (int)json_num(j);
    else if (!strcmp(key, "replay")) s->replay = json_num(j) != 0;
    else return false;
    return true;
}

/* walks a JSON array of objects, handing each to fn with a fresh item */
static bool each(js *j, key_fn fn, void *item, size_t size, void (*keep)(void *ctx, void *item), void *ctx)
{
    if (*j->p != '[')
        return false;
    j->p++;
    for (;;) {
        json_ws(j);
        if (*j->p == ']') {
            j->p++;
            return true;
        }
        memset(item, 0, size);
        if (!json_object(j, fn, item))
            return false;
        keep(ctx, item);
        json_ws(j);
        if (*j->p == ',')
            j->p++;
    }
}

static void keep_score(void *ctx, void *item)
{
    global_board *b = ctx;
    if (b->count < GLOBAL_MAX)
        b->s[b->count++] = *(global_score *)item;
}

static bool board_key(js *j, const char *key, void *ctx)
{
    global_board *b = ctx;
    if (!strcmp(key, "status")) {
        b->status = !strncmp(j->p, "null", 4) ? -99 : (int)json_num(j);
        if (b->status == -99)
            json_skip(j);
        return true;
    }
    if (strcmp(key, "scores"))
        return false;
    global_score s;
    return each(j, score_key, &s, sizeof(s), keep_score, b);
}

static void *board_thread(void *arg)
{
    char *md5 = arg, path[128], *out = NULL;
    size_t len;
    snprintf(path, sizeof(path), "/api/scores/top?md5=%s", md5);
    int st = get(path, &out, &len);
    static global_board b;
    memset(&b, 0, sizeof(b));
    b.status = -99;
    snprintf(b.md5, sizeof(b.md5), "%s", md5);
    if (st == 200 && out) {
        js j = {out};
        json_object(&j, board_key, &b);
    }
    free(out);
    pthread_mutex_lock(&lock);
    if (!strcmp(md5, board.md5) || board_state == ONLINE_LOADING) {
        board = b;
        board_state = st == 200 ? ONLINE_READY : ONLINE_FAILED;
        board_when = time(NULL);
    }
    board_busy = false;
    pthread_mutex_unlock(&lock);
    free(md5);
    return NULL;
}

int account_board(const char *md5, global_board *out)
{
    if (!md5[0])
        return ONLINE_OFF;
    pthread_mutex_lock(&lock);
    bool same = !strcmp(md5, board.md5);
    int st = same ? board_state : ONLINE_LOADING;
    /* a fresh copy for a new map, or every minute for the same one */
    if (!board_busy && (!same || time(NULL) - board_when > 60)) {
        char *arg = strdup(md5);
        if (arg) {
            board_busy = true;
            if (!same) {
                memset(&board, 0, sizeof(board));
                snprintf(board.md5, sizeof(board.md5), "%s", md5);
                board_state = ONLINE_LOADING;
                st = ONLINE_LOADING;
            }
            pthread_mutex_unlock(&lock);
            spawn(board_thread, arg);
            pthread_mutex_lock(&lock);
        }
    }
    if (st == ONLINE_READY || st == ONLINE_FAILED)
        *out = board;
    pthread_mutex_unlock(&lock);
    return st;
}

/* ---- replays ---- */

static struct {
    int state, gen;
    replay r;
} rp_got;

typedef struct {
    int gen, mode;
    char md5[33], player[24], path[768];
} rp_job;

static void *replay_thread(void *arg)
{
    rp_job *q = arg;
    char who[80], path[200], *out = NULL;
    size_t len = 0;
    url_encode(q->player, who, sizeof(who));
    snprintf(path, sizeof(path), "/api/replay?md5=%s&name=%s", q->md5, who);
    int st = get(path, &out, &len);
    replay r;
    memset(&r, 0, sizeof(r));
    bool ok = false;
    if (st == 200 && out && len > 13) {
        replay_reset(&r, q->path, 0);
        ok = osr_unpack((const uint8_t *)out, len, q->mode, &r);
    }
    free(out);
    LOG("account: replay %s by %s -> %d, %d frames", q->md5, q->player, st, r.count);
    pthread_mutex_lock(&lock);
    if (q->gen == rp_got.gen) {
        replay_free(&rp_got.r);
        rp_got.r = r;
        rp_got.state = ok ? ONLINE_READY : ONLINE_FAILED;
    } else {
        replay_free(&r);
    }
    pthread_mutex_unlock(&lock);
    free(q);
    return NULL;
}

void account_replay_get(const char *md5, const char *player, int mode, const char *osu_path)
{
    rp_job *q = calloc(1, sizeof(*q));
    if (!q)
        return;
    pthread_mutex_lock(&lock);
    q->gen = ++rp_got.gen;
    rp_got.state = ONLINE_LOADING;
    pthread_mutex_unlock(&lock);
    q->mode = mode;
    snprintf(q->md5, sizeof(q->md5), "%s", md5);
    snprintf(q->player, sizeof(q->player), "%s", player);
    snprintf(q->path, sizeof(q->path), "%s", osu_path);
    spawn(replay_thread, q);
}

int account_replay_take(replay *out)
{
    pthread_mutex_lock(&lock);
    int st = rp_got.state;
    if (st == ONLINE_READY) {
        replay_free(out);
        *out = rp_got.r;
        memset(&rp_got.r, 0, sizeof(rp_got.r));
        rp_got.state = ONLINE_OFF;
    } else if (st == ONLINE_FAILED) {
        rp_got.state = ONLINE_OFF;
    }
    pthread_mutex_unlock(&lock);
    return st;
}

/* ---- rankings ---- */

static cadence_rankings ranks;
static int ranks_gen;

static bool rank_key(js *j, const char *key, void *ctx)
{
    cadence_rank *r = ctx;
    if (!strcmp(key, "name")) return json_str(j, r->name, sizeof(r->name));
    if (!strcmp(key, "clan")) return json_str(j, r->clan, sizeof(r->clan));
    if (!strcmp(key, "rank")) r->rank = (int)json_num(j);
    else if (!strcmp(key, "pp")) r->pp = (int)json_num(j);
    else if (!strcmp(key, "acc")) r->acc = (float)json_num(j);
    else if (!strcmp(key, "plays")) r->plays = (int)json_num(j);
    else return false;
    return true;
}

static void keep_rank(void *ctx, void *item)
{
    cadence_rankings *r = ctx;
    if (r->count < GLOBAL_MAX)
        r->r[r->count++] = *(cadence_rank *)item;
}

static bool ranks_key(js *j, const char *key, void *ctx)
{
    cadence_rankings *r = ctx;
    if (!strcmp(key, "total")) {
        r->total = (int)json_num(j);
        return true;
    }
    if (strcmp(key, "users"))
        return false;
    cadence_rank item;
    return each(j, rank_key, &item, sizeof(item), keep_rank, r);
}

typedef struct {
    int gen, mode, page;
    char name[24];
} list_job;

static void *ranks_thread(void *arg)
{
    list_job *q = arg;
    char path[96], *out = NULL;
    size_t len;
    snprintf(path, sizeof(path), "/api/users/top?mode=%d&page=%d", q->mode, q->page);
    int st = get(path, &out, &len);
    static cadence_rankings got;
    memset(&got, 0, sizeof(got));
    if (st == 200 && out) {
        js j = {out};
        json_object(&j, ranks_key, &got);
    }
    free(out);
    pthread_mutex_lock(&lock);
    if (q->gen == ranks_gen) {
        got.state = st == 200 ? ONLINE_READY : ONLINE_FAILED;
        got.mode = q->mode;
        got.page = q->page;
        ranks = got;
    }
    pthread_mutex_unlock(&lock);
    free(q);
    return NULL;
}

void account_rankings_get(int mode, int page)
{
    list_job *q = calloc(1, sizeof(*q));
    if (!q)
        return;
    pthread_mutex_lock(&lock);
    q->gen = ++ranks_gen;
    ranks.state = ONLINE_LOADING;
    ranks.mode = q->mode = mode;
    ranks.page = q->page = page;
    pthread_mutex_unlock(&lock);
    spawn(ranks_thread, q);
}

void account_rankings_read(cadence_rankings *out)
{
    pthread_mutex_lock(&lock);
    *out = ranks;
    pthread_mutex_unlock(&lock);
}

/* ---- profiles ---- */

static cadence_user prof;
static int prof_gen;

typedef struct {
    cadence_user *u;
    int i;
} top_ctx;

static bool top_key(js *j, const char *key, void *ctx)
{
    typeof(prof.top[0]) *t = ctx;
    if (!strcmp(key, "title")) {
        if (*j->p == '"')
            return json_str(j, t->title, sizeof(t->title));
        json_skip(j);
        return true;
    }
    if (!strcmp(key, "acc")) t->acc = (float)json_num(j);
    else if (!strcmp(key, "pp")) t->pp = (float)json_num(j);
    else if (!strcmp(key, "max_combo")) t->combo = (int)json_num(j);
    else if (!strcmp(key, "mods")) t->mods = (int)json_num(j);
    else if (!strcmp(key, "rank")) t->rank = (int)json_num(j);
    else return false;
    return true;
}

static void keep_top(void *ctx, void *item)
{
    cadence_user *u = ctx;
    if (u->ntop < 10)
        u->top[u->ntop++] = *(typeof(prof.top[0]) *)item;
}

static bool prof_key(js *j, const char *key, void *ctx)
{
    cadence_user *u = ctx;
    if (!strcmp(key, "name")) return json_str(j, u->name, sizeof(u->name));
    if (!strcmp(key, "clan")) return json_str(j, u->clan, sizeof(u->clan));
    if (!strcmp(key, "clan_name")) return json_str(j, u->clan_name, sizeof(u->clan_name));
    if (!strcmp(key, "rank")) u->rank = (int)json_num(j);
    else if (!strcmp(key, "pp")) u->pp = (int)json_num(j);
    else if (!strcmp(key, "acc")) u->acc = (float)json_num(j);
    else if (!strcmp(key, "plays")) u->plays = (int)json_num(j);
    else if (!strcmp(key, "max_combo")) u->max_combo = (int)json_num(j);
    else if (!strcmp(key, "ranked_score")) u->ranked_score = (long long)json_num(j);
    else if (!strcmp(key, "total_score")) u->total_score = (long long)json_num(j);
    else if (!strcmp(key, "joined")) u->joined_ms = (long long)json_num(j);
    else if (!strcmp(key, "friend")) u->friend_ = !strncmp(j->p, "true", 4), json_skip(j);
    else if (!strcmp(key, "restricted")) u->restricted = !strncmp(j->p, "true", 4), json_skip(j);
    else if (!strcmp(key, "top")) {
        typeof(prof.top[0]) t;
        return each(j, top_key, &t, sizeof(t), keep_top, u);
    } else
        return false;
    return true;
}

static void *prof_thread(void *arg)
{
    list_job *q = arg;
    char who[80], path[256], *out = NULL;
    size_t len;
    url_encode(q->name, who, sizeof(who));
    snprintf(path, sizeof(path), "/api/users/profile?name=%s&mode=%d&session=%s", who, q->mode, session);
    int st = get(path, &out, &len);
    static cadence_user got;
    memset(&got, 0, sizeof(got));
    if (st == 200 && out) {
        js j = {out};
        json_object(&j, prof_key, &got);
    }
    free(out);
    pthread_mutex_lock(&lock);
    if (q->gen == prof_gen) {
        got.state = st == 200 && got.name[0] ? ONLINE_READY : ONLINE_FAILED;
        got.mode = q->mode;
        if (!got.name[0])
            snprintf(got.name, sizeof(got.name), "%s", q->name);
        prof = got;
    }
    pthread_mutex_unlock(&lock);
    free(q);
    return NULL;
}

void account_profile_get(const char *who, int mode)
{
    list_job *q = calloc(1, sizeof(*q));
    if (!q)
        return;
    snprintf(q->name, sizeof(q->name), "%s", who);
    pthread_mutex_lock(&lock);
    q->gen = ++prof_gen;
    q->mode = mode;
    memset(&prof, 0, sizeof(prof));
    prof.state = ONLINE_LOADING;
    prof.mode = mode;
    snprintf(prof.name, sizeof(prof.name), "%s", who);
    pthread_mutex_unlock(&lock);
    spawn(prof_thread, q);
}

void account_profile_read(cadence_user *out)
{
    pthread_mutex_lock(&lock);
    *out = prof;
    pthread_mutex_unlock(&lock);
}

/* ---- account changes ---- */

static int act_state = ACC_IDLE;
static char act_msg[128];

enum { ACT_FRIEND_ADD, ACT_FRIEND_DEL, ACT_CLAN_JOIN, ACT_CLAN_CREATE, ACT_CLAN_LEAVE, ACT_PASSWORD, ACT_DELETE,
       ACT_PL_CREATE, ACT_PL_ADD };

typedef struct {
    int kind;
    char a[64], b[64];
    char c[200]; /* playlist adds: the title */
    int n1, n2, n3;
} act_job;

typedef struct {
    char error[128], tag[8], session[80];
} act_reply;

static bool act_key(js *j, const char *key, void *ctx)
{
    act_reply *r = ctx;
    if (!strcmp(key, "error")) return json_str(j, r->error, sizeof(r->error));
    if (!strcmp(key, "tag")) return json_str(j, r->tag, sizeof(r->tag));
    if (!strcmp(key, "session")) return json_str(j, r->session, sizeof(r->session));
    return false;
}

static void *act_thread(void *arg)
{
    act_job *q = arg;
    char ea[140], eb[140], body[512];
    escape(q->a, ea, sizeof(ea));
    escape(q->b, eb, sizeof(eb));
    const char *path = "";
    switch (q->kind) {
    case ACT_FRIEND_ADD:
    case ACT_FRIEND_DEL:
        path = q->kind == ACT_FRIEND_ADD ? "/api/friends/add" : "/api/friends/remove";
        snprintf(body, sizeof(body), "{\"session\":\"%s\",\"name\":\"%s\"}", session, ea);
        break;
    case ACT_CLAN_JOIN:
        path = "/api/clans/join";
        snprintf(body, sizeof(body), "{\"session\":\"%s\",\"tag\":\"%s\"}", session, ea);
        break;
    case ACT_CLAN_CREATE:
        path = "/api/clans/create";
        snprintf(body, sizeof(body), "{\"session\":\"%s\",\"tag\":\"%s\",\"name\":\"%s\"}", session, ea, eb);
        break;
    case ACT_CLAN_LEAVE:
        path = "/api/clans/leave";
        snprintf(body, sizeof(body), "{\"session\":\"%s\"}", session);
        break;
    case ACT_PASSWORD:
        path = "/api/account/password";
        snprintf(body, sizeof(body), "{\"session\":\"%s\",\"password\":\"%s\",\"new_password\":\"%s\"}", session, ea, eb);
        break;
    case ACT_DELETE:
        path = "/api/account/delete";
        snprintf(body, sizeof(body), "{\"session\":\"%s\",\"password\":\"%s\"}", session, ea);
        break;
    case ACT_PL_CREATE:
        path = "/api/playlists/create";
        snprintf(body, sizeof(body), "{\"session\":\"%s\",\"name\":\"%s\"}", session, ea);
        break;
    case ACT_PL_ADD: {
        char et[300];
        escape(q->c, et, sizeof(et));
        path = "/api/playlists/add";
        snprintf(body, sizeof(body), "{\"session\":\"%s\",\"id\":%d,\"md5\":\"%s\",\"beatmap_id\":%d,\"set_id\":%d,\"title\":\"%.200s\"}",
                 session, q->n1, ea, q->n2, q->n3, et);
        break;
    }
    }
    char *out = NULL;
    int st = post(path, body, &out);
    act_reply r;
    memset(&r, 0, sizeof(r));
    if (out) {
        js j = {out};
        json_object(&j, act_key, &r);
        free(out);
    }
    pthread_mutex_lock(&lock);
    if (st == 200) {
        act_state = ACC_OK;
        switch (q->kind) {
        case ACT_FRIEND_ADD: snprintf(act_msg, sizeof(act_msg), "added %s as a friend", q->a); break;
        case ACT_FRIEND_DEL: snprintf(act_msg, sizeof(act_msg), "removed %s from your friends", q->a); break;
        case ACT_CLAN_JOIN:
        case ACT_CLAN_CREATE:
            snprintf(clan, sizeof(clan), "%s", r.tag);
            snprintf(act_msg, sizeof(act_msg), "you're in [%s] now", r.tag);
            save();
            break;
        case ACT_CLAN_LEAVE:
            clan[0] = 0;
            snprintf(act_msg, sizeof(act_msg), "you left your clan");
            save();
            break;
        case ACT_PASSWORD:
            if (r.session[0])
                snprintf(session, sizeof(session), "%s", r.session);
            save();
            snprintf(act_msg, sizeof(act_msg), "password changed");
            break;
        case ACT_DELETE:
            name[0] = session[0] = clan[0] = 0;
            remove(ACCOUNT_FILE);
            snprintf(act_msg, sizeof(act_msg), "account deleted");
            break;
        case ACT_PL_CREATE: snprintf(act_msg, sizeof(act_msg), "playlist made"); break;
        case ACT_PL_ADD: snprintf(act_msg, sizeof(act_msg), "added to the playlist"); break;
        }
    } else {
        act_state = ACC_FAILED;
        snprintf(act_msg, sizeof(act_msg), "%s", r.error[0] ? r.error
                 : st == 401 ? "wrong password, or sign in again"
                 : st == 404 ? "not found"
                 : st == 409 ? "that's taken, or you're already in a clan"
                 : "couldn't reach the server");
    }
    pthread_mutex_unlock(&lock);
    LOG("account: %s -> %d", path, st);
    free(q);
    return NULL;
}

static void act_full(int kind, const char *a, const char *b, const char *c, int n1, int n2, int n3);

static void act(int kind, const char *a, const char *b) { act_full(kind, a, b, NULL, 0, 0, 0); }

static void act_full(int kind, const char *a, const char *b, const char *c, int n1, int n2, int n3)
{
    if (!session[0]) {
        pthread_mutex_lock(&lock);
        act_state = ACC_FAILED;
        snprintf(act_msg, sizeof(act_msg), "sign in first");
        pthread_mutex_unlock(&lock);
        return;
    }
    act_job *q = calloc(1, sizeof(*q));
    if (!q)
        return;
    q->kind = kind;
    snprintf(q->a, sizeof(q->a), "%s", a ? a : "");
    snprintf(q->b, sizeof(q->b), "%s", b ? b : "");
    snprintf(q->c, sizeof(q->c), "%s", c ? c : "");
    q->n1 = n1;
    q->n2 = n2;
    q->n3 = n3;
    pthread_mutex_lock(&lock);
    act_state = ACC_BUSY;
    act_msg[0] = 0;
    pthread_mutex_unlock(&lock);
    spawn(act_thread, q);
}

void account_friend(const char *who, bool add) { act(add ? ACT_FRIEND_ADD : ACT_FRIEND_DEL, who, NULL); }
void account_clan_join(const char *tag) { act(ACT_CLAN_JOIN, tag, NULL); }
void account_clan_create(const char *tag, const char *cname) { act(ACT_CLAN_CREATE, tag, cname); }
void account_clan_leave(void) { act(ACT_CLAN_LEAVE, NULL, NULL); }
void account_password(const char *old_pw, const char *new_pw) { act(ACT_PASSWORD, old_pw, new_pw); }
void account_delete(const char *password) { act(ACT_DELETE, password, NULL); }

int account_action(char *msg, int n)
{
    pthread_mutex_lock(&lock);
    int s = act_state;
    if (msg)
        snprintf(msg, (size_t)n, "%s", act_msg);
    if (s == ACC_OK || s == ACC_FAILED)
        act_state = ACC_IDLE; /* read once */
    pthread_mutex_unlock(&lock);
    return s;
}

void account_playlist_create(const char *pname) { act(ACT_PL_CREATE, pname, NULL); }

void account_playlist_add(int id, const char *md5, int beatmap_id, int set_id, const char *title)
{
    act_full(ACT_PL_ADD, md5, NULL, title, id, beatmap_id, set_id);
}

/* ---- the daily challenge and playlists ---- */

static cadence_list list_got;
static int list_gen;

static bool item_key(js *j, const char *key, void *ctx)
{
    cadence_item *it = ctx;
    if (!strcmp(key, "md5")) return json_str(j, it->md5, sizeof(it->md5));
    if (!strcmp(key, "title")) return json_str(j, it->title, sizeof(it->title));
    if (!strcmp(key, "beatmap_id")) it->beatmap_id = (int)json_num(j);
    else if (!strcmp(key, "set_id")) it->set_id = (int)json_num(j);
    else return false;
    return true;
}

static bool entry_key(js *j, const char *key, void *ctx)
{
    cadence_entry *e = ctx;
    if (!strcmp(key, "name")) return json_str(j, e->name, sizeof(e->name));
    if (!strcmp(key, "score") || !strcmp(key, "total")) e->score = (long long)json_num(j);
    else if (!strcmp(key, "acc")) e->acc = (float)json_num(j);
    else if (!strcmp(key, "max_combo")) e->combo = (int)json_num(j);
    else if (!strcmp(key, "mods")) e->mods = (int)json_num(j);
    else if (!strcmp(key, "played")) e->played = (int)json_num(j);
    else return false;
    return true;
}

static void keep_item(void *ctx, void *item)
{
    cadence_list *l = ctx;
    if (l->nitems < 50)
        l->items[l->nitems++] = *(cadence_item *)item;
}

static void keep_entry(void *ctx, void *item)
{
    cadence_list *l = ctx;
    if (l->nboard < 50)
        l->board[l->nboard++] = *(cadence_entry *)item;
}

static bool list_key(js *j, const char *key, void *ctx)
{
    cadence_list *l = ctx;
    if (!strcmp(key, "items")) {
        cadence_item it;
        return each(j, item_key, &it, sizeof(it), keep_item, l);
    }
    if (!strcmp(key, "board") || !strcmp(key, "scores")) {
        cadence_entry e;
        return each(j, entry_key, &e, sizeof(e), keep_entry, l);
    }
    if (!strcmp(key, "name")) return json_str(j, l->name, sizeof(l->name));
    if (!strcmp(key, "owner")) return json_str(j, l->owner, sizeof(l->owner));
    if (!strcmp(key, "day")) return json_str(j, l->day, sizeof(l->day));
    if (!strcmp(key, "id")) l->id = (int)json_num(j);
    /* the daily's own map fields sit at the top level */
    else if (!strcmp(key, "md5")) return json_str(j, l->items[0].md5, sizeof(l->items[0].md5));
    else if (!strcmp(key, "title")) return json_str(j, l->items[0].title, sizeof(l->items[0].title));
    else if (!strcmp(key, "beatmap_id")) l->items[0].beatmap_id = (int)json_num(j);
    else if (!strcmp(key, "set_id")) l->items[0].set_id = (int)json_num(j);
    else return false;
    return true;
}

static void *list_thread(void *arg)
{
    list_job *q = arg;
    char path[96], *out = NULL;
    size_t len;
    if (q->page)
        snprintf(path, sizeof(path), "/api/playlists/get?id=%d", q->page);
    else
        snprintf(path, sizeof(path), "/api/daily");
    int st = get(path, &out, &len);
    static cadence_list got;
    memset(&got, 0, sizeof(got));
    if (st == 200 && out) {
        js j = {out};
        json_object(&j, list_key, &got);
        if (!q->page) {
            got.nitems = got.items[0].md5[0] ? 1 : 0;
            snprintf(got.name, sizeof(got.name), "daily challenge");
        }
    }
    free(out);
    pthread_mutex_lock(&lock);
    if (q->gen == list_gen) {
        got.state = st == 200 ? ONLINE_READY : ONLINE_FAILED;
        list_got = got;
    }
    pthread_mutex_unlock(&lock);
    free(q);
    return NULL;
}

static void list_ask(int id)
{
    list_job *q = calloc(1, sizeof(*q));
    if (!q)
        return;
    q->page = id;
    pthread_mutex_lock(&lock);
    q->gen = ++list_gen;
    memset(&list_got, 0, sizeof(list_got));
    list_got.state = ONLINE_LOADING;
    list_got.id = id;
    pthread_mutex_unlock(&lock);
    spawn(list_thread, q);
}

void account_daily_get(void) { list_ask(0); }
void account_playlist_get(int id) { list_ask(id); }

void account_list_read(cadence_list *out)
{
    pthread_mutex_lock(&lock);
    *out = list_got;
    pthread_mutex_unlock(&lock);
}

static cadence_playlists pls;

static bool pl_key(js *j, const char *key, void *ctx)
{
    typeof(pls.p[0]) *p = ctx;
    if (!strcmp(key, "name")) return json_str(j, p->name, sizeof(p->name));
    if (!strcmp(key, "owner")) return json_str(j, p->owner, sizeof(p->owner));
    if (!strcmp(key, "id")) p->id = (int)json_num(j);
    else if (!strcmp(key, "items")) p->items = (int)json_num(j);
    else return false;
    return true;
}

static void keep_pl(void *ctx, void *item)
{
    cadence_playlists *c = ctx;
    if (c->count < 50)
        c->p[c->count++] = *(typeof(pls.p[0]) *)item;
}

static bool pls_key(js *j, const char *key, void *ctx)
{
    if (strcmp(key, "playlists"))
        return false;
    typeof(pls.p[0]) p;
    return each(j, pl_key, &p, sizeof(p), keep_pl, ctx);
}

static void *pls_thread(void *arg)
{
    (void)arg;
    char *out = NULL;
    size_t len;
    int st = get("/api/playlists", &out, &len);
    static cadence_playlists got;
    memset(&got, 0, sizeof(got));
    if (st == 200 && out) {
        js j = {out};
        json_object(&j, pls_key, &got);
    }
    free(out);
    pthread_mutex_lock(&lock);
    got.state = st == 200 ? ONLINE_READY : ONLINE_FAILED;
    pls = got;
    pthread_mutex_unlock(&lock);
    return NULL;
}

void account_playlists_get(void)
{
    pthread_mutex_lock(&lock);
    pls.state = ONLINE_LOADING;
    pthread_mutex_unlock(&lock);
    pthread_t t;
    if (pthread_create(&t, NULL, pls_thread, NULL) == 0)
        pthread_detach(t);
}

void account_playlists_read(cadence_playlists *out)
{
    pthread_mutex_lock(&lock);
    *out = pls;
    pthread_mutex_unlock(&lock);
}
