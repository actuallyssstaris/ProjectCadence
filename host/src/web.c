#include "web.h"
#include "json.h"
#include "log.h"
#include "net.h"
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

/* ---- a beatmap set's page ---- */

static web_set set_got;
static int set_gen;

typedef struct {
    web_set *s;
    int user_ids[WEB_COMMENTS];
    int ratings[11];
} set_ctx;

static bool named_key(js *j, const char *key, void *ctx)
{
    if (strcmp(key, "name"))
        return false;
    return json_str(j, (char *)ctx, 32);
}

static bool set_key(js *j, const char *key, void *ctx)
{
    set_ctx *c = ctx;
    web_set *s = c->s;
    if (!strcmp(key, "title")) return json_str(j, s->title, sizeof(s->title));
    if (!strcmp(key, "artist")) return json_str(j, s->artist, sizeof(s->artist));
    if (!strcmp(key, "creator")) return json_str(j, s->creator, sizeof(s->creator));
    if (!strcmp(key, "source")) return json_str(j, s->source, sizeof(s->source));
    if (!strcmp(key, "tags")) return json_str(j, s->tags, sizeof(s->tags));
    if (!strcmp(key, "status") && *j->p == '"') return json_str(j, s->status, sizeof(s->status));
    if (!strcmp(key, "play_count")) s->plays = (int)json_num(j);
    else if (!strcmp(key, "favourite_count")) s->favourites = (int)json_num(j);
    else if (!strcmp(key, "bpm")) s->bpm = (float)json_num(j);
    else if (!strcmp(key, "genre") && *j->p == '{') return json_object(j, named_key, s->genre);
    else if (!strcmp(key, "language") && *j->p == '{') return json_object(j, named_key, s->language);
    else if (!strcmp(key, "ratings") && *j->p == '[') {
        j->p++;
        for (int i = 0; i < 11; i++) {
            json_ws(j);
            if (*j->p == ']')
                break;
            c->ratings[i] = (int)json_num(j);
            json_ws(j);
            if (*j->p == ',')
                j->p++;
        }
        json_ws(j);
        if (*j->p == ']')
            j->p++;
    } else
        return false;
    return true;
}

typedef struct {
    int user_id, votes;
    char text[240];
} comment_tmp;

static bool comment_key(js *j, const char *key, void *ctx)
{
    comment_tmp *c = ctx;
    if (!strcmp(key, "user_id")) c->user_id = (int)json_num(j);
    else if (!strcmp(key, "votes_count")) c->votes = (int)json_num(j);
    else if (!strcmp(key, "message") && *j->p == '"') return json_str(j, c->text, sizeof(c->text));
    else return false;
    return true;
}

typedef struct {
    int id;
    char name[32];
} cuser;

static bool cuser_key(js *j, const char *key, void *ctx)
{
    cuser *u = ctx;
    if (!strcmp(key, "id")) u->id = (int)json_num(j);
    else if (!strcmp(key, "username")) return json_str(j, u->name, sizeof(u->name));
    else return false;
    return true;
}

static bool comments_key(js *j, const char *key, void *ctx)
{
    set_ctx *c = ctx;
    bool users = !strcmp(key, "users");
    if ((strcmp(key, "comments") && !users) || *j->p != '[')
        return false;
    j->p++;
    for (;;) {
        json_ws(j);
        if (*j->p == ']') {
            j->p++;
            return true;
        }
        if (users) {
            cuser u = {0};
            if (!json_object(j, cuser_key, &u))
                return false;
            for (int i = 0; i < c->s->ncomments; i++)
                if (c->user_ids[i] == u.id)
                    snprintf(c->s->comments[i].user, sizeof(c->s->comments[0].user), "%s", u.name);
        } else {
            comment_tmp t = {0};
            if (!json_object(j, comment_key, &t))
                return false;
            if (c->s->ncomments < WEB_COMMENTS && t.text[0]) {
                int i = c->s->ncomments++;
                c->user_ids[i] = t.user_id;
                c->s->comments[i].votes = t.votes;
                /* comments are markdown; newlines become spaces here */
                for (char *p = t.text; *p; p++)
                    if (*p == '\n' || *p == '\r')
                        *p = ' ';
                snprintf(c->s->comments[i].text, sizeof(c->s->comments[0].text), "%s", t.text);
            }
        }
        json_ws(j);
        if (*j->p == ',')
            j->p++;
    }
}

