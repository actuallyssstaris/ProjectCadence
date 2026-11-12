#include "log.h"
#include "sce.h"

#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define LOG_PATH "/data/cadence/cadence.log"
#define PREV_PATH "/data/cadence/cadence.prev.log"
#define CLEAN_MARK "-- closed cleanly --"

static FILE *log_file;
static double started;
static bool last_crashed;
static char where[96] = "starting";

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* did the last run crash? its log's last lines tell */
static bool ended_badly(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char tail[256] = "";
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, size > 200 ? size - 200 : 0, SEEK_SET);
    size_t n = fread(tail, 1, sizeof(tail) - 1, f);
    tail[n] = 0;
    fclose(f);
    /* closing from the PS button never logs the clean mark, so only an actual
     * crash or fatal line counts */
    return strstr(tail, "crash:") || strstr(tail, "fatal:");
}

static void on_signal(int sig)
{
    /* nothing fancy in here: say what and where, flush, then let it die */
    if (log_file) {
        fprintf(log_file, "%9.3f  crash: signal %d while %s\n", (now_ms() - started) / 1000, sig, where);
        fflush(log_file);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

void log_open(void)
{
    started = now_ms();
    /* before the rename everything lived in /data/osu5: carry it over once */
    struct stat st;
    bool moved = stat("/data/cadence", &st) != 0 && stat("/data/osu5", &st) == 0 && rename("/data/osu5", "/data/cadence") == 0;

    /* first run: nothing makes these but us */
    mkdir("/data/cadence", 0777);
    mkdir("/data/cadence/Songs", 0777);
    mkdir("/data/cadence/skin", 0777);
    /* the last run's log stays next to this one, so a crash can still be reported */
    last_crashed = ended_badly(LOG_PATH);
    rename(LOG_PATH, PREV_PATH);
    log_file = fopen(LOG_PATH, "w");
    if (moved)
        logf_("moved /data/osu5 to /data/cadence");
    if (last_crashed)
        logf_("the last run didn't close cleanly; its log is in cadence.prev.log");
    signal(SIGSEGV, on_signal);
    signal(SIGBUS, on_signal);
    signal(SIGILL, on_signal);
    signal(SIGFPE, on_signal);
    signal(SIGABRT, on_signal);
}

bool log_last_crashed(void) { return last_crashed; }

void log_enable(bool on)
{
    if (!on && log_file) {
        /* off: nothing kept on the console; klog still gets the lines */
        fclose(log_file);
        log_file = NULL;
        remove(LOG_PATH);
        remove(PREV_PATH);
    } else if (on && !log_file) {
        log_file = fopen(LOG_PATH, "a");
    }
}

void log_where(const char *what)
{
    if (strcmp(where, what)) {
        snprintf(where, sizeof(where), "%s", what);
        logf_("screen: %s", what);
    }
}

void log_close(void)
{
    logf_("%s", CLEAN_MARK);
}

static void vlog(const char *fmt, va_list ap)
{
    char line[512];
    vsnprintf(line, sizeof(line), fmt, ap);
    printf("[cadence] %s\n", line);
    if (log_file) {
        /* seconds since launch, so the order of things and gaps between them show */
        fprintf(log_file, "%9.3f  %s\n", (now_ms() - started) / 1000, line);
        fflush(log_file);
    }
}

void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

/* the end of a log file, at most max bytes, starting on a whole line */
char *log_tail(bool previous, size_t max, size_t *len)
{
    FILE *f = fopen(previous ? PREV_PATH : LOG_PATH, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long from = size > (long)max ? size - (long)max : 0;
    fseek(f, from, SEEK_SET);
    char *buf = malloc((size_t)(size - from) + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)(size - from), f);
    fclose(f);
    buf[n] = 0;
    char *start = buf;
    if (from > 0) {
        char *nl = strchr(buf, '\n');
        if (nl)
            start = nl + 1;
    }
    *len = strlen(start);
    memmove(buf, start, *len + 1);
    return buf;
}

void die(const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    logf_("fatal: %s", msg);

    OrbisNotificationRequest req;
    memset(&req, 0, sizeof(req));
    snprintf(req.message, sizeof(req.message), "ProjectCadence: %s", msg);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);

    for (;;)
        sceKernelUsleep(1000000);
}
