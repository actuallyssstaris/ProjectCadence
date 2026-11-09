#ifndef OSU5_GFX_H
#define OSU5_GFX_H

#include <stdbool.h>
#include <stdint.h>

#define GFX_W 1920
#define GFX_H 1080

enum {
    GK_RECT = 0,
    GK_TEX = 1,
    GK_DISC = 2,
    GK_RING = 3,
    GK_CAPSULE = 4,
    GK_HITCIRCLE = 5,
    GK_ARROW = 6,
    GK_GLYPH = 7,
};

typedef struct {
    float x, y, w, h;  /* centre + half extents; capsule: start point, radius in w */
    float rot, z;
    uint32_t colour;   /* rgba8, r in the low byte (unpackUnorm4x8 order) */
    uint32_t kind;
    float p[4];        /* kind specific: uv rect, ring thickness, capsule end point */
} gfx_inst;

bool gfx_init(void);

void gfx_begin(float r, float g, float b);
void gfx_end(void);

/* rgba8 pixels; returns a handle, 0 is the built-in white texture */
int gfx_texture(int w, int h, const void *rgba);
int gfx_texture_r8(int w, int h, const void *r8);
void gfx_texture_free(int id);

void gfx_bind(int tex);
void gfx_push(const gfx_inst *in);

/* painter order through the depth buffer: every call hands out a nearer z.
 * Things that must not blend over themselves (slider bodies) share one z. */
float gfx_next_z(void);

static inline uint32_t gfx_rgba(float r, float g, float b, float a)
{
    uint32_t R = (uint32_t)(r * 255 + 0.5f), G = (uint32_t)(g * 255 + 0.5f);
    uint32_t B = (uint32_t)(b * 255 + 0.5f), A = (uint32_t)(a * 255 + 0.5f);
    return R | G << 8 | B << 16 | A << 24;
}

/* 0xAARRGGBB from the core, alpha multiplied in */
static inline uint32_t gfx_argb(uint32_t c, float alpha)
{
    float a = ((c >> 24) & 0xFF) / 255.0f * alpha;
    if (a < 0) a = 0;
    if (a > 1) a = 1;
    return ((c >> 16) & 0xFF) | (c & 0xFF00) | (c & 0xFF) << 16 | (uint32_t)(a * 255 + 0.5f) << 24;
}

#endif