/* news and comments are public; with keys we ask as ourselves anyway */
static int public_get(const char *url, char **body)
{
    if (online_enabled())
        return online_api(url, body);
    size_t len;
    return net_get(url, body, &len);
}

static void *set_thread(void *arg)
{
    req *q = arg;
    static web_set s;
    static set_ctx c;
    memset(&s, 0, sizeof(s));
    memset(&c, 0, sizeof(c));
    c.s = &s;
    s.id = q->mode;
    s.rating = -1;
    char url[200], *body = NULL;
    int st = -1;
    if (online_enabled()) {
        snprintf(url, sizeof(url), "https://osu.ppy.sh/api/v2/beatmapsets/%d", s.id);
        st = online_api(url, &body);
        if (st == 200 && body) {
            js j = {body};
            json_object(&j, set_key, &c);
            int votes = 0, sum = 0;
            for (int i = 1; i <= 10; i++) {
                votes += c.ratings[i];
                sum += i * c.ratings[i];
            }
            if (votes)
                s.rating = (float)sum / votes;
        }
        free(body);
        body = NULL;
    }
    snprintf(url, sizeof(url), "https://osu.ppy.sh/api/v2/comments?commentable_type=beatmapset&commentable_id=%d&sort=new", s.id);
    int cst = public_get(url, &body);
    if (cst == 200 && body) {
        js j = {body};
        json_object(&j, comments_key, &c);
    }
    free(body);
    pthread_mutex_lock(&lock);
    if (q->gen == set_gen) {
        s.state = st == 200 || cst == 200 ? WEB_READY : WEB_FAILED;
        set_got = s;
    }
    pthread_mutex_unlock(&lock);
    free(q);
    return NULL;
}

void web_set_get(int set_id)
{
    req *q = calloc(1, sizeof(*q));
    if (!q)
        return;
    pthread_mutex_lock(&lock);
    q->gen = ++set_gen;
    q->mode = set_id; /* the id rides in mode */
    memset(&set_got, 0, sizeof(set_got));
    set_got.state = set_id > 0 ? WEB_LOADING : WEB_FAILED;
    set_got.id = set_id;
    pthread_mutex_unlock(&lock);
    if (set_id <= 0 || !spawn(set_thread, q))
        free(q);
}

void web_set_read(web_set *out)
{
    pthread_mutex_lock(&lock);
    *out = set_got;
    pthread_mutex_unlock(&lock);
}

/* ---- news ---- */

static web_news news_got;

static bool post_key(js *j, const char *key, void *ctx)
{
    typeof(news_got.post[0]) *p = ctx;
    if (!strcmp(key, "title")) return json_str(j, p->title, sizeof(p->title));
    if (!strcmp(key, "author")) return json_str(j, p->author, sizeof(p->author));
    if (!strcmp(key, "published_at")) return json_str(j, p->date, sizeof(p->date)); /* keeps yyyy-mm-dd */
    if (!strcmp(key, "preview")) return json_str(j, p->preview, sizeof(p->preview));
    if (!strcmp(key, "slug")) return json_str(j, p->slug, sizeof(p->slug));
    return false;
}

static bool news_key(js *j, const char *key, void *ctx)
{
    web_news *n = ctx;
    if (strcmp(key, "news_posts") || *j->p != '[')
        return false;
    j->p++;
    for (;;) {
        json_ws(j);
        if (*j->p == ']') {
            j->p++;
            return true;
        }
        typeof(n->post[0]) p;
        memset(&p, 0, sizeof(p));
        if (!json_object(j, post_key, &p))
            return false;
        if (n->count < WEB_NEWS)
            n->post[n->count++] = p;
        json_ws(j);
        if (*j->p == ',')
            j->p++;
    }
}

