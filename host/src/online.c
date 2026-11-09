#include "online.h"
#include "../include/osu5.h"
#include "json.h"
#include "log.h"
#include "md5.h"
#include "net.h"
#include "sce.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define KEY_FILE "/data/cadence/osuapi.txt"
#define CACHED 16

static char client_id[32], secret[128];
static char token[2048];
static time_t token_until;
static bool enabled;

typedef struct {
    int state;
    time_t when;
    online_board b;
} slot;

static slot cache[CACHED];
static int wanted;
static char wanted_path[768];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
        s[--n] = 0;
}

/* ---- token ---- */

static bool token_key(js *j, const char *key, void *ctx)
{
    if (!strcmp(key, "access_token"))
        return json_str(j, token, sizeof(token));
    if (!strcmp(key, "expires_in")) {
        *(double *)ctx = json_num(j);
        return true;
    }
    return false;
}

static bool get_token(void)
{
    if (token[0] && time(NULL) < token_until)
        return true;
    char body[256];
    snprintf(body, sizeof(body),
             "{\"client_id\":%s,\"client_secret\":\"%s\",\"grant_type\":\"client_credentials\",\"scope\":\"public\"}",
             client_id, secret);
    static const char *const hdr[] = {"Content-Type", "application/json", "Accept", "application/json", NULL};
    char *out;
    size_t len;
    int st = net_request("POST", "https://osu.ppy.sh/oauth/token", hdr, body, &out, &len);
    if (st != 200) {
        LOG("online: token %d", st);
        return false;
    }
    token[0] = 0;
    double expires = 0;
    js j = {out};
    json_object(&j, token_key, &expires);
    free(out);
    if (!token[0])
        return false;
    token_until = time(NULL) + (time_t)(expires > 120 ? expires - 60 : 600);
    return true;
}

/* ---- leaderboard ---- */

static bool user_key(js *j, const char *key, void *ctx)
{
    if (strcmp(key, "username"))
        return false;
    return json_str(j, (char *)ctx, 32);
}

static bool mod_key(js *j, const char *key, void *ctx)
{
    if (strcmp(key, "acronym"))
        return false;
    char m[8];
    if (!json_str(j, m, sizeof(m)))
        return false;
    char *mods = ctx;
    if (strcmp(m, "CL") && strlen(mods) + strlen(m) < 31)
        strcat(mods, m);
    return true;
}

static bool score_key(js *j, const char *key, void *ctx)
{
    online_score *s = ctx;
    if (!strcmp(key, "total_score"))
        s->score = (long long)json_num(j);
    else if (!strcmp(key, "pp"))
        s->pp = (float)json_num(j);
    else if (!strcmp(key, "accuracy"))
        s->accuracy = (float)json_num(j);
    else if (!strcmp(key, "max_combo"))
        s->combo = (int)json_num(j);
    else if (!strcmp(key, "rank")) {
        static const char *names[] = {"D", "C", "B", "A", "S", "SH", "X", "XH", "F"};
        char r[4];
        if (!json_str(j, r, sizeof(r)))
            return false;
        s->rank = OSU5_RANK_F;
        for (int i = 0; i < 9; i++)
            if (!strcmp(r, names[i]))
                s->rank = i;
    }
    else if (!strcmp(key, "user"))
        return json_object(j, user_key, s->user);
    else if (!strcmp(key, "mods") && *j->p == '[') {
        j->p++;
        for (;;) {
            json_ws(j);
            if (*j->p == ']') {
                j->p++;
                return true;
            }
            if (!json_object(j, mod_key, s->mods))
                return false;
            json_ws(j);
            if (*j->p == ',')
                j->p++;
        }
    } else
        return false;
    return true;
}

static bool board_key(js *j, const char *key, void *ctx)
{
    online_board *b = ctx;
    if (strcmp(key, "scores") || *j->p != '[')
        return false;
    j->p++;
    for (;;) {
        json_ws(j);
        if (*j->p == ']') {
            j->p++;
            return true;
        }
        online_score s;
        memset(&s, 0, sizeof(s));
        if (!json_object(j, score_key, &s))
            return false;
        if (b->count < ONLINE_MAX)
            b->s[b->count++] = s;
        json_ws(j);
        if (*j->p == ',')
            j->p++;
    }
}

static bool id_key(js *j, const char *key, void *ctx)
{
    if (strcmp(key, "id"))
        return false;
    *(int *)ctx = (int)json_num(j);
    return true;
}

static int api_get(const char *url, char **out)
{
    char auth[2100];
    snprintf(auth, sizeof(auth), "Bearer %s", token);
    const char *hdr[] = {"Authorization", auth, "Accept", "application/json", "x-api-version", "20240529", NULL};
    size_t len;
    int st = net_request("GET", url, hdr, NULL, out, &len);
    if (st == 401)
        token[0] = 0;
    return st;
}

