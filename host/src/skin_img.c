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

#define MAX_TEXTURES 192

skin_images skin_img;

static int textures[MAX_TEXTURES];
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
    int tex = (w > 1 || h > 1) && ntextures < MAX_TEXTURES ? gfx_texture(w, h, px) : 0;
    stbi_image_free(px);
    if (tex)
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
    char first[200];
    snprintf(first, sizeof(first), "%s-0", name);
    if (!try_load(s, dir, first, 1))
        try_load(s, dir, first, 0);
}

static void font(skin_sprite *digits, const char *dir, const char *prefix)
{
    for (int i = 0; i < 10; i++) {
        char name[160];
        snprintf(name, sizeof(name), "%s-%d", prefix, i);
        sprite(&digits[i], dir, name);
    }
}

static void path_value(char *out, size_t n, const char *v)
{
    snprintf(out, n, "%s", v);
    for (char *c = out; *c; c++)
        *c = (char)(*c == '\\' ? '/' : tolower((unsigned char)*c));
}

static bool colour(const char *v, uint32_t *out)
{
    int r, g, b;
    if (sscanf(v, "%d , %d , %d", &r, &g, &b) != 3)
        return false;
    *out = 0xFF000000u | (uint32_t)(r & 255) << 16 | (uint32_t)(g & 255) << 8 | (uint32_t)(b & 255);
    return true;
}

typedef struct {
    char circle_prefix[128], score_prefix[128], combo_prefix[128];
} ini_fonts;

/* the skin.ini keys we use; their names don't clash across sections */
static void read_ini(const char *dir, ini_fonts *f)
{
    char path[512], line[256];
    snprintf(path, sizeof(path), "%s/skin.ini", dir);
    FILE *fp = fopen(path, "r");
    if (!fp)
        return;
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '/' && line[1] == '/')
            continue;
        char *v = strchr(line, ':');
        if (!v)
            continue;
        *v++ = 0;
        while (*v == ' ' || *v == '\t')
            v++;
        v[strcspn(v, "\r\n")] = 0;
        /* trailing comments and spaces */
        char *cm = strstr(v, "//");
        if (cm) *cm = 0;
        for (size_t l = strlen(v); l && (v[l - 1] == ' ' || v[l - 1] == '\t'); l--)
            v[l - 1] = 0;

        char *k = line;
        while (*k == ' ' || *k == '\t' || (unsigned char)*k == 0xEF || (unsigned char)*k == 0xBB || (unsigned char)*k == 0xBF)
            k++;
        for (size_t l = strlen(k); l && (k[l - 1] == ' ' || k[l - 1] == '\t'); l--)
            k[l - 1] = 0;

        if (!strcmp(k, "HitCirclePrefix") && *v)
            path_value(f->circle_prefix, sizeof(f->circle_prefix), v);
        else if (!strcmp(k, "ScorePrefix") && *v)
            path_value(f->score_prefix, sizeof(f->score_prefix), v);
        else if (!strcmp(k, "ComboPrefix") && *v)
            path_value(f->combo_prefix, sizeof(f->combo_prefix), v);
        else if (!strcmp(k, "HitCircleOverlap"))
            skin_img.overlap = (float)atof(v);
        else if (!strcmp(k, "ScoreOverlap"))
            skin_img.score_overlap = (float)atof(v);
        else if (!strcmp(k, "ComboOverlap"))
            skin_img.combo_overlap = (float)atof(v);
        else if (!strcmp(k, "SliderBorder"))
            skin_img.has_border = colour(v, &skin_img.border);
        else if (!strcmp(k, "SliderTrackOverride"))
            skin_img.has_track = colour(v, &skin_img.track);
        else if (!strncmp(k, "Combo", 5) && k[5] >= '1' && k[5] <= '8' && !k[6]) {
            uint32_t c;
            int i = k[5] - '1';
            if (colour(v, &c)) {
                skin_img.combo_colours[i] = c;
                if (i + 1 > skin_img.ncombo)
                    skin_img.ncombo = i + 1;
            }
        }
    }
    fclose(fp);

    /* Combo1..n with a hole in the middle: keep what's contiguous */
    for (int i = 0; i < skin_img.ncombo; i++) {
        if (!skin_img.combo_colours[i]) {
            skin_img.ncombo = i;
            break;
        }
    }
}

void skin_images_load(const char *dir)
{
    for (int i = 0; i < ntextures; i++)
        gfx_texture_free(textures[i]);
    ntextures = 0;
    memset(&skin_img, 0, sizeof(skin_img));
    if (!dir)
        return;

    ini_fonts f = {"default", "score", "score"};
    skin_img.overlap = -2;
    skin_img.score_overlap = 0;
    skin_img.combo_overlap = 0;
    read_ini(dir, &f);

    sprite(&skin_img.circle, dir, "hitcircle");
    sprite(&skin_img.overlay, dir, "hitcircleoverlay");
    sprite(&skin_img.approach, dir, "approachcircle");
    font(skin_img.digit, dir, f.circle_prefix);
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

    char name[200];
    font(skin_img.score, dir, f.score_prefix);
    snprintf(name, sizeof(name), "%s-comma", f.score_prefix);
    sprite(&skin_img.score_comma, dir, name);
    snprintf(name, sizeof(name), "%s-dot", f.score_prefix);
    sprite(&skin_img.score_dot, dir, name);
    snprintf(name, sizeof(name), "%s-percent", f.score_prefix);
    sprite(&skin_img.score_percent, dir, name);
    snprintf(name, sizeof(name), "%s-x", f.score_prefix);
    sprite(&skin_img.score_x, dir, name);

    if (!strcmp(f.combo_prefix, f.score_prefix)) {
        memcpy(skin_img.combo, skin_img.score, sizeof(skin_img.combo));
        skin_img.combo_x = skin_img.score_x;
    } else {
        font(skin_img.combo, dir, f.combo_prefix);
        snprintf(name, sizeof(name), "%s-x", f.combo_prefix);
        sprite(&skin_img.combo_x, dir, name);
    }

    sprite(&skin_img.bar_bg, dir, "scorebar-bg");
    sprite(&skin_img.bar_colour, dir, "scorebar-colour");

    sprite(&skin_img.sp_glow, dir, "spinner-glow");
    sprite(&skin_img.sp_bottom, dir, "spinner-bottom");
    sprite(&skin_img.sp_top, dir, "spinner-top");
    sprite(&skin_img.sp_middle, dir, "spinner-middle");
    sprite(&skin_img.sp_middle2, dir, "spinner-middle2");
    sprite(&skin_img.sp_circle, dir, "spinner-circle");
    sprite(&skin_img.sp_background, dir, "spinner-background");
    sprite(&skin_img.sp_metre, dir, "spinner-metre");
    sprite(&skin_img.sp_approach, dir, "spinner-approachcircle");
    sprite(&skin_img.sp_clear, dir, "spinner-clear");

    static const char *ranks[8] = {"d", "c", "b", "a", "s", "sh", "x", "xh"};
    for (int i = 0; i < 8; i++) {
        snprintf(name, sizeof(name), "ranking-%s", ranks[i]);
        sprite(&skin_img.rank[i], dir, name);
        snprintf(name, sizeof(name), "ranking-%s-small", ranks[i]);
        sprite(&skin_img.rank_small[i], dir, name);
    }

    /* a skin with no circle art at all isn't worth switching the look for */
    skin_img.on = skin_img.circle.tex != 0;
    LOG("skin: %s, %d textures, %d combo colours%s", dir, ntextures, skin_img.ncombo,
        skin_img.on ? "" : " (no hitcircle, drawn look kept)");
}
