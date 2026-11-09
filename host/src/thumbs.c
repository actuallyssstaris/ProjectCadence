/* small copies of set backgrounds for the song list, decoded on a worker and
 * uploaded on the main thread (the renderer isn't thread safe) */
#include "thumbs.h"
#include "gfx.h"

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "../third_party/stb_image.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS 48

enum { EMPTY, QUEUED, DECODED, DONE, FAILED };

typedef struct {
    char path[768];
    int state;
    unsigned char *px;
    int tex;
    unsigned used;
} slot;

static slot slots[SLOTS];
static unsigned clock_;
static bool started;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;

/* cover-crop to THUMB_W x THUMB_H, averaging the source pixels under each one */
static unsigned char *shrink(const unsigned char *src, int w, int h)
{
    unsigned char *out = malloc(THUMB_W * THUMB_H * 4);
    if (!out)
        return NULL;
    float scale = (float)w / THUMB_W > (float)h / THUMB_H ? (float)h / THUMB_H : (float)w / THUMB_W;
    float ox = (w - THUMB_W * scale) / 2, oy = (h - THUMB_H * scale) / 2;
    int step = scale > 2 ? (int)scale / 2 : 1;
    for (int y = 0; y < THUMB_H; y++)
        for (int x = 0; x < THUMB_W; x++) {
            int sx = (int)(ox + x * scale), sy = (int)(oy + y * scale);
            unsigned acc[4] = {0}, n = 0;
            for (int dy = 0; dy < (int)scale && sy + dy < h; dy += step)
                for (int dx = 0; dx < (int)scale && sx + dx < w; dx += step) {
                    const unsigned char *p = src + ((sy + dy) * w + sx + dx) * 4;
                    for (int c = 0; c < 4; c++)
                        acc[c] += p[c];
                    n++;
                }
            if (!n) {
                memcpy(out + (y * THUMB_W + x) * 4, src + (sy * w + sx) * 4, 4);
                continue;
            }
            for (int c = 0; c < 4; c++)
                out[(y * THUMB_W + x) * 4 + c] = (unsigned char)(acc[c] / n);
        }
    return out;
}

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&lock);
        slot *job = NULL;
        while (!job) {
            /* the most recently asked for first: that's what's on screen */
            for (int i = 0; i < SLOTS; i++)
                if (slots[i].state == QUEUED && (!job || slots[i].used > job->used))
                    job = &slots[i];
            if (!job)
                pthread_cond_wait(&wake, &lock);
        }
        char path[768];
        snprintf(path, sizeof(path), "%s", job->path);
        pthread_mutex_unlock(&lock);

        int w, h, n;
        unsigned char *px = stbi_load(path, &w, &h, &n, 4), *small = NULL;
        if (px) {
            small = shrink(px, w, h);
            stbi_image_free(px);
        }

        pthread_mutex_lock(&lock);
        if (job->state == QUEUED && !strcmp(job->path, path)) {
            job->px = small;
            job->state = small ? DECODED : FAILED;
            small = NULL;
        }
        pthread_mutex_unlock(&lock);
        free(small);
    }
    return NULL;
}

int thumb_get(const char *path)
{
    if (!path || !path[0])
        return 0;
    pthread_mutex_lock(&lock);
    if (!started) {
        pthread_t t;
        started = pthread_create(&t, NULL, worker, NULL) == 0;
        if (started)
            pthread_detach(t);
    }
    clock_++;
    slot *s = NULL, *oldest = &slots[0];
    for (int i = 0; i < SLOTS; i++) {
        if (slots[i].state != EMPTY && !strcmp(slots[i].path, path)) {
            s = &slots[i];
            break;
        }
        if (slots[i].used < oldest->used)
            oldest = &slots[i];
    }
    int tex = 0;
    if (s) {
        s->used = clock_;
        tex = s->state == DONE ? s->tex : 0;
    } else {
        s = oldest;
        if (s->tex)
            gfx_texture_free(s->tex);
        free(s->px);
        memset(s, 0, sizeof(*s));
        snprintf(s->path, sizeof(s->path), "%s", path);
        s->state = QUEUED;
        s->used = clock_;
        pthread_cond_signal(&wake);
    }
    pthread_mutex_unlock(&lock);
    return tex;
}

void thumb_tick(void)
{
    pthread_mutex_lock(&lock);
    int uploads = 0;
    for (int i = 0; i < SLOTS && uploads < 2; i++)
        if (slots[i].state == DECODED) {
            slots[i].tex = gfx_texture(THUMB_W, THUMB_H, slots[i].px);
            free(slots[i].px);
            slots[i].px = NULL;
            slots[i].state = slots[i].tex ? DONE : FAILED;
            uploads++;
        }
    pthread_mutex_unlock(&lock);
}
