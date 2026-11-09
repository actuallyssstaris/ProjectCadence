#include "draw.h"
#include "gfx.h"
#include "skin_img.h"

#include "../third_party/font8x8_basic.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "../third_party/stb_truetype.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CELL 40   /* atlas cell, glyph is drawn 4x into the middle 32px */
#define GLYPH 32

#define FONT_FILE "/app0/assets/Inter-SemiBold.ttf"
#define BAKE_PX 56.0f /* atlas glyph height; text is drawn scaled from it */
#define ATLAS_W 1024
#define ATLAS_H 512

static int font_tex;
static float pf_scale = 2.25f, pf_x = 384, pf_y = 108;
static float cursor_scale = 1;

/* the UI font, when it loaded; otherwise the 8x8 one below */
static bool ttf;
static stbtt_bakedchar baked[96];
static float ascent; /* at BAKE_PX */

void draw_set_cursor_scale(float s) { cursor_scale = s; }

static bool load_ttf(void)
{
    FILE *f = fopen(FONT_FILE, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc((size_t)n);
    bool ok = data && fread(data, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    uint8_t *px = ok ? calloc(ATLAS_W * ATLAS_H, 1) : NULL;
    if (px && stbtt_BakeFontBitmap(data, 0, BAKE_PX, px, ATLAS_W, ATLAS_H, 32, 96, baked) > 0) {
        stbtt_fontinfo info;
        int asc, desc, gap;
        stbtt_InitFont(&info, data, 0);
        stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
        ascent = asc * stbtt_ScaleForPixelHeight(&info, BAKE_PX);
        font_tex = gfx_texture_r8(ATLAS_W, ATLAS_H, px);
        ttf = font_tex != 0;
    }
    free(px);
    free(data);
    return ttf;
}

void draw_init(void)
{
    if (load_ttf())
        return;
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

/* the 8x8 font's size meant its full cell; the TTF is drawn a bit taller so the
 * capitals end up about as big */
#define TTF_SCALE(size) ((size) * 1.25f / BAKE_PX)

float draw_text_width(float size, const char *s)
{
    if (!ttf)
        return strlen(s) * size * 0.85f;
    float w = 0, k = TTF_SCALE(size);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        int c = *p >= 32 && *p < 128 ? *p : '?';
        w += baked[c - 32].xadvance * k;
    }
    return w;
}

static float draw_ttf(float x, float y, float size, uint32_t rgba, const char *s)
{
    float k = TTF_SCALE(size);
    /* y is the top of the line, as with the old font: put the caps' top there */
    float base = y + ascent * k * 0.82f;
    float start = x;
    gfx_bind(font_tex);
    float z = gfx_next_z();
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        int c = *p >= 32 && *p < 128 ? *p : '?';
        const stbtt_bakedchar *b = &baked[c - 32];
        float w = (b->x1 - b->x0) * k, h = (b->y1 - b->y0) * k;
        if (w > 0 && h > 0) {
            float gx = x + b->xoff * k, gy = base + b->yoff * k;
            gfx_push(&(gfx_inst){gx + w / 2, gy + h / 2, w / 2, h / 2, 0, z, rgba, GK_GLYPH,
                                 {(float)b->x0 / ATLAS_W, (float)b->y0 / ATLAS_H, (float)b->x1 / ATLAS_W, (float)b->y1 / ATLAS_H}});
        }
        x += b->xadvance * k;
    }
    return x - start;
}

/* size = glyph height in pixels */
float draw_text(float x, float y, float size, uint32_t rgba, int align, const char *s)
{
    if (ttf) {
        float w = draw_text_width(size, s);
        if (align == ALIGN_CENTRE)
            x -= w / 2;
        else if (align == ALIGN_RIGHT)
            x -= w;
        return draw_ttf(x, y, size, rgba, s);
    }
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
    gfx_bind(0);
    gfx_push(&(gfx_inst){x, y, r, r, 0, z, c, GK_DISC, {0}});
}

static void ring(float x, float y, float r, float thick, uint32_t c, float z)
{
    gfx_bind(0);
    gfx_push(&(gfx_inst){x, y, r, r, 0, z, c, GK_RING, {thick, 0, 0, 0}});
}

/* skin images are authored for 640x480 at circle radius 64, so scale = r / 64
 * for anything that follows circle size and pf_scale for anything that doesn't */
static void sprite(const skin_sprite *s, float x, float y, float scale, float rot, uint32_t col)
{
    gfx_bind(s->tex);
    gfx_push(&(gfx_inst){x, y, s->w * scale / 2, s->h * scale / 2, rot, gfx_next_z(), col, GK_TEX, {0, 0, 1, 1}});
}

static bool skinned(const skin_sprite *s)
{
    return skin_img.on && s->tex;
}

/* left part of a sprite only, for bars that fill up; x, y = top left */
static void sprite_cropped(const skin_sprite *s, float x, float y, float scale, float fill, uint32_t col)
{
    if (fill <= 0)
        return;
    if (fill > 1)
        fill = 1;
    float w = s->w * scale * fill, h = s->h * scale;
    gfx_bind(s->tex);
    gfx_push(&(gfx_inst){x + w / 2, y + h / 2, w / 2, h / 2, 0, gfx_next_z(), col, GK_TEX, {0, 0, fill, 1}});
}

/* ---- skin fonts: score-*, combo digits ---- */

static const skin_sprite *glyph(bool combo, char c)
{
    if (c >= '0' && c <= '9')
        return combo ? &skin_img.combo[c - '0'] : &skin_img.score[c - '0'];
    switch (c) {
    case ',': return &skin_img.score_comma;
    case '.': return &skin_img.score_dot;
    case '%': return &skin_img.score_percent;
    case 'x': return combo ? &skin_img.combo_x : &skin_img.score_x;
    default: return NULL;
    }
}

bool draw_skin_font(bool combo)
{
    return (combo ? skin_img.combo[0].tex : skin_img.score[0].tex) != 0;
}

/* scale is per SD pixel; y is the top of the line */
float draw_skin_text(float x, float y, float scale, bool combo, int align, const char *s, float alpha)
{
    float overlap = (combo ? skin_img.combo_overlap : skin_img.score_overlap) * scale;
    float w = 0;
    int n = 0;
    for (const char *c = s; *c; c++) {
        const skin_sprite *g = glyph(combo, *c);
        if (g && g->tex) {
            w += g->w * scale;
            n++;
        }
    }
    if (n > 1)
        w -= overlap * (n - 1);
    if (align == ALIGN_CENTRE) x -= w / 2;
    else if (align == ALIGN_RIGHT) x -= w;

    for (const char *c = s; *c; c++) {
        const skin_sprite *g = glyph(combo, *c);
        if (!g || !g->tex)
            continue;
        float gw = g->w * scale, gh = g->h * scale;
        gfx_bind(g->tex);
        gfx_push(&(gfx_inst){x + gw / 2, y + gh / 2, gw / 2, gh / 2, 0, gfx_next_z(), gfx_rgba(1, 1, 1, alpha), GK_TEX, {0, 0, 1, 1}});
        x += gw - overlap;
    }
    return w;
}

void draw_rank(int rank, float cx, float cy, float height, bool small, float alpha)
{
    static const char *names[] = {"D", "C", "B", "A", "S", "S", "SS", "SS", "F"};
    static const float tint[][3] = {
        {0.85f, 0.35f, 0.35f}, {0.75f, 0.45f, 0.85f}, {0.35f, 0.6f, 1.0f}, {0.4f, 0.9f, 0.4f},
        {1.0f, 0.8f, 0.3f}, {0.8f, 0.85f, 0.9f}, {1.0f, 0.85f, 0.35f}, {0.85f, 0.9f, 0.95f}, {0.9f, 0.3f, 0.3f},
    };
    if (rank < 0 || rank > 8)
        return;
    const skin_sprite *s = rank < 8 ? (small ? &skin_img.rank_small[rank] : &skin_img.rank[rank]) : NULL;
    if (s && s->tex) {
        float k = height / s->h;
        gfx_bind(s->tex);
        gfx_push(&(gfx_inst){cx, cy, s->w * k / 2, s->h * k / 2, 0, gfx_next_z(), gfx_rgba(1, 1, 1, alpha), GK_TEX, {0, 0, 1, 1}});
        return;
    }
    draw_text(cx, cy - height / 2, height, gfx_rgba(tint[rank][0], tint[rank][1], tint[rank][2], alpha), ALIGN_CENTRE, names[rank]);
}

static void skin_number(float x, float y, float r, int n, float alpha)
{
    char buf[12];
    int len = snprintf(buf, sizeof(buf), "%d", n);
    float s = r / 64 * 0.8f;
    float w = 0;
    for (int i = 0; i < len; i++)
        w += skin_img.digit[buf[i] - '0'].w * s;
    w -= skin_img.overlap * s * (len - 1);

    float cx = x - w / 2;
    for (int i = 0; i < len; i++) {
        const skin_sprite *d = &skin_img.digit[buf[i] - '0'];
        sprite(d, cx + d->w * s / 2, y, s, 0, gfx_rgba(1, 1, 1, alpha));
        cx += d->w * s - skin_img.overlap * s;
    }
}

static void number(float x, float y, float r, int n, float alpha)
{
    if (skinned(&skin_img.digit[0])) {
        skin_number(x, y, r, n, alpha);
        return;
    }
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

    /* skin.ini SliderTrackOverride / SliderBorder, else a darker combo colour and white */
    uint32_t body = skin_img.has_track ? gfx_argb(skin_img.track, d->alpha * 0.85f)
                                       : gfx_argb((d->colour & 0xFF000000) | ((d->colour >> 1) & 0x7F7F7F), d->alpha * 0.85f);
    uint32_t edge = skin_img.has_border ? gfx_argb(skin_img.border, d->alpha) : gfx_rgba(1, 1, 1, d->alpha);

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

/* p0 rotation, p1 progress to clear, radius = time left (1..0), number rpm,
 * flags bonus spins. Sizes are SD pixels times pf_scale, as stable lays them out. */
static bool skinned_spinner(const osu5_draw *d, float cx, float cy)
{
    bool fresh = skin_img.sp_top.tex != 0, old = skin_img.sp_circle.tex != 0;
    if (!skin_img.on || (!fresh && !old))
        return false;

    float a = d->alpha, rot = d->p0, p = d->p1;
    uint32_t white = gfx_rgba(1, 1, 1, a);

    if (fresh) {
        float s = pf_scale * 0.8f;
        if (skin_img.sp_glow.tex)
            sprite(&skin_img.sp_glow, cx, cy, s, 0, gfx_rgba(0.01f, 0.6f, 1, a * p));
        if (skin_img.sp_bottom.tex)
            sprite(&skin_img.sp_bottom, cx, cy, s, rot / 3, white);
        sprite(&skin_img.sp_top, cx, cy, s, rot, white);
        if (skin_img.sp_middle2.tex)
            sprite(&skin_img.sp_middle2, cx, cy, s, 0, white);
        /* middle goes red as the spin completes */
        if (skin_img.sp_middle.tex)
            sprite(&skin_img.sp_middle, cx, cy, s, 0, gfx_rgba(1, 1 - p, 1 - p, a));
    } else {
        if (skin_img.sp_background.tex) {
            float s = GFX_H / skin_img.sp_background.h;
            sprite(&skin_img.sp_background, cx, GFX_H / 2.0f, s, 0, white);
        }
        if (skin_img.sp_metre.tex) {
            /* fills from the bottom up; the crop goes the other way, so flip the uv */
            const skin_sprite *m = &skin_img.sp_metre;
            float s = GFX_H / skin_img.sp_metre.h, w = m->w * s, h = m->h * s * p;
            gfx_bind(m->tex);
            gfx_push(&(gfx_inst){cx, GFX_H - h / 2, w / 2, h / 2, 0, gfx_next_z(), white, GK_TEX, {0, 1 - p, 1, 1}});
        }
        sprite(&skin_img.sp_circle, cx, cy, pf_scale * 0.6f, rot, white);
    }

    if (skin_img.sp_approach.tex && d->radius > 0)
        sprite(&skin_img.sp_approach, cx, cy, pf_scale * 1.2f * d->radius, 0, white);
    if (p >= 1 && skin_img.sp_clear.tex)
        sprite(&skin_img.sp_clear, cx, cy - 170 * pf_scale / 2.25f, pf_scale * 0.7f, 0, white);

    char buf[32];
    if (d->flags > 0) {
        snprintf(buf, sizeof(buf), "%d", d->flags * 1000);
        if (draw_skin_font(false))
            draw_skin_text(cx, cy + 90, 1.3f, false, ALIGN_CENTRE, buf, a);
        else
            draw_text(cx, cy + 90, 70, gfx_rgba(1, 1, 1, a), ALIGN_CENTRE, buf);
    }
    snprintf(buf, sizeof(buf), "%d RPM", d->number);
    draw_text(cx, GFX_H - 70, 28, gfx_rgba(1, 1, 1, a), ALIGN_CENTRE, buf);
    gfx_bind(0);
    return true;
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
            gfx_bind(0);
            slider_body(d, points, npoints);
            break;

        case OSU5_DRAW_CIRCLE: {
            if (skinned(&skin_img.circle)) {
                sprite(&skin_img.circle, x, y, r / 64, 0, gfx_argb(d->colour, d->alpha));
                if (skin_img.overlay.tex)
                    sprite(&skin_img.overlay, x, y, r / 64, 0, gfx_rgba(1, 1, 1, d->alpha));
                if (d->number > 0 && !(d->flags & 1))
                    number(x, y, r, d->number, d->alpha);
                break;
            }
            gfx_bind(0);
            float z = gfx_next_z();
            disc(x, y, r * 1.04f, gfx_rgba(0, 0, 0, d->alpha * 0.35f), z); /* soft shadow */
            gfx_push(&(gfx_inst){x, y, r, r, 0, gfx_next_z(), gfx_argb(d->colour, d->alpha), GK_HITCIRCLE, {0}});
            if (d->number > 0 && !(d->flags & 1))
                number(x, y, r, d->number, d->alpha);
            break;
        }

        case OSU5_DRAW_APPROACH:
            if (skinned(&skin_img.approach))
                sprite(&skin_img.approach, x, y, r / 64, 0, gfx_argb(d->colour, d->alpha));
            else
                ring(x, y, r, fmaxf(3.0f, r * 0.06f), gfx_argb(d->colour, d->alpha), gfx_next_z());
            break;

        case OSU5_DRAW_SLIDER_BALL: {
            if (skinned(&skin_img.ball)) {
                sprite(&skin_img.ball, x, y, r / 64, 0, gfx_rgba(1, 1, 1, d->alpha));
                break;
            }
            float z = gfx_next_z();
            disc(x, y, r * 0.86f, gfx_argb(d->colour, d->alpha), z);
            ring(x, y, r * 0.86f, r * 0.1f, gfx_rgba(1, 1, 1, d->alpha), gfx_next_z());
            break;
        }

        case OSU5_DRAW_FOLLOW_CIRCLE:
            /* r grows to 2.4x the circle; the art is drawn for the circle's own scale */
            if (skinned(&skin_img.follow))
                sprite(&skin_img.follow, x, y, r / 2.4f / 64, 0, gfx_rgba(1, 1, 1, d->alpha));
            else
                ring(x, y, r, r * 0.06f, gfx_rgba(1, 1, 1, d->alpha * 0.9f), gfx_next_z());
            break;

        case OSU5_DRAW_TICK:
            /* ticks come in at 0.15 of the circle radius */
            if (skinned(&skin_img.tick))
                sprite(&skin_img.tick, x, y, r / 0.15f / 64, 0, gfx_rgba(1, 1, 1, d->alpha));
            else
                disc(x, y, fmaxf(r, 5), gfx_rgba(1, 1, 1, d->alpha), gfx_next_z());
            break;

        case OSU5_DRAW_REVERSE:
            if (skinned(&skin_img.reverse)) {
                sprite(&skin_img.reverse, x, y, r / 64, d->p0, gfx_rgba(1, 1, 1, d->alpha));
                break;
            }
            gfx_bind(0);
            gfx_push(&(gfx_inst){x, y, r * 0.55f, r * 0.55f, d->p0, gfx_next_z(), gfx_rgba(1, 1, 1, d->alpha), GK_ARROW, {0}});
            break;

        case OSU5_DRAW_SPINNER: {
            float cx, cy;
            draw_to_screen(256, 192, &cx, &cy);
            if (skinned_spinner(d, cx, cy))
                break;
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
            const skin_sprite *js = d->number == OSU5_RESULT_MISS ? &skin_img.hit0
                                  : d->number == OSU5_RESULT_MEH  ? &skin_img.hit50
                                  : d->number == OSU5_RESULT_OK   ? &skin_img.hit100
                                                                  : &skin_img.hit300;
            /* hit lighting: the skin's glow, added on in the object's colour */
            if (skin_img.lighting.tex && d->number != OSU5_RESULT_MISS && d->p0 < 0.6f) {
                float k = d->p0 / 0.6f;
                uint32_t c = d->colour ? gfx_argb(d->colour, (1 - k) * 0.8f) : gfx_rgba(1, 1, 1, (1 - k) * 0.8f);
                gfx_additive(true);
                sprite(&skin_img.lighting, x, y, r / 64 * (0.8f + 0.4f * k), 0, c);
                gfx_additive(false);
            }
            if (skinned(js)) {
                float pop = 1.0f + 0.15f * (1 - d->p0);
                float drop = d->number == OSU5_RESULT_MISS ? d->p0 * 30 : 0;
                sprite(js, x, y + drop, r / 64 * pop, 0, gfx_rgba(1, 1, 1, d->alpha));
                break;
            }
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
            if (skinned(&skin_img.point)) {
                float rot = atan2f(dy, dx);
                for (float t = r * 1.2f; t < len - r * 1.2f; t += step)
                    sprite(&skin_img.point, x + dx / len * t, y + dy / len * t, pf_scale, rot, gfx_rgba(1, 1, 1, d->alpha));
                break;
            }
            for (float t = r * 1.2f; t < len - r * 1.2f; t += step)
                disc(x + dx / len * t, y + dy / len * t, 4, gfx_rgba(1, 1, 1, d->alpha * 0.6f), z);
            break;
        }

        case OSU5_DRAW_CURSOR: {
            float k = cursor_scale;
            if (skinned(&skin_img.cursor)) {
                sprite(&skin_img.cursor, x, y, pf_scale * k, 0, gfx_rgba(1, 1, 1, 1));
                break;
            }
            float z = gfx_next_z();
            disc(x, y, 22 * k, gfx_rgba(1, 0.85f, 0.4f, 0.25f), z);
            disc(x, y, 12 * k, gfx_rgba(1, 0.9f, 0.5f, 0.95f), gfx_next_z());
            ring(x, y, 13 * k, 3, gfx_rgba(1, 1, 1, 1), gfx_next_z());
            break;
        }
        }
    }
}

