#include "bweb.h"
#include "../include/osu5.h"
#include "log.h"
#include "lzma.h"
#include "md5.h"
#include "mp.h"
#include "net.h"
#include "online.h"
#include "rijndael.h"

#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static struct {
    bool on;
    char domain[96], user[32], pass[33], version[16], client[256], uid1[40], uid2[40], token[128];
} ses;

void bweb_session(const char *domain, const char *user, const char *pass_md5, const char *version,
                  const char *client_hash, const char *uid1, const char *uid2, const char *token)
{
    pthread_mutex_lock(&lock);
    ses.on = true;
    snprintf(ses.domain, sizeof(ses.domain), "%s", domain);
    snprintf(ses.user, sizeof(ses.user), "%s", user);
    snprintf(ses.pass, sizeof(ses.pass), "%s", pass_md5);
    /* the web side wants the build date alone: 20260924, not b20260924 */
    snprintf(ses.version, sizeof(ses.version), "%s", version[0] == 'b' ? version + 1 : version);
    snprintf(ses.client, sizeof(ses.client), "%s", client_hash);
    snprintf(ses.uid1, sizeof(ses.uid1), "%s", uid1);
    snprintf(ses.uid2, sizeof(ses.uid2), "%s", uid2);
    snprintf(ses.token, sizeof(ses.token), "%s", token);
    pthread_mutex_unlock(&lock);
}

void bweb_end(void)
{
    pthread_mutex_lock(&lock);
    ses.on = false;
    pthread_mutex_unlock(&lock);
}

bool bweb_on(void)
{
    pthread_mutex_lock(&lock);
    bool on = ses.on;
    pthread_mutex_unlock(&lock);
    return on;
}

static bool session_copy(typeof(ses) *out)
{
    pthread_mutex_lock(&lock);
    *out = ses;
    pthread_mutex_unlock(&lock);
    return out->on;
}

/* ---- small helpers ---- */

static void url_encode(const char *in, char *out, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t k = 0;
    for (; *in && k + 4 < n; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[k++] = (char)c;
        } else {
            out[k++] = '%';
            out[k++] = hex[c >> 4];
            out[k++] = hex[c & 15];
        }
    }
    out[k] = 0;
}

static size_t b64(const uint8_t *in, size_t n, char *out)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t k = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[k++] = t[v >> 18 & 63];
        out[k++] = t[v >> 12 & 63];
        out[k++] = i + 1 < n ? t[v >> 6 & 63] : '=';
        out[k++] = i + 2 < n ? t[v & 63] : '=';
    }
    out[k] = 0;
    return k;
}

static void spawn(void *(*fn)(void *), void *arg)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_t t;
    if (pthread_create(&t, &attr, fn, arg) == 0)
        pthread_detach(t);
    else
        free(arg);
    pthread_attr_destroy(&attr);
}

static const char *const grade_names[] = {"D", "C", "B", "A", "S", "SH", "X", "XH", "F"};

/* stable's osu! grades from the counts; the server's list doesn't carry them */
static int grade_of(int n300, int n100, int n50, int miss, int mods, int mode)
{
    int total = n300 + n100 + n50 + miss;
    if (total <= 0)
        return OSU5_RANK_D;
    bool silver = mods & (OSU5_MOD_HIDDEN | OSU5_MOD_FLASHLIGHT);
    float acc = mode == 1 ? (n300 + n100 * 0.5f) / total : (300.0f * n300 + 100.0f * n100 + 50.0f * n50) / (300.0f * total);
    float r300 = (float)n300 / total;
    if (acc >= 1)
        return silver ? OSU5_RANK_XH : OSU5_RANK_X;
    if (r300 > 0.9f && (float)n50 / total < 0.01f && !miss)
        return silver ? OSU5_RANK_SH : OSU5_RANK_S;
    if ((r300 > 0.8f && !miss) || r300 > 0.9f)
        return OSU5_RANK_A;
    if ((r300 > 0.7f && !miss) || r300 > 0.8f)
        return OSU5_RANK_B;
    if (r300 > 0.6f)
        return OSU5_RANK_C;
    return OSU5_RANK_D;
}

/* ---- leaderboards ---- */

#define SLOTS 8
static struct {
    int state;
    time_t when;
    bweb_board b;
} boards[SLOTS];

typedef struct {
    char md5[33], name[256];
    int set_id, mode;
} board_req;

