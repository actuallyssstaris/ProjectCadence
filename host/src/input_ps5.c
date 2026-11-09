#include "input.h"
#include "sce.h"

#include <math.h>
#include <string.h>

static int pad_handle = -1;
static uint32_t prev_buttons;

bool input_init(int user)
{
    scePadInit();
    pad_handle = scePadOpen(user, 0, 0, NULL);
    return pad_handle >= 0;
}

static float stick(uint8_t v)
{
    float f = (v - 128) / 127.0f;
    if (f < -1) f = -1;
    if (f > 1) f = 1;
    return f;
}

/* radial deadzone, rescaled so the edge is still 1 */
static void deadzone(float *x, float *y, float dz)
{
    float m = sqrtf(*x * *x + *y * *y);
    if (m < dz) {
        *x = *y = 0;
        return;
    }
    float k = (fminf(m, 1.0f) - dz) / (1.0f - dz) / m;
    *x *= k;
    *y *= k;
}

void input_poll(pad_state *out)
{
    memset(out, 0, sizeof(*out));
    if (pad_handle < 0)
        return;

    ScePadData d;
    memset(&d, 0, sizeof(d));
    if (scePadReadState(pad_handle, &d) != 0)
        return;

    out->connected = d.connected;
    out->held = d.buttons;
    out->pressed = d.buttons & ~prev_buttons;
    prev_buttons = d.buttons;

    out->lx = stick(d.lx);
    out->ly = stick(d.ly);
    out->rx = stick(d.rx);
    out->ry = stick(d.ry);
    deadzone(&out->lx, &out->ly, 0.08f);
    deadzone(&out->rx, &out->ry, 0.12f);

    if (d.touch_count > 0) {
        out->touching = true;
        out->tx = d.touch[0].x / 1919.0f;
        out->ty = d.touch[0].y / 1079.0f;
    }
}

/*
 * Left stick is absolute: full deflection reaches the playfield edge, the way
 * tablet players map their area. It's lightly smoothed because the raw stick
 * jitters by a pixel or two at rest. Right stick nudges the cursor relatively
 * on top of that for fine corrections. The touchpad overrides both while touched.
 */
void input_to_play(const pad_state *pad, play_input *io, float dt_ms, float sensitivity)
{
    static float nudge_x, nudge_y;

    float tx, ty;

    if (pad->touching) {
        tx = pad->tx * 512.0f;
        ty = pad->ty * 384.0f;
        nudge_x = nudge_y = 0;
        io->x = tx;
        io->y = ty;
    } else {
        nudge_x += pad->rx * dt_ms * 0.4f;
        nudge_y += pad->ry * dt_ms * 0.4f;
        if (pad->rx == 0 && pad->ry == 0) {
            nudge_x *= 0.98f;
            nudge_y *= 0.98f;
        }

        tx = 256 + pad->lx * 256 * sensitivity + nudge_x;
        ty = 192 + pad->ly * 256 * sensitivity + nudge_y;

        float a = 1.0f - expf(-dt_ms / 12.0f);
        io->x += (tx - io->x) * a;
        io->y += (ty - io->y) * a;
    }

    if (io->x < -64) io->x = -64;
    if (io->x > 576) io->x = 576;
    if (io->y < -64) io->y = -64;
    if (io->y > 448) io->y = 448;

    io->keys = 0;
    /* pressing the touchpad down clicks where you're pointing, tablet style */
    if (pad->held & (PAD_R1 | PAD_R2 | PAD_CROSS | PAD_TOUCHPAD))
        io->keys |= 1;
    if (pad->held & (PAD_L1 | PAD_L2 | PAD_SQUARE))
        io->keys |= 2;
}
