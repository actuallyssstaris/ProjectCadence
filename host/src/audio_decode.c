#define DR_MP3_IMPLEMENTATION
#define DR_WAV_IMPLEMENTATION
#define STB_VORBIS_NO_PUSHDATA_API
#include "../third_party/dr_mp3.h"
#include "../third_party/dr_wav.h"
#include "../third_party/stb_vorbis.c"

#include "audio.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* linear resample + channel fixup, good enough for music, osu! never cared either */
static bool to_output(const int16_t *src, uint64_t frames, int channels, uint32_t rate, sound_buf *out)
{
    if (!src || frames == 0 || channels < 1)
        return false;

    double step = (double)rate / AUDIO_RATE;
    uint64_t n = (uint64_t)(frames / step);
    int16_t *dst = malloc(n * 2 * sizeof(int16_t));
    if (!dst)
        return false;

    for (uint64_t i = 0; i < n; i++) {
        double pos = i * step;
        uint64_t a = (uint64_t)pos;
        uint64_t b = a + 1 < frames ? a + 1 : a;
        float t = (float)(pos - a);

        for (int c = 0; c < 2; c++) {
            int sc = channels == 1 ? 0 : c;
            float s0 = src[a * channels + sc];
            float s1 = src[b * channels + sc];
            dst[i * 2 + c] = (int16_t)(s0 + (s1 - s0) * t);
        }
    }

    out->data = dst;
    out->frames = (uint32_t)n;
    return true;
}

static const char *ext_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot ? dot + 1 : "";
}

bool audio_decode_file(const char *path, sound_buf *out)
{
    memset(out, 0, sizeof(*out));
    const char *ext = ext_of(path);
    bool ok = false;

    if (strcasecmp(ext, "ogg") == 0) {
        int ch = 0, rate = 0;
        short *pcm = NULL;
        int frames = stb_vorbis_decode_filename(path, &ch, &rate, &pcm);
        if (frames > 0)
            ok = to_output(pcm, (uint64_t)frames, ch, (uint32_t)rate, out);
        free(pcm);
        return ok;
    }

    if (strcasecmp(ext, "wav") == 0) {
        unsigned int ch, rate;
        drwav_uint64 frames;
        int16_t *pcm = drwav_open_file_and_read_pcm_frames_s16(path, &ch, &rate, &frames, NULL);
        if (pcm)
            ok = to_output(pcm, frames, (int)ch, rate, out);
        drwav_free(pcm, NULL);
        if (ok)
            return true;
        /* some skins ship mp3s named .wav, fall through and try that */
    }

    drmp3_config cfg;
    drmp3_uint64 frames;
    int16_t *pcm = drmp3_open_file_and_read_pcm_frames_s16(path, &cfg, &frames, NULL);
    if (pcm)
        ok = to_output(pcm, frames, (int)cfg.channels, cfg.sampleRate, out);
    drmp3_free(pcm, NULL);

    if (!ok && strcasecmp(ext, "ogg") != 0) {
        int ch = 0, rate = 0;
        short *vpcm = NULL;
        int vframes = stb_vorbis_decode_filename(path, &ch, &rate, &vpcm);
        if (vframes > 0)
            ok = to_output(vpcm, (uint64_t)vframes, ch, (uint32_t)rate, out);
        free(vpcm);
    }

    return ok;
}

void sound_free(sound_buf *s)
{
    free(s->data);
    s->data = NULL;
    s->frames = 0;
}
