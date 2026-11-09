#include "draw.h"
#include "gfx.h"

#include "../third_party/font8x8_basic.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CELL 40   /* atlas cell, glyph is drawn 4x into the middle 32px */
#define GLYPH 32

static int font_tex;
static float pf_scale = 2.25f, pf_x = 384, pf_y = 108;

void draw_init(void)
{
    const int cols = 16, rows = 8;
    int w = cols * CELL, h = rows * CELL;
    uint8_t *px = calloc((size_t)w * h, 1);

    for (int c = 0; c < 128; c++) {
        int ox = (c % cols) * CELL + (CELL - GLYPH) / 2;
        int oy = (c / cols) * CELL + (CELL - GLYPH) / 2;
        for (int y = 0; y < GLYPH; y++) {
            uint8_t row = (uint8_t)font8x8_basic[c][y / 4];
            for (int x = 0; x < GLYPH; x++) {
                if (row & (1 << (x / 4)))
                    px[(oy + y) * w + ox + x] = 255;
            }
        }
    }

    font_tex = gfx_texture_r8(w, h, px);
    free(px);
}

void draw_rect(float x, float y, float w, float h, uint32_t rgba)
{
    gfx_bind(0);
    gfx_push(&(gfx_inst){x + w / 2, y + h / 2, w / 2, h / 2, 0, gfx_next_z(), rgba, GK_RECT, {0, 0, 1, 1}});
}

void draw_image(int tex, float x, float y, float w, float h, uint32_t rgba)
{
    gfx_bind(tex);
    gfx_push(&(gfx_inst){x + w / 2, y + h / 2, w / 2, h / 2, 0, gfx_next_z(), rgba, GK_TEX, {0, 0, 1, 1}});
}

float draw_text_width(float size, const char *s)
{
    return strlen(s) * size * 0.85f;
}

/* size = glyph height in pixels */
float draw_text(float x, float y, float size, uint32_t rgba, int align, const char *s)
{
    float adv = size * 0.85f;
    float w = draw_text_width(size, s);
    if (align == ALIGN_CENTRE)
        x -= w / 2;
    else if (align == ALIGN_RIGHT)
        x -= w;

    gfx_bind(font_tex);
    float z = gfx_next_z();
    float cell = size * CELL / GLYPH;

    for (const unsigned char *p = (const unsigned char *)s; *p; p++, x += adv) {
        int c = *p < 128 ? *p : '?';
        if (c == ' ')
            continue;
        float u = (c % 16) * (1.0f / 16), v = (c / 16) * (1.0f / 8);
        gfx_push(&(gfx_inst){x + size / 2, y + size / 2, cell / 2, cell / 2, 0, z, rgba, GK_GLYPH,
                             {u, v, u + 1.0f / 16, v + 1.0f / 8}});
    }
    return w;
}

void draw_set_playfield(float scale, float ox, float oy)
{
    pf_scale = scale;
    pf_x = ox;
    pf_y = oy;
}

void draw_to_screen(float px, float py, float *sx, float *sy)
{
    *sx = pf_x + px * pf_scale;
    *sy = pf_y + py * pf_scale;
}

static void disc(float x, float y, float r, uint32_t c, float z)
{
    gfx_push(&(gfx_inst){x, y, r, r, 0, z, c, GK_DISC, {0}});
}

static void ring(float x, float y, float r, float thick, uint32_t c, float z)
{
    gfx_push(&(gfx_inst){x, y, r, r, 0, z, c, GK_RING, {thick, 0, 0, 0}});
}

static void number(float x, float y, float r, int n, float alpha)
{
    char buf[12];
    snprintf(buf, sizeof(buf), "%d", n);
    float size = r * 0.7f;
    draw_text(x, y - size / 2, size, gfx_rgba(1, 1, 1, alpha), ALIGN_CENTRE, buf);
    gfx_bind(0);
}

