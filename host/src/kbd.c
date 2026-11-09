#include "kbd.h"
#include "draw.h"
#include "gfx.h"
#include "sce.h"

#include <string.h>

static const char *keys[4] = {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm-.!"};
enum { KEY_SPACE, KEY_DEL, KEY_GO }; /* the wide bottom row */

/* kx stays a column of the 10 wide rows; on the bottom row it picks one of the
 * three wide keys, so going up and down keeps roughly the same spot */
static int wide_key(const kbd *k) { return k->kx * 3 / 10; }

static void put(kbd *k, char c)
{
    size_t n = strlen(k->text);
    if (n + 1 < k->cap) {
        k->text[n] = c;
        k->text[n + 1] = 0;
    }
}

static void backspace(kbd *k)
{
    size_t n = strlen(k->text);
    if (n)
        k->text[n - 1] = 0;
}

int kbd_frame(kbd *k, const pad_state *pad)
{
    k->ky = (k->ky + pad->nav_y + 5) % 5;
    if (pad->nav_x) {
        if (k->ky == 4) {
            static const int centre[3] = {1, 5, 8};
            k->kx = centre[(wide_key(k) + pad->nav_x + 3) % 3];
        } else {
            k->kx = (k->kx + pad->nav_x + 10) % 10;
        }
    }

    if (pad->pressed & PAD_CROSS) {
        if (k->ky < 4)
            put(k, keys[k->ky][k->kx]);
        else if (wide_key(k) == KEY_SPACE)
            put(k, ' ');
        else if (wide_key(k) == KEY_DEL)
            backspace(k);
        else
            return KBD_DONE;
    }
    if (pad->pressed & PAD_SQUARE)
        backspace(k);
    if (pad->pressed & PAD_TRIANGLE)
        put(k, ' ');
    if (pad->pressed & (PAD_OPTIONS | PAD_R2))
        return KBD_DONE;
    if (pad->pressed & PAD_CIRCLE)
        return KBD_CANCEL;
    return KBD_TYPING;
}

void kbd_render(const kbd *k)
{
    const float kw = 92, kh = 78, x0 = (GFX_W - kw * 10) / 2, y0 = GFX_H - 5 * kh - 120;
    draw_rect(x0 - 20, y0 - 20, kw * 10 + 40, kh * 5 + 40, gfx_rgba(0, 0, 0, 0.85f));
    char c[2] = {0, 0};
    for (int r = 0; r < 4; r++) {
        for (int i = 0; i < 10; i++) {
            bool on = k->ky == r && k->kx == i;
            float x = x0 + i * kw, y = y0 + r * kh;
            draw_rect(x + 4, y + 4, kw - 8, kh - 8, on ? gfx_rgba(1, 1, 1, 0.95f) : gfx_rgba(1, 1, 1, 0.12f));
            c[0] = keys[r][i];
            draw_text(x + kw / 2, y + kh / 2 - 18, 36, on ? gfx_rgba(0.1f, 0.1f, 0.15f, 1) : gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, c);
        }
    }
    static const char *wide[3] = {"space", "delete", "search"};
    for (int i = 0; i < 3; i++) {
        bool on = k->ky == 4 && wide_key(k) == i;
        float w = kw * 10 / 3, x = x0 + i * w, y = y0 + 4 * kh;
        draw_rect(x + 4, y + 4, w - 8, kh - 8, on ? gfx_rgba(1, 1, 1, 0.95f) : gfx_rgba(1, 1, 1, 0.12f));
        draw_text(x + w / 2, y + kh / 2 - 15, 30, on ? gfx_rgba(0.1f, 0.1f, 0.15f, 1) : gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, wide[i]);
    }
    draw_text(GFX_W / 2, GFX_H - 90, 24, gfx_rgba(1, 1, 1, 0.75f), ALIGN_CENTRE,
              "CROSS type   SQUARE delete   TRIANGLE space   OPTIONS search   CIRCLE close");
}
