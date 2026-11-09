/*
 * osu5 - osu! for PS5 homebrew.
 * Gameplay comes from the C# core (core/, NativeAOT); this file owns the
 * console side: screens, clock, input, sound.
 */
#include "../include/osu5.h"
#include "audio.h"
#include "draw.h"
#include "gfx.h"
#include "input.h"
#include "log.h"
#include "mapdl.h"
#include "mapdl_ui.h"
#include "preview.h"
#include "sce.h"
#include "scores.h"
#include "skin.h"
#include "skin_img.h"
#include "skin_ui.h"
#include "skins.h"
#include "songs.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_WRITE
#include "../third_party/stb_image.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DATA_DIR "/data/osu5"
#define SONGS_DIR DATA_DIR "/Songs"
#define SKIN_DIR DATA_DIR "/skin"
#define SETTINGS_FILE DATA_DIR "/settings.txt"

#define MAX_CMDS 4096
#define MAX_POINTS 32768
#define SHOWN_SCORES 5

typedef enum {
    SCREEN_SELECT,
    SCREEN_PLAY,
    SCREEN_PAUSE,
    SCREEN_FAILED,
    SCREEN_RESULTS,
    SCREEN_SKINS,
    SCREEN_MODS,
    SCREEN_SETTINGS,
    SCREEN_DOWNLOAD,
} screen;

static struct {
    screen scr;
    song_list songs;
    int sel;
    float sel_scroll;
    double sel_changed;   /* wall ms, previews wait for the selection to settle */
    int previewed;        /* song index the preview was asked for, -1 none */
    int mods;

    int offset_ms;        /* added to the audio clock, positive = notes later */
    float sensitivity;
    float music_vol, fx_vol;
    float bg_dim;         /* during play */
    float cursor_scale;

    int bg_tex;
    int bg_w, bg_h;
    char bg_path[768];

    double play_time;
    double fail_time;
    double finish_seen;
    osu5_hud last_hud;
    play_input cursor;
    const song_entry *playing;

    score_rec scores[SHOWN_SCORES];
    int nscores;
    int scores_for;       /* song index the list above belongs to */
    int new_place;        /* where the last result landed, -1 not saved */

    int mod_sel;
    int set_sel;
    int seen_installs;
    skin_job topup;       /* default skin from an older version, fetched quietly */

    char status[256];
} app = {.previewed = -1, .scores_for = -1, .new_place = -1};

static osu5_draw cmds[MAX_CMDS];
static float points[MAX_POINTS * 2];

static double wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void notify(const char *msg)
{
    OrbisNotificationRequest req;
    memset(&req, 0, sizeof(req));
    snprintf(req.message, sizeof(req.message), "%s", msg);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

/* ---- settings ---- */

static void settings_load(void)
{
    app.offset_ms = 0;
    app.sensitivity = 1.0f;
    app.music_vol = 0.7f;
    app.fx_vol = 0.8f;
    app.bg_dim = 0.8f;
    app.cursor_scale = 1.0f;

    FILE *f = fopen(SETTINGS_FILE, "r");
    if (f) {
        char key[64];
        float v;
        while (fscanf(f, "%63s %f", key, &v) == 2) {
            if (!strcmp(key, "offset")) app.offset_ms = (int)v;
            else if (!strcmp(key, "sensitivity")) app.sensitivity = v;
            else if (!strcmp(key, "music")) app.music_vol = v;
            else if (!strcmp(key, "effects")) app.fx_vol = v;
            else if (!strcmp(key, "dim")) app.bg_dim = v;
            else if (!strcmp(key, "cursor")) app.cursor_scale = v;
            else if (!strcmp(key, "mods")) app.mods = (int)v & ~OSU5_MOD_AUTO;
        }
        fclose(f);
    }
    draw_set_cursor_scale(app.cursor_scale);
}

static void settings_save(void)
{
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f)
        return;
    fprintf(f, "offset %d\nsensitivity %.2f\nmusic %.2f\neffects %.2f\ndim %.2f\ncursor %.2f\nmods %d\n", app.offset_ms,
            app.sensitivity, app.music_vol, app.fx_vol, app.bg_dim, app.cursor_scale, app.mods & ~OSU5_MOD_AUTO);
    fclose(f);
}

/* ---- backgrounds ---- */

static void set_background(unsigned char *px, int w, int h, const char *path)
{
    if (app.bg_tex) {
        gfx_texture_free(app.bg_tex);
        app.bg_tex = 0;
    }
    snprintf(app.bg_path, sizeof(app.bg_path), "%s", path);
    if (!px)
        return;
    app.bg_tex = gfx_texture(w, h, px);
    app.bg_w = w;
    app.bg_h = h;
}