static bool parse_score(char *line, bweb_score *s, int mode)
{
    char *f[16];
    int n = 0;
    for (char *p = line; n < 16; n++) {
        f[n] = p;
        char *bar = strchr(p, '|');
        if (!bar) {
            n++;
            break;
        }
        *bar = 0;
        p = bar + 1;
    }
    if (n < 14)
        return false;
    memset(s, 0, sizeof(*s));
    s->id = atoi(f[0]);
    snprintf(s->user, sizeof(s->user), "%s", f[1]);
    s->score = atoll(f[2]);
    s->combo = atoi(f[3]);
    s->n50 = atoi(f[4]);
    s->n100 = atoi(f[5]);
    s->n300 = atoi(f[6]);
    s->miss = atoi(f[7]);
    s->mods = mp_mods_from_osu((uint32_t)strtoul(f[11], NULL, 10));
    int total = s->n300 + s->n100 + s->n50 + s->miss;
    if (total > 0)
        s->acc = mode == 1 ? (s->n300 + s->n100 * 0.5f) / total
                           : (300.0f * s->n300 + 100.0f * s->n100 + 50.0f * s->n50) / (300.0f * total);
    s->rank = grade_of(s->n300, s->n100, s->n50, s->miss, s->mods, mode);
    return true;
}

static void *board_thread(void *arg)
{
    board_req *q = arg;
    typeof(ses) s;
    static bweb_board b;
    memset(&b, 0, sizeof(b));
    snprintf(b.md5, sizeof(b.md5), "%s", q->md5);
    b.status = -1;
    int st = -1;
    if (session_copy(&s)) {
        char url[1024], name[600], user[100];
        url_encode(q->name, name, sizeof(name));
        url_encode(s.user, user, sizeof(user));
        snprintf(url, sizeof(url),
                 "https://osu.%s/web/osu-osz2-getscores.php?s=0&vv=4&v=1&c=%s&f=%s&m=%d&i=%d&mods=0&h=&a=0&us=%s&ha=%s",
                 s.domain, q->md5, name, q->mode, q->set_id, user, s.pass);
        static const char *const hdr[] = {"User-Agent", "osu!", NULL};
        char *body = NULL;
        size_t len;
        st = net_request("GET", url, hdr, NULL, &body, &len);
        if (st == 200 && body) {
            /* status line, then offset / name / rating, then your best, then the list */
            char *line = body, *next;
            for (int i = 0; line; i++, line = next) {
                next = strchr(line, '\n');
                if (next)
                    *next++ = 0;
                if (i == 0)
                    b.status = atoi(line);
                else if (i == 4 && line[0])
                    b.has_mine = parse_score(line, &b.mine, q->mode);
                else if (i >= 5 && line[0] && b.count < BWEB_MAX && parse_score(line, &b.s[b.count], q->mode))
                    b.count++;
            }
        }
        free(body);
        if (st != 200)
            LOG("bweb: scores %d", st);
    }

    pthread_mutex_lock(&lock);
    int slot = 0;
    for (int i = 0; i < SLOTS; i++) {
        if (!strcmp(boards[i].b.md5, q->md5)) {
            slot = i;
            break;
        }
        if (boards[i].when < boards[slot].when)
            slot = i;
    }
    boards[slot].b = b;
    boards[slot].state = st == 200 ? ONLINE_READY : ONLINE_FAILED;
    boards[slot].when = time(NULL);
    pthread_mutex_unlock(&lock);
    free(q);
    return NULL;
}

int bweb_board_for(const char *md5, const char *osu_name, int set_id, int mode, bweb_board *out)
{
    if (!bweb_on())
        return ONLINE_OFF;
    if (!md5[0])
        return ONLINE_FAILED;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < SLOTS; i++) {
        if (strcmp(boards[i].b.md5, md5))
            continue;
        int st = boards[i].state;
        bool fresh = time(NULL) - boards[i].when < (st == ONLINE_READY ? 120 : 20);
        if (st == ONLINE_LOADING || fresh) {
            if (st == ONLINE_READY)
                *out = boards[i].b;
            pthread_mutex_unlock(&lock);
            return st;
        }
        boards[i].state = ONLINE_LOADING;
        goto ask;
    }
    {
        /* claim the oldest slot while it loads */
        int slot = 0;
        for (int i = 1; i < SLOTS; i++)
            if (boards[i].when < boards[slot].when)
                slot = i;
        memset(&boards[slot], 0, sizeof(boards[slot]));
        snprintf(boards[slot].b.md5, sizeof(boards[slot].b.md5), "%s", md5);
        boards[slot].state = ONLINE_LOADING;
        boards[slot].when = time(NULL);
    }
ask:
    pthread_mutex_unlock(&lock);
    board_req *q = calloc(1, sizeof(*q));
    if (!q)
        return ONLINE_FAILED;
    snprintf(q->md5, sizeof(q->md5), "%s", md5);
    snprintf(q->name, sizeof(q->name), "%s", osu_name);
    q->set_id = set_id;
    q->mode = mode;
    spawn(board_thread, q);
    return ONLINE_LOADING;
}

