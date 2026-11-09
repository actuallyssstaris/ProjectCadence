#ifndef OSU5_SKINS_H
#define OSU5_SKINS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Skins live in /data/osu5/skins/<id>/; skin.txt says which one is in use
 * ("builtin" for the drawn look). */

typedef struct {
    const char *id;      /* folder name */
    const char *name;
    const char *author;
    const char *licence;
    const char *url;     /* .osk, or NULL for the default skin's loose files */
} skin_entry;

extern const skin_entry skin_catalog[];
extern const int skin_catalog_count;

bool skins_chosen(void);                  /* has the player picked anything yet */
const char *skins_active(void);           /* id, or "builtin" */
void skins_set_active(const char *id);
void skins_path(const char *id, char *out, size_t n);
bool skins_installed(const char *id);
int skins_local(char ids[][64], int max); /* folders under skins/, catalog or not */

typedef struct {
    volatile int state;       /* 0 idle, 1 working, 2 done, 3 failed */
    volatile uint64_t bytes;
    volatile int done, total; /* files, for the default skin */
    bool force;               /* default skin: fetch files that are already there too */
    char msg[128];
} skin_job;

/* downloads catalog entry i on a worker thread; poll job->state */
bool skins_install(int i, skin_job *job, bool force);

/* the default skin from an older version lacks the HUD and spinner art */
bool skins_default_outdated(void);

#endif