/* synchronous, for starting a map; song select gets them from the preview thread */
static void load_background(const char *path)
{
    if (!strcmp(path, app.bg_path) && app.bg_tex)
        return;
    int w, h, n;
    unsigned char *px = path[0] ? stbi_load(path, &w, &h, &n, 4) : NULL;
    while (px && (w > 2048 || h > 2048)) {
        int nw = w / 2, nh = h / 2;
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++)
                memcpy(px + (y * nw + x) * 4, px + ((y * 2) * w + x * 2) * 4, 4);
        w = nw;
        h = nh;
    }
    set_background(px, w, h, path);
    if (px)
        stbi_image_free(px);
}

static void draw_background(float dim)
{
    if (!app.bg_tex)
        return;
    /* cover the screen, crop the overflow */
    float s = fmaxf((float)GFX_W / app.bg_w, (float)GFX_H / app.bg_h);
    float w = app.bg_w * s, h = app.bg_h * s;
    draw_image(app.bg_tex, (GFX_W - w) / 2, (GFX_H - h) / 2, w, h, gfx_rgba(1 - dim, 1 - dim, 1 - dim, 1));
}

/* ---- mods ---- */

static const struct {
    int bit;
    const char *tag, *name, *what;
} mod_list[] = {
    {OSU5_MOD_EASY, "EZ", "Easy", "bigger circles, more forgiving"},
    {OSU5_MOD_NOFAIL, "NF", "No Fail", "you can't fail"},
    {OSU5_MOD_HALFTIME, "HT", "Half Time", "0.75x speed (pitch drops too)"},
    {OSU5_MOD_HARDROCK, "HR", "Hard Rock", "smaller circles, mirrored map"},
    {OSU5_MOD_HIDDEN, "HD", "Hidden", "circles fade out before you hit them"},
    {OSU5_MOD_DOUBLETIME, "DT", "Double Time", "1.5x speed (pitch rises too)"},
    {OSU5_MOD_AUTO, "AT", "Auto", "watch a perfect play"},
};
#define MOD_COUNT (int)(sizeof(mod_list) / sizeof(mod_list[0]))

static double mod_rate(int mods)
{
    return mods & OSU5_MOD_DOUBLETIME ? 1.5 : mods & OSU5_MOD_HALFTIME ? 0.75 : 1.0;
}

/* same numbers as ModRules.ScoreMultiplier in the core */
static double mod_multiplier(int mods)
{
    double m = 1;
    if (mods & OSU5_MOD_NOFAIL) m *= 0.5;
    if (mods & OSU5_MOD_EASY) m *= 0.5;
    if (mods & OSU5_MOD_HALFTIME) m *= 0.3;
    if (mods & OSU5_MOD_HIDDEN) m *= 1.06;
    if (mods & OSU5_MOD_HARDROCK) m *= 1.06;
    if (mods & OSU5_MOD_DOUBLETIME) m *= 1.12;
    return m;
}

static void mod_tags(int mods, char *out, size_t n)
{
    out[0] = 0;
    for (int i = 0; i < MOD_COUNT; i++) {
        if (mods & mod_list[i].bit) {
            size_t l = strlen(out);
            snprintf(out + l, n - l, "%s%s", l ? " " : "", mod_list[i].tag);
        }
    }
}

static void toggle_mod(int bit)
{
    app.mods ^= bit;
    if (!(app.mods & bit))
        return;
    /* pairs that cancel each other out */
    if (bit == OSU5_MOD_EASY) app.mods &= ~OSU5_MOD_HARDROCK;
    if (bit == OSU5_MOD_HARDROCK) app.mods &= ~OSU5_MOD_EASY;
    if (bit == OSU5_MOD_DOUBLETIME) app.mods &= ~OSU5_MOD_HALFTIME;
    if (bit == OSU5_MOD_HALFTIME) app.mods &= ~OSU5_MOD_DOUBLETIME;
}

/* ---- playing ---- */

static void refresh_scores(void)
{
    if (app.scores_for == app.sel || app.songs.count == 0)
        return;
    app.scores_for = app.sel;
    app.nscores = scores_for(app.songs.items[app.sel].file, app.scores, SHOWN_SCORES);
}

static bool start_song(const song_entry *e)
{
    snprintf(app.status, sizeof(app.status), "loading...");
    preview_cancel();

    osu5_set_mods(app.mods);
    if (osu5_load(e->file) != 0) {
        char err[200];
        osu5_string(OSU5_STR_ERROR, err, sizeof(err));
        snprintf(app.status, sizeof(app.status), "couldn't load map: %s", err);
        LOG("%s", app.status);
        app.previewed = -1;
        return false;
    }

    char path[768];
    sound_buf music;
    if (!songs_file(e->dir, e->info.audio, path, sizeof(path)) || !audio_decode_file(path, &music)) {
        snprintf(app.status, sizeof(app.status), "couldn't decode %s", e->info.audio);
        LOG("%s", app.status);
        osu5_unload();
        app.previewed = -1;
        return false;
    }

    if (songs_file(e->dir, e->info.background, path, sizeof(path)))
        load_background(path);

    audio_music_set(&music);
    audio_music_volume(app.music_vol);
    audio_music_rate(mod_rate(app.mods));
    audio_music_play(osu5_start_time());
    LOG("play %s from %.0f ms, mods %d", e->file, osu5_start_time(), app.mods);

    app.playing = e;
    app.cursor.x = 256;
    app.cursor.y = 192;
    app.finish_seen = 0;
    app.new_place = -1;
    app.status[0] = 0;
    app.scr = SCREEN_PLAY;
    return true;
}

