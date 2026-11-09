#include "audio.h"
#include "log.h"
#include "sce.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define GRAIN 256 /* frames per sceAudioOutOutput, ~5.3ms */
#define VOICES 32
/* what's queued in the driver past what we've handed it, measured by ear more than anything */
#define OUTPUT_LATENCY_MS 16.0

typedef struct {
    const sound_buf *buf;
    uint32_t pos;
    float l, r;
} voice;

static int port = -1;
static pthread_t thread;
static atomic_bool running;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static sound_buf music;
static int64_t music_pos;  /* in frames, negative = silence before the track */
static bool music_playing;
static bool music_paused;
static float music_vol = 0.8f;

static voice voices[VOICES];

/* clock: music_pos at the last mix plus wall time since, so the game sees a smooth time */
static int64_t clock_frames;
static double clock_wall;
static bool clock_valid;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static double last_mix;

/* a port that opened but never takes a grain looks just like no port */
static bool mixing(void)
{
    return atomic_load(&running) && now_ms() - last_mix < 200;
}

static inline int16_t clip(float v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static void mix(int16_t *out)
{
    static float acc[GRAIN * 2];
    memset(acc, 0, sizeof(acc));

    pthread_mutex_lock(&lock);

    if (music_playing && !music_paused) {
        for (int i = 0; i < GRAIN; i++) {
            int64_t p = music_pos + i;
            if (p >= 0 && p < (int64_t)music.frames) {
                acc[i * 2] += music.data[p * 2] * music_vol;
                acc[i * 2 + 1] += music.data[p * 2 + 1] * music_vol;
            }
        }
        music_pos += GRAIN;
    }

    for (int v = 0; v < VOICES; v++) {
        voice *vc = &voices[v];
        if (!vc->buf)
            continue;
        const sound_buf *b = vc->buf;
        int n = GRAIN;
        if (vc->pos + n > b->frames)
            n = (int)(b->frames - vc->pos);
        for (int i = 0; i < n; i++) {
            acc[i * 2] += b->data[(vc->pos + i) * 2] * vc->l;
            acc[i * 2 + 1] += b->data[(vc->pos + i) * 2 + 1] * vc->r;
        }
        vc->pos += n;
        if (vc->pos >= b->frames)
            vc->buf = NULL;
    }

    if (mixing() || last_mix == 0) {
        clock_frames = music_pos;
        clock_wall = now_ms();
        clock_valid = music_playing && !music_paused;
    }

    pthread_mutex_unlock(&lock);

    for (int i = 0; i < GRAIN * 2; i++)
        out[i] = clip(acc[i]);
}

static void *audio_thread(void *arg)
{
    (void)arg;
    static int16_t bufs[2][GRAIN * 2];
    int which = 0;

    unsigned long grains = 0;
    while (atomic_load(&running)) {
        mix(bufs[which]);
        int rc = sceAudioOutOutput(port, bufs[which]); /* blocks until the previous one is consumed */
        last_mix = now_ms();
        if (grains++ == 0 || rc < 0)
            LOG("audio: output -> 0x%08x (grain %lu)", (unsigned)rc, grains);
        which ^= 1;
    }

    sceAudioOutOutput(port, NULL);
    return NULL;
}

bool audio_init(int user)
{
    int rc = sceAudioOutInit();
    /* the main port belongs to the system user; the logged-in one gets 0x80260011 */
    (void)user;
    port = sceAudioOutOpen(0xFF, AUDIO_PORT_MAIN, 0, GRAIN, AUDIO_RATE, AUDIO_FORMAT_S16_STEREO);
    LOG("audio: init 0x%08x, open -> 0x%08x", (unsigned)rc, (unsigned)port);
    if (port < 0)
        return false;

    int32_t vol[2] = {32768, 32768};
    sceAudioOutSetVolume(port, 0x3, vol);

    atomic_store(&running, true);
    if (pthread_create(&thread, NULL, audio_thread, NULL) != 0) {
        atomic_store(&running, false);
        return false;
    }
    return true;
}

void audio_music_set(sound_buf *m)
{
    pthread_mutex_lock(&lock);
    sound_buf old = music;
    music = *m;
    m->data = NULL;
    m->frames = 0;
    music_playing = false;
    music_pos = 0;
    pthread_mutex_unlock(&lock);
    sound_free(&old);
}

/* Without a working mixer nothing advances music_pos, so the clock runs off wall
 * time from the last anchor instead. Call with the lock held. */
static void anchor(void)
{
    if (!mixing() && clock_valid)
        music_pos = clock_frames + (int64_t)((now_ms() - clock_wall) * AUDIO_RATE / 1000.0);
    clock_frames = music_pos;
    clock_wall = now_ms();
    clock_valid = music_playing && !music_paused;
}

void audio_music_play(double from_ms)
{
    pthread_mutex_lock(&lock);
    music_pos = (int64_t)(from_ms * AUDIO_RATE / 1000.0);
    music_playing = true;
    music_paused = false;
    clock_valid = false;
    anchor();
    pthread_mutex_unlock(&lock);
}

void audio_music_pause(bool paused)
{
    pthread_mutex_lock(&lock);
    anchor();
    music_paused = paused;
    clock_valid = music_playing && !music_paused;
    pthread_mutex_unlock(&lock);
}

void audio_music_seek(double ms)
{
    pthread_mutex_lock(&lock);
    music_pos = (int64_t)(ms * AUDIO_RATE / 1000.0);
    clock_valid = false;
    anchor();
    pthread_mutex_unlock(&lock);
}

void audio_music_stop(void)
{
    pthread_mutex_lock(&lock);
    anchor();
    music_playing = false;
    clock_valid = false;
    pthread_mutex_unlock(&lock);
}

void audio_music_volume(float v)
{
    pthread_mutex_lock(&lock);
    music_vol = v;
    pthread_mutex_unlock(&lock);
}

double audio_music_time(void)
{
    pthread_mutex_lock(&lock);
    bool live = mixing();
    double t = clock_frames * 1000.0 / AUDIO_RATE - (live ? OUTPUT_LATENCY_MS : 0);
    if (clock_valid) {
        double since = now_ms() - clock_wall;
        /* never run more than two grains ahead of the mixer */
        double cap = 2.0 * GRAIN * 1000.0 / AUDIO_RATE;
        t += !live || since < cap ? since : cap;
    }
    pthread_mutex_unlock(&lock);
    return t;
}

bool audio_music_ended(void)
{
    pthread_mutex_lock(&lock);
    anchor();
    bool e = music.frames == 0 || music_pos >= (int64_t)music.frames;
    pthread_mutex_unlock(&lock);
    return e;
}

void audio_stop_voices(void)
{
    pthread_mutex_lock(&lock);
    memset(voices, 0, sizeof(voices));
    pthread_mutex_unlock(&lock);
}

void audio_play(const sound_buf *s, float volume, float pan)
{
    if (!s || !s->data || s->frames == 0)
        return;

    float l = volume * (pan > 0 ? 1 - pan : 1);
    float r = volume * (pan < 0 ? 1 + pan : 1);

    pthread_mutex_lock(&lock);
    int slot = -1;
    uint32_t oldest = 0;
    for (int v = 0; v < VOICES; v++) {
        if (!voices[v].buf) {
            slot = v;
            break;
        }
        if (voices[v].pos >= oldest) {
            oldest = voices[v].pos;
            slot = v;
        }
    }
    voices[slot] = (voice){s, 0, l, r};
    pthread_mutex_unlock(&lock);
}
