#include "report.h"
#include "account.h"
#include "log.h"
#include "net.h"
#include "update.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int state, sent_id;
static char message[96];

typedef struct {
    char url[200], text[1024], info[2048];
    bool prev;
} job;

/* JSON string body: quotes, backslashes and control characters escaped, anything
 * past ASCII turned into '?' so a broken log line can't break the request */
static size_t put_json(char *out, size_t cap, const char *s)
{
    size_t k = 0;
    for (; *s && k + 8 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            out[k++] = '\\';
            out[k++] = (char)c;
        } else if (c == '\n') {
            out[k++] = '\\';
            out[k++] = 'n';
        } else if (c < 32 || c > 126) {
            out[k++] = c == '\t' ? ' ' : '?';
        } else {
            out[k++] = (char)c;
        }
    }
    out[k] = 0;
    return k;
}

static void finish(int st, int id, const char *msg)
{
    pthread_mutex_lock(&lock);
    state = st;
    sent_id = id;
    snprintf(message, sizeof(message), "%s", msg);
    pthread_mutex_unlock(&lock);
}

static void *send_thread(void *arg)
{
    job *q = arg;
    size_t log_n = 0, prev_n = 0;
    char *log = log_tail(false, 60000, &log_n);
    char *prev = q->prev ? log_tail(true, 60000, &prev_n) : NULL;
    size_t cap = 4096 + sizeof(q->text) * 2 + sizeof(q->info) * 2 + (log_n + prev_n) * 2;
    char *body = malloc(cap);
    if (!body) {
        free(log);
        free(prev);
        finish(3, 0, "out of memory");
        free(q);
        return NULL;
    }
    size_t k = (size_t)snprintf(body, cap, "{\"session\":\"%s\",\"name\":\"", account_session());
    k += put_json(body + k, cap - k, account_signed_in() ? account_name() : "");
    k += (size_t)snprintf(body + k, cap - k, "\",\"version\":\"%s\",\"crashed\":%s,\"text\":\"", CADENCE_VERSION,
                          log_last_crashed() ? "true" : "false");
    k += put_json(body + k, cap - k, q->text);
    k += (size_t)snprintf(body + k, cap - k, "\",\"info\":\"");
    k += put_json(body + k, cap - k, q->info);
    k += (size_t)snprintf(body + k, cap - k, "\",\"log\":\"");
    k += put_json(body + k, cap - k, log ? log : "");
    k += (size_t)snprintf(body + k, cap - k, "\",\"prev_log\":\"");
    k += put_json(body + k, cap - k, prev ? prev : "");
    snprintf(body + k, cap - k, "\"}");
    free(log);
    free(prev);

    static const char *const hdr[] = {"Content-Type", "application/json", NULL};
    char *out = NULL;
    size_t len;
    int st = net_request("POST", q->url, hdr, body, &out, &len);
    free(body);
    int id = 0;
    if (st == 200 && out) {
        const char *p = strstr(out, "\"id\":");
        if (p)
            id = atoi(p + 5);
    }
    free(out);
    LOG("report: sent -> %d, id %d", st, id);
    if (st == 200)
        finish(2, id, "sent, thanks");
    else
        finish(3, 0, st == 429 ? "too many reports this hour, try later" : "couldn't reach the server");
    free(q);
    return NULL;
}

void report_send(const char *server, const char *text, const char *info, bool with_prev_log)
{
    job *q = calloc(1, sizeof(*q));
    if (!q)
        return;
    snprintf(q->url, sizeof(q->url), "%s/api/reports", server);
    snprintf(q->text, sizeof(q->text), "%s", text);
    snprintf(q->info, sizeof(q->info), "%s", info);
    q->prev = with_prev_log;
    finish(1, 0, "sending...");
    LOG("report: sending (%zu characters from the player)", strlen(text));
    pthread_t t;
    if (pthread_create(&t, NULL, send_thread, q) == 0)
        pthread_detach(t);
    else {
        free(q);
        finish(3, 0, "couldn't start sending");
    }
}

int report_state(int *id, char *msg, int n)
{
    pthread_mutex_lock(&lock);
    int s = state;
    if (id)
        *id = sent_id;
    if (msg)
        snprintf(msg, (size_t)n, "%s", message);
    pthread_mutex_unlock(&lock);
    return s;
}
