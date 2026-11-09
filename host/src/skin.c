/* Hitsounds. Whatever the active skin has, and some passable made-up clicks for
 * what it doesn't, so the game is never silent out of the box. */
#include "audio.h"
#include "skin.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *bank_names[3] = {"normal", "soft", "drum"};
static const char *hit_names[4] = {"hitnormal", "hitwhistle", "hitfinish", "hitclap"};

static sound_buf hits[3][4];
static sound_buf ticks[3];
static sound_buf combobreak, spinnerbonus;

static uint32_t rng = 0x1234567;
static float noise(void)
{
    rng = rng * 1664525u + 1013904223u;
    return ((rng >> 9) & 0x7FFF) / 16384.0f - 1.0f;
}

typedef enum { SYN_CLICK, SYN_WHISTLE, SYN_FINISH, SYN_CLAP, SYN_TICK, SYN_BREAK, SYN_BONUS } synth;

static void make(sound_buf *out, synth kind, float pitch)
{
    float len_s = kind == SYN_FINISH ? 0.6f : kind == SYN_BREAK ? 0.35f : kind == SYN_WHISTLE ? 0.18f : 0.08f;
    uint32_t n = (uint32_t)(len_s * AUDIO_RATE);
    out->data = malloc(n * 2 * sizeof(int16_t));
    out->frames = out->data ? n : 0;
    if (!out->data)
        return;

    float lp = 0;
    for (uint32_t i = 0; i < n; i++) {
        float t = (float)i / AUDIO_RATE;
        float v = 0;
        switch (kind) {
        case SYN_CLICK:
            v = sinf(t * 2 * 3.14159f * 1150 * pitch) * expf(-t * 60) * 0.6f + noise() * expf(-t * 160) * 0.5f;
            break;
        case SYN_WHISTLE:
            v = sinf(t * 2 * 3.14159f * 2100 * pitch) * expf(-t * 18) * 0.35f;
            break;
        case SYN_FINISH:
            lp += (noise() - lp) * 0.6f;
            v = (noise() - lp) * expf(-t * 7) * 0.4f;
            break;
        case SYN_CLAP:
            lp += (noise() - lp) * 0.35f;
            v = lp * (expf(-t * 70) + 0.5f * expf(-fabsf(t - 0.012f) * 300)) * 0.9f;
            break;
        case SYN_TICK:
            v = sinf(t * 2 * 3.14159f * 3000 * pitch) * expf(-t * 200) * 0.3f;
            break;
        case SYN_BREAK:
            v = sinf(t * 2 * 3.14159f * (300 - t * 500)) * expf(-t * 8) * 0.4f;
            break;
        case SYN_BONUS:
            v = sinf(t * 2 * 3.14159f * 1600) * expf(-t * 30) * 0.4f;
            break;
        }
        int16_t s = (int16_t)(fmaxf(-1, fminf(1, v)) * 32000);
        out->data[i * 2] = s;
        out->data[i * 2 + 1] = s;
    }
}

static void load_or_make(sound_buf *out, const char *dir, const char *name, synth kind, float pitch)
{
    char path[512];
    const char *exts[] = {"wav", "ogg", "mp3"};
    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof(path), "%s/%s.%s", dir, name, exts[i]);
        if (audio_decode_file(path, out))
            return;
    }
    make(out, kind, pitch);
}

void skin_load(const char *dir)
{
    /* nothing may still be playing out of the buffers about to go */
    audio_stop_voices();
    for (int b = 0; b < 3; b++) {
        for (int h = 0; h < 4; h++)
            sound_free(&hits[b][h]);
        sound_free(&ticks[b]);
    }
    sound_free(&combobreak);
    sound_free(&spinnerbonus);

    const float pitch[3] = {1.0f, 0.8f, 1.3f};
    const synth kinds[4] = {SYN_CLICK, SYN_WHISTLE, SYN_FINISH, SYN_CLAP};
    char name[64];

    for (int b = 0; b < 3; b++) {
        for (int h = 0; h < 4; h++) {
            snprintf(name, sizeof(name), "%s-%s", bank_names[b], hit_names[h]);
            load_or_make(&hits[b][h], dir, name, kinds[h], pitch[b]);
        }
        snprintf(name, sizeof(name), "%s-slidertick", bank_names[b]);
        load_or_make(&ticks[b], dir, name, SYN_TICK, pitch[b]);
    }
    load_or_make(&combobreak, dir, "combobreak", SYN_BREAK, 1);
    load_or_make(&spinnerbonus, dir, "spinnerbonus", SYN_BONUS, 1);
}

static int bank_index(int bank)
{
    return bank >= 1 && bank <= 3 ? bank - 1 : 0;
}

void skin_play(const osu5_sound *ev, float master)
{
    float vol = (ev->volume > 0 ? ev->volume : 100) / 100.0f * master;

    switch (ev->kind) {
    case OSU5_SOUND_HIT: {
        int b = bank_index(ev->bank), a = bank_index(ev->add_bank);
        audio_play(&hits[b][0], vol, ev->pan); /* hitnormal always plays */
        if (ev->sounds & OSU5_HS_WHISTLE) audio_play(&hits[a][1], vol, ev->pan);
        if (ev->sounds & OSU5_HS_FINISH) audio_play(&hits[a][2], vol, ev->pan);
        if (ev->sounds & OSU5_HS_CLAP) audio_play(&hits[a][3], vol, ev->pan);
        break;
    }
    case OSU5_SOUND_SLIDER_TICK:
        audio_play(&ticks[bank_index(ev->bank)], vol, ev->pan);
        break;
    case OSU5_SOUND_SPINNER_BONUS:
        audio_play(&spinnerbonus, vol, 0);
        break;
    case OSU5_SOUND_COMBO_BREAK:
        audio_play(&combobreak, master, 0);
        break;
    }
}
