#include "log.h"
#include "sce.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static FILE *log_file;

void log_open(void)
{
    /* first run: nothing makes these but us */
    mkdir("/data/osu5", 0777);
    mkdir("/data/osu5/Songs", 0777);
    mkdir("/data/osu5/skin", 0777);
    log_file = fopen("/data/osu5/osu5.log", "w");
}

static void vlog(const char *fmt, va_list ap)
{
    char line[512];
    vsnprintf(line, sizeof(line), fmt, ap);
    printf("[osu5] %s\n", line);
    if (log_file) {
        fprintf(log_file, "%s\n", line);
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
    snprintf(req.message, sizeof(req.message), "osu5: %s", msg);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);

    for (;;)
        sceKernelUsleep(1000000);
}
