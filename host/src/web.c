#include "web.h"
#include "json.h"
#include "log.h"
#include "online.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const mode_slug[4] = {"osu", "taiko", "fruits", "mania"};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static web_rankings ranks;
static web_user who;
static int ranks_gen, who_gen;

/* ---- rankings ---- */

static bool rank_user_key(js *j, const char *key, void *ctx)
{
    web_rank *r = ctx;
    if (!strcmp(key, "id")) r->id = (int)json_num(j);
    else if (!strcmp(key, "username")) return json_str(j, r->user, sizeof(r->user));
    else if (!strcmp(key, "country_code")) return json_str(j, r->country, sizeof(r->country));
    else return false;
    return true;
}

static bool rank_key(js *j, const char *key, void *ctx)
{
    web_rank *r = ctx;
    if (!strcmp(key, "pp")) r->pp = (float)json_num(j);
    else if (!strcmp(key, "hit_accuracy")) r->acc = (float)json_num(j);
    else if (!strcmp(key, "play_count")) r->plays = (int)json_num(j);
    else if (!strcmp(key, "global_rank")) r->rank = (int)json_num(j);
    else if (!strcmp(key, "user") && *j->p == '{') return json_object(j, rank_user_key, r);
    else return false;
    return true;
}

static bool ranking_key(js *j, const char *key, void *ctx)
{
    web_rankings *out = ctx;
    if (strcmp(key, "ranking") || *j->p != '[')
        return false;
    j->p++;
    for (;;) {
        json_ws(j);
        if (*j->p == ']') {
            j->p++;
            return true;
        }
        web_rank r;
        memset(&r, 0, sizeof(r));
        if (!json_object(j, rank_key, &r))
            return false;
        if (out->count < WEB_RANKS)
            out->r[out->count++] = r;
        json_ws(j);
        if (*j->p == ',')
            j->p++;
    }
}

typedef struct {
    int gen, mode, page;
    char who[40];
} req;

static void *ranks_thread(void *arg)
{
    req *q = arg;
    char url[160], *body = NULL;
    snprintf(url, sizeof(url), "https://osu.ppy.sh/api/v2/rankings/%s/performance?cursor[page]=%d", mode_slug[q->mode],
             q->page + 1);
    static web_rankings got;
    memset(&got, 0, sizeof(got));
    int st = online_api(url, &body);
    if (st == 200) {
        js j = {body};
        json_object(&j, ranking_key, &got);
    } else {
        LOG("web: rankings %d", st);
    }
    free(body);

    pthread_mutex_lock(&lock);
    if (q->gen == ranks_gen) {
        got.state = st == 200 ? WEB_READY : WEB_FAILED;
        got.mode = q->mode;
        got.page = q->page;
        /* numbering for the page, in case the API left global_rank out */
        for (int i = 0; i < got.count; i++)
            if (!got.r[i].rank)
                got.r[i].rank = q->page * WEB_RANKS + i + 1;
        ranks = got;
    }
    pthread_mutex_unlock(&lock);
    free(q);
    return NULL;
}

static bool spawn(void *(*fn)(void *), req *q)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_t t;
    bool ok = pthread_create(&t, &attr, fn, q) == 0;
    if (ok)
        pthread_detach(t);
    pthread_attr_destroy(&attr);
    return ok;
}

void web_rankings_get(int mode, int page)
{
    req *q = calloc(1, sizeof(*q));
    if (!q)
        return;
    pthread_mutex_lock(&lock);
    q->gen = ++ranks_gen;
    ranks.state = online_enabled() ? WEB_LOADING : WEB_FAILED;
    ranks.mode = q->mode = mode & 3;
    ranks.page = q->page = page < 0 ? 0 : page;
    pthread_mutex_unlock(&lock);
    if (!online_enabled() || !spawn(ranks_thread, q))
        free(q);
}

void web_rankings_read(web_rankings *out)
{
    pthread_mutex_lock(&lock);
    *out = ranks;
    pthread_mutex_unlock(&lock);
}

/* ---- a player ---- */

static bool level_key(js *j, const char *key, void *ctx)
{
    web_user *u = ctx;
    if (!strcmp(key, "current")) u->level = (int)json_num(j);
    else if (!strcmp(key, "progress")) u->level_pct = (int)json_num(j);
    else return false;
    return true;
}