static void back_to_select(void)
{
    audio_music_stop();
    audio_music_rate(1);
    audio_music_volume(app.music_vol);
    osu5_unload();
    app.previewed = -1; /* get the preview going again */
    app.sel_changed = 0;
    app.scores_for = -1;
    app.scr = SCREEN_SELECT;
}

static double game_time(void) { return audio_music_time() + app.offset_ms; }

static void play_frame(const pad_state *pad, float dt)
{
    double t = game_time();
    app.play_time = t;

    if (pad->pressed & PAD_OPTIONS) {
        audio_music_pause(true);
        app.scr = SCREEN_PAUSE;
        return;
    }

    input_to_play(pad, &app.cursor, dt, app.sensitivity);
    osu5_update(t, app.cursor.x, app.cursor.y, app.cursor.keys);

    osu5_sound ev;
    while (osu5_poll_sound(&ev))
        skin_play(&ev, app.fx_vol);

    osu5_get_hud(t, &app.last_hud);

    static double next_log;
    if (wall_ms() >= next_log) {
        next_log = wall_ms() + 2000;
        LOG("t %.0f state %d combo %d score %lld keys %d at %.0f,%.0f", t, app.last_hud.state, app.last_hud.combo,
            (long long)app.last_hud.score, app.cursor.keys, app.cursor.x, app.cursor.y);
    }

    if (pad->pressed & PAD_TRIANGLE && app.last_hud.skip_target > 0)
        audio_music_seek(app.last_hud.skip_target - app.offset_ms);

    if (app.last_hud.state == OSU5_STATE_FAILED) {
        app.fail_time = wall_ms();
        app.scr = SCREEN_FAILED;
    } else if (app.last_hud.state == OSU5_STATE_FINISHED) {
        if (app.finish_seen == 0) {
            app.finish_seen = wall_ms();
        } else if (wall_ms() - app.finish_seen > 1500) {
            if (!(app.mods & OSU5_MOD_AUTO) && app.playing)
                app.new_place = scores_add(app.playing->file, &app.last_hud, app.mods);
            app.scr = SCREEN_RESULTS;
        }
    }
}

static void render_play(void)
{
    double t = app.play_time;
    int np = 0;
    int n = osu5_frame(t, cmds, MAX_CMDS, points, MAX_POINTS, &np);

    draw_background(app.last_hud.state == OSU5_STATE_BREAK ? app.bg_dim * 0.6f : app.bg_dim);
    draw_gameplay(cmds, n, points, np);
    draw_hud(&app.last_hud, t);
}

/* ---- song select ---- */

static void rescan(void)
{
    char keep[768] = "";
    if (app.songs.count > 0)
        snprintf(keep, sizeof(keep), "%s", app.songs.items[app.sel].file);
    songs_scan(SONGS_DIR, &app.songs);
    int at = keep[0] ? songs_find(&app.songs, keep) : -1;
    app.sel = at >= 0 ? at : 0;
    app.sel_scroll = (float)app.sel;
    app.previewed = -1;
    app.scores_for = -1;
    LOG("%d maps", app.songs.count);
}

/* a set is a run of difficulties from one folder (the scan sorts them together) */
static bool same_set(int a, int b)
{
    return !strcmp(app.songs.items[a].dir, app.songs.items[b].dir);
}

static int set_start(int i)
{
    while (i > 0 && same_set(i - 1, i))
        i--;
    return i;
}

/* first difficulty of the next set, or count */
static int set_end(int i)
{
    while (i < app.songs.count - 1 && same_set(i + 1, i))
        i++;
    return i + 1;
}

/* entering a set lands on the difficulty nearest the one you were on, the way
 * osu! does it (with stars there; CS+AR+OD here until we have star rating) */
static int closest_in_set(int start, int from)
{
    const osu5_peek_info *f = &app.songs.items[from].info;
    float want = f->cs + f->ar + f->od, best = 1e9f;
    int pick = start;
    for (int i = start; i < set_end(start); i++) {
        const osu5_peek_info *in = &app.songs.items[i].info;
        float d = fabsf(in->cs + in->ar + in->od - want);
        if (d < best) {
            best = d;
            pick = i;
        }
    }
    return pick;
}

