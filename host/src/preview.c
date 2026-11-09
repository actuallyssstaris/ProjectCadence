#include "preview.h"
#include "audio.h"
#include "log.h"

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "../third_party/stb_image.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static bool started;

static struct {
    int gen;
    char audio[768];
    char bg[768];
    int preview_ms;
} req;

static int done_gen;          /* last request the worker picked up */
static char playing[768];     /* audio file the preview is playing now */
static double loop_from = -1; /* where it restarts, ms; < 0 = not playing */

static struct {
    bool ready;
    unsigned char *px;
    int w, h;
    char path[768];
} bg;

/* halve until it fits 2048, big backgrounds only waste memory */
static unsigned char *load_bg(const char *path, int *w, int *h)
{
    int n;
    unsigned char *px = stbi_load(path, w, h, &n, 4);
    if (!px)
        return NULL;
    while (*w > 2048 || *h > 2048) {
        int nw = *w / 2, nh = *h / 2;
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++)
                memcpy(px + (y * nw + x) * 4, px + ((y * 2) * *w + x * 2) * 4, 4);
        *w = nw;
        *h = nh;
    }
    return px;
}

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&lock);
        while (req.gen == done_gen)
            pthread_cond_wait(&wake, &lock);
        int gen = req.gen;
        done_gen = gen;
        char audio[768], bgpath[768];
        snprintf(audio, sizeof(audio), "%s", req.audio);
        snprintf(bgpath, sizeof(bgpath), "%s", req.bg);
        int preview_ms = req.preview_ms;
        bool same_song = audio[0] && !strcmp(audio, playing);
        pthread_mutex_unlock(&lock);

        if (bgpath[0]) {
            int w = 0, h = 0;
            unsigned char *px = load_bg(bgpath, &w, &h);
            pthread_mutex_lock(&lock);
            if (gen == req.gen) {
                if (bg.px)
                    stbi_image_free(bg.px);
                bg.px = px;
                bg.w = w;
                bg.h = h;
                bg.ready = true;
                snprintf(bg.path, sizeof(bg.path), "%s", bgpath);
                px = NULL;
            }
            pthread_mutex_unlock(&lock);
            if (px)
                stbi_image_free(px);
        }

        /* another difficulty of the same set keeps the music going */
        if (!audio[0] || same_song)
            continue;

        sound_buf music;
        if (!audio_decode_file(audio, &music)) {
            LOG("preview: couldn't decode %s", audio);
            continue;
        }

        pthread_mutex_lock(&lock);
        if (gen == req.gen && req.audio[0]) {
            /* lazer starts at 40% in when the map doesn't say */
            double from = preview_ms >= 0 ? preview_ms : music.frames * 0.4 * 1000.0 / AUDIO_RATE;
            audio_music_set(&music);
            audio_music_rate(1);
            audio_music_play(from);
            snprintf(playing, sizeof(playing), "%s", audio);
            loop_from = from;
        }
        pthread_mutex_unlock(&lock);
        sound_free(&music); /* no-op when audio_music_set took it */
    }
    return NULL;
}

void preview_request(const char *audio_path, const char *bg_path, int preview_ms)
{
    pthread_mutex_lock(&lock);
    if (!started) {
        pthread_t t;
        started = pthread_create(&t, NULL, worker, NULL) == 0;
        if (started)
            pthread_detach(t);
    }
    req.gen++;
    snprintf(req.audio, sizeof(req.audio), "%s", audio_path ? audio_path : "");
    snprintf(req.bg, sizeof(req.bg), "%s", bg_path ? bg_path : "");
    req.preview_ms = preview_ms;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
}

void preview_cancel(void)
{
    pthread_mutex_lock(&lock);
    req.gen++;
    done_gen = req.gen; /* nothing new for the worker to pick up */
    req.audio[0] = 0;
    playing[0] = 0;
    loop_from = -1;
    pthread_mutex_unlock(&lock);
}

void preview_tick(void)
{
    pthread_mutex_lock(&lock);
    double from = loop_from;
    pthread_mutex_unlock(&lock);
    if (from >= 0 && audio_music_ended())
        audio_music_play(from);
}

bool preview_take_bg(unsigned char **px, int *w, int *h, char *path, int path_len)
{
    pthread_mutex_lock(&lock);
    bool ok = bg.ready;
    if (ok) {
        *px = bg.px;
        *w = bg.w;
        *h = bg.h;
        snprintf(path, (size_t)path_len, "%s", bg.path);
        bg.px = NULL;
        bg.ready = false;
    }
    pthread_mutex_unlock(&lock);
    return ok;
}