static void slider_body(const osu5_draw *d, const float *pts, int npoints)
{
    if (d->path_count <= 0 || d->path_start + d->path_count > npoints)
        return;

    float r = d->radius * pf_scale;
    float border = r * 0.13f;
    const float *p = pts + d->path_start * 2;
    int n = d->path_count;

    /* body first at the nearer depth, then the border at a farther one: the border
     * can't land on the body and neither pass blends over itself */
    float zborder = gfx_next_z();
    float zbody = gfx_next_z();

    uint32_t body = gfx_argb((d->colour & 0xFF000000) | ((d->colour >> 1) & 0x7F7F7F), d->alpha * 0.85f);
    uint32_t edge = gfx_rgba(1, 1, 1, d->alpha);

    for (int pass = 0; pass < 2; pass++) {
        float rad = pass == 0 ? r - border : r;
        float z = pass == 0 ? zbody : zborder;
        uint32_t col = pass == 0 ? body : edge;
        float hl = pass == 0 ? 1.0f : 0.0f;

        float x0, y0;
        draw_to_screen(p[0], p[1], &x0, &y0);
        if (n == 1)
            gfx_push(&(gfx_inst){x0, y0, rad, rad, 0, z, col, GK_CAPSULE, {x0, y0, hl, 0}});

        for (int i = 1; i < n; i++) {
            float x1, y1;
            draw_to_screen(p[i * 2], p[i * 2 + 1], &x1, &y1);
            gfx_push(&(gfx_inst){x0, y0, rad, rad, 0, z, col, GK_CAPSULE, {x1, y1, hl, 0}});
            x0 = x1;
            y0 = y1;
        }
    }
}

static const char *result_text(int r)
{
    switch (r) {
    case OSU5_RESULT_MISS: return "X";
    case OSU5_RESULT_MEH: return "50";
    case OSU5_RESULT_OK: return "100";
    case OSU5_RESULT_GREAT: return "300";
    default: return "";
    }
}

static uint32_t result_colour(int r, float a)
{
    switch (r) {
    case OSU5_RESULT_MISS: return gfx_rgba(0.93f, 0.2f, 0.2f, a);
    case OSU5_RESULT_MEH: return gfx_rgba(1.0f, 0.8f, 0.3f, a);
    case OSU5_RESULT_OK: return gfx_rgba(0.4f, 0.9f, 0.3f, a);
    default: return gfx_rgba(0.4f, 0.8f, 1.0f, a);
    }
}