static void select_frame(const pad_state *pad)
{
    if (pad->pressed & PAD_TOUCHPAD) {
        skin_ui_open(false);
        app.scr = SCREEN_SKINS;
        return;
    }
    if (pad->pressed & PAD_TRIANGLE) {
        mapdl_ui_open();
        app.scr = SCREEN_DOWNLOAD;
        return;
    }
    if (pad->pressed & PAD_OPTIONS) {
        app.set_sel = 0;
        app.scr = SCREEN_SETTINGS;
        return;
    }
    if (pad->pressed & PAD_SQUARE) {
        app.mod_sel = 0;
        app.scr = SCREEN_MODS;
        return;
    }

    /* maps that finished downloading while we were here */
    mapdl_status dl;
    mapdl_progress(&dl);
    if (dl.installed != app.seen_installs && !dl.current) {
        app.seen_installs = dl.installed;
        rescan();
    }

    if (app.songs.count == 0) {
        if (pad->pressed & PAD_CROSS)
            rescan();
        return;
    }

    /* the stick (or d-pad left/right) scrolls through sets, d-pad up/down picks
     * the difficulty inside the open one */
    int prev = app.sel;
    int sets = pad->stick_y + pad->dpad_x;
    if (sets > 0 && set_end(app.sel) < app.songs.count)
        app.sel = closest_in_set(set_end(app.sel), app.sel);
    else if (sets < 0 && set_start(app.sel) > 0)
        app.sel = closest_in_set(set_start(set_start(app.sel) - 1), app.sel);
    if (pad->dpad_y) {
        int s = set_start(app.sel), e = set_end(app.sel) - 1;
        app.sel += pad->dpad_y;
        if (app.sel < s) app.sel = s;
        if (app.sel > e) app.sel = e;
    }
    if (app.sel < 0) app.sel = 0;
    if (app.sel >= app.songs.count) app.sel = app.songs.count - 1;

    if (pad->pressed & PAD_L1) { app.offset_ms -= 5; settings_save(); }
    if (pad->pressed & PAD_R1) { app.offset_ms += 5; settings_save(); }

    const song_entry *e = &app.songs.items[app.sel];
    if (app.sel != prev) {
        app.status[0] = 0;
        app.sel_changed = wall_ms();
    }

    /* music and background once the cursor has stopped for a moment */
    if (app.previewed != app.sel && wall_ms() - app.sel_changed > 150) {
        char audio[768] = "", bg[768] = "";
        songs_file(e->dir, e->info.audio, audio, sizeof(audio));
        if (!songs_file(e->dir, e->info.background, bg, sizeof(bg)))
            bg[0] = 0;
        preview_request(audio, bg, e->info.preview_time);
        app.previewed = app.sel;
    }

    unsigned char *px;
    int w, h;
    char path[768];
    if (preview_take_bg(&px, &w, &h, path, sizeof(path))) {
        set_background(px, w, h, path);
        if (px)
            stbi_image_free(px);
    }
    preview_tick();
    refresh_scores();

    if (pad->pressed & PAD_CROSS)
        start_song(e);
}

