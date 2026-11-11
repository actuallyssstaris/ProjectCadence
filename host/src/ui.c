#include "ui.h"
#include "draw.h"
#include "gfx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int tri_lr, tri_ul, tri_up; /* lower-right half, upper-left half, upward triangle */

uint32_t ui_hex(uint32_t rgb, float a)
{
    return gfx_rgba(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

uint32_t ui_mix(uint32_t a, uint32_t b, float k)
{
    uint32_t out = 0;
    for (int s = 0; s < 32; s += 8) {
        float x = ((a >> s) & 0xFF), y = ((b >> s) & 0xFF);
        out |= (uint32_t)(x + (y - x) * k + 0.5f) << s;
    }
    return out;
}

/* coverage with a pixel of soft edge, so scaled up they stay smooth */
static int bake(int which)
{
    enum { N = 128 };
    unsigned char *px = malloc(N * N * 4);
    if (!px)
        return 0;
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            float u = (x + 0.5f) / N, v = (y + 0.5f) / N, d;
            if (which == 0)
                d = (u + v - 1) * N / 1.414f; /* inside where u + v > 1 */
            else if (which == 1)
                d = (1 - u - v) * N / 1.414f;
            else {
                /* apex at the top middle */
                float left = (v - (1 - 2 * u)) * N / 2.236f;  /* right of the left edge */
                float right = (v - (2 * u - 1)) * N / 2.236f; /* left of the right edge */
                d = fminf(left, right);
                d = fminf(d, (1 - v) * N);
            }
            float a = fminf(fmaxf(d + 0.5f, 0), 1);
            unsigned char *p = px + (y * N + x) * 4;
            p[0] = p[1] = p[2] = 255;
            p[3] = (unsigned char)(a * 255);
        }
    }
    int t = gfx_texture(N, N, px);
    free(px);
    return t;
}

void ui_init(void)
{
    tri_lr = bake(0);
    tri_ul = bake(1);
    tri_up = bake(2);
}

static void box(float x, float y, float w, float h, float z, uint32_t c)
{
    if (w <= 0 || h <= 0)
        return;
    gfx_push(&(gfx_inst){x + w / 2, y + h / 2, w / 2, h / 2, 0, z, c, GK_RECT, {0}});
}

void ui_round_rect(float x, float y, float w, float h, float r, uint32_t c)
{
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    gfx_bind(0);
    float z = gfx_next_z(); /* one depth for every piece: no doubled alpha where they meet */
    box(x + r, y, w - 2 * r, h, z, c);
    box(x, y + r, r, h - 2 * r, z, c);
    box(x + w - r, y + r, r, h - 2 * r, z, c);
    const float cx[2] = {x + r, x + w - r}, cy[2] = {y + r, y + h - r};
    for (int i = 0; i < 4; i++)
        gfx_push(&(gfx_inst){cx[i & 1], cy[i >> 1], r, r, 0, z, c, GK_DISC, {0}});
}

void ui_shear_rect(float x, float y, float w, float h, float lean, uint32_t c)
{
    float z = gfx_next_z();
    gfx_bind(0);
    box(x + lean, y, w - 2 * lean, h, z, c);
    if (lean <= 0)
        return;
    gfx_bind(tri_lr);
    gfx_push(&(gfx_inst){x + lean / 2, y + h / 2, lean / 2, h / 2, 0, z, c, GK_TEX, {0, 0, 1, 1}});
    gfx_bind(tri_ul);
    gfx_push(&(gfx_inst){x + w - lean / 2, y + h / 2, lean / 2, h / 2, 0, z, c, GK_TEX, {0, 0, 1, 1}});
    gfx_bind(0);
}

void ui_shadow(float x, float y, float w, float h, float r)
{
    for (int i = 3; i >= 1; i--) {
        float g = i * 5.0f;
        ui_round_rect(x - g, y - g + 6, w + 2 * g, h + 2 * g, r + g, gfx_rgba(0, 0, 0, 0.07f));
    }
}

static float rnd(unsigned *s)
{
    *s = *s * 1664525u + 1013904223u;
    return (*s >> 8) / 16777216.0f;
}