static bool grades_key(js *j, const char *key, void *ctx)
{
    web_user *u = ctx;
    /* silver ones count with the gold */
    if (!strcmp(key, "ss") || !strcmp(key, "ssh")) u->ss += (int)json_num(j);
    else if (!strcmp(key, "s") || !strcmp(key, "sh")) u->s += (int)json_num(j);
    else if (!strcmp(key, "a")) u->a = (int)json_num(j);
    else return false;
    return true;
}

static bool stats_key(js *j, const char *key, void *ctx)
{
    web_user *u = ctx;
    if (!strcmp(key, "global_rank")) u->rank = (int)json_num(j);
    else if (!strcmp(key, "country_rank")) u->country_rank = (int)json_num(j);
    else if (!strcmp(key, "pp")) u->pp = (float)json_num(j);
    else if (!strcmp(key, "hit_accuracy")) u->acc = (float)json_num(j);
    else if (!strcmp(key, "play_count")) u->plays = (int)json_num(j);
    else if (!strcmp(key, "play_time")) u->hours = (int)(json_num(j) / 3600);
    else if (!strcmp(key, "maximum_combo")) u->max_combo = (int)json_num(j);
    else if (!strcmp(key, "ranked_score")) u->ranked_score = (long long)json_num(j);
    else if (!strcmp(key, "total_score")) u->total_score = (long long)json_num(j);
    else if (!strcmp(key, "level") && *j->p == '{') return json_object(j, level_key, u);
    else if (!strcmp(key, "grade_counts") && *j->p == '{') return json_object(j, grades_key, u);
    else return false;
    return true;
}

static bool user_key(js *j, const char *key, void *ctx)
{
    web_user *u = ctx;
    if (!strcmp(key, "id")) u->id = (int)json_num(j);
    else if (!strcmp(key, "username")) return json_str(j, u->user, sizeof(u->user));
    else if (!strcmp(key, "country_code")) return json_str(j, u->country, sizeof(u->country));
    else if (!strcmp(key, "join_date")) return json_str(j, u->joined, sizeof(u->joined)); /* keeps the date part */
    else if (!strcmp(key, "is_supporter")) {
        u->supporter = !strncmp(j->p, "true", 4);
        json_skip(j);
    } else if (!strcmp(key, "statistics") && *j->p == '{')
        return json_object(j, stats_key, u);
    else
        return false;
    return true;
}

static void *user_thread(void *arg)
{
    req *q = arg;
    char url[200], name[120], *body = NULL;
    /* names go in as @name so one made of digits isn't read as an id */
    bool digits = q->who[0] != 0;
    for (const char *c = q->who; *c; c++)
        digits &= isdigit((unsigned char)*c) != 0;
    size_t k = 0;
    for (const char *c = q->who; *c && k + 4 < sizeof(name); c++) {
        unsigned char ch = (unsigned char)*c;
        if (isalnum(ch) || ch == '-' || ch == '_') {
            name[k++] = (char)ch;
        } else {
            k += (size_t)snprintf(name + k, sizeof(name) - k, "%%%02X", ch);
        }
    }
    name[k] = 0;
    snprintf(url, sizeof(url), "https://osu.ppy.sh/api/v2/users/%s%s/%s", digits ? "" : "@", name, mode_slug[q->mode]);

    static web_user got;
    memset(&got, 0, sizeof(got));
    int st = online_api(url, &body);
    if (st == 200) {
        js j = {body};
        json_object(&j, user_key, &got);
    } else {
        LOG("web: user %s: %d", q->who, st);
    }
    free(body);

    pthread_mutex_lock(&lock);
    if (q->gen == who_gen) {
        got.state = st == 200 && got.id ? WEB_READY : WEB_FAILED;
        got.mode = q->mode;
        who = got;
    }
    pthread_mutex_unlock(&lock);
    free(q);
    return NULL;
}

void web_user_get(const char *name, int mode)
{
    req *q = calloc(1, sizeof(*q));
    if (!q)
        return;
    snprintf(q->who, sizeof(q->who), "%s", name);
    pthread_mutex_lock(&lock);
    q->gen = ++who_gen;
    q->mode = mode & 3;
    memset(&who, 0, sizeof(who));
    who.state = online_enabled() ? WEB_LOADING : WEB_FAILED;
    who.mode = q->mode;
    snprintf(who.user, sizeof(who.user), "%s", name);
    pthread_mutex_unlock(&lock);
    if (!online_enabled() || !spawn(user_thread, q))
        free(q);
}

void web_user_read(web_user *out)
{
    pthread_mutex_lock(&lock);
    *out = who;
    pthread_mutex_unlock(&lock);
}
