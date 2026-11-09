/* osu! storyboards (https://osu.ppy.sh/wiki/Storyboard/Scripting): parsed from
 * the .osb and the map's [Events], evaluated each frame in storyboard space
 * (640x480, 854 wide for widescreen ones) and drawn as textured quads. */
#include "storyboard.h"
#include "draw.h"
#include "gfx.h"
#include "log.h"
#include "songs.h"

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "../third_party/stb_image.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_TEXTURES 640
#define TEXTURE_BUDGET (768u * 1024 * 1024) /* bytes of RGBA we're willing to upload */
#define SB_SCALE 2.25f                      /* 480 storyboard units to 1080 pixels */
#define SB_X 240.0f                         /* where x = 0 lands: (1920 - 640 * 2.25) / 2 */

enum { L_BACKGROUND, L_FAIL, L_PASS, L_FOREGROUND, L_OVERLAY };
enum { C_F, C_M, C_MX, C_MY, C_S, C_V, C_R, C_C, C_PH, C_PV, C_PA, C_COUNT };

typedef struct {
    uint8_t type, easing;
    double t0, t1;
    float a[3], b[3];
} command;

typedef struct {
    int layer, origin;
    float x, y;
    int frame0, nframes; /* into sb->frames */
    double delay;
    bool loop_once;
    command *cmds;
    int ncmds, cap;
    double start, end;
    bool has[C_COUNT];
} sprite;

typedef struct {
    char path[256];
    int tex;
    int w, h; /* original size, storyboard units */
} texture;

struct storyboard {
    sprite *sprites;
    int count, cap;
    texture tex[MAX_TEXTURES];
    int ntex;
    size_t bytes;
    int *frames;
    int nframes, frames_cap;
    char dir[512];
    char background[256];
    bool replaces_bg;
    /* variables from the .osb */
    char (*var_name)[64];
    char (*var_value)[256];
    int nvars;
};

/* ---- easing, osu!'s numbering ---- */

static double out_bounce(double t)
{
    if (t < 1 / 2.75) return 7.5625 * t * t;
    if (t < 2 / 2.75) { t -= 1.5 / 2.75; return 7.5625 * t * t + 0.75; }
    if (t < 2.5 / 2.75) { t -= 2.25 / 2.75; return 7.5625 * t * t + 0.9375; }
    t -= 2.625 / 2.75;
    return 7.5625 * t * t + 0.984375;
}

static double ease(int e, double t)
{
    const double pi = 3.14159265358979, s = 1.70158, s2 = s * 1.525;
    switch (e) {
    case 1: case 4: return t * (2 - t);
    case 2: case 3: return t * t;
    case 5: return t < 0.5 ? 2 * t * t : -1 + (4 - 2 * t) * t;
    case 6: return t * t * t;
    case 7: { double u = t - 1; return u * u * u + 1; }
    case 8: return t < 0.5 ? 4 * t * t * t : (t - 1) * (2 * t - 2) * (2 * t - 2) + 1;
    case 9: return t * t * t * t;
    case 10: { double u = t - 1; return 1 - u * u * u * u; }
    case 11: { if (t < 0.5) return 8 * t * t * t * t; double u = t - 1; return 1 - 8 * u * u * u * u; }
    case 12: return t * t * t * t * t;
    case 13: { double u = t - 1; return u * u * u * u * u + 1; }
    case 14: { if (t < 0.5) return 16 * t * t * t * t * t; double u = t - 1; return 1 + 16 * u * u * u * u * u; }
    case 15: return 1 - cos(t * pi / 2);
    case 16: return sin(t * pi / 2);
    case 17: return 0.5 - 0.5 * cos(pi * t);
    case 18: return t == 0 ? 0 : pow(2, 10 * (t - 1));
    case 19: return t == 1 ? 1 : 1 - pow(2, -10 * t);
    case 20:
        if (t == 0 || t == 1) return t;
        return t < 0.5 ? 0.5 * pow(2, 20 * t - 10) : 1 - 0.5 * pow(2, -20 * t + 10);
    case 21: return 1 - sqrt(1 - t * t);
    case 22: { double u = t - 1; return sqrt(1 - u * u); }
    case 23: return t < 0.5 ? 0.5 - 0.5 * sqrt(1 - 4 * t * t) : 0.5 + 0.5 * sqrt(1 - (2 * t - 2) * (2 * t - 2));
    case 24: return -pow(2, -10 + 10 * t) * sin((1 - 0.075 - t) * (2 * pi) / 0.3);
    case 25: return pow(2, -10 * t) * sin((t - 0.075) * (2 * pi) / 0.3) + 1;
    case 26: return pow(2, -10 * t) * sin((0.5 * t - 0.075) * (2 * pi) / 0.3) + 1;
    case 27: return pow(2, -10 * t) * sin((0.25 * t - 0.075) * (2 * pi) / 0.3) + 1;
    case 28:
        if (t < 0.5) return -0.5 * pow(2, -10 + 20 * t) * sin((1 - 0.1125 - 2 * t) * (2 * pi) / 0.45);
        return 0.5 * pow(2, 10 - 20 * t) * sin((2 * t - 0.1125) * (2 * pi) / 0.45) + 1;
    case 29: return t * t * ((s + 1) * t - s);
    case 30: { double u = t - 1; return u * u * ((s + 1) * u + s) + 1; }
    case 31:
        if (t < 0.5) return 0.5 * (4 * t * t * ((s2 + 1) * 2 * t - s2));
        { double u = 2 * t - 2; return 0.5 * (u * u * ((s2 + 1) * u + s2) + 2); }
    case 32: return 1 - out_bounce(1 - t);
    case 33: return out_bounce(t);
    case 34: return t < 0.5 ? 0.5 - 0.5 * out_bounce(1 - 2 * t) : 0.5 + 0.5 * out_bounce(2 * t - 1);
    default: return t;
    }
}