static void render_select(void)
{
    draw_background(0.75f);
    char buf[300];

    if (app.songs.count == 0) {
        draw_text(GFX_W / 2, 400, 48, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, "no beatmaps yet");
        draw_text(GFX_W / 2, 480, 28, gfx_rgba(1, 1, 1, 0.8f), ALIGN_CENTRE, "press TRIANGLE to download some");
        draw_text(GFX_W / 2, 525, 28, gfx_rgba(1, 1, 1, 0.8f), ALIGN_CENTRE, "or copy extracted .osz folders to " SONGS_DIR);
        draw_text(GFX_W / 2, 570, 28, gfx_rgba(1, 1, 1, 0.8f), ALIGN_CENTRE, "and press CROSS to rescan");
        draw_text(40, GFX_H - 60, 22, gfx_rgba(1, 1, 1, 0.7f), ALIGN_LEFT,
                  "TRIANGLE get maps   OPTIONS settings   TOUCHPAD skins");
        return;
    }

    /* One panel per set like osu! does; the selected set opens into its
     * difficulties under a header. Scrolled so the selection sits mid screen. */
    enum { ROW_SET, ROW_HEADER, ROW_DIFF };
    static struct { int kind, index; } *rows;
    static int rows_cap;
    if (rows_cap < app.songs.count + 1) {
        void *grown = realloc(rows, sizeof(*rows) * (size_t)(app.songs.count + 1));
        if (!grown)
            return;
        rows = grown;
        rows_cap = app.songs.count + 1;
    }
    int nrows = 0, sel_row = 0;
    int open = set_start(app.sel);
    for (int s = 0; s < app.songs.count; s = set_end(s)) {
        if (s != open) {
            rows[nrows].kind = ROW_SET;
            rows[nrows++].index = s;
            continue;
        }
        rows[nrows].kind = ROW_HEADER;
        rows[nrows++].index = s;
        for (int i = s; i < set_end(s); i++) {
            if (i == app.sel)
                sel_row = nrows;
            rows[nrows].kind = ROW_DIFF;
            rows[nrows++].index = i;
        }
    }

    const float row = 74;
    app.sel_scroll += (sel_row - app.sel_scroll) * 0.25f;
    if (fabsf(sel_row - app.sel_scroll) > 30)
        app.sel_scroll = (float)sel_row; /* big jumps: don't fly past hundreds of rows */
    int first = (int)app.sel_scroll - 8;
    for (int r = first < 0 ? 0 : first; r < nrows && r < app.sel_scroll + 9; r++) {
        float y = GFX_H / 2 - row / 2 + (r - app.sel_scroll) * row;
        const song_entry *e = &app.songs.items[rows[r].index];
        const osu5_peek_info *in = &e->info;

        if (rows[r].kind == ROW_DIFF) {
            bool on = r == sel_row;
            float x = on ? 940 : 990;
            draw_rect(x, y, GFX_W - x, row - 6, on ? gfx_rgba(1, 1, 1, 0.92f) : gfx_rgba(0.15f, 0.25f, 0.45f, 0.75f));
            uint32_t tc = on ? gfx_rgba(0.1f, 0.1f, 0.15f, 1) : gfx_rgba(1, 1, 1, 0.95f);
            snprintf(buf, sizeof(buf), "%.90s", in->version);
            draw_text(x + 24, y + 8, 26, tc, ALIGN_LEFT, buf);
            snprintf(buf, sizeof(buf), "CS %.1f  AR %.1f  OD %.1f  HP %.1f", in->cs, in->ar, in->od, in->hp);
            draw_text(x + 24, y + 42, 18, tc, ALIGN_LEFT, buf);
            continue;
        }

        /* a set: closed panel, or the header above the open one's difficulties */
        bool header = rows[r].kind == ROW_HEADER;
        float x = header ? 900 : 960;
        int n = set_end(rows[r].index) - rows[r].index;
        draw_rect(x, y, GFX_W - x, row - 6, header ? gfx_rgba(1, 0.8f, 0.35f, 0.9f) : gfx_rgba(0, 0, 0, 0.55f));
        uint32_t tc = header ? gfx_rgba(0.15f, 0.1f, 0.02f, 1) : gfx_rgba(1, 1, 1, 0.95f);
        snprintf(buf, sizeof(buf), "%.100s", in->title);
        draw_text(x + 20, y + 8, 26, tc, ALIGN_LEFT, buf);
        snprintf(buf, sizeof(buf), "%.60s  //  %.40s  //  %d %s", in->artist, in->creator, n, n == 1 ? "difficulty" : "difficulties");
        draw_text(x + 20, y + 42, 18, tc, ALIGN_LEFT, buf);
    }

    const osu5_peek_info *in = &app.songs.items[app.sel].info;
    draw_rect(40, 60, 800, 260, gfx_rgba(0, 0, 0, 0.6f));
    draw_text(60, 80, 34, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, in->title);
    draw_text(60, 126, 26, gfx_rgba(1, 1, 1, 0.85f), ALIGN_LEFT, in->artist);
    snprintf(buf, sizeof(buf), "[%.90s] mapped by %.60s", in->version, in->creator);
    draw_text(60, 166, 22, gfx_rgba(1, 1, 1, 0.75f), ALIGN_LEFT, buf);
    snprintf(buf, sizeof(buf), "CS %.1f  AR %.1f  OD %.1f  HP %.1f", in->cs, in->ar, in->od, in->hp);
    draw_text(60, 214, 26, gfx_rgba(1, 1, 1, 0.95f), ALIGN_LEFT, buf);

    char tags[64];
    mod_tags(app.mods, tags, sizeof(tags));
    snprintf(buf, sizeof(buf), "%s%soffset %+dms", tags, tags[0] ? "   " : "", app.offset_ms);
    draw_text(60, 262, 24, gfx_rgba(1, 0.85f, 0.4f, 1), ALIGN_LEFT, buf);

    /* local scores for this difficulty */
    draw_rect(40, 340, 800, 70 + SHOWN_SCORES * 64, gfx_rgba(0, 0, 0, 0.5f));
    draw_text(60, 356, 24, gfx_rgba(1, 1, 1, 0.8f), ALIGN_LEFT, app.nscores ? "your best" : "no scores yet");
    for (int i = 0; i < app.nscores; i++) {
        const score_rec *r = &app.scores[i];
        float y = 400 + i * 64;
        draw_rank(r->rank, 95, y + 26, 44, true, 1);
        snprintf(buf, sizeof(buf), "%lld", (long long)r->score);
        draw_text(150, y + 6, 28, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, buf);
        mod_tags(r->mods, tags, sizeof(tags));
        snprintf(buf, sizeof(buf), "%.2f%%  %dx  %s", r->accuracy * 100, r->max_combo, tags);
        draw_text(150, y + 38, 18, gfx_rgba(1, 1, 1, 0.7f), ALIGN_LEFT, buf);
    }

    mapdl_status dl;
    mapdl_progress(&dl);
    if (dl.current) {
        snprintf(buf, sizeof(buf), "downloading a map, %.1f MB%s", dl.bytes / 1048576.0, dl.queued ? ", more waiting" : "");
        draw_text(40, GFX_H - 100, 22, gfx_rgba(0.5f, 0.85f, 1, 1), ALIGN_LEFT, buf);
    }

    draw_text(40, GFX_H - 60, 22, gfx_rgba(1, 1, 1, 0.7f), ALIGN_LEFT,
              "CROSS play  STICK maps  UP/DOWN difficulty  SQUARE mods  TRIANGLE get maps  OPTIONS settings");

    if (app.status[0])
        draw_text(GFX_W / 2, GFX_H - 150, 26, gfx_rgba(1, 0.5f, 0.5f, 1), ALIGN_CENTRE, app.status);
}