/* key > 0 is the beatmap id; otherwise the map didn't say, so ask by checksum */
static int fetch(int key, const char *path, online_board *b)
{
    if (!get_token())
        return ONLINE_FAILED;
    char url[200], *out;
    int id = key;
    if (id <= 0) {
        char md5[33];
        if (!md5_file(path, md5))
            return ONLINE_FAILED;
        snprintf(url, sizeof(url), "https://osu.ppy.sh/api/v2/beatmaps/lookup?checksum=%s", md5);
        int st = api_get(url, &out);
        if (st == 404)
            return ONLINE_READY; /* edited or never uploaded */
        if (st != 200)
            return ONLINE_FAILED;
        js j = {out};
        id = 0;
        json_object(&j, id_key, &id);
        free(out);
        if (id <= 0)
            return ONLINE_READY;
        b->online_id = id;
    }
    snprintf(url, sizeof(url), "https://osu.ppy.sh/api/v2/beatmaps/%d/scores?mode=osu&limit=%d", id, ONLINE_MAX);
    int st = api_get(url, &out);
    if (st == 404)
        return ONLINE_READY; /* not on the site */
    if (st != 200) {
        LOG("online: map %d: %d", id, st);
        return ONLINE_FAILED;
    }
    js j = {out};
    json_object(&j, board_key, b);
    free(out);
    b->online_id = id;
    return ONLINE_READY;
}

static slot *find(int id)
{
    for (int i = 0; i < CACHED; i++)
        if (cache[i].b.beatmap_id == id)
            return &cache[i];
    return NULL;
}

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&lock);
        while (!wanted)
            pthread_cond_wait(&wake, &lock);
        int id = wanted;
        char path[768];
        snprintf(path, sizeof(path), "%s", wanted_path);
        wanted = 0;
        pthread_mutex_unlock(&lock);

        static online_board b;
        memset(&b, 0, sizeof(b));
        b.beatmap_id = id;
        int st = fetch(id, path, &b);

        pthread_mutex_lock(&lock);
        slot *s = find(id);
        if (!s) {
            /* oldest goes */
            s = &cache[0];
            for (int i = 1; i < CACHED; i++)
                if (cache[i].when < s->when)
                    s = &cache[i];
        }
        s->b = b;
        s->state = st;
        s->when = time(NULL);
        pthread_mutex_unlock(&lock);

        sceKernelUsleep(1000 * 1000); /* the API asks for about one request a second */
    }
    return NULL;
}

void online_init(void)
{
    FILE *f = fopen(KEY_FILE, "r");
    if (!f)
        return;
    bool ok = fgets(client_id, sizeof(client_id), f) && fgets(secret, sizeof(secret), f);
    fclose(f);
    trim(client_id);
    trim(secret);
    if (!ok || !client_id[0] || !secret[0])
        return;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_t t;
    enabled = pthread_create(&t, &attr, worker, NULL) == 0;
    if (enabled)
        pthread_detach(t);
    pthread_attr_destroy(&attr);
    LOG("online: %s", enabled ? "on" : "thread failed");
}

bool online_enabled(void)
{
    return enabled;
}

static bool version_key(js *j, const char *key, void *ctx)
{
    char *out = ctx;
    if (strcmp(key, "version") || out[0])
        return false;
    char v[32];
    if (!json_str(j, v, sizeof(v)))
        return false;
    /* stable's builds look like 20260924 or 20260924.1; lazer's don't */
    if (strlen(v) >= 8 && v[0] == '2' && v[4] != '.')
        snprintf(out, 32, "b%s", v);
    return true;
}

static bool builds_key(js *j, const char *key, void *ctx)
{
    if (strcmp(key, "builds") || *j->p != '[')
        return false;
    j->p++;
    for (;;) {
        json_ws(j);
        if (*j->p == ']') {
            j->p++;
            return true;
        }
        if (!json_object(j, version_key, ctx))
            return false;
        json_ws(j);
        if (*j->p == ',')
            j->p++;
    }
}

/* the newest osu! stable build, which bancho servers want to see at login */
bool online_stable_version(char *out, size_t n)
{
    if (!enabled || !get_token())
        return false;
    char *body;
    if (api_get("https://osu.ppy.sh/api/v2/changelog?stream=stable40", &body) != 200)
        return false;
    char v[32] = "";
    js j = {body};
    json_object(&j, builds_key, v);
    free(body);
    if (!v[0])
        return false;
    snprintf(out, n, "%s", v);
    return true;
}

int online_board_for(int beatmap_id, const char *osu_path, online_board *out)
{
    if (!enabled)
        return ONLINE_OFF;
    int id = beatmap_id;
    if (id <= 0) {
        /* keyed by the path instead, negative so it can't meet a real id */
        uint32_t h = 2166136261u;
        for (const char *c = osu_path; *c; c++)
            h = (h ^ (uint8_t)*c) * 16777619u;
        id = -(int)(h & 0x7FFFFFFF) - 1;
    }
    pthread_mutex_lock(&lock);
    slot *s = find(id);
    int st = ONLINE_LOADING;
    /* failures and boards older than ten minutes get asked for again */
    if (s && s->state == ONLINE_READY && time(NULL) - s->when < 600) {
        *out = s->b;
        st = ONLINE_READY;
    } else if (s && s->state == ONLINE_FAILED && time(NULL) - s->when < 30) {
        st = ONLINE_FAILED;
    } else if (wanted != id) {
        wanted = id;
        snprintf(wanted_path, sizeof(wanted_path), "%s", osu_path);
        pthread_cond_signal(&wake);
    }
    pthread_mutex_unlock(&lock);
    return st;
}
