#ifndef OSU5_MAPDL_H
#define OSU5_MAPDL_H

#include <stdbool.h>
#include <stdint.h>

/* Beatmap search and download through a public mirror (catboy.best, nerinyan as
 * the fallback). Sets land in /data/cadence/Songs/<id> <artist> - <title>/. */

#define MAPDL_RESULTS 50

typedef struct {
    int id;
    char artist[96];
    char title[128];
    char creator[48];
    int diffs;     /* osu!standard difficulties only */
    float stars;   /* the hardest of them */
    bool video;
    char status[16]; /* ranked, loved, graveyard... */
} mapdl_set;

typedef struct {
    int state; /* 0 nothing yet, 1 searching, 2 results, 3 failed */
    int count;
    mapdl_set sets[MAPDL_RESULTS];
    char msg[128];
} mapdl_search;

/* starts a search on a worker; empty query = newest ranked sets */
bool mapdl_find(const char *query, int page);
/* copies the current search state out (safe to call every frame) */
void mapdl_results(mapdl_search *out);

/* queue a set; false if the queue is full or it's already in it */
bool mapdl_queue(const mapdl_set *s);

typedef struct {
    int queued;          /* waiting, not counting the one in flight */
    int current;         /* set id being fetched, 0 = idle */
    uint64_t bytes;
    int installed;       /* finished since start, the song list needs a rescan when this moves */
    char msg[128];       /* last error or "done" line */
} mapdl_status;

void mapdl_progress(mapdl_status *out);

/* 0 not here, 1 queued or downloading, 2 installed */
int mapdl_state(int set_id);

/* tiny JSON reader for the mirror's search reply, exposed for testing */
int mapdl_parse(const char *json, mapdl_set *out, int max);

#endif