/* ---- mod select ---- */

static void mods_frame(const pad_state *pad)
{
    /* 3 x 3 grid, Auto alone on the last row */
    int r = app.mod_sel / 3, c = app.mod_sel % 3;
    r = (r + pad->nav_y + 3) % 3;
    c = (c + pad->nav_x + 3) % 3;
    app.mod_sel = r * 3 + c;
    if (app.mod_sel >= MOD_COUNT)
        app.mod_sel = MOD_COUNT - 1;

    if (pad->pressed & PAD_CROSS)
        toggle_mod(mod_list[app.mod_sel].bit);
    if (pad->pressed & PAD_TRIANGLE)
        app.mods = 0;
    if (pad->pressed & (PAD_CIRCLE | PAD_SQUARE)) {
        settings_save();
        app.scr = SCREEN_SELECT;
    }
}

static void render_mods(void)
{
    char buf[128];
    draw_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0, 0, 0, 0.7f));
    draw_text(GFX_W / 2, 110, 52, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, "mods");

    const float w = 420, h = 150, x0 = (GFX_W - 3 * w) / 2, y0 = 220;
    for (int i = 0; i < MOD_COUNT; i++) {
        float x = x0 + (i % 3) * w, y = y0 + (i / 3) * h;
        bool on = app.mods & mod_list[i].bit, cur = i == app.mod_sel;
        draw_rect(x + 10, y + 10, w - 20, h - 20,
                  on ? gfx_rgba(1, 0.8f, 0.3f, cur ? 1 : 0.85f) : gfx_rgba(1, 1, 1, cur ? 0.35f : 0.1f));
        uint32_t tc = on ? gfx_rgba(0.12f, 0.08f, 0.02f, 1) : gfx_rgba(1, 1, 1, 1);
        draw_text(x + 36, y + 32, 40, tc, ALIGN_LEFT, mod_list[i].tag);
        draw_text(x + 130, y + 40, 28, tc, ALIGN_LEFT, mod_list[i].name);
        draw_text(x + 36, y + 92, 17, tc, ALIGN_LEFT, mod_list[i].what);
    }

    snprintf(buf, sizeof(buf), "score x%.2f", mod_multiplier(app.mods));
    draw_text(GFX_W / 2, y0 + 3 * h + 40, 34, gfx_rgba(1, 0.85f, 0.4f, 1), ALIGN_CENTRE, buf);
    draw_text(GFX_W / 2, GFX_H - 90, 26, gfx_rgba(1, 1, 1, 0.8f), ALIGN_CENTRE, "CROSS toggle   TRIANGLE clear all   CIRCLE done");
}

/* ---- settings ---- */

enum { SET_MUSIC, SET_EFFECTS, SET_OFFSET, SET_SENS, SET_DIM, SET_CURSOR, SET_RESCAN, SET_COUNT };

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static void settings_frame(const pad_state *pad)
{
    app.set_sel = (app.set_sel + pad->nav_y + SET_COUNT) % SET_COUNT;
    int d = pad->nav_x;
    switch (app.set_sel) {
    case SET_MUSIC:
        app.music_vol = clampf(app.music_vol + d * 0.05f, 0, 1);
        if (d) audio_music_volume(app.music_vol);
        break;
    case SET_EFFECTS: app.fx_vol = clampf(app.fx_vol + d * 0.05f, 0, 1); break;
    case SET_OFFSET: app.offset_ms += d; break;
    case SET_SENS: app.sensitivity = clampf(app.sensitivity + d * 0.05f, 0.5f, 2); break;
    case SET_DIM: app.bg_dim = clampf(app.bg_dim + d * 0.05f, 0, 1); break;
    case SET_CURSOR:
        app.cursor_scale = clampf(app.cursor_scale + d * 0.1f, 0.5f, 2);
        draw_set_cursor_scale(app.cursor_scale);
        break;
    case SET_RESCAN:
        if (pad->pressed & PAD_CROSS) {
            rescan();
            snprintf(app.status, sizeof(app.status), "found %d difficulties", app.songs.count);
        }
        break;
    }
    if (pad->pressed & (PAD_CIRCLE | PAD_OPTIONS)) {
        settings_save();
        app.scr = SCREEN_SELECT;
    }
}