/* ---- textures ---- */

static void norm_path(char *out, size_t n, const char *in)
{
    while (*in == ' ' || *in == '"')
        in++;
    snprintf(out, n, "%s", in);
    for (char *c = out; *c; c++)
        if (*c == '\\')
            *c = '/';
    size_t l = strlen(out);
    while (l && (out[l - 1] == '"' || out[l - 1] == ' ' || out[l - 1] == '\r'))
        out[--l] = 0;
}

/* index into sb->tex, -1 if it won't load */
static int texture_for(storyboard *sb, const char *rel)
{
    for (int i = 0; i < sb->ntex; i++)
        if (!strcasecmp(sb->tex[i].path, rel))
            return sb->tex[i].tex ? i : -1;
    if (sb->ntex >= MAX_TEXTURES)
        return -1;

    texture *t = &sb->tex[sb->ntex++];
    snprintf(t->path, sizeof(t->path), "%s", rel);
    t->tex = 0;

    char full[1024];
    if (!songs_file(sb->dir, rel, full, sizeof(full)) || sb->bytes > TEXTURE_BUDGET)
        return -1;
    int w, h, n;
    unsigned char *px = stbi_load(full, &w, &h, &n, 4);
    if (!px)
        return -1;
    t->w = w;
    t->h = h;
    /* halve big ones, they're shown at storyboard size anyway */
    while (w > 2048 || h > 2048) {
        int nw = w / 2, nh = h / 2;
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++)
                memcpy(px + (y * nw + x) * 4, px + ((y * 2) * w + x * 2) * 4, 4);
        w = nw;
        h = nh;
    }
    t->tex = gfx_texture(w, h, px);
    stbi_image_free(px);
    sb->bytes += (size_t)w * h * 4;
    return t->tex ? sb->ntex - 1 : -1;
}

/* ---- parsing ---- */

static int split(char *line, char **f, int max)
{
    int n = 0;
    char *p = line;
    while (n < max) {
        f[n++] = p;
        char *c = strchr(p, ',');
        if (!c)
            break;
        *c = 0;
        p = c + 1;
    }
    return n;
}

static int layer_of(const char *s)
{
    static const char *names[] = {"Background", "Fail", "Pass", "Foreground", "Overlay"};
    for (int i = 0; i < 5; i++)
        if (!strcasecmp(s, names[i]))
            return i;
    return isdigit((unsigned char)*s) ? atoi(s) : -1;
}

