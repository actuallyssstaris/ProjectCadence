#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t held;
    uint32_t pressed;   /* went down this frame */
    float lx, ly;       /* -1..1 with deadzone applied */
    float rx, ry;
    bool touching;
    float tx, ty;       /* 0..1 over the touchpad */
    bool connected;
} pad_state;

typedef struct {
    float x, y;  /* playfield coordinates, 0..512 x 0..384 */
    int keys;    /* bit 0 = K1, bit 1 = K2 */
} play_input;

bool input_init(int user);
void input_poll(pad_state *out);

/* turn pad state into a cursor + keys for gameplay */
void input_to_play(const pad_state *pad, play_input *io, float dt_ms, float sensitivity);

#endif