static void render_settings(void)
{
    char val[64];
    draw_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0, 0, 0, 0.7f));
    draw_text(160, 90, 52, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, "settings");

    for (int i = 0; i < SET_COUNT; i++) {
        const char *name = "";
        val[0] = 0;
        switch (i) {
        case SET_MUSIC: name = "music volume"; snprintf(val, sizeof(val), "%d%%", (int)lroundf(app.music_vol * 100)); break;
        case SET_EFFECTS: name = "effects volume"; snprintf(val, sizeof(val), "%d%%", (int)lroundf(app.fx_vol * 100)); break;
        case SET_OFFSET: name = "audio offset"; snprintf(val, sizeof(val), "%+d ms", app.offset_ms); break;
        case SET_SENS: name = "left stick reach"; snprintf(val, sizeof(val), "%.2fx", app.sensitivity); break;
        case SET_DIM: name = "background dim in play"; snprintf(val, sizeof(val), "%d%%", (int)lroundf(app.bg_dim * 100)); break;
        case SET_CURSOR: name = "cursor size"; snprintf(val, sizeof(val), "%.1fx", app.cursor_scale); break;
        case SET_RESCAN: name = "rescan the Songs folder"; snprintf(val, sizeof(val), "CROSS"); break;
        }
        float y = 200 + i * 90;
        bool on = i == app.set_sel;
        draw_rect(160, y, 1600, 76, on ? gfx_rgba(1, 1, 1, 0.92f) : gfx_rgba(1, 1, 1, 0.08f));
        uint32_t tc = on ? gfx_rgba(0.1f, 0.1f, 0.15f, 1) : gfx_rgba(1, 1, 1, 1);
        draw_text(190, y + 22, 32, tc, ALIGN_LEFT, name);
        draw_text(1730, y + 22, 32, tc, ALIGN_RIGHT, val);
    }

    const char *hint = "";
    switch (app.set_sel) {
    case SET_OFFSET: hint = "positive if you hit late everywhere; L1/R1 in song select change it by 5 too"; break;
    case SET_SENS: hint = "how far a full stick tilt reaches; above 1 you can reach the edge before full tilt"; break;
    case SET_RESCAN: hint = "after copying maps over FTP"; break;
    default: break;
    }
    draw_text(160, 200 + SET_COUNT * 90 + 20, 22, gfx_rgba(1, 1, 1, 0.7f), ALIGN_LEFT, hint);
    if (app.status[0])
        draw_text(160, 200 + SET_COUNT * 90 + 60, 24, gfx_rgba(1, 0.85f, 0.4f, 1), ALIGN_LEFT, app.status);
    draw_text(160, GFX_H - 90, 26, gfx_rgba(1, 1, 1, 0.8f), ALIGN_LEFT, "LEFT/RIGHT change   CIRCLE save and back");
}

/* ---- pause / fail / results ---- */

static void render_overlay(const char *title, uint32_t colour, const char *hint)
{
    draw_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0, 0, 0, 0.6f));
    draw_text(GFX_W / 2, 380, 96, colour, ALIGN_CENTRE, title);
    draw_text(GFX_W / 2, 560, 30, gfx_rgba(1, 1, 1, 0.85f), ALIGN_CENTRE, hint);
}

static void render_results(void)
{
    const osu5_hud *h = &app.last_hud;
    char buf[128];

    draw_background(0.7f);
    draw_rect(260, 140, 1400, 800, gfx_rgba(0, 0, 0, 0.65f));

    char title[200];
    osu5_string(OSU5_STR_TITLE, title, sizeof(title));
    draw_text(GFX_W / 2, 170, 40, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, title);

    draw_rank(h->rank, 520, 460, 300, false, 1);

    float x = 900, y = 300;
    snprintf(buf, sizeof(buf), "%08lld", (long long)h->score);
    draw_text(x, y, 72, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, buf);
    snprintf(buf, sizeof(buf), "accuracy %.2f%%", h->accuracy * 100);
    draw_text(x, y + 110, 34, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, buf);
    snprintf(buf, sizeof(buf), "max combo %dx", h->max_combo);
    draw_text(x, y + 160, 34, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, buf);
    snprintf(buf, sizeof(buf), "300 %d   100 %d   50 %d   miss %d", h->great, h->ok, h->meh, h->miss);
    draw_text(x, y + 230, 30, gfx_rgba(1, 1, 1, 0.9f), ALIGN_LEFT, buf);
    snprintf(buf, sizeof(buf), "UR %.1f", h->unstable_rate);
    draw_text(x, y + 280, 30, gfx_rgba(1, 1, 1, 0.7f), ALIGN_LEFT, buf);

    char tags[64];
    mod_tags(app.mods, tags, sizeof(tags));
    if (tags[0])
        draw_text(x, y + 330, 30, gfx_rgba(1, 0.85f, 0.4f, 1), ALIGN_LEFT, tags);
    if (app.new_place == 0)
        draw_text(x, y + 390, 34, gfx_rgba(0.5f, 1, 0.6f, 1), ALIGN_LEFT, "new personal best!");
    else if (app.mods & OSU5_MOD_AUTO)
        draw_text(x, y + 390, 26, gfx_rgba(1, 1, 1, 0.6f), ALIGN_LEFT, "auto plays aren't saved");

    draw_text(GFX_W / 2, 860, 28, gfx_rgba(1, 1, 1, 0.8f), ALIGN_CENTRE, "CROSS retry   CIRCLE back");
}