/* origin as a fraction of the sprite, x then y */
static void origin_of(int o, float *fx, float *fy)
{
    static const float tab[10][2] = {
        {0, 0}, {0.5f, 0.5f}, {0, 0.5f}, {1, 0}, {0.5f, 1}, {0.5f, 0}, {0, 0}, {1, 0.5f}, {0, 1}, {1, 1},
    };
    if (o < 0 || o > 9)
        o = 1;
    *fx = tab[o][0];
    *fy = tab[o][1];
}

static int origin_index(const char *s)
{
    static const char *names[] = {"TopLeft", "Centre", "CentreLeft", "TopRight", "BottomCentre",
                                  "TopCentre", "Custom", "CentreRight", "BottomLeft", "BottomRight"};
    for (int i = 0; i < 10; i++)
        if (!strcasecmp(s, names[i]))
            return i;
    return isdigit((unsigned char)*s) ? atoi(s) : 1;
}

static void push_cmd(command **list, int *n, int *cap, const command *c)
{
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 8;
        command *grown = realloc(*list, sizeof(command) * (size_t)nc);
        if (!grown)
            return;
        *list = grown;
        *cap = nc;
    }
    (*list)[(*n)++] = *c;
}

/* one command line into its segments; offset shifts the times (loops) */
static void parse_command(char **f, int nf, command **list, int *n, int *cap)
{
    static const struct { const char *name; int type, vals; } kinds[] = {
        {"F", C_F, 1}, {"M", C_M, 2}, {"MX", C_MX, 1}, {"MY", C_MY, 1}, {"S", C_S, 1},
        {"V", C_V, 2}, {"R", C_R, 1}, {"C", C_C, 3}, {"P", -1, 1},
    };
    if (nf < 5)
        return;
    int k = -1;
    for (int i = 0; i < (int)(sizeof(kinds) / sizeof(kinds[0])); i++)
        if (!strcmp(f[0], kinds[i].name))
            k = i;
    if (k < 0)
        return;

    command c = {0};
    c.easing = (uint8_t)atoi(f[1]);
    double start = atof(f[2]);
    double end = f[3][0] ? atof(f[3]) : start;
    if (end < start)
        end = start;

    if (kinds[k].type < 0) {
        /* P: a flag on for the command's span (or for good when it's instant) */
        c.type = f[4][0] == 'H' ? C_PH : f[4][0] == 'V' ? C_PV : C_PA;
        c.t0 = start;
        c.t1 = end;
        push_cmd(list, n, cap, &c);
        return;
    }

    c.type = (uint8_t)kinds[k].type;
    int vals = kinds[k].vals, groups = (nf - 4) / vals;
    if (groups < 1)
        return;
    float scale = c.type == C_C ? 1.0f / 255 : 1;
    double dur = end - start;
    for (int g = 0; g < (groups > 1 ? groups - 1 : 1); g++) {
        int bg = groups > 1 ? g + 1 : g;
        for (int v = 0; v < vals; v++) {
            c.a[v] = (float)atof(f[4 + g * vals + v]) * scale;
            c.b[v] = (float)atof(f[4 + bg * vals + v]) * scale;
        }
        c.t0 = start + g * dur;
        c.t1 = end + g * dur;
        push_cmd(list, n, cap, &c);
    }
}

static sprite *add_sprite(storyboard *sb)
{
    if (sb->count == sb->cap) {
        int nc = sb->cap ? sb->cap * 2 : 64;
        sprite *grown = realloc(sb->sprites, sizeof(sprite) * (size_t)nc);
        if (!grown)
            return NULL;
        sb->sprites = grown;
        sb->cap = nc;
    }
    sprite *s = &sb->sprites[sb->count++];
    memset(s, 0, sizeof(*s));
    return s;
}

static void add_frame(storyboard *sb, int tex)
{
    if (sb->nframes == sb->frames_cap) {
        int nc = sb->frames_cap ? sb->frames_cap * 2 : 64;
        int *grown = realloc(sb->frames, sizeof(int) * (size_t)nc);
        if (!grown)
            return;
        sb->frames = grown;
        sb->frames_cap = nc;
    }
    sb->frames[sb->nframes++] = tex;
}