void bweb_forget(const char *md5)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < SLOTS; i++)
        if (!strcmp(boards[i].b.md5, md5) && boards[i].state != ONLINE_LOADING)
            boards[i].when = 0;
    pthread_mutex_unlock(&lock);
}

/* ---- replays: osu!'s frame text, LZMA'd ---- */

/* our pad bits to osu!'s per mode, and back */
static int keys_out(int keys, int mode)
{
    switch (mode) {
    case 1: /* taiko: dons on M1 / K1, kats on M2 / K2 */
        return (keys & 1 ? 1 : 0) | (keys & 2 ? 4 : 0) | (keys & 4 ? 2 : 0) | (keys & 8 ? 8 : 0);
    case 0: /* K1 / K2 carry the mouse bits with them */
        return (keys & 1 ? 5 : 0) | (keys & 2 ? 10 : 0);
    default:
        return keys & 1;
    }
}

static int keys_in(int keys, int mode)
{
    if (mode == 1)
        return (keys & 1 ? 1 : 0) | (keys & 4 ? 2 : 0) | (keys & 2 ? 4 : 0) | (keys & 8 ? 8 : 0);
    return (keys & 1 ? 1 : 0) | (keys & 2 ? 2 : 0);
}

static uint8_t *frames_text(const replay *rp, int mode, size_t *n)
{
    size_t cap = 64 + (size_t)rp->count * 40, k = 0;
    char *t = malloc(cap);
    if (!t)
        return NULL;
    /* stable starts every replay with these two */
    k += (size_t)snprintf(t + k, cap - k, "0|256|-500|0,-1|256|-500|0,");
    long prev = -1;
    for (int i = 0; i < rp->count && k + 64 < cap; i++) {
        const replay_frame *f = &rp->frames[i];
        long now = lround(f->t);
        if (now < prev)
            now = prev;
        float x = f->x, y = f->y;
        int keys = keys_out(f->keys, mode);
        if (mode == 3) {
            /* mania keeps the columns in x */
            x = (float)f->keys;
            y = 0;
            keys = 0;
        }
        k += (size_t)snprintf(t + k, cap - k, "%ld|%.4g|%.4g|%d,", now - prev, x, y, keys);
        prev = now;
    }
    k += (size_t)snprintf(t + k, cap - k, "-12345|0|0|0,");
    uint8_t *packed = lzma_pack((uint8_t *)t, k, n);
    free(t);
    return packed;
}

static bool frames_read(const uint8_t *text, size_t n, int mode, replay *out)
{
    long now = 0;
    const char *p = (const char *)text, *end = p + n;
    while (p < end) {
        const char *comma = memchr(p, ',', (size_t)(end - p));
        if (!comma)
            comma = end;
        char frame[96];
        size_t len = (size_t)(comma - p);
        if (len < sizeof(frame)) {
            memcpy(frame, p, len);
            frame[len] = 0;
            long w;
            float x, y;
            int keys;
            if (sscanf(frame, "%ld|%f|%f|%d", &w, &x, &y, &keys) == 4 && w != -12345) {
                now += w;
                if (mode == 3)
                    replay_push(out, now, 256, 192, (int)x);
                else
                    replay_push(out, now, x, y, keys_in(keys, mode));
            }
        }
        p = comma + 1;
    }
    return out->count > 0;
}

/* ---- submitting ---- */

static struct {
    int state;
    char msg[128];
} sub;

typedef struct {
    bweb_play p;
    uint8_t *rp;
    size_t rp_n;
} sub_req;

static void put(char **buf, size_t *n, size_t *cap, const void *data, size_t len)
{
    if (*n + len + 1 > *cap) {
        size_t c = (*cap + len) * 2;
        char *b = realloc(*buf, c);
        if (!b)
            return;
        *buf = b;
        *cap = c;
    }
    memcpy(*buf + *n, data, len);
    *n += len;
}

static void field(char **buf, size_t *n, size_t *cap, const char *name, const char *value)
{
    char head[160];
    int h = snprintf(head, sizeof(head), "--CadenceBoundary\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n", name);
    put(buf, n, cap, head, (size_t)h);
    put(buf, n, cap, value, strlen(value));
    put(buf, n, cap, "\r\n", 2);
}

static void sub_done(int state, const char *msg)
{
    pthread_mutex_lock(&lock);
    sub.state = state;
    snprintf(sub.msg, sizeof(sub.msg), "%s", msg);
    pthread_mutex_unlock(&lock);
    LOG("bweb: submit: %s", msg);
}

