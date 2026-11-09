#include "skin_img.h"
#include "gfx.h"
#include "log.h"

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "../third_party/stb_image.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

skin_images skin_img;

static int textures[64];
static int ntextures;

static bool try_load(skin_sprite *s, const char *dir, const char *name, int hd)
{
    char path[512];
    snprintf(path, sizeof(path), hd ? "%s/%s@2x.png" : "%s/%s.png", dir, name);
    int w, h, n;
    unsigned char *px = stbi_load(path, &w, &h, &n, 4);
    if (!px)
        return false;
    /* a 1x1 png is how skinners blank an element out */
    int tex = w > 1 || h > 1 ? gfx_texture(w, h, px) : 0;
    stbi_image_free(px);
    if (tex && ntextures < 64)
        textures[ntextures++] = tex;
    s->tex = tex;
    s->w = hd ? w / 2.0f : (float)w;
    s->h = hd ? h / 2.0f : (float)h;
    return true;
}

static void sprite(skin_sprite *s, const char *dir, const char *name)
{
    if (try_load(s, dir, name, 1) || try_load(s, dir, name, 0))
        return;
    /* animated elements: take the first frame */
    char first[96];
    snprintf(first, sizeof(first), "%s-0", name);
    if (!try_load(s, dir, first, 1))
        try_load(s, dir, first, 0);
}

/* the [Fonts] bits of skin.ini we use; names were lowercased on unpack */
static void read_ini(const char *dir, char *prefix, size_t plen, float *overlap)
{
    char path[512], line[256];
    snprintf(path, sizeof(path), "%s/skin.ini", dir);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        char *v = strchr(line, ':');
        if (!v)
            continue;
        *v++ = 0;
        while (*v == ' ' || *v == '\t')
            v++;
        v[strcspn(v, "\r\n")] = 0;
        char *k = line;
        while (*k == ' ' || *k == '\t' || (unsigned char)*k == 0xEF || (unsigned char)*k == 0xBB || (unsigned char)*k == 0xBF)
            k++;
        if (!strcmp(k, "HitCirclePrefix") && *v) {
            snprintf(prefix, plen, "%s", v);
            for (char *c = prefix; *c; c++)
                *c = (char)(*c == '\\' ? '/' : tolower((unsigned char)*c));
        } else if (!strcmp(k, "HitCircleOverlap")) {
            *overlap = (float)atof(v);
        }
    }
    fclose(f);
}

void skin_images_load(const char *dir)
{
    for (int i = 0; i < ntextures; i++)
        gfx_texture_free(textures[i]);
    ntextures = 0;
    memset(&skin_img, 0, sizeof(skin_img));
    if (!dir)
        return;

    char prefix[128] = "default";
    skin_img.overlap = -2;
    read_ini(dir, prefix, sizeof(prefix), &skin_img.overlap);

    sprite(&skin_img.circle, dir, "hitcircle");
    sprite(&skin_img.overlay, dir, "hitcircleoverlay");
    sprite(&skin_img.approach, dir, "approachcircle");
    for (int i = 0; i < 10; i++) {
        char name[160];
        snprintf(name, sizeof(name), "%s-%d", prefix, i);
        sprite(&skin_img.digit[i], dir, name);
    }
    sprite(&skin_img.cursor, dir, "cursor");
    sprite(&skin_img.ball, dir, "sliderb");
    if (!skin_img.ball.tex)
        sprite(&skin_img.ball, dir, "sliderb0");
    sprite(&skin_img.follow, dir, "sliderfollowcircle");
    sprite(&skin_img.reverse, dir, "reversearrow");
    sprite(&skin_img.tick, dir, "sliderscorepoint");
    sprite(&skin_img.point, dir, "followpoint");
    sprite(&skin_img.hit0, dir, "hit0");
    sprite(&skin_img.hit50, dir, "hit50");
    sprite(&skin_img.hit100, dir, "hit100");
    sprite(&skin_img.hit300, dir, "hit300");

    /* a skin with no circle art at all isn't worth switching the look for */
    skin_img.on = skin_img.circle.tex != 0;
    LOG("skin: %s, %d textures%s", dir, ntextures, skin_img.on ? "" : " (no hitcircle, drawn look kept)");
}