static void substitute(const storyboard *sb, char *line, size_t cap)
{
    if (!strchr(line, '$'))
        return;
    for (int v = 0; v < sb->nvars; v++) {
        char *at;
        size_t nl = strlen(sb->var_name[v]), vl = strlen(sb->var_value[v]);
        while ((at = strstr(line, sb->var_name[v]))) {
            size_t rest = strlen(at + nl);
            if ((size_t)(at - line) + vl + rest + 1 > cap)
                break;
            memmove(at + vl, at + nl, rest + 1);
            memcpy(at, sb->var_value[v], vl);
        }
    }
}

typedef struct {
    sprite *cur;
    bool in_loop, in_trigger;
    double loop_start;
    int loop_count;
    command *loop;
    int nloop, loop_cap;
} parse_state;

/* a loop's commands, repeated count times after its start */
static void close_loop(parse_state *ps)
{
    if (ps->in_loop && ps->cur && ps->nloop > 0) {
        double len = 0;
        for (int i = 0; i < ps->nloop; i++)
            len = fmax(len, ps->loop[i].t1);
        int count = ps->loop_count < 1 ? 1 : ps->loop_count;
        for (int r = 0; r < count; r++) {
            for (int i = 0; i < ps->nloop; i++) {
                command c = ps->loop[i];
                c.t0 += ps->loop_start + r * len;
                c.t1 += ps->loop_start + r * len;
                push_cmd(&ps->cur->cmds, &ps->cur->ncmds, &ps->cur->cap, &c);
            }
        }
    }
    ps->in_loop = ps->in_trigger = false;
    ps->nloop = 0;
}

static void parse_line(storyboard *sb, parse_state *ps, char *raw)
{
    char line[2048];
    snprintf(line, sizeof(line), "%s", raw);
    line[strcspn(line, "\r\n")] = 0;
    if (!line[0] || (line[0] == '/' && line[1] == '/'))
        return;
    substitute(sb, line, sizeof(line));

    int depth = 0;
    while (line[depth] == ' ' || line[depth] == '_')
        depth++;
    char *f[64];
    int nf = split(line + depth, f, 64);

    if (depth == 0) {
        close_loop(ps);
        ps->cur = NULL;
        bool anim = !strcmp(f[0], "Animation") || !strcmp(f[0], "6");
        if (!anim && strcmp(f[0], "Sprite") && strcmp(f[0], "4"))
            return; /* backgrounds, videos, samples, breaks */
        if (nf < (anim ? 8 : 6))
            return;
        int layer = layer_of(f[1]);
        if (layer < 0 || layer > L_OVERLAY || layer == L_FAIL)
            return;
        char rel[256];
        norm_path(rel, sizeof(rel), f[3]);

        sprite *s = add_sprite(sb);
        if (!s)
            return;
        s->layer = layer;
        s->origin = origin_index(f[2]);
        s->x = (float)atof(f[4]);
        s->y = (float)atof(f[5]);
        s->frame0 = sb->nframes;
        if (anim) {
            int count = atoi(f[6]);
            s->delay = atof(f[7]);
            s->loop_once = nf > 8 && (!strcasecmp(f[8], "LoopOnce") || !strcmp(f[8], "1"));
            /* name.png -> name0.png, name1.png ... */
            char *dot = strrchr(rel, '.');
            for (int i = 0; i < count && i < 256; i++) {
                char frame[300];
                if (dot)
                    snprintf(frame, sizeof(frame), "%.*s%d%s", (int)(dot - rel), rel, i, dot);
                else
                    snprintf(frame, sizeof(frame), "%s%d", rel, i);
                add_frame(sb, texture_for(sb, frame));
            }
        } else {
            add_frame(sb, texture_for(sb, rel));
            if (!strcasecmp(rel, sb->background))
                sb->replaces_bg = true;
        }
        s->nframes = sb->nframes - s->frame0;
        ps->cur = s;
        return;
    }

    if (!ps->cur)
        return;
    if (depth == 1) {
        close_loop(ps);
        if (!strcmp(f[0], "L") && nf >= 3) {
            ps->in_loop = true;
            ps->loop_start = atof(f[1]);
            ps->loop_count = atoi(f[2]);
        } else if (!strcmp(f[0], "T")) {
            ps->in_trigger = true; /* their commands only run on hitsounds/fails; skipped */
        } else {
            parse_command(f, nf, &ps->cur->cmds, &ps->cur->ncmds, &ps->cur->cap);
        }
    } else if (ps->in_loop) {
        parse_command(f, nf, &ps->loop, &ps->nloop, &ps->loop_cap);
    }
}