int main(void)
{
    log_open();
    LOG("starting");

    SceUserServiceInitializeParams up = {.priority = 256};
    sceUserServiceInitialize(&up);
    int32_t user = -1;
    sceUserServiceGetInitialUser(&user);
    LOG("user %d", user);

    if (!gfx_init())
        die("couldn't start Vulkan");
    draw_init();
    LOG("gfx up");

    if (!input_init(user))
        notify("osu5: no controller");
    if (!audio_init(user))
        notify("osu5: audio out failed, playing silent");
    LOG("input/audio done");

    LOG("gc check: %d collections", osu5_gc_check());

    settings_load();
    audio_music_volume(app.music_vol);
    scores_load();
    LOG("settings read");
    skin_apply(skins_active());
    LOG("skin %s", skins_active());
    if (!skins_chosen()) {
        skin_ui_open(true);
        app.scr = SCREEN_SKINS;
    } else if (!strcmp(skins_active(), "default") && skins_default_outdated()) {
        LOG("default skin is missing newer files, fetching them");
        skins_install(0, &app.topup, false);
    }
    rescan();

    double last = wall_ms();

    /* no way out but the PS button: returning from main dies in exit() */
    for (;;) {
        double now = wall_ms();
        float dt = (float)(now - last);
        last = now;
        if (dt > 100) dt = 100;

        pad_state pad;
        input_poll(&pad);

        if (app.topup.state >= 2) {
            if (app.topup.state == 2 && !strcmp(skins_active(), "default") && app.scr != SCREEN_PLAY)
                skin_apply("default");
            if (app.topup.state == 3 || app.scr != SCREEN_PLAY)
                app.topup.state = 0;
        }

        switch (app.scr) {
        case SCREEN_SELECT:
            select_frame(&pad);
            break;
        case SCREEN_PLAY:
            play_frame(&pad, dt);
            break;
        case SCREEN_PAUSE:
            if (pad.pressed & (PAD_CROSS | PAD_OPTIONS)) {
                audio_music_pause(false);
                app.scr = SCREEN_PLAY;
            } else if (pad.pressed & PAD_CIRCLE) {
                back_to_select();
            } else if (pad.pressed & PAD_TRIANGLE) {
                start_song(app.playing);
            }
            break;
        case SCREEN_FAILED: {
            /* fade the music out, like stable's fail */
            double k = (wall_ms() - app.fail_time) / 1500.0;
            audio_music_volume(app.music_vol * (float)fmax(0, 1 - k));
            if (k > 1) audio_music_stop();
            if (pad.pressed & PAD_CROSS) start_song(app.playing);
            else if (pad.pressed & PAD_CIRCLE) back_to_select();
            break;
        }
        case SCREEN_RESULTS:
            if (pad.pressed & PAD_CROSS) start_song(app.playing);
            else if (pad.pressed & PAD_CIRCLE) back_to_select();
            break;
        case SCREEN_SKINS:
            if (skin_ui_frame(&pad))
                app.scr = SCREEN_SELECT;
            break;
        case SCREEN_MODS:
            mods_frame(&pad);
            break;
        case SCREEN_SETTINGS:
            settings_frame(&pad);
            break;
        case SCREEN_DOWNLOAD:
            if (mapdl_ui_frame(&pad))
                app.scr = SCREEN_SELECT;
            break;
        }

        gfx_begin(0.06f, 0.06f, 0.09f);
        switch (app.scr) {
        case SCREEN_SELECT:
            render_select();
            break;
        case SCREEN_PLAY:
            render_play();
            break;
        case SCREEN_PAUSE:
            render_play();
            render_overlay("paused", gfx_rgba(1, 1, 1, 1), "CROSS continue   TRIANGLE retry   CIRCLE quit");
            break;
        case SCREEN_FAILED:
            render_play();
            render_overlay("failed", gfx_rgba(1, 0.35f, 0.35f, 1), "CROSS retry   CIRCLE back");
            break;
        case SCREEN_RESULTS:
            render_results();
            break;
        case SCREEN_SKINS:
            draw_background(0.7f);
            skin_ui_render();
            break;
        case SCREEN_MODS:
            render_select();
            render_mods();
            break;
        case SCREEN_SETTINGS:
            draw_background(0.7f);
            render_settings();
            break;
        case SCREEN_DOWNLOAD:
            draw_background(0.7f);
            mapdl_ui_render();
            break;
        }
        gfx_end();
    }
}
