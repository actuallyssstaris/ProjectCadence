#ifndef OSU5_LOG_H
#define OSU5_LOG_H

/* printf to klog, plus a copy in /data/osu5/osu5.log (klog drops lines when
 * nobody is connected, the file doesn't) */
void log_open(void);
void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG(...) logf_(__VA_ARGS__)

/* tell the user, then park; returning from main dies in exit() on this setup */
_Noreturn void die(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