static void *submit_thread(void *arg)
{
    sub_req *q = arg;
    const bweb_play *p = &q->p;
    typeof(ses) s;
    if (!session_copy(&s)) {
        sub_done(3, "not signed in to the bancho server");
        goto out;
    }

    /* the score line, and the checksum the server recomputes */
    {
        time_t now = time(NULL);
        struct tm tm;
        gmtime_r(&now, &tm);
        char when[16];
        strftime(when, sizeof(when), "%y%m%d%H%M%S", &tm);
        uint32_t mods = mp_mods_to_osu(p->mods);
        const char *grade = p->passed ? grade_names[p->rank >= 0 && p->rank < 9 ? p->rank : 0] : "F";
        char check_src[1024], check[33];
        snprintf(check_src, sizeof(check_src), "chickenmcnuggets%do15%d%dsmustard%d%duu%s%d%s%s%lld%s%uQ%s%d%s%s%s",
                 p->n100 + p->n300, p->n50, p->geki, p->katu, p->miss, p->md5, p->max_combo,
                 p->perfect ? "True" : "False", s.user, p->score, grade, mods, p->passed ? "True" : "False", p->mode,
                 s.version, when, s.client);
        md5_text(check_src, check);
        char line[1024];
        int ln = snprintf(line, sizeof(line), "%s:%s:%s:%d:%d:%d:%d:%d:%d:%lld:%d:%s:%s:%u:%s:%d:%s:%s", p->md5, s.user,
                          check, p->n300, p->n100, p->n50, p->geki, p->katu, p->miss, p->score, p->max_combo,
                          p->perfect ? "True" : "False", grade, mods, p->passed ? "True" : "False", p->mode, when,
                          s.version);

        uint8_t key[32], iv[32];
        char key_src[64];
        snprintf(key_src, sizeof(key_src), "osu!-scoreburgr---------%s", s.version);
        memset(key, 0, sizeof(key));
        memcpy(key, key_src, strlen(key_src) < 32 ? strlen(key_src) : 32);
        for (int i = 0; i < 32; i++)
            iv[i] = (uint8_t)(rand() ^ (int)(now >> (i % 24)));

        static uint8_t enc[1100], enc2[300];
        static char line64[1500], client64[420], iv64[48], fs64[16];
        size_t en = rijndael256_cbc(key, iv, (const uint8_t *)line, (size_t)ln, enc);
        b64(enc, en, line64);
        size_t en2 = rijndael256_cbc(key, iv, (const uint8_t *)s.client, strlen(s.client), enc2);
        b64(enc2, en2, client64);
        b64(iv, 32, iv64);
        b64((const uint8_t *)"", 0, fs64);

        char num[32], uids[90];
        size_t n = 0, cap = 4096 + q->rp_n;
        char *body = malloc(cap);
        if (!body) {
            sub_done(3, "out of memory");
            goto out;
        }
        field(&body, &n, &cap, "x", "0");
        snprintf(num, sizeof(num), "%d", p->fail_ms);
        field(&body, &n, &cap, "ft", num);
        field(&body, &n, &cap, "fs", fs64);
        field(&body, &n, &cap, "bmk", p->md5);
        field(&body, &n, &cap, "iv", iv64);
        snprintf(uids, sizeof(uids), "%s|%s", s.uid1, s.uid2);
        field(&body, &n, &cap, "c1", uids);
        snprintf(num, sizeof(num), "%d", p->play_ms);
        field(&body, &n, &cap, "st", num);
        field(&body, &n, &cap, "pass", s.pass);
        field(&body, &n, &cap, "osuver", s.version);
        field(&body, &n, &cap, "s", client64);
        field(&body, &n, &cap, "score", line64);
        static const char rp_head[] = "--CadenceBoundary\r\nContent-Disposition: form-data; name=\"score\"; filename=\"score\"\r\n"
                                      "Content-Type: application/octet-stream\r\n\r\n";
        put(&body, &n, &cap, rp_head, sizeof(rp_head) - 1);
        put(&body, &n, &cap, q->rp, q->rp_n);
        put(&body, &n, &cap, "\r\n--CadenceBoundary--\r\n", 23);

        char url[200];
        snprintf(url, sizeof(url), "https://osu.%s/web/osu-submit-modular-selector.php", s.domain);
        const char *hdr[] = {"User-Agent", "osu!", "token", s.token,
                             "Content-Type", "multipart/form-data; boundary=CadenceBoundary", NULL};
        char *out = NULL;
        size_t len;
        int st = net_request_raw("POST", url, hdr, body, n, &out, &len, NULL, 0);
        free(body);
        if (st != 200) {
            char msg[64];
            snprintf(msg, sizeof(msg), "the server answered %d", st);
            sub_done(3, msg);
        } else if (!out || !len) {
            sub_done(3, "the server didn't take it (are you still online?)");
        } else if (!strncmp(out, "error:", 6)) {
            char msg[96];
            snprintf(msg, sizeof(msg), "%.80s", out + 6);
            sub_done(p->passed ? 3 : 2, p->passed ? msg : "fail recorded");
        } else {
            /* the charts: pick out the new rank and pp from the overall one */
            char msg[128] = "score submitted";
            const char *o = strstr(out, "chartId:overall");
            const char *r = o ? strstr(o, "rankAfter:") : NULL;
            const char *pp = o ? strstr(o, "ppAfter:") : NULL;
            if (r && pp)
                snprintf(msg, sizeof(msg), "score submitted: #%d, %dpp", atoi(r + 10), atoi(pp + 8));
            sub_done(2, msg);
        }
        free(out);
        bweb_forget(p->md5);
    }
out:
    free(q->rp);
    free(q);
    return NULL;
}

