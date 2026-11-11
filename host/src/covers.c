#include "covers.h"
#include "gfx.h"
#include "log.h"
#include "net.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../third_party/stb_image.h"

/* Set covers from osu!'s asset server, the same square thumbnails lazer's
 * listing cards use. One worker fetches and decodes; textures are made on the
 * render thread since that's the only place gfx may be touched. */

#define SLOTS 64

enum { EMPTY, WANTED, LOADING, DECODED, READY, FAILED };

typedef struct {
    int id;
    int state;
    int tex;
    unsigned char *px;
    int w, h;
    unsigned last; /* frame it was last asked for, for eviction */
} slot;

static slot slots[SLOTS];
static unsigned frame;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static bool worker_up;

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&lock);
        int pick = -1;
        while (pick < 0) {
            /* newest request first: what's on screen now beats what scrolled away */
            unsigned best = 0;
            for (int i = 0; i < SLOTS; i++)
                if (slots[i].state == WANTED && (pick < 0 || slots[i].last > best)) {
                    pick = i;
                    best = slots[i].last;
                }
            if (pick < 0)
                pthread_cond_wait(&wake, &lock);
        }
        int id = slots[pick].id;
        slots[pick].state = LOADING;
        pthread_mutex_unlock(&lock);

        char url[128];
        snprintf(url, sizeof(url), "https://assets.ppy.sh/beatmaps/%d/covers/list@2x.jpg", id);
        char *body = NULL;
        size_t len = 0;
        int st = net_get(url, &body, &len);
        unsigned char *px = NULL;
        int w = 0, h = 0, n;
        if (st == 200 && body)
            px = stbi_load_from_memory((const unsigned char *)body, (int)len, &w, &h, &n, 4);
        free(body);

        pthread_mutex_lock(&lock);
        /* the slot may have been handed to another set while we were out */
        if (slots[pick].id == id && slots[pick].state == LOADING) {
            slots[pick].px = px;
            slots[pick].w = w;
            slots[pick].h = h;
            slots[pick].state = px ? DECODED : FAILED;
            px = NULL;
        }
        pthread_mutex_unlock(&lock);
        if (px)
            stbi_image_free(px);
    }
    return NULL;
}

void covers_frame(void)
{
    frame++;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < SLOTS; i++) {
        slot *s = &slots[i];
        if (s->state == DECODED) {
            s->tex = gfx_texture(s->w, s->h, s->px);
            stbi_image_free(s->px);
            s->px = NULL;
            s->state = s->tex ? READY : FAILED;
        }
    }
    pthread_mutex_unlock(&lock);
}

int covers_get(int set_id)
{
    if (set_id <= 0)
        return 0;
    pthread_mutex_lock(&lock);
    if (!worker_up) {
        pthread_t t;
        worker_up = pthread_create(&t, NULL, worker, NULL) == 0;
    }
    int tex = 0, free_slot = -1, oldest = -1;
    for (int i = 0; i < SLOTS; i++) {
        slot *s = &slots[i];
        if (s->state != EMPTY && s->id == set_id) {
            s->last = frame;
            tex = s->state == READY ? s->tex : 0;
            pthread_mutex_unlock(&lock);
            return tex;
        }
        if (s->state == EMPTY && free_slot < 0)
            free_slot = i;
        /* only settled slots can be reused; one in flight belongs to the worker */
        if ((s->state == READY || s->state == FAILED || s->state == WANTED) && s->last + 2 < frame &&
            (oldest < 0 || s->last < slots[oldest].last))
            oldest = i;
    }
    int use = free_slot >= 0 ? free_slot : oldest;
    if (use >= 0) {
        slot *s = &slots[use];
        if (s->state == READY && s->tex)
            gfx_texture_free(s->tex);
        memset(s, 0, sizeof(*s));
        s->id = set_id;
        s->state = WANTED;
        s->last = frame;
        pthread_cond_signal(&wake);
    }
    pthread_mutex_unlock(&lock);
    return tex;
}