static void parse_file(storyboard *sb, const char *path, bool osb)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return;
    char line[2048];
    enum { OTHER, EVENTS, VARIABLES } section = OTHER;
    parse_state ps = {0};
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '[') {
            close_loop(&ps);
            ps.cur = NULL;
            section = !strncmp(line, "[Events]", 8) ? EVENTS : !strncmp(line, "[Variables]", 11) ? VARIABLES : OTHER;
            continue;
        }
        if (section == VARIABLES && osb && line[0] == '$') {
            char *eq = strchr(line, '=');
            if (eq && sb->nvars < 256) {
                *eq = 0;
                snprintf(sb->var_name[sb->nvars], 64, "%s", line);
                snprintf(sb->var_value[sb->nvars], 256, "%s", eq + 1);
                sb->var_value[sb->nvars][strcspn(sb->var_value[sb->nvars], "\r\n")] = 0;
                sb->nvars++;
            }
        } else if (section == EVENTS) {
            parse_line(sb, &ps, line);
        }
    }
    close_loop(&ps);
    free(ps.loop);
    fclose(fp);
}

static int by_start(const void *a, const void *b)
{
    const command *x = a, *y = b;
    return x->t0 < y->t0 ? -1 : x->t0 > y->t0;
}

storyboard *sb_load(const char *dir, const char *osu_path, const char *background)
{
    storyboard *sb = calloc(1, sizeof(*sb));
    if (!sb)
        return NULL;
    sb->var_name = calloc(256, 64);
    sb->var_value = calloc(256, 256);
    snprintf(sb->dir, sizeof(sb->dir), "%s", dir);
    norm_path(sb->background, sizeof(sb->background), background ? background : "");

    /* the set's .osb first (it's shared by every difficulty), then the map's own events */
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            size_t l = strlen(e->d_name);
            if (l > 4 && !strcasecmp(e->d_name + l - 4, ".osb")) {
                char path[1024];
                snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
                parse_file(sb, path, true);
                break;
            }
        }
        closedir(d);
    }
    parse_file(sb, osu_path, false);

    /* lifetimes, and which properties each sprite animates */
    int kept = 0;
    for (int i = 0; i < sb->count; i++) {
        sprite *s = &sb->sprites[i];
        bool any_tex = false;
        for (int k = 0; k < s->nframes; k++)
            any_tex |= sb->frames[s->frame0 + k] >= 0;
        if (!s->ncmds || !any_tex) {
            free(s->cmds);
            continue;
        }
        qsort(s->cmds, (size_t)s->ncmds, sizeof(command), by_start);
        s->start = 1e30;
        s->end = -1e30;
        for (int k = 0; k < s->ncmds; k++) {
            s->start = fmin(s->start, s->cmds[k].t0);
            s->end = fmax(s->end, s->cmds[k].t1);
            s->has[s->cmds[k].type] = true;
        }
        sb->sprites[kept++] = *s;
    }
    sb->count = kept;

    LOG("storyboard: %d sprites, %d textures, %.0f MB", sb->count, sb->ntex, sb->bytes / 1048576.0);
    if (sb->count == 0) {
        sb_free(sb);
        return NULL;
    }
    return sb;
}

/* value of one property at t: the running command, else the last one that
 * finished, else the first one's start value */
static bool value_at(const sprite *s, int type, double t, float *out, int n)
{
    if (!s->has[type])
        return false;
    const command *last = NULL, *first = NULL;
    for (int i = 0; i < s->ncmds; i++) {
        const command *c = &s->cmds[i];
        if (c->type != type)
            continue;
        if (!first)
            first = c;
        if (c->t0 > t)
            break;
        last = c;
    }
    if (!last) {
        memcpy(out, first->a, sizeof(float) * (size_t)n);
        return true;
    }
    if (t >= last->t1 || last->t1 <= last->t0) {
        memcpy(out, last->b, sizeof(float) * (size_t)n);
        return true;
    }
    double k = ease(last->easing, (t - last->t0) / (last->t1 - last->t0));
    for (int v = 0; v < n; v++)
        out[v] = (float)(last->a[v] + (last->b[v] - last->a[v]) * k);
    return true;
}