void bweb_submit(const bweb_play *p)
{
    if (!bweb_on() || !p->rp)
        return;
    sub_req *q = calloc(1, sizeof(*q));
    if (!q)
        return;
    q->p = *p;
    q->p.rp = NULL;
    q->rp = frames_text(p->rp, p->mode, &q->rp_n);
    if (!q->rp) {
        free(q);
        return;
    }
    pthread_mutex_lock(&lock);
    sub.state = 1;
    snprintf(sub.msg, sizeof(sub.msg), "submitting...");
    pthread_mutex_unlock(&lock);
    spawn(submit_thread, q);
}

int bweb_submit_state(char *msg, int n)
{
    pthread_mutex_lock(&lock);
    int st = sub.state;
    if (msg)
        snprintf(msg, (size_t)n, "%s", sub.msg);
    pthread_mutex_unlock(&lock);
    return st;
}

/* ---- watching someone's replay ---- */

static struct {
    int state, gen;
    replay r;
} got;

typedef struct {
    int gen, id, mode;
    char path[768];
} rp_req;

static void *replay_thread(void *arg)
{
    rp_req *q = arg;
    typeof(ses) s;
    replay r;
    memset(&r, 0, sizeof(r));
    bool ok = false;
    if (session_copy(&s)) {
        char url[400], user[100];
        url_encode(s.user, user, sizeof(user));
        snprintf(url, sizeof(url), "https://osu.%s/web/osu-getreplay.php?c=%d&m=%d&u=%s&h=%s", s.domain, q->id, q->mode,
                 user, s.pass);
        static const char *const hdr[] = {"User-Agent", "osu!", NULL};
        char *body = NULL;
        size_t len = 0;
        int st = net_request_raw("GET", url, hdr, NULL, 0, &body, &len, NULL, 0);
        if (st == 200 && body && len > 13) {
            size_t tn;
            uint8_t *text = lzma_unpack((uint8_t *)body, len, &tn);
            if (text) {
                replay_reset(&r, q->path, 0);
                ok = frames_read(text, tn, q->mode, &r);
                free(text);
            }
        }
        LOG("bweb: replay %d: %d, %d frames", q->id, st, r.count);
        free(body);
    }
    pthread_mutex_lock(&lock);
    if (q->gen == got.gen) {
        replay_free(&got.r);
        got.r = r;
        got.state = ok ? ONLINE_READY : ONLINE_FAILED;
    } else {
        replay_free(&r);
    }
    pthread_mutex_unlock(&lock);
    free(q);
    return NULL;
}

void bweb_replay_get(int score_id, int mode, const char *osu_path)
{
    rp_req *q = calloc(1, sizeof(*q));
    if (!q)
        return;
    pthread_mutex_lock(&lock);
    q->gen = ++got.gen;
    got.state = ONLINE_LOADING;
    pthread_mutex_unlock(&lock);
    q->id = score_id;
    q->mode = mode;
    snprintf(q->path, sizeof(q->path), "%s", osu_path);
    spawn(replay_thread, q);
}

int bweb_replay_take(replay *out)
{
    pthread_mutex_lock(&lock);
    int st = got.state;
    if (st == ONLINE_READY) {
        replay_free(out);
        *out = got.r;
        memset(&got.r, 0, sizeof(got.r));
        got.state = ONLINE_OFF;
    } else if (st == ONLINE_FAILED) {
        got.state = ONLINE_OFF;
    }
    pthread_mutex_unlock(&lock);
    return st;
}
