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
void audio_music_rate(double rate); /* 1.5 for DT, 0.75 for HT */
uint32_t audio_music_frames(void);
double audio_music_time(void); /* ms, can be negative during lead-in */
bool audio_music_ended(void);

/* fire and forget, volume 0..1, pan -1..1 */
void audio_play(const sound_buf *s, float volume, float pan);
/* drop every playing effect, so their buffers can be freed */
void audio_stop_voices(void);

#endif