void draw_gameplay(const osu5_draw *cmds, int n, const float *points, int npoints)
{
    gfx_bind(0);

    for (int i = 0; i < n; i++) {
        const osu5_draw *d = &cmds[i];
        if (d->alpha <= 0.001f)
            continue;

        float x, y;
        draw_to_screen(d->x, d->y, &x, &y);
        float r = d->radius * pf_scale;

        switch (d->kind) {
        case OSU5_DRAW_SLIDER_BODY:
            slider_body(d, points, npoints);
            break;

        case OSU5_DRAW_CIRCLE: {
            float z = gfx_next_z();
            disc(x, y, r * 1.04f, gfx_rgba(0, 0, 0, d->alpha * 0.35f), z); /* soft shadow */
            gfx_push(&(gfx_inst){x, y, r, r, 0, gfx_next_z(), gfx_argb(d->colour, d->alpha), GK_HITCIRCLE, {0}});
            if (d->number > 0 && !(d->flags & 1))
                number(x, y, r, d->number, d->alpha);
            break;
        }

        case OSU5_DRAW_APPROACH:
            ring(x, y, r, fmaxf(3.0f, r * 0.06f), gfx_argb(d->colour, d->alpha), gfx_next_z());
            break;

        case OSU5_DRAW_SLIDER_BALL: {
            float z = gfx_next_z();
            disc(x, y, r * 0.86f, gfx_argb(d->colour, d->alpha), z);
            ring(x, y, r * 0.86f, r * 0.1f, gfx_rgba(1, 1, 1, d->alpha), gfx_next_z());
            break;
        }

        case OSU5_DRAW_FOLLOW_CIRCLE:
            ring(x, y, r, r * 0.06f, gfx_rgba(1, 1, 1, d->alpha * 0.9f), gfx_next_z());
            break;

        case OSU5_DRAW_TICK:
            disc(x, y, fmaxf(r, 5), gfx_rgba(1, 1, 1, d->alpha), gfx_next_z());
            break;

        case OSU5_DRAW_REVERSE:
            gfx_push(&(gfx_inst){x, y, r * 0.55f, r * 0.55f, d->p0, gfx_next_z(), gfx_rgba(1, 1, 1, d->alpha), GK_ARROW, {0}});
            break;

        case OSU5_DRAW_SPINNER: {
            float cx, cy;
            draw_to_screen(256, 192, &cx, &cy);
            float big = 384 * pf_scale * 0.5f;
            float z = gfx_next_z();
            disc(cx, cy, big, gfx_rgba(0, 0, 0, d->alpha * 0.45f), z);
            ring(cx, cy, big, 6, gfx_rgba(1, 1, 1, d->alpha * 0.6f), gfx_next_z());
            /* progress fill */
            ring(cx, cy, big * (0.2f + 0.78f * d->p1), 10, gfx_rgba(0.4f, 0.8f, 1, d->alpha), gfx_next_z());
            /* shrinking approach */
            if (d->radius > 0)
                ring(cx, cy, big * d->radius, 4, gfx_rgba(1, 1, 1, d->alpha), gfx_next_z());
            /* spinning marker so you can see it turn */
            float a = d->p0;
            disc(cx + cosf(a) * big * 0.6f, cy + sinf(a) * big * 0.6f, 14, gfx_rgba(1, 1, 1, d->alpha), gfx_next_z());

            char buf[32];
            snprintf(buf, sizeof(buf), "%d RPM", d->number);
            draw_text(cx, cy + big + 16, 28, gfx_rgba(1, 1, 1, d->alpha), ALIGN_CENTRE, buf);
            if (d->flags > 0) {
                snprintf(buf, sizeof(buf), "%d", d->flags * 1000);
                draw_text(cx, cy - 40, 80, gfx_rgba(1, 1, 1, d->alpha * 0.8f), ALIGN_CENTRE, buf);
            }
            gfx_bind(0);
            break;
        }

        case OSU5_DRAW_JUDGEMENT: {
            const char *t = result_text(d->number);
            float size = r * 0.8f * (1.0f + 0.15f * (1 - d->p0));
            float yy = y - size / 2 + (d->number == OSU5_RESULT_MISS ? d->p0 * 30 : 0);
            draw_text(x, yy, size, result_colour(d->number, d->alpha), ALIGN_CENTRE, t);
            gfx_bind(0);
            break;
        }

        case OSU5_DRAW_FOLLOW_POINT: {
            float x1, y1;
            draw_to_screen(d->p0, d->p1, &x1, &y1);
            float dx = x1 - x, dy = y1 - y;
            float len = sqrtf(dx * dx + dy * dy);
            if (len < 1)
                break;
            float z = gfx_next_z();
            /* dots every 32 playfield px, skipping the bits under the circles */
            float step = 32 * pf_scale;
            for (float t = r * 1.2f; t < len - r * 1.2f; t += step)
                disc(x + dx / len * t, y + dy / len * t, 4, gfx_rgba(1, 1, 1, d->alpha * 0.6f), z);
            break;
        }

        case OSU5_DRAW_CURSOR: {
            float z = gfx_next_z();
            disc(x, y, 22, gfx_rgba(1, 0.85f, 0.4f, 0.25f), z);
            disc(x, y, 12, gfx_rgba(1, 0.9f, 0.5f, 0.95f), gfx_next_z());
            ring(x, y, 13, 3, gfx_rgba(1, 1, 1, 1), gfx_next_z());
            break;
        }
        }
    }
}

void draw_hud(const osu5_hud *hud, double time)
{
    (void)time;
    char buf[64];

    /* health */
    draw_rect(40, 30, 600, 16, gfx_rgba(0, 0, 0, 0.5f));
    draw_rect(40, 30, 600 * hud->health, 16, gfx_rgba(1, 1, 1, 0.9f));

    snprintf(buf, sizeof(buf), "%08lld", (long long)hud->score);
    draw_text(GFX_W - 40, 24, 56, gfx_rgba(1, 1, 1, 1), ALIGN_RIGHT, buf);
    snprintf(buf, sizeof(buf), "%.2f%%", hud->accuracy * 100);
    draw_text(GFX_W - 40, 90, 32, gfx_rgba(1, 1, 1, 0.9f), ALIGN_RIGHT, buf);

    snprintf(buf, sizeof(buf), "%dx", hud->combo);
    draw_text(40, GFX_H - 100, 64, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, buf);

    /* song progress */
    draw_rect(0, GFX_H - 6, GFX_W, 6, gfx_rgba(0, 0, 0, 0.4f));
    draw_rect(0, GFX_H - 6, GFX_W * hud->progress, 6, gfx_rgba(1, 1, 1, 0.7f));

    if (hud->skip_target > 0) {
        draw_rect(GFX_W - 330, GFX_H - 140, 290, 70, gfx_rgba(0, 0, 0, 0.6f));
        draw_text(GFX_W - 185, GFX_H - 122, 32, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, "TRIANGLE SKIP");
    }
}