/* P flags: on while a command runs, and for good after an instant one */
static bool flag_at(const sprite *s, int type, double t)
{
    if (!s->has[type])
        return false;
    for (int i = 0; i < s->ncmds; i++) {
        const command *c = &s->cmds[i];
        if (c->type == type && t >= c->t0 && (t <= c->t1 || c->t0 == c->t1))
            return true;
    }
    return false;
}

static void draw_sprite(storyboard *sb, const sprite *s, double t, float dim)
{
    float v[3];
    float alpha = value_at(s, C_F, t, v, 1) ? v[0] : 1;
    if (alpha <= 0.002f)
        return;

    float x = s->x, y = s->y;
    if (value_at(s, C_M, t, v, 2)) { x = v[0]; y = v[1]; }
    if (value_at(s, C_MX, t, v, 1)) x = v[0];
    if (value_at(s, C_MY, t, v, 1)) y = v[0];
    float sx = 1, sy = 1;
    if (value_at(s, C_S, t, v, 1)) sx = sy = v[0];
    if (value_at(s, C_V, t, v, 2)) { sx *= v[0]; sy *= v[1]; }
    float rot = value_at(s, C_R, t, v, 1) ? v[0] : 0;
    float r = 1, g = 1, b = 1;
    if (value_at(s, C_C, t, v, 3)) { r = v[0]; g = v[1]; b = v[2]; }
    bool fh = flag_at(s, C_PH, t), fv = flag_at(s, C_PV, t);
    if (sx < 0) { fh = !fh; sx = -sx; }
    if (sy < 0) { fv = !fv; sy = -sy; }
    if (sx == 0 || sy == 0)
        return;

    int frame = 0;
    if (s->nframes > 1 && s->delay > 0) {
        frame = (int)((t - s->start) / s->delay);
        frame = s->loop_once ? (frame >= s->nframes ? s->nframes - 1 : frame) : frame % s->nframes;
        if (frame < 0)
            frame = 0;
    }
    int ti = sb->frames[s->frame0 + frame];
    if (ti < 0)
        return;
    const texture *tex = &sb->tex[ti];

    float w = tex->w * sx * SB_SCALE, h = tex->h * sy * SB_SCALE;
    float ox, oy;
    origin_of(s->origin, &ox, &oy);
    if (fh) ox = 1 - ox;
    if (fv) oy = 1 - oy;
    /* the origin stays put, the quad's centre turns around it */
    float dx = (0.5f - ox) * w, dy = (0.5f - oy) * h;
    float px = SB_X + x * SB_SCALE, py = y * SB_SCALE;
    float cx = px + dx * cosf(rot) - dy * sinf(rot), cy = py + dx * sinf(rot) + dy * cosf(rot);

    float u0 = fh ? 1 : 0, u1 = fh ? 0 : 1, v0 = fv ? 1 : 0, v1 = fv ? 0 : 1;
    float lit = 1 - dim;
    gfx_bind(tex->tex);
    gfx_push(&(gfx_inst){cx, cy, w / 2, h / 2, rot, gfx_next_z(), gfx_rgba(r * lit, g * lit, b * lit, alpha), GK_TEX, {u0, v0, u1, v1}});
}

void sb_draw(storyboard *sb, double t, int which, float dim)
{
    static const int under[] = {L_BACKGROUND, L_PASS, L_FOREGROUND}, over[] = {L_OVERLAY};
    const int *layers = which == SB_OVER ? over : under;
    int nl = which == SB_OVER ? 1 : 3;
    for (int l = 0; l < nl; l++)
        for (int i = 0; i < sb->count; i++) {
            const sprite *s = &sb->sprites[i];
            if (s->layer == layers[l] && t >= s->start && t <= s->end)
                draw_sprite(sb, s, t, dim);
        }
    gfx_bind(0);
}

bool sb_replaces_bg(const storyboard *sb) { return sb && sb->replaces_bg; }

void sb_free(storyboard *sb)
{
    if (!sb)
        return;
    for (int i = 0; i < sb->ntex; i++)
        if (sb->tex[i].tex)
            gfx_texture_free(sb->tex[i].tex);
    for (int i = 0; i < sb->count; i++)
        free(sb->sprites[i].cmds);
    free(sb->sprites);
    free(sb->frames);
    free(sb->var_name);
    free(sb->var_value);
    free(sb);
}