static void *news_thread(void *arg)
{
    (void)arg;
    static web_news n;
    memset(&n, 0, sizeof(n));
    char *body = NULL;
    int st = public_get("https://osu.ppy.sh/api/v2/news", &body);
    if (st == 200 && body) {
        js j = {body};
        json_object(&j, news_key, &n);
    }
    free(body);
    pthread_mutex_lock(&lock);
    n.state = st == 200 ? WEB_READY : WEB_FAILED;
    news_got = n;
    pthread_mutex_unlock(&lock);
    return NULL;
}

void web_news_get(void)
{
    pthread_mutex_lock(&lock);
    news_got.state = WEB_LOADING;
    pthread_mutex_unlock(&lock);
    spawn(news_thread, NULL);
}

void web_news_read(web_news *out)
{
    pthread_mutex_lock(&lock);
    *out = news_got;
    pthread_mutex_unlock(&lock);
}

/* ---- wiki pages and our changelog, both shown as wrapped plain lines ---- */

static web_page page_got;
static int page_gen;

static void page_line(web_page *pg, const char *s, size_t n)
{
    /* wrap at about 120 characters, on a space when there is one */
    while (n > 0 && pg->count < WEB_LINES) {
        size_t take = n;
        if (take > 120) {
            take = 120;
            while (take > 60 && s[take] != ' ')
                take--;
        }
        snprintf(pg->line[pg->count++], sizeof(pg->line[0]), "%.*s", (int)take, s);
        s += take;
        n -= take;
        while (n && *s == ' ')
            s++, n--;
    }
}

/* markdown to something readable: links keep their text (and are remembered),
 * images, html and front matter go */
static void page_markdown(web_page *pg, const char *md)
{
    const char *p = md;
    if (!strncmp(p, "---", 3)) {
        const char *end = strstr(p + 3, "\n---");
        if (end)
            p = end + 4;
    }
    static char out[2048];
    while (*p && pg->count < WEB_LINES) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        size_t k = 0;
        for (size_t i = 0; i < len && k + 2 < sizeof(out);) {
            if (p[i] == '!' && i + 1 < len && p[i + 1] == '[') {
                const char *close = memchr(p + i, ')', len - i);
                i = close ? (size_t)(close - p) + 1 : len;
                continue;
            }
            if (p[i] == '[') {
                const char *mid = memchr(p + i, ']', len - i);
                if (mid && (size_t)(mid - p) + 1 < len && mid[1] == '(') {
                    const char *close = memchr(mid, ')', len - (size_t)(mid - p));
                    if (close) {
                        size_t tl = (size_t)(mid - (p + i + 1));
                        if (k + tl + 2 < sizeof(out)) {
                            memcpy(out + k, p + i + 1, tl);
                            k += tl;
                        }
                        /* wiki links look like /wiki/Some_page */
                        const char *href = mid + 2;
                        size_t hl = (size_t)(close - href);
                        if (hl > 6 && !strncmp(href, "/wiki/", 6)) {
                            href += 6;
                            hl -= 6;
                            if (pg->nlinks < 40 && hl < 90) {
                                snprintf(pg->links[pg->nlinks], sizeof(pg->links[0]), "%.*s", (int)hl, href);
                                char *hash = strchr(pg->links[pg->nlinks], '#');
                                if (hash)
                                    *hash = 0;
                                pg->nlinks++;
                            }
                        }
                        i = (size_t)(close - p) + 1;
                        continue;
                    }
                }
            }
            if (p[i] == '<') {
                const char *close = memchr(p + i, '>', len - i);
                if (close) {
                    i = (size_t)(close - p) + 1;
                    continue;
                }
            }
            if (p[i] == '*' || p[i] == '`' || p[i] == '\r') {
                i++;
                continue;
            }
            out[k++] = p[i++];
        }
        out[k] = 0;
        if (k)
            page_line(pg, out, k);
        else if (pg->count && pg->line[pg->count - 1][0] && pg->count < WEB_LINES)
            pg->line[pg->count++][0] = 0; /* one blank between paragraphs */
        p = eol ? eol + 1 : p + len;
    }
}

