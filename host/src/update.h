#ifndef OSU5_UPDATE_H
#define OSU5_UPDATE_H

#include <stdint.h>

#define CADENCE_VERSION "v2.0.0"

enum { UPDATE_NONE, UPDATE_CHECKING, UPDATE_AVAILABLE, UPDATE_INSTALLING, UPDATE_DONE, UPDATE_FAILED };

typedef struct {
    int state;
    char latest[32];
    volatile uint64_t bytes; /* while installing */
    char msg[128];
} update_info;

void update_check(void);   /* background, once at start */
void update_install(void); /* after the player said yes */
void update_status(update_info *out);

#endif
