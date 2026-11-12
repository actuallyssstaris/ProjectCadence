#ifndef OSU5_LOG_H
#define OSU5_LOG_H

#include <stdbool.h>
#include <stddef.h>

/* printf to klog, plus a copy in /data/cadence/cadence.log with the seconds since
 * launch on each line. The last run's log is kept as cadence.prev.log. */
void log_open(void);
void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG(...) logf_(__VA_ARGS__)

/* what the game is doing, for crash lines and reports; logged when it changes */
void log_where(const char *what);
/* the last line of a run that ended on purpose */
void log_close(void);
/* the previous run never got to log_close: it crashed or was killed */
bool log_last_crashed(void);
/* the end of this run's log (or the last run's), malloc'd */
char *log_tail(bool previous, size_t max, size_t *len);

/* tell the user, then park; returning from main dies in exit() on this setup */
_Noreturn void die(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
