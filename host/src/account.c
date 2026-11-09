/* ProjectCadence accounts and global scores (server/cloudflare, the /api/account
 * and /api/scores calls). Every call runs on a short-lived thread of its own. */
#include "account.h"
#include "json.h"
#include "log.h"
#include "net.h"
#include "online.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ACCOUNT_FILE "/data/cadence/account.txt"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char base[160];
static char name[24], session[80];
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

static int post(const char *path, const char *body, char **out)
{
    char url[256];
    snprintf(url, sizeof(url), "%s%s", base, path);
    static const char *const hdr[] = {"Content-Type", "application/json", NULL};
    size_t len;
    return net_request("POST", url, hdr, body, out, &len);
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
    } else {
        name[0] = session[0] = 0;
    }
    fclose(f);
}

bool account_signed_in(void) { return session[0] != 0; }
const char *account_name(void) { return name; }
const char *account_session(void) { return session; }

/* ---- signing in ---- */

typedef struct {
    char name[24], password[64];
    bool create;
} sign_job;

static bool sign_key(js *j, const char *key, void *ctx)
{
    char **f = ctx;
    if (!strcmp(key, "session")) return json_str(j, f[0], 80);
    if (!strcmp(key, "name")) return json_str(j, f[1], 24);
    if (!strcmp(key, "error")) return json_str(j, f[2], 128);
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

    char s[80] = "", n[24] = "", err[128] = "";
    char *fields[3] = {s, n, err};
    if (out) {
        js j = {out};
        json_object(&j, sign_key, fields);
        free(out);
    }
    pthread_mutex_lock(&lock);
    if (st == 200 && s[0]) {
        snprintf(session, sizeof(session), "%s", s);
        snprintf(name, sizeof(name), "%s", n);
        FILE *f = fopen(ACCOUNT_FILE, "w");
        if (f) {
            fprintf(f, "%s\n%s\n", name, session);
            fclose(f);
        }
        state = ACC_OK;
        message[0] = 0;
    } else {
        state = ACC_FAILED;
        /* the server's reason doesn't survive a non-200 here, so say it ourselves */
        snprintf(message, sizeof(message), "%s", err[0] ? err
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
    name[0] = session[0] = 0;
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
                    float pp, int rank, int n300, int n100, int n50, int miss)
{
    if (!session[0] || !md5[0])
        return;
    char *body = malloc(600);
    if (!body)
        return;
    snprintf(body, 600,
             "{\"session\":\"%s\",\"md5\":\"%s\",\"beatmap_id\":%d,\"mode\":%d,\"score\":%lld,\"acc\":%.5f,\"max_combo\":%d,"
             "\"mods\":%d,\"pp\":%.2f,\"rank\":%d,\"n300\":%d,\"n100\":%d,\"n50\":%d,\"miss\":%d}",
             session, md5, beatmap_id, mode, score, acc, max_combo, mods, pp, rank, n300, n100, n50, miss);
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
    else return false;
    return true;
}

static bool board_key(js *j, const char *key, void *ctx)
{
    global_board *b = ctx;
    if (strcmp(key, "scores") || *j->p != '[')
        return false;
    j->p++;
    for (;;) {
        json_ws(j);
        if (*j->p == ']') {
            j->p++;
            return true;
        }
        global_score s;
        memset(&s, 0, sizeof(s));
        if (!json_object(j, score_key, &s))
            return false;
        if (b->count < GLOBAL_MAX)
            b->s[b->count++] = s;
        json_ws(j);
        if (*j->p == ',')
            j->p++;
    }
}

static void *board_thread(void *arg)
{
    char *md5 = arg, url[256], *out = NULL;
    size_t len;
    snprintf(url, sizeof(url), "%s/api/scores/top?md5=%s", base, md5);
    int st = net_get(url, &out, &len);
    static global_board b;
    memset(&b, 0, sizeof(b));
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