void draw_hud(const osu5_hud *hud, double time)
{
    (void)time;
    char buf[64];

    /* health; the skin's bar, colour part cropped to what's left */
    if (skin_img.bar_bg.tex && skin_img.bar_colour.tex) {
        const float s = 1.6f;
        sprite_cropped(&skin_img.bar_bg, 0, 0, s, 1, gfx_rgba(1, 1, 1, 1));
        sprite_cropped(&skin_img.bar_colour, 5 * s, 16 * s, s, hud->health, gfx_rgba(1, 1, 1, 1));
    } else {
        draw_rect(40, 30, 600, 16, gfx_rgba(0, 0, 0, 0.5f));
        draw_rect(40, 30, 600 * hud->health, 16, gfx_rgba(1, 1, 1, 0.9f));
    }

    snprintf(buf, sizeof(buf), "%08lld", (long long)hud->score);
    if (draw_skin_font(false)) {
        draw_skin_text(GFX_W - 30, 16, 1.4f, false, ALIGN_RIGHT, buf, 1);
        snprintf(buf, sizeof(buf), "%.2f%%", hud->accuracy * 100);
        draw_skin_text(GFX_W - 30, 100, 0.8f, false, ALIGN_RIGHT, buf, 1);
    } else {
        draw_text(GFX_W - 40, 24, 56, gfx_rgba(1, 1, 1, 1), ALIGN_RIGHT, buf);
        snprintf(buf, sizeof(buf), "%.2f%%", hud->accuracy * 100);
        draw_text(GFX_W - 40, 90, 32, gfx_rgba(1, 1, 1, 0.9f), ALIGN_RIGHT, buf);
    }

    snprintf(buf, sizeof(buf), "%dx", hud->combo);
    if (draw_skin_font(true))
        draw_skin_text(24, GFX_H - 120, 1.6f, true, ALIGN_LEFT, buf, 1);
    else
        draw_text(40, GFX_H - 100, 64, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, buf);

    /* song progress */
    draw_rect(0, GFX_H - 6, GFX_W, 6, gfx_rgba(0, 0, 0, 0.4f));
    draw_rect(0, GFX_H - 6, GFX_W * hud->progress, 6, gfx_rgba(1, 1, 1, 0.7f));

    if (hud->skip_target > 0) {
        draw_rect(GFX_W - 330, GFX_H - 140, 290, 70, gfx_rgba(0, 0, 0, 0.6f));
        draw_text(GFX_W - 185, GFX_H - 122, 32, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, "TRIANGLE SKIP");
    }
}