typedef struct {
    int gen;
    bool changelog;
    char path[128];
} page_req;

static bool release_key(js *j, const char *key, void *ctx)
{
    char **f = ctx;
    if (!strcmp(key, "name") && *j->p == '"') return json_str(j, f[0], 120);
    if (!strcmp(key, "published_at")) return json_str(j, f[1], 11);
    if (!strcmp(key, "body") && *j->p == '"') return json_str(j, f[2], 8000);
    return false;
}

static void *page_thread(void *arg)
{
    page_req *q = arg;
    static web_page pg;
    memset(&pg, 0, sizeof(pg));
    snprintf(pg.path, sizeof(pg.path), "%s", q->changelog ? "changelog" : q->path);
    char url[400], *body = NULL;
    size_t len;
    int st;
    if (q->changelog) {
        static const char *const hdr[] = {"User-Agent", "ProjectCadence", "Accept", "application/vnd.github+json", NULL};
        st = net_request("GET", "https://api.github.com/repos/actuallyssstaris/ProjectCadence/releases?per_page=10", hdr, NULL, &body, &len);
        if (st == 200 && body && *body == '[') {
            js j = {body + 1};
            static char name[120], date[12], text[8000];
            for (;;) {
                json_ws(&j);
                if (*j.p != '{')
                    break;
                name[0] = date[0] = text[0] = 0;
                char *f[3] = {name, date, text};
                if (!json_object(&j, release_key, f))
                    break;
                char head[160];
                snprintf(head, sizeof(head), "# %s  (%s)", name, date);
                page_line(&pg, head, strlen(head));
                page_markdown(&pg, text);
                if (pg.count < WEB_LINES)
                    pg.line[pg.count++][0] = 0;
                json_ws(&j);
                if (*j.p == ',')
                    j.p++;
            }
        }
    } else {
        /* the wiki's own source: ppy/osu-wiki on GitHub, English pages */
        char path[300];
        size_t k = 0;
        for (const char *c = q->path; *c && k + 4 < sizeof(path); c++)
            path[k++] = *c == ' ' ? '_' : *c;
        path[k] = 0;
        snprintf(url, sizeof(url), "https://raw.githubusercontent.com/ppy/osu-wiki/master/wiki/%s/en.md", path);
        st = net_get(url, &body, &len);
        if (st == 200 && body)
            page_markdown(&pg, body);
    }
    free(body);
    pthread_mutex_lock(&lock);
    if (q->gen == page_gen) {
        pg.state = st == 200 && pg.count ? WEB_READY : WEB_FAILED;
        page_got = pg;
    }
    pthread_mutex_unlock(&lock);
    free(q);
    return NULL;
}

static void page_ask(const char *path, bool changelog)
{
    page_req *q = calloc(1, sizeof(*q));
    if (!q)
        return;
    q->changelog = changelog;
    snprintf(q->path, sizeof(q->path), "%s", path);
    pthread_mutex_lock(&lock);
    q->gen = ++page_gen;
    page_got.state = WEB_LOADING;
    snprintf(page_got.path, sizeof(page_got.path), "%s", changelog ? "changelog" : path);
    pthread_mutex_unlock(&lock);
    if (!spawn(page_thread, (req *)q))
        free(q);
}

void web_wiki_get(const char *path) { page_ask(path, false); }
void web_changelog_get(void) { page_ask("", true); }

void web_page_read(web_page *out)
{
    pthread_mutex_lock(&lock);
    *out = page_got;
    pthread_mutex_unlock(&lock);
}
