#ifndef OSU5_SKINS_H
#define OSU5_SKINS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Skins live in /data/cadence/skins/<id>/; skin.txt says which one is in use
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

/* o!rdr's skin list (ordr.issou.best): hundreds of community skins with direct
 * .osk links, most used first */
typedef struct {
    char id[96];      /* folder name, o!rdr's skin name */
    char name[96];
    char author[64];
    char url[256];
    int used;         /* renders made with it, the popularity order */
} skin_remote;

void skins_remote_load(void);             /* fetch the list once, in the background */
int skins_remote_state(void);             /* 0 not asked, 1 loading, 2 ready, 3 failed */
int skins_remote(const skin_remote **out); /* the list, valid once ready */
bool skins_install_remote(const skin_remote *r, skin_job *job);

/* the default skin from an older version lacks the HUD and spinner art */
bool skins_default_outdated(void);

#endif
