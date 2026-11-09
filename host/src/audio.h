#ifndef AUDIO_H
#define AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#define AUDIO_RATE 48000

typedef struct {
    int16_t *data; /* interleaved stereo at AUDIO_RATE */
    uint32_t frames;
} sound_buf;

bool audio_init(int user);

/* decode mp3 / ogg / wav into 48k stereo; false on failure */
bool audio_decode_file(const char *path, sound_buf *out);
void sound_free(sound_buf *s);

/* music: takes ownership of the buffer */
void audio_music_set(sound_buf *music);
void audio_music_play(double from_ms);
void audio_music_pause(bool paused);
void audio_music_seek(double ms);
void audio_music_stop(void);
void audio_music_volume(float v);
void audio_music_rate(double rate); /* resampled playback speed; audio_music_set resets it */
/* the buffer was time-stretched by this much: the clock reports song time */
void audio_music_timescale(double scale);
/* tempo change without the pitch moving (WSOLA); out is in.frames / rate long */
bool audio_stretch(const sound_buf *in, double rate, sound_buf *out);
uint32_t audio_music_frames(void);
double audio_music_time(void); /* ms, can be negative during lead-in */
bool audio_music_ended(void);

/* fire and forget, volume 0..1, pan -1..1 */
void audio_play(const sound_buf *s, float volume, float pan);
/* drop every playing effect, so their buffers can be freed */
void audio_stop_voices(void);

#endif
