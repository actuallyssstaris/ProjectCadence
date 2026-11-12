#ifndef OSU5_REPORT_H
#define OSU5_REPORT_H

#include <stdbool.h>

/* "report a problem": what the player wrote, a few facts about the console and
 * game, and the end of the logs, to our server where admins read them. */

void report_send(const char *server, const char *text, const char *info, bool with_prev_log);
/* 0 idle, 1 sending, 2 sent (id says which), 3 failed */
int report_state(int *id, char *msg, int n);

#endif