void ui_triangles(float x, float y, float w, float h, uint32_t base, double time_ms, unsigned seed, int count)
{
    gfx_bind(tri_up);
    for (int i = 0; i < count; i++) {
        unsigned s = seed * 7919u + (unsigned)i * 104729u;
        float px = rnd(&s), size = 0.25f + rnd(&s) * 0.75f, speed = 0.02f + rnd(&s) * 0.03f, start = rnd(&s);
        float shade = 0.85f + rnd(&s) * 0.3f;
        /* there's no clipping, so each one stays whole inside the box and fades
         * in at the bottom and out at the top instead of crossing the edge */
        float sz = fminf(size * fminf(w, h) * 0.45f + 20, fminf(w, h) * 0.35f), hh = sz * 0.866f;
        float travel = h - 2 * hh;
        if (travel <= 0 || w < 2 * sz)
            continue;
        float k = fmodf(start + (float)(time_ms / 1000.0) * speed * (0.5f + size), 1.0f);
        float cy = y + h - hh - k * travel, cx = x + sz + px * (w - 2 * sz);
        float a = ((base >> 24) & 0xFF) / 255.0f;
        uint32_t c = ui_mix(base, shade > 1 ? gfx_rgba(1, 1, 1, a) : gfx_rgba(0, 0, 0, a), fabsf(shade - 1) * 0.6f);
        uint32_t fade = (uint32_t)(((c >> 24) & 0xFF) * sinf(k * 3.1415927f));
        c = (c & 0x00FFFFFF) | (fade << 24);
        gfx_push(&(gfx_inst){cx, cy, sz, sz * 0.866f, 0, gfx_next_z(), c, GK_TEX, {0, 0, 1, 1}});
    }
    gfx_bind(0);
}

void ui_play_icon(float cx, float cy, float size, uint32_t c)
{
    gfx_bind(tri_up);
    gfx_push(&(gfx_inst){cx + size * 0.08f, cy, size / 2, size / 2, 1.5707963f, gfx_next_z(), c, GK_TEX, {0, 0, 1, 1}});
    gfx_bind(0);
}

void ui_gear_icon(float cx, float cy, float size, uint32_t c)
{
    gfx_bind(0);
    float z = gfx_next_z();
    float r = size * 0.32f;
    for (int i = 0; i < 8; i++) {
        float a = i * 0.7853982f;
        gfx_push(&(gfx_inst){cx + cosf(a) * r, cy + sinf(a) * r, size * 0.1f, size * 0.12f, a, z, c, GK_RECT, {0}});
    }
    gfx_push(&(gfx_inst){cx, cy, r, r, 0, z, c, GK_DISC, {0}});
    gfx_push(&(gfx_inst){cx, cy, r * 0.45f, r * 0.45f, 0, gfx_next_z(), gfx_rgba(0, 0, 0, 0.55f), GK_DISC, {0}});
}

float ui_approach(float cur, float target, float rate)
{
    return cur + (target - cur) * rate;
}

void ui_disc(float cx, float cy, float r, uint32_t c)
{
    gfx_bind(0);
    gfx_push(&(gfx_inst){cx, cy, r, r, 0, gfx_next_z(), c, GK_DISC, {0}});
}

void ui_arrow_down(float cx, float cy, float size, uint32_t c)
{
    gfx_bind(tri_up);
    gfx_push(&(gfx_inst){cx, cy, size / 2, size * 0.43f, 3.1415927f, gfx_next_z(), c, GK_TEX, {0, 0, 1, 1}});
    gfx_bind(0);
}

void ui_arrow_left(float cx, float cy, float size, uint32_t c)
{
    gfx_bind(tri_up);
    gfx_push(&(gfx_inst){cx, cy, size / 2, size * 0.43f, -1.5707963f, gfx_next_z(), c, GK_TEX, {0, 0, 1, 1}});
    gfx_bind(0);
}

void ui_fit(char *s, float size, float max_w)
{
    if (draw_text_width(size, s) <= max_w)
        return;
    size_t n = strlen(s);
    while (n > 0) {
        do
            n--;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80);
        s[n] = 0;
        if (draw_text_width(size, s) + draw_text_width(size, "...") <= max_w)
            break;
    }
    while (n > 0 && s[n - 1] == ' ')
        s[--n] = 0;
    strcat(s, "...");
}

void ui_search_icon(float cx, float cy, float size, uint32_t c)
{
    float r = size * 0.3f;
    ui_disc(cx - size * 0.08f, cy - size * 0.08f, r, c);
    ui_disc(cx - size * 0.08f, cy - size * 0.08f, r * 0.62f, gfx_rgba(0.1f, 0.1f, 0.12f, 1));
    gfx_push(&(gfx_inst){cx + size * 0.22f, cy + size * 0.22f, size * 0.17f, size * 0.06f, 0.7853982f, gfx_next_z(), c, GK_RECT, {0}});
}
