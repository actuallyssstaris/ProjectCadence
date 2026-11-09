#include "log.h"
#include "sce.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static FILE *log_file;

void log_open(void)
{
    /* before the rename everything lived in /data/osu5: carry it over once */
    struct stat st;
    bool moved = stat("/data/cadence", &st) != 0 && stat("/data/osu5", &st) == 0 && rename("/data/osu5", "/data/cadence") == 0;

    /* first run: nothing makes these but us */
    mkdir("/data/cadence", 0777);
    mkdir("/data/cadence/Songs", 0777);
    mkdir("/data/cadence/skin", 0777);
    log_file = fopen("/data/cadence/cadence.log", "w");
    if (moved)
        logf_("moved /data/osu5 to /data/cadence");
}

static void vlog(const char *fmt, va_list ap)
{
    char line[512];
    vsnprintf(line, sizeof(line), fmt, ap);
    printf("[cadence] %s\n", line);
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
    snprintf(req.message, sizeof(req.message), "ProjectCadence: %s", msg);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);

    for (;;)
        sceKernelUsleep(1000000);
}
