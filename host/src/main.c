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
#include "kbd.h"
#include "log.h"
#include "mapdl.h"
#include "mapdl_ui.h"
#include "preview.h"
#include "replay.h"
#include "online.h"
#include "storyboard.h"
#include "sce.h"
#include "scores.h"
#include "skin.h"
#include "skin_img.h"
#include "skin_ui.h"
#include "skins.h"
#include "songs.h"
#include "stars.h"
#include "thumbs.h"
#include "prefs.h"
#include "update.h"
#include "video.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_WRITE
#include "../third_party/stb_image.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DATA_DIR "/data/cadence"
#define SONGS_DIR DATA_DIR "/Songs"
#define SKIN_DIR DATA_DIR "/skin"
#define SETTINGS_FILE DATA_DIR "/settings.txt"

#define MAX_CMDS 4096
#define MAX_POINTS 32768
#define SHOWN_SCORES 5
#define ACC_SAMPLES 200

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
    SCREEN_WIZARD,
    SCREEN_MENU,
    SCREEN_HELP,
} screen;

static struct {
    screen scr;
    song_list all;        /* everything in Songs/ */
    song_list songs;      /* what song select shows: filtered and sorted */
    char filter[64];
    int sort;
    bool searching;
    bool show_online;     /* osu.ppy.sh leaderboard instead of local scores */
    kbd search;
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
    bool storyboards;
    bool videos;
    int map_offset;       /* per map, from offsets.txt */
    bool fav_only;
    double delete_armed;  /* wall ms R3 was pressed once; a second press deletes */
    int ruleset, keys;    /* of the map being played: 0 osu, 1 taiko, 3 mania; mania columns */
    double break_flash;   /* wall ms a combo of 20+ broke */
    double volume_shown;
    float acc[ACC_SAMPLES];
    int acc_n;
    bool show_help;       /* the controls pages */
    int help_page;
    bool onboarded;
    double wiz_start, wiz_sum; /* offset finder: first beat (wall ms), summed tap errors */
    int wiz_taps, wiz_next;
    int menu_sel;
    int logo_tex;

    int bg_tex;
    int bg_w, bg_h;
    char bg_path[768];

    double play_time;
    double fail_time;
    int prev_state;
    double section_at;    /* wall time the break's pass/fail icon went up */
    bool section_pass;
    double finish_seen;
    osu5_hud last_hud;
    play_input cursor;
    const song_entry *playing;

    score_rec scores[SHOWN_SCORES];
    int nscores;
    int scores_for;       /* song index the list above belongs to */
    int new_place;        /* where the last result landed, -1 not saved */

    replay rec;           /* the play going on now */
    replay watch;         /* the one being watched */
    bool watching;
    int rp_pos;
    int saved_mods;       /* the player's own, while a replay's are in */
    int64_t last_when;    /* last saved score / replay, 0 none */
    float last_pp;
    storyboard *sb;

    int mod_sel;
    int set_sel;
    int seen_installs;
    skin_job topup;       /* default skin from an older version, fetched quietly */

    char status[256];
} app = {.previewed = -1, .scores_for = -1, .new_place = -1};

static void help_open(screen back);

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
    app.storyboards = true;
    app.videos = true;

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
            else if (!strcmp(key, "sort")) app.sort = (int)v >= 0 && (int)v <= SORT_COUNT ? (int)v : 0;
            else if (!strcmp(key, "storyboard")) app.storyboards = v != 0;
            else if (!strcmp(key, "video")) app.videos = v != 0;
            else if (!strcmp(key, "onboarded")) app.onboarded = v != 0;
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
    fprintf(f, "offset %d\nsensitivity %.2f\nmusic %.2f\neffects %.2f\ndim %.2f\ncursor %.2f\nmods %d\nsort %d\nstoryboard %d\nvideo %d\nonboarded %d\n",
            app.offset_ms, app.sensitivity, app.music_vol, app.fx_vol, app.bg_dim, app.cursor_scale, app.mods & ~OSU5_MOD_AUTO,
            app.sort, app.storyboards, app.videos, app.onboarded);
    fclose(f);
}

static const char *sort_label(void)
{
    return app.sort == SORT_COUNT ? "favourites" : songs_sort_name(app.sort);
}

/* the difficulty song select was on, so a relaunch lands there again */
#define LAST_FILE DATA_DIR "/last.txt"

static void remember_selection(const char *file)
{
    FILE *f = fopen(LAST_FILE, "w");
    if (f) {
        fprintf(f, "%s\n", file);
        fclose(f);
    }
}

static void restore_selection(void)
{
    FILE *f = fopen(LAST_FILE, "r");
    char file[768] = "";
    if (!f)
        return;
    if (fgets(file, sizeof(file), f))
        file[strcspn(file, "\r\n")] = 0;
    fclose(f);
    int at = file[0] ? songs_find(&app.songs, file) : -1;
    if (at >= 0) {
        app.sel = at;
        app.sel_scroll = (float)at;
    }
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
    /* easier */
    {OSU5_MOD_EASY, "EZ", "Easy", "bigger circles, more forgiving"},
    {OSU5_MOD_NOFAIL, "NF", "No Fail", "you can't fail"},
    {OSU5_MOD_HALFTIME, "HT", "Half Time", "0.75x speed, same pitch"},
    {OSU5_MOD_DAYCORE, "DC", "Daycore", "0.75x speed, pitch drops"},
    /* harder */
    {OSU5_MOD_HARDROCK, "HR", "Hard Rock", "smaller circles, flipped map"},
    {OSU5_MOD_SUDDENDEATH, "SD", "Sudden Death", "one miss and it's over"},
    {OSU5_MOD_PERFECT, "PF", "Perfect", "anything but a 300 fails"},
    {OSU5_MOD_DOUBLETIME, "DT", "Double Time", "1.5x speed, same pitch"},
    {OSU5_MOD_NIGHTCORE, "NC", "Nightcore", "1.5x speed, pitch rises"},
    {OSU5_MOD_HIDDEN, "HD", "Hidden", "circles fade out before you hit them"},
    {OSU5_MOD_FLASHLIGHT, "FL", "Flashlight", "you only see around the cursor"},
    {OSU5_MOD_MIRROR, "MR", "Mirror", "the map flipped left to right"},
    /* help */
    {OSU5_MOD_RELAX, "RX", "Relax", "aim yourself, the clicks are done for you"},
    {OSU5_MOD_AUTOPILOT, "AP", "Autopilot", "click yourself, the aim is done for you"},
    {OSU5_MOD_SPUNOUT, "SO", "Spun Out", "spinners spin themselves"},
    {OSU5_MOD_AUTO, "AT", "Auto", "watch a perfect play"},
};
#define MOD_COUNT (int)(sizeof(mod_list) / sizeof(mod_list[0]))
#define MOD_COLS 4

static double mod_rate(int mods)
{
    return mods & (OSU5_MOD_DOUBLETIME | OSU5_MOD_NIGHTCORE) ? 1.5 : mods & (OSU5_MOD_HALFTIME | OSU5_MOD_DAYCORE) ? 0.75 : 1.0;
}

/* what the core gets: Nightcore and Daycore are its speed mods as well */
static int core_mods(int mods)
{
    if (mods & OSU5_MOD_NIGHTCORE) mods |= OSU5_MOD_DOUBLETIME;
    if (mods & OSU5_MOD_DAYCORE) mods |= OSU5_MOD_HALFTIME;
    return mods;
}

/* same numbers as ModRules.ScoreMultiplier in the core */
static double mod_multiplier(int mods)
{
    mods = core_mods(mods);
    double m = 1;
    if (mods & OSU5_MOD_NOFAIL) m *= 0.5;
    if (mods & OSU5_MOD_EASY) m *= 0.5;
    if (mods & OSU5_MOD_HALFTIME) m *= 0.3;
    if (mods & OSU5_MOD_HIDDEN) m *= 1.06;
    if (mods & OSU5_MOD_HARDROCK) m *= 1.06;
    if (mods & OSU5_MOD_DOUBLETIME) m *= 1.12;
    if (mods & OSU5_MOD_FLASHLIGHT) m *= 1.12;
    if (mods & (OSU5_MOD_RELAX | OSU5_MOD_AUTOPILOT)) m *= 0.1;
    if (mods & OSU5_MOD_SPUNOUT) m *= 0.9;
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
    /* groups where only one can be on */
    static const int groups[] = {
        OSU5_MOD_EASY | OSU5_MOD_HARDROCK,
        OSU5_MOD_DOUBLETIME | OSU5_MOD_HALFTIME | OSU5_MOD_NIGHTCORE | OSU5_MOD_DAYCORE,
        OSU5_MOD_NOFAIL | OSU5_MOD_SUDDENDEATH | OSU5_MOD_PERFECT,
        OSU5_MOD_RELAX | OSU5_MOD_AUTOPILOT | OSU5_MOD_AUTO,
        OSU5_MOD_AUTOPILOT | OSU5_MOD_SPUNOUT,
    };
    for (size_t g = 0; g < sizeof(groups) / sizeof(groups[0]); g++)
        if (groups[g] & bit)
            app.mods &= ~(groups[g] & ~bit);
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
    if (app.sb) {
        sb_free(app.sb);
        app.sb = NULL;
    }

    osu5_set_mods(core_mods(app.mods));
    if (osu5_load(e->file) != 0) {
        char err[200];
        osu5_string(OSU5_STR_ERROR, err, sizeof(err));
        snprintf(app.status, sizeof(app.status), "couldn't load map: %s", err);
        LOG("%s", app.status);
        app.previewed = -1;
        return false;
    }
    app.ruleset = osu5_ruleset(&app.keys);

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
    if (app.storyboards)
        app.sb = sb_load(e->dir, e->file, e->info.background);
    video_close();
    double voff;
    if (app.videos && mod_rate(app.mods) == 1 && video_find(e->dir, e->file, path, sizeof(path), &voff))
        LOG("video: %s", video_open(path, voff) ? "opened" : "couldn't open, still background");

    /* DT/HT: stretch the song so the pitch stays, and let the clock run at the
     * mod's rate over the stretched buffer; plain resampling if that fails */
    double rate = mod_rate(app.mods);
    sound_buf stretched;
    /* Nightcore and Daycore want the pitch to move: plain resampling */
    bool kept_pitch = rate != 1 && !(app.mods & (OSU5_MOD_NIGHTCORE | OSU5_MOD_DAYCORE)) && audio_stretch(&music, rate, &stretched);
    if (kept_pitch) {
        sound_free(&music);
        music = stretched;
    }
    audio_music_set(&music);
    audio_music_volume(app.music_vol);
    if (kept_pitch)
        audio_music_timescale(rate);
    else
        audio_music_rate(rate);
    audio_music_play(osu5_start_time());
    LOG("play %s from %.0f ms, mods %d%s%s", e->file, osu5_start_time(), app.mods, app.watching ? ", replay" : "",
        app.sb ? ", storyboard" : "");

    if (!app.watching)
        replay_reset(&app.rec, e->file, app.mods);
    app.rp_pos = 0;
    app.playing = e;
    app.cursor.x = 256;
    app.cursor.y = 192;
    app.finish_seen = 0;
    app.map_offset = offsets_get(e->file);
    remember_selection(e->file);
    app.section_at = 0;
    app.acc_n = 0;
    app.break_flash = 0;
    app.prev_state = -1;
    sb_reset(app.sb);
    app.new_place = -1;
    app.last_when = 0;
    app.last_pp = 0;
    app.status[0] = 0;
    app.scr = SCREEN_PLAY;
    return true;
}

/* plays back a saved replay; its mods stand in for the chosen ones until we're back */
static void watch_replay(const song_entry *e, int64_t when)
{
    if (!replay_load(&app.watch, when)) {
        snprintf(app.status, sizeof(app.status), "couldn't read that replay");
        return;
    }
    app.saved_mods = app.mods;
    app.mods = app.watch.mods;
    app.watching = true;
    if (!start_song(e)) {
        app.watching = false;
        app.mods = app.saved_mods;
    }
}

static void back_to_select(void)
{
    audio_music_stop();
    audio_music_rate(1);
    audio_music_volume(app.music_vol);
    osu5_unload();
    video_close();
    if (app.sb) {
        sb_free(app.sb);
        app.sb = NULL;
    }
    if (app.watching) {
        app.watching = false;
        app.mods = app.saved_mods;
    }
    app.previewed = -1; /* get the preview going again */
    app.sel_changed = 0;
    app.scores_for = -1;
    app.scr = SCREEN_SELECT;
}

static double game_time(void) { return audio_music_time() + app.offset_ms + app.map_offset; }

/* taiko: bits 0-1 don (left, right), 2-3 kat. mania: one bit a column, laid out
 * so the shoulders take the outer columns and the face buttons the middle */
static int ruleset_keys(uint32_t held)
{
    if (app.ruleset == 1) {
        int k = 0;
        if (held & (PAD_L2 | PAD_DOWN)) k |= 1;
        if (held & (PAD_R2 | PAD_CROSS)) k |= 2;
        if (held & (PAD_L1 | PAD_LEFT)) k |= 4;
        if (held & (PAD_R1 | PAD_CIRCLE)) k |= 8;
        return k;
    }
    static const uint32_t layouts[11][10] = {
        {0},
        {PAD_CROSS | PAD_R1 | PAD_L1},
        {PAD_L1 | PAD_LEFT, PAD_R1 | PAD_CROSS},
        {PAD_L1 | PAD_LEFT, PAD_CROSS | PAD_DOWN, PAD_R1 | PAD_CIRCLE},
        {PAD_L2 | PAD_LEFT, PAD_L1 | PAD_DOWN, PAD_R1 | PAD_CROSS, PAD_R2 | PAD_CIRCLE},
        {PAD_L2 | PAD_LEFT, PAD_L1 | PAD_DOWN, PAD_CROSS | PAD_UP, PAD_R1 | PAD_SQUARE, PAD_R2 | PAD_CIRCLE},
        {PAD_L2, PAD_L1, PAD_LEFT, PAD_CIRCLE, PAD_R1, PAD_R2},
        {PAD_L2, PAD_L1, PAD_LEFT, PAD_CROSS | PAD_DOWN, PAD_CIRCLE, PAD_R1, PAD_R2},
        {PAD_L2, PAD_L1, PAD_LEFT, PAD_DOWN, PAD_CROSS, PAD_CIRCLE, PAD_R1, PAD_R2},
        {PAD_L2, PAD_L1, PAD_LEFT, PAD_DOWN, PAD_UP, PAD_CROSS, PAD_CIRCLE, PAD_R1, PAD_R2},
        {PAD_L2, PAD_L1, PAD_LEFT, PAD_DOWN, PAD_UP, PAD_TRIANGLE, PAD_CROSS, PAD_CIRCLE, PAD_R1, PAD_R2},
    };
    int cols = app.keys < 1 ? 1 : app.keys > 10 ? 10 : app.keys, k = 0;
    for (int c = 0; c < cols; c++)
        if (held & layouts[cols][c])
            k |= 1 << c;
    return k;
}

static void play_frame(const pad_state *pad, float dt)
{
    double t = game_time();
    app.play_time = t;

    if (pad->pressed & PAD_OPTIONS) {
        audio_music_pause(true);
        app.scr = SCREEN_PAUSE;
        return;
    }

    if (app.watching) {
        /* the recorded calls, at their own times, up to now */
        while (app.rp_pos < app.watch.count && app.watch.frames[app.rp_pos].t <= t) {
            const replay_frame *f = &app.watch.frames[app.rp_pos++];
            osu5_update(f->t, f->x, f->y, f->keys);
            app.cursor.x = f->x;
            app.cursor.y = f->y;
        }
    } else {
        if (app.ruleset == 0)
            input_to_play(pad, &app.cursor, dt, app.sensitivity);
        else
            app.cursor.keys = ruleset_keys(pad->held);
        osu5_update(t, app.cursor.x, app.cursor.y, app.cursor.keys);
        if (!(app.mods & OSU5_MOD_AUTO))
            replay_push(&app.rec, t, app.cursor.x, app.cursor.y, app.cursor.keys);
    }

    osu5_sound ev;
    while (osu5_poll_sound(&ev)) {
        skin_play(&ev, app.fx_vol);
        sb_hitsound(app.sb, &ev, t);
    }

    int prev_combo = app.last_hud.combo;
    osu5_get_hud(t, &app.last_hud);
    if (prev_combo >= 20 && app.last_hud.combo < prev_combo)
        app.break_flash = wall_ms();

    /* accuracy over the map, for the results graph */
    int slot = (int)(app.last_hud.progress * (ACC_SAMPLES - 1));
    if (app.last_hud.state != OSU5_STATE_INTRO && slot >= 0 && slot < ACC_SAMPLES) {
        for (int i = app.acc_n; i <= slot; i++)
            app.acc[i] = app.last_hud.accuracy;
        if (slot + 1 > app.acc_n)
            app.acc_n = slot + 1;
    }

    /* music volume on the d-pad, while playing (taiko and mania use it to play) */
    if (pad->dpad_y && !app.watching && app.ruleset == 0) {
        app.music_vol = fminf(1, fmaxf(0, app.music_vol - pad->dpad_y * 0.05f));
        audio_music_volume(app.music_vol);
        app.volume_shown = wall_ms();
    }

    /* every break judges the section before it, like stable did */
    int state = app.last_hud.state;
    if (state == OSU5_STATE_BREAK && app.prev_state == OSU5_STATE_PLAYING) {
        app.section_pass = app.last_hud.health >= 0.5f;
        app.section_at = wall_ms();
        skin_section(app.section_pass, app.fx_vol);
        sb_set_failing(app.sb, !app.section_pass, t);
    }
    app.prev_state = state;
    video_tick(t, false);

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
            app.last_pp = osu5_pp();
            if (!(app.mods & OSU5_MOD_AUTO) && !app.watching && app.playing) {
                app.new_place = scores_add(app.playing->file, &app.last_hud, app.mods, app.last_pp, &app.last_when);
                if (!replay_save(&app.rec, app.last_when))
                    app.last_when = 0;
            }
            app.scr = SCREEN_RESULTS;
        }
    }
}

static void render_play(void)
{
    double t = app.play_time;
    int np = 0;
    int n = osu5_frame(t, cmds, MAX_CMDS, points, MAX_POINTS, &np);

    float dim = app.last_hud.state == OSU5_STATE_BREAK ? app.bg_dim * 0.6f : app.bg_dim;
    if (!video_draw(dim) && (!app.sb || !sb_replaces_bg(app.sb)))
        draw_background(dim);
    if (app.sb)
        sb_draw(app.sb, t, SB_UNDER, dim);
    draw_gameplay(cmds, n, points, np);

    if (app.mods & OSU5_MOD_FLASHLIGHT && app.ruleset == 0) {
        /* dark everywhere but a circle round the cursor, smaller as the combo grows */
        float cx = GFX_W / 2, cy = GFX_H / 2;
        for (int i = n - 1; i >= 0; i--)
            if (cmds[i].kind == OSU5_DRAW_CURSOR) {
                draw_to_screen(cmds[i].x, cmds[i].y, &cx, &cy);
                break;
            }
        int combo = app.last_hud.combo;
        float r = (combo < 100 ? 180 : combo < 200 ? 160 : 140) * 2.25f;
        if (app.last_hud.state == OSU5_STATE_BREAK)
            r *= 2.5f;
        gfx_bind(0);
        float z = gfx_next_z();
        gfx_push(&(gfx_inst){cx, cy, r + 2400, r + 2400, 0, z, gfx_rgba(0, 0, 0, 1), GK_RING, {2400, 0, 0, 0}});
        for (int k = 0; k < 6; k++) /* soft edge */
            gfx_push(&(gfx_inst){cx, cy, r - k * 10, r - k * 10, 0, z, gfx_rgba(0, 0, 0, 0.5f - k * 0.08f), GK_RING, {10, 0, 0, 0}});
    }

    if (app.sb)
        sb_draw(app.sb, t, SB_OVER, 0);
    draw_hud(&app.last_hud, t);

    /* red at the edges when a decent combo breaks */
    double flash = wall_ms() - app.break_flash;
    if (app.break_flash > 0 && flash < 400) {
        float a = 0.45f * (float)(1 - flash / 400);
        draw_rect(0, 0, GFX_W, 40, gfx_rgba(1, 0.15f, 0.15f, a));
        draw_rect(0, GFX_H - 40, GFX_W, 40, gfx_rgba(1, 0.15f, 0.15f, a));
        draw_rect(0, 40, 40, GFX_H - 80, gfx_rgba(1, 0.15f, 0.15f, a));
        draw_rect(GFX_W - 40, 40, 40, GFX_H - 80, gfx_rgba(1, 0.15f, 0.15f, a));
    }

    if (app.last_hud.skip_target > 0 && !app.watching) {
        draw_rect(GFX_W - 300, GFX_H - 150, 260, 80, gfx_rgba(1, 1, 1, 0.9f));
        draw_text(GFX_W - 170, GFX_H - 128, 34, gfx_rgba(0.08f, 0.08f, 0.12f, 1), ALIGN_CENTRE, "skip  /\\");
    }

    if (wall_ms() - app.volume_shown < 1200) {
        char vb[32];
        snprintf(vb, sizeof(vb), "music %d%%", (int)lroundf(app.music_vol * 100));
        draw_rect(GFX_W / 2 - 160, 120, 320, 70, gfx_rgba(0, 0, 0, 0.7f));
        draw_rect(GFX_W / 2 - 140, 170, 280 * app.music_vol, 8, gfx_rgba(0.5f, 0.9f, 0.95f, 1));
        draw_text(GFX_W / 2, 130, 28, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, vb);
    }

    /* the break's verdict blinks a few times */
    double since = wall_ms() - app.section_at;
    if (app.section_at > 0 && since < 1600 && ((int)(since / 200) % 2 == 0 || since > 800)) {
        const skin_sprite *s = app.section_pass ? &skin_img.section_pass : &skin_img.section_fail;
        float a = since > 1200 ? (float)(1600 - since) / 400 : 1;
        if (s->tex) {
            gfx_bind(s->tex);
            gfx_push(&(gfx_inst){GFX_W / 2, GFX_H / 2, s->w * 1.125f, s->h * 1.125f, 0, gfx_next_z(), gfx_rgba(1, 1, 1, a), GK_TEX, {0, 0, 1, 1}});
            gfx_bind(0);
        } else {
            draw_text(GFX_W / 2, GFX_H / 2 - 60, 120, app.section_pass ? gfx_rgba(0.5f, 1, 0.5f, a) : gfx_rgba(1, 0.4f, 0.4f, a),
                      ALIGN_CENTRE, app.section_pass ? "O" : "X");
        }
    }
    if (app.watching)
        draw_text(GFX_W / 2, 20, 30, gfx_rgba(1, 1, 1, 0.8f), ALIGN_CENTRE, "replay");
}

/* ---- song select ---- */

/* rebuilds the visible list from everything, keeping the selection if it survived */
static void apply_view(void)
{
    char keep[768] = "";
    if (app.songs.count > 0)
        snprintf(keep, sizeof(keep), "%s", app.songs.items[app.sel].file);
    songs_filter(&app.all, &app.songs, app.filter);
    if (app.sort == SORT_COUNT) {
        /* the favourites view: only starred sets, by artist */
        int n = 0;
        for (int i = 0; i < app.songs.count; i++)
            if (fav_is(app.songs.items[i].dir))
                app.songs.items[n++] = app.songs.items[i];
        app.songs.count = n;
    }
    songs_sort(&app.songs, app.sort == SORT_COUNT ? SORT_ARTIST : app.sort);
    int at = keep[0] ? songs_find(&app.songs, keep) : -1;
    if (at < 0 || at != app.sel) {
        app.sel = at >= 0 ? at : 0;
        app.sel_scroll = (float)app.sel;
        app.previewed = -1;
        app.scores_for = -1;
    }
}

static bool remove_tree(const char *path)
{
    DIR *d = opendir(path);
    if (!d)
        return remove(path) == 0;
    bool ok = true;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char sub[1024];
        snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
        ok &= remove_tree(sub);
    }
    closedir(d);
    return rmdir(path) == 0 && ok;
}

static void rescan(void)
{
    songs_scan(SONGS_DIR, &app.all);
    /* everything gets rated in the background, what's on screen first */
    for (int i = 0; i < app.all.count; i++)
        stars_want(app.all.items[i].file, 0, false);
    apply_view();
    LOG("%d maps", app.all.count);
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
    if (!app.searching && pad->pressed & PAD_CIRCLE) {
        app.scr = SCREEN_MENU;
        return;
    }
    /* typing a search: the list follows every key */
    if (app.searching) {
        char before[64];
        snprintf(before, sizeof(before), "%s", app.filter);
        int k = kbd_frame(&app.search, pad);
        if (strcmp(before, app.filter))
            apply_view();
        if (k != KBD_TYPING)
            app.searching = false;
        return;
    }
    if (pad->pressed & PAD_L1) {
        app.search.text = app.filter;
        app.search.cap = sizeof(app.filter);
        app.searching = true;
        return;
    }
    if (pad->pressed & PAD_L2 && online_enabled())
        app.show_online = !app.show_online;
    if (pad->pressed & PAD_R1) {
        app.sort = (app.sort + 1) % (SORT_COUNT + 1);
        apply_view();
        settings_save();
    }
    if (pad->pressed & PAD_L3 && app.songs.count > 0) {
        bool on = fav_toggle(app.songs.items[app.sel].dir);
        snprintf(app.status, sizeof(app.status), on ? "added to favourites" : "removed from favourites");
        if (app.sort == SORT_COUNT)
            apply_view();
    }
    if (pad->pressed & PAD_R3 && app.songs.count > 0) {
        if (wall_ms() - app.delete_armed < 3000) {
            char dir[512];
            snprintf(dir, sizeof(dir), "%s", app.songs.items[app.sel].dir);
            preview_cancel();
            audio_music_stop();
            bool ok = remove_tree(dir);
            LOG("deleted %s: %s", dir, ok ? "ok" : "failed");
            snprintf(app.status, sizeof(app.status), ok ? "deleted" : "couldn't delete everything");
            app.delete_armed = 0;
            rescan();
            return;
        }
        app.delete_armed = wall_ms();
        snprintf(app.status, sizeof(app.status), "press R3 again to delete this whole set");
    }

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
        if (pad->pressed & PAD_CROSS && !app.filter[0])
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


    /* the open set and the selection with the current mods get rated first;
     * once new numbers land, the open set re-sorts by stars */
    int open = set_start(app.sel), open_end = set_end(app.sel);
    for (int i = open; i < open_end; i++)
        stars_want(app.songs.items[i].file, 0, true);
    if (stars_mods(app.mods))
        stars_want(app.songs.items[app.sel].file, stars_mods(app.mods), true);
    static int seen_gen;
    if (stars_generation() != seen_gen) {
        seen_gen = stars_generation();
        char keep[768];
        snprintf(keep, sizeof(keep), "%s", app.songs.items[app.sel].file);
        songs_sort_range(&app.songs, open, open_end);
        int at = songs_find(&app.songs, keep);
        if (at >= 0 && at != app.sel) {
            if (app.previewed == app.sel)
                app.previewed = at; /* same song, the preview carries on */
            app.sel = at;
            app.scores_for = -1;
        }
    }

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
    thumb_tick();
    refresh_scores();

    if (pad->pressed & PAD_CROSS)
        start_song(e);
    else if (pad->pressed & PAD_R2 && app.nscores > 0 && replay_exists(app.scores[0].when))
        watch_replay(e, app.scores[0].when);
}

/* ---- bits the menus share ---- */

/* blue for easy through green, yellow, red, purple, to near black past 8 */
static uint32_t star_colour(float st, float a)
{
    static const float stops[][4] = {
        {0, 0.31f, 0.75f, 1}, {2, 0.31f, 0.75f, 1}, {2.7f, 0.31f, 1, 0.53f}, {3.5f, 0.96f, 0.94f, 0.36f},
        {4.5f, 1, 0.5f, 0.4f}, {5.5f, 1, 0.31f, 0.5f}, {6.5f, 0.42f, 0.29f, 0.85f}, {8, 0.12f, 0.09f, 0.3f},
    };
    const int n = sizeof(stops) / sizeof(stops[0]);
    if (st >= stops[n - 1][0])
        return gfx_rgba(stops[n - 1][1], stops[n - 1][2], stops[n - 1][3], a);
    int i = 0;
    while (i < n - 2 && st >= stops[i + 1][0])
        i++;
    float k = (st - stops[i][0]) / (stops[i + 1][0] - stops[i][0]);
    if (k < 0) k = 0;
    if (k > 1) k = 1;
    return gfx_rgba(stops[i][1] + (stops[i + 1][1] - stops[i][1]) * k, stops[i][2] + (stops[i + 1][2] - stops[i][2]) * k,
                    stops[i][3] + (stops[i + 1][3] - stops[i][3]) * k, a);
}

/* a pill with the rating in it; dark text on the light colours */
static float star_pill(float x, float y, float h, float st)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f*", st);
    float size = h * 0.62f, w = draw_text_width(size, buf) + h;
    gfx_bind(0);
    float z = gfx_next_z();
    gfx_push(&(gfx_inst){x + h / 2, y + h / 2, h / 2, h / 2, 0, z, star_colour(st, 1), GK_CAPSULE, {x + w - h / 2, y + h / 2, 0, 0}});
    uint32_t tc = st < 6.5f ? gfx_rgba(0.08f, 0.08f, 0.12f, 1) : gfx_rgba(1, 0.85f, 0.4f, 1);
    draw_text(x + h / 2, y + (h - size) / 2, size, tc, ALIGN_LEFT, buf);
    return w;
}

/* clockwise from the top, from and to in turns (0..1) */
static void arc(float cx, float cy, float r, float thick, float from, float to, uint32_t c)
{
    if (to <= from)
        return;
    int n = (int)((to - from) * r / 6) + 2;
    gfx_bind(0);
    float z = gfx_next_z();
    float px = 0, py = 0;
    for (int i = 0; i <= n; i++) {
        float a = (from + (to - from) * i / n) * 6.2831853f - 1.5707963f;
        float x = cx + cosf(a) * r, y = cy + sinf(a) * r;
        if (i > 0)
            gfx_push(&(gfx_inst){px, py, thick / 2, thick / 2, 0, z, c, GK_CAPSULE, {x, y, 0, 0}});
        px = x;
        py = y;
    }
}

static void stat_bar(float x, float y, float w, const char *label, float v, float max)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%s %.1f", label, v);
    draw_text(x, y, 18, gfx_rgba(1, 1, 1, 0.8f), ALIGN_LEFT, buf);
    float k = v / max;
    if (k > 1) k = 1;
    if (k < 0) k = 0;
    draw_rect(x, y + 24, w, 6, gfx_rgba(1, 1, 1, 0.15f));
    draw_rect(x, y + 24, w * k, 6, gfx_rgba(1, 1, 1, 0.85f));
}

/* the set's background, small; 0 until it's loaded */
static int set_thumb(const song_entry *e)
{
    if (!e->info.background[0])
        return 0;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", e->dir, e->info.background);
    return thumb_get(path);
}

static void thumb_box(int tex, float x, float y, float w, float h, float alpha)
{
    if (!tex) {
        draw_rect(x, y, w, h, gfx_rgba(0.1f, 0.12f, 0.2f, alpha));
        return;
    }
    gfx_bind(tex);
    gfx_push(&(gfx_inst){x + w / 2, y + h / 2, w / 2, h / 2, 0, gfx_next_z(), gfx_rgba(1, 1, 1, alpha), GK_TEX, {0, 0, 1, 1}});
    gfx_bind(0);
}

/* top of the osu.ppy.sh leaderboard, in the box local scores use */
static void render_online(const song_entry *e)
{
    static online_board b;
    char buf[160];
    int st = online_board_for(e->info.beatmap_id, e->file, &b);
    const char *head = "online leaderboard";
    if (st == ONLINE_LOADING)
        head = "loading leaderboard...";
    else if (st == ONLINE_FAILED)
        head = "couldn't reach osu.ppy.sh";
    else if (!b.online_id)
        head = "this difficulty isn't on osu.ppy.sh";
    else if (b.count == 0)
        head = "no online scores";
    /* where your local best would land */
    if (st == ONLINE_READY && b.count > 0 && app.nscores > 0) {
        int place = 1;
        while (place <= b.count && b.s[place - 1].score >= app.scores[0].score)
            place++;
        if (place <= b.count)
            snprintf(buf, sizeof(buf), "online top %d  -  your best would be #%d", b.count, place);
        else
            snprintf(buf, sizeof(buf), "online top %d  -  your best isn't in it yet", b.count);
        head = buf;
    }
    draw_text(60, 356, 24, gfx_rgba(1, 1, 1, 0.8f), ALIGN_LEFT, head);
    draw_text(820, 356, 18, gfx_rgba(1, 1, 1, 0.5f), ALIGN_RIGHT, "L2 local");
    if (st != ONLINE_READY)
        return;

    int rows = SHOWN_SCORES * 64 / 40;
    for (int i = 0; i < b.count && i < rows; i++) {
        const online_score *s = &b.s[i];
        float y = 396 + i * 40;
        snprintf(buf, sizeof(buf), "%d", i + 1);
        draw_text(90, y + 6, 22, gfx_rgba(1, 1, 1, 0.6f), ALIGN_RIGHT, buf);
        draw_rank(s->rank, 125, y + 18, 28, true, 1);
        draw_text(160, y + 6, 22, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, s->user);
        snprintf(buf, sizeof(buf), "%.2f%%  %dx  %s", s->accuracy * 100, s->combo, s->mods);
        draw_text(400, y + 9, 18, gfx_rgba(1, 1, 1, 0.7f), ALIGN_LEFT, buf);
        snprintf(buf, sizeof(buf), "%.0fpp", s->pp);
        draw_text(820, y + 6, 22, gfx_rgba(1, 0.85f, 0.4f, 1), ALIGN_RIGHT, buf);
    }
}

static void render_select(void)
{
    draw_background(0.75f);
    char buf[300];

    /* search and sort, top right */
    if (app.filter[0] || app.searching)
        snprintf(buf, sizeof(buf), "search: %s%s   sort: %s", app.filter, app.searching ? "_" : "", sort_label());
    else
        snprintf(buf, sizeof(buf), "L1 search   R1 sort: %s", sort_label());
    draw_rect(0, 0, GFX_W, 52, gfx_rgba(0, 0, 0, 0.6f));
    draw_text(GFX_W - 30, 12, 26, gfx_rgba(1, 1, 1, 0.9f), ALIGN_RIGHT, buf);
    draw_text(40, 12, 28, gfx_rgba(0.5f, 0.9f, 0.95f, 1), ALIGN_LEFT, "ProjectCadence");

    if (app.songs.count == 0 && app.all.count > 0) {
        snprintf(buf, sizeof(buf), "nothing matches \"%s\"", app.filter);
        draw_text(GFX_W / 2, 400, 40, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, buf);
        if (app.searching)
            kbd_render(&app.search);
        return;
    }

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
            float st = stars_get(e->file, 0);
            draw_rect(x, y, GFX_W - x, row - 6, on ? gfx_rgba(1, 1, 1, 0.92f) : gfx_rgba(0.08f, 0.1f, 0.18f, 0.8f));
            /* the rating's colour down the left edge */
            draw_rect(x, y, 8, row - 6, st >= 0 ? star_colour(st, 1) : gfx_rgba(0.5f, 0.5f, 0.5f, 1));
            uint32_t tc = on ? gfx_rgba(0.1f, 0.1f, 0.15f, 1) : gfx_rgba(1, 1, 1, 0.95f);
            float tx = x + 28;
            if (st >= 0)
                tx += star_pill(tx, y + 10, 30, st) + 14;
            if (in->mode != 0) {
                /* which game this difficulty is */
                char tag[16];
                snprintf(tag, sizeof(tag), in->mode == 1 ? "TAIKO" : "%dK", (int)lroundf(in->cs));
                float tw = draw_text_width(16, tag) + 20;
                draw_rect(tx, y + 12, tw, 26, in->mode == 1 ? gfx_rgba(0.92f, 0.3f, 0.3f, 1) : gfx_rgba(0.6f, 0.45f, 1, 1));
                draw_text(tx + tw / 2, y + 17, 16, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, tag);
                tx += tw + 12;
            }
            snprintf(buf, sizeof(buf), "%.80s", in->version);
            draw_text(tx, y + 14, 24, tc, ALIGN_LEFT, buf);
            if (in->mode == 3)
                snprintf(buf, sizeof(buf), "%d keys  OD %.1f  HP %.1f", (int)lroundf(in->cs), in->od, in->hp);
            else if (in->mode == 1)
                snprintf(buf, sizeof(buf), "OD %.1f  HP %.1f", in->od, in->hp);
            else
                snprintf(buf, sizeof(buf), "CS %.1f  AR %.1f  OD %.1f  HP %.1f", in->cs, in->ar, in->od, in->hp);
            draw_text(x + 28, y + 46, 16, tc, ALIGN_LEFT, buf);
            continue;
        }

        /* a set: closed panel, or the header above the open one's difficulties */
        bool header = rows[r].kind == ROW_HEADER;
        float x = header ? 900 : 960;
        int n = set_end(rows[r].index) - rows[r].index;
        float th = row - 6, tw = th * 16 / 9;
        draw_rect(x, y, GFX_W - x, th, header ? gfx_rgba(1, 0.8f, 0.35f, 0.92f) : gfx_rgba(0, 0, 0, 0.6f));
        thumb_box(set_thumb(e), x, y, tw, th, header ? 1 : 0.75f);
        x += tw;
        uint32_t tc = header ? gfx_rgba(0.15f, 0.1f, 0.02f, 1) : gfx_rgba(1, 1, 1, 0.95f);
        snprintf(buf, sizeof(buf), "%.90s", in->title);
        draw_text(x + 20, y + 8, 26, tc, ALIGN_LEFT, buf);
        if (e->video) {
            draw_rect(GFX_W - 120, y + 10, 96, 26, gfx_rgba(1, 0.47f, 0.63f, 0.95f));
            draw_text(GFX_W - 72, y + 15, 16, gfx_rgba(0.1f, 0.05f, 0.08f, 1), ALIGN_CENTRE, "VIDEO");
        }
        /* star range of the set, once every difficulty in it is rated */
        float lo = 1e9f, hi = -1;
        for (int i = rows[r].index; i < rows[r].index + n; i++) {
            float st = stars_get(app.songs.items[i].file, 0);
            if (st < 0) {
                hi = -1;
                break;
            }
            lo = fminf(lo, st);
            hi = fmaxf(hi, st);
        }
        char range[32] = "";
        if (hi >= 0)
            snprintf(range, sizeof(range), n > 1 ? "  //  %.1f-%.1f*" : "  //  %.2f*", lo, hi);
        snprintf(buf, sizeof(buf), "%.60s  //  %.40s  //  %d %s%s", in->artist, in->creator, n, n == 1 ? "difficulty" : "difficulties", range);
        draw_text(x + 20, y + 42, 18, tc, ALIGN_LEFT, buf);
    }

    const osu5_peek_info *in = &app.songs.items[app.sel].info;
    draw_rect(40, 60, 800, 260, gfx_rgba(0, 0, 0, 0.6f));
    draw_text(60, 80, 34, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, in->title);
    draw_text(60, 126, 26, gfx_rgba(1, 1, 1, 0.85f), ALIGN_LEFT, in->artist);
    snprintf(buf, sizeof(buf), "[%.90s] mapped by %.60s", in->version, in->creator);
    draw_text(60, 166, 22, gfx_rgba(1, 1, 1, 0.75f), ALIGN_LEFT, buf);
    const char *selfile = app.songs.items[app.sel].file;
    float st = stars_get(selfile, 0), mst = stars_mods(app.mods) ? stars_get(selfile, stars_mods(app.mods)) : st;
    /* rating (with mods when they change it), then the four numbers as bars */
    float bx = 60;
    if (mst >= 0)
        bx += star_pill(60, 204, 38, mst) + 20;
    else if (st >= 0)
        bx += star_pill(60, 204, 38, st) + 20;
    float bw = (820 - bx - 3 * 16) / 4;
    stat_bar(bx, 204, bw, "CS", in->cs, 10);
    stat_bar(bx + (bw + 16), 204, bw, "AR", in->ar, 10);
    stat_bar(bx + (bw + 16) * 2, 204, bw, "OD", in->od, 10);
    stat_bar(bx + (bw + 16) * 3, 204, bw, "HP", in->hp, 10);

    char tags[64];
    mod_tags(app.mods, tags, sizeof(tags));
    snprintf(buf, sizeof(buf), "%s%soffset %+dms", tags, tags[0] ? "   " : "", app.offset_ms);
    draw_text(60, 262, 24, gfx_rgba(1, 0.85f, 0.4f, 1), ALIGN_LEFT, buf);

    draw_rect(40, 340, 800, 70 + SHOWN_SCORES * 64, gfx_rgba(0, 0, 0, 0.5f));
    if (app.show_online) {
        render_online(&app.songs.items[app.sel]);
        goto scores_done;
    }

    /* local scores for this difficulty */
    draw_text(60, 356, 24, gfx_rgba(1, 1, 1, 0.8f), ALIGN_LEFT, app.nscores ? "your best" : "no scores yet");
    if (app.nscores && replay_exists(app.scores[0].when))
        draw_text(820, 356, 20, gfx_rgba(1, 1, 1, 0.6f), ALIGN_RIGHT, "R2 watch replay");
    for (int i = 0; i < app.nscores; i++) {
        const score_rec *r = &app.scores[i];
        float y = 400 + i * 64;
        draw_rank(r->rank, 95, y + 26, 44, true, 1);
        snprintf(buf, sizeof(buf), "%lld", (long long)r->score);
        draw_text(150, y + 6, 28, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, buf);
        if (r->pp > 0) {
            snprintf(buf, sizeof(buf), "%.0fpp", r->pp);
            draw_text(820, y + 10, 26, gfx_rgba(1, 0.85f, 0.4f, 1), ALIGN_RIGHT, buf);
        }
        mod_tags(r->mods, tags, sizeof(tags));
        snprintf(buf, sizeof(buf), "%.2f%%  %dx  %s", r->accuracy * 100, r->max_combo, tags);
        draw_text(150, y + 38, 18, gfx_rgba(1, 1, 1, 0.7f), ALIGN_LEFT, buf);
    }
    if (online_enabled())
        draw_text(820, 356 + (app.nscores && replay_exists(app.scores[0].when) ? 24 : 0), 18,
                  gfx_rgba(1, 1, 1, 0.5f), ALIGN_RIGHT, "L2 online");
scores_done:;

    mapdl_status dl;
    mapdl_progress(&dl);
    if (dl.current) {
        snprintf(buf, sizeof(buf), "downloading a map, %.1f MB%s", dl.bytes / 1048576.0, dl.queued ? ", more waiting" : "");
        draw_text(40, GFX_H - 100, 22, gfx_rgba(0.5f, 0.85f, 1, 1), ALIGN_LEFT, buf);
    }


    if (app.status[0])
        draw_text(GFX_W / 2, GFX_H - 150, 26, gfx_rgba(1, 0.5f, 0.5f, 1), ALIGN_CENTRE, app.status);
    if (app.searching)
        kbd_render(&app.search);
}

/* ---- mod select ---- */

static void mods_frame(const pad_state *pad)
{
    /* a row each: easier, harder, harder still, help */
    int rows = (MOD_COUNT + MOD_COLS - 1) / MOD_COLS;
    int r = app.mod_sel / MOD_COLS, c = app.mod_sel % MOD_COLS;
    r = (r + pad->nav_y + rows) % rows;
    c = (c + pad->nav_x + MOD_COLS) % MOD_COLS;
    app.mod_sel = r * MOD_COLS + c;
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

    /* one colour per row, so the kinds read at a glance */
    static const float row_col[4][3] = {{0.5f, 0.9f, 0.5f}, {1, 0.45f, 0.45f}, {1, 0.75f, 0.3f}, {0.5f, 0.75f, 1}};
    const float w = 400, h = 150, x0 = (GFX_W - MOD_COLS * w) / 2, y0 = 190;
    for (int i = 0; i < MOD_COUNT; i++) {
        float x = x0 + (i % MOD_COLS) * w, y = y0 + (i / MOD_COLS) * h;
        const float *rc = row_col[(i / MOD_COLS) % 4];
        bool on = app.mods & mod_list[i].bit, cur = i == app.mod_sel;
        draw_rect(x + 10, y + 10, w - 20, h - 20,
                  on ? gfx_rgba(rc[0], rc[1], rc[2], cur ? 1 : 0.85f) : gfx_rgba(1, 1, 1, cur ? 0.3f : 0.08f));
        if (cur)
            draw_rect(x + 10, y + h - 16, w - 20, 6, gfx_rgba(1, 1, 1, 0.9f));
        uint32_t tc = on ? gfx_rgba(0.08f, 0.06f, 0.06f, 1) : gfx_rgba(1, 1, 1, 1);
        draw_text(x + 30, y + 30, 38, on ? tc : gfx_rgba(rc[0], rc[1], rc[2], 1), ALIGN_LEFT, mod_list[i].tag);
        draw_text(x + 110, y + 38, 26, tc, ALIGN_LEFT, mod_list[i].name);
        draw_text(x + 30, y + 92, 16, tc, ALIGN_LEFT, mod_list[i].what);
    }

    int rows = (MOD_COUNT + MOD_COLS - 1) / MOD_COLS;
    snprintf(buf, sizeof(buf), "score x%.2f", mod_multiplier(app.mods));
    draw_text(GFX_W / 2, y0 + rows * h + 30, 34, gfx_rgba(1, 0.85f, 0.4f, 1), ALIGN_CENTRE, buf);
}

/* ---- settings ---- */

enum { SET_MUSIC, SET_EFFECTS, SET_OFFSET, SET_SENS, SET_DIM, SET_CURSOR, SET_STORYBOARD, SET_VIDEO, SET_WIZARD, SET_HELP, SET_RESCAN, SET_COUNT };

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
    case SET_STORYBOARD:
        if (d || pad->pressed & PAD_CROSS)
            app.storyboards = !app.storyboards;
        break;
    case SET_VIDEO:
        if (d || pad->pressed & PAD_CROSS)
            app.videos = !app.videos;
        break;
    case SET_HELP:
        if (pad->pressed & PAD_CROSS) {
            help_open(SCREEN_SETTINGS);
            return;
        }
        break;
    case SET_WIZARD:
        if (pad->pressed & PAD_CROSS) {
            app.wiz_start = wall_ms() + 1000;
            app.wiz_taps = 0;
            app.wiz_sum = 0;
            app.wiz_next = 0;
            audio_music_stop();
            app.scr = SCREEN_WIZARD;
            return;
        }
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
        case SET_STORYBOARD: name = "storyboards"; snprintf(val, sizeof(val), "%s", app.storyboards ? "on" : "off"); break;
        case SET_VIDEO: name = "video backgrounds (mp4)"; snprintf(val, sizeof(val), "%s", app.videos ? "on" : "off"); break;
        case SET_WIZARD: name = "find my offset"; snprintf(val, sizeof(val), "CROSS"); break;
        case SET_HELP: name = "show the controls"; snprintf(val, sizeof(val), "CROSS"); break;
        case SET_RESCAN: name = "rescan the Songs folder"; snprintf(val, sizeof(val), "CROSS"); break;
        }
        float y = 170 + i * 74;
        bool on = i == app.set_sel;
        draw_rect(160, y, 1600, 64, on ? gfx_rgba(1, 1, 1, 0.92f) : gfx_rgba(1, 1, 1, 0.08f));
        uint32_t tc = on ? gfx_rgba(0.1f, 0.1f, 0.15f, 1) : gfx_rgba(1, 1, 1, 1);
        draw_text(190, y + 16, 30, tc, ALIGN_LEFT, name);
        draw_text(1730, y + 16, 30, tc, ALIGN_RIGHT, val);
    }

    const char *hint = "";
    switch (app.set_sel) {
    case SET_OFFSET: hint = "positive if you hit late everywhere; L1/R1 in song select change it by 5 too"; break;
    case SET_SENS: hint = "how far a full stick tilt reaches; above 1 you can reach the edge before full tilt"; break;
    case SET_RESCAN: hint = "after copying maps over FTP"; break;
    default: break;
    }
    draw_text(160, 170 + SET_COUNT * 74 + 10, 22, gfx_rgba(1, 1, 1, 0.7f), ALIGN_LEFT, hint);
    if (app.status[0])
        draw_text(160, 170 + SET_COUNT * 74 + 44, 24, gfx_rgba(1, 0.85f, 0.4f, 1), ALIGN_LEFT, app.status);
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

    /* accuracy ring: hits split by judgement around the rank, accuracy as the inner arc */
    float cx = 560, cy = 520, R = 230;
    int total = h->great + h->ok + h->meh + h->miss;
    arc(cx, cy, R, 26, 0, 1, gfx_rgba(1, 1, 1, 0.08f));
    if (total > 0) {
        const int counts[4] = {h->great, h->ok, h->meh, h->miss};
        const uint32_t cols[4] = {gfx_rgba(0.4f, 0.8f, 1, 1), gfx_rgba(0.5f, 1, 0.5f, 1), gfx_rgba(1, 0.85f, 0.35f, 1),
                                  gfx_rgba(1, 0.35f, 0.35f, 1)};
        float at = 0;
        for (int i = 0; i < 4; i++) {
            float span = (float)counts[i] / total;
            arc(cx, cy, R, 26, at, at + span, cols[i]);
            at += span;
        }
    }
    arc(cx, cy, R - 34, 8, 0, h->accuracy, gfx_rgba(1, 1, 1, 0.9f));
    draw_rank(h->rank, cx, cy, 240, false, 1);

    float x = 900, y = 230;
    snprintf(buf, sizeof(buf), "%08lld", (long long)h->score);
    draw_text(x, y, 72, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, buf);
    snprintf(buf, sizeof(buf), "%.0fpp", app.last_pp);
    draw_text(1620, y, 72, gfx_rgba(1, 0.85f, 0.4f, 1), ALIGN_RIGHT, buf);
    snprintf(buf, sizeof(buf), "%.2f%%   %dx   UR %.1f   %.2f*", h->accuracy * 100, h->max_combo, h->unstable_rate, osu5_play_stars());
    draw_text(x, y + 100, 32, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, buf);
    snprintf(buf, sizeof(buf), "300 %d   100 %d   50 %d   miss %d", h->great, h->ok, h->meh, h->miss);
    draw_text(x, y + 150, 28, gfx_rgba(1, 1, 1, 0.85f), ALIGN_LEFT, buf);

    char tags[64];
    mod_tags(app.mods, tags, sizeof(tags));
    if (tags[0])
        draw_text(x, y + 200, 28, gfx_rgba(1, 0.85f, 0.4f, 1), ALIGN_LEFT, tags);
    if (app.watching)
        draw_text(x, y + 250, 26, gfx_rgba(1, 1, 1, 0.6f), ALIGN_LEFT, "watched a replay");
    else if (app.new_place == 0)
        draw_text(x, y + 250, 32, gfx_rgba(0.5f, 1, 0.6f, 1), ALIGN_LEFT, "new personal best!");
    else if (app.mods & OSU5_MOD_AUTO)
        draw_text(x, y + 250, 26, gfx_rgba(1, 1, 1, 0.6f), ALIGN_LEFT, "auto plays aren't saved");

    /* hit offsets: where the taps landed against the beat */
    static float errs[8192];
    int ne = osu5_hit_errors(errs, 8192);
    enum { BINS = 41 };
    int bins[BINS] = {0}, top = 1;
    float sum = 0;
    for (int i = 0; i < ne; i++) {
        int b = (int)lroundf(errs[i] / 5) + BINS / 2; /* 5 ms a bin, +-100 ms */
        if (b >= 0 && b < BINS && ++bins[b] > top)
            top = bins[b];
        sum += errs[i];
    }
    float gx = 900, gy = 560, gw = 340, gh = 150;
    draw_rect(gx, gy, gw, gh, gfx_rgba(0, 0, 0, 0.35f));
    draw_rect(gx + gw / 2 - 1, gy, 2, gh, gfx_rgba(1, 1, 1, 0.35f));
    for (int b = 0; b < BINS; b++) {
        float bh = (gh - 10) * bins[b] / top;
        float off = fabsf((b - BINS / 2) * 5.0f);
        uint32_t c = off < 20 ? gfx_rgba(0.4f, 0.8f, 1, 0.9f) : off < 50 ? gfx_rgba(0.5f, 1, 0.5f, 0.9f) : gfx_rgba(1, 0.85f, 0.35f, 0.9f);
        draw_rect(gx + b * gw / BINS + 1, gy + gh - bh, gw / BINS - 2, bh, c);
    }
    snprintf(buf, sizeof(buf), "hits: %+.1f ms on average", ne ? sum / ne : 0);
    draw_text(gx, gy + gh + 10, 20, gfx_rgba(1, 1, 1, 0.7f), ALIGN_LEFT, buf);

    /* accuracy through the map */
    float ax = 1280, aw = 340;
    draw_rect(ax, gy, aw, gh, gfx_rgba(0, 0, 0, 0.35f));
    float lo = 1;
    for (int i = 0; i < app.acc_n; i++)
        lo = fminf(lo, app.acc[i]);
    lo = fminf(lo, 0.9f);
    for (int i = 1; i < app.acc_n; i++) {
        float x0 = ax + aw * (i - 1) / (ACC_SAMPLES - 1), x1 = ax + aw * i / (ACC_SAMPLES - 1);
        float y0 = gy + gh - 5 - (gh - 10) * (app.acc[i - 1] - lo) / (1 - lo);
        float y1 = gy + gh - 5 - (gh - 10) * (app.acc[i] - lo) / (1 - lo);
        gfx_bind(0);
        gfx_push(&(gfx_inst){x0, y0, 2, 2, 0, gfx_next_z(), gfx_rgba(1, 0.85f, 0.4f, 1), GK_CAPSULE, {x1, y1, 0, 0}});
    }
    snprintf(buf, sizeof(buf), "accuracy, %.0f%% to 100%%", lo * 100);
    draw_text(ax, gy + gh + 10, 20, gfx_rgba(1, 1, 1, 0.7f), ALIGN_LEFT, buf);
}

/* ---- offset finder: tap along to a click, the average miss is the offset ---- */

#define WIZ_BEAT 500.0
#define WIZ_TAPS 16

static void wizard_frame(const pad_state *pad)
{
    double now = wall_ms();
    if (now >= app.wiz_start + app.wiz_next * WIZ_BEAT && app.wiz_next < WIZ_TAPS + 8) {
        osu5_sound click = {OSU5_SOUND_HIT, 0, 1, 1, 100, 0, 0};
        skin_play(&click, app.fx_vol);
        app.wiz_next++;
    }
    if (pad->pressed & PAD_CROSS && now > app.wiz_start - 250) {
        double beat = floor((now - app.wiz_start) / WIZ_BEAT + 0.5);
        double err = now - (app.wiz_start + beat * WIZ_BEAT);
        if (fabs(err) < 200) {
            app.wiz_sum += err;
            app.wiz_taps++;
        }
    }
    if (app.wiz_taps >= WIZ_TAPS) {
        app.offset_ms = (int)lround(app.wiz_sum / app.wiz_taps);
        snprintf(app.status, sizeof(app.status), "offset set to %+d ms", app.offset_ms);
        settings_save();
        app.scr = SCREEN_SETTINGS;
    } else if (pad->pressed & PAD_CIRCLE || app.wiz_next >= WIZ_TAPS + 8) {
        snprintf(app.status, sizeof(app.status), app.wiz_taps < WIZ_TAPS ? "not enough taps, offset unchanged" : "");
        app.scr = SCREEN_SETTINGS;
    }
}

static void render_wizard(void)
{
    char buf[64];
    draw_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0, 0, 0, 0.8f));
    draw_text(GFX_W / 2, 300, 52, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, "tap CROSS on every click");
    double k = fmod(wall_ms() - app.wiz_start + WIZ_BEAT * 10, WIZ_BEAT) / WIZ_BEAT;
    float r = 60 + 30 * (float)(1 - k);
    arc(GFX_W / 2, 560, r, 10, 0, 1, gfx_rgba(0.5f, 0.9f, 0.95f, (float)(1 - k)));
    snprintf(buf, sizeof(buf), "%d / %d", app.wiz_taps, WIZ_TAPS);
    draw_text(GFX_W / 2, 760, 34, gfx_rgba(1, 1, 1, 0.8f), ALIGN_CENTRE, buf);
    draw_text(GFX_W / 2, 860, 24, gfx_rgba(1, 1, 1, 0.6f), ALIGN_CENTRE, "CIRCLE cancel");
}

/* ---- controls pages: shown once on the first launch, and from settings ---- */

static const struct help_page {
    const char *title;
    const char *rows[14][2];
} help_pages[] = {
    {"playing", {
        {"left stick", "aim: full tilt reaches the playfield edge"},
        {"right stick", "nudge the cursor"},
        {"touchpad", "touch to aim with it, press to click"},
        {"R1  R2  CROSS", "hit (first key)"},
        {"L1  L2  SQUARE", "hit (second key)"},
        {"TRIANGLE", "skip the intro or a break"},
        {"D-PAD up / down", "music volume"},
        {"OPTIONS", "pause"},
    }},
    {"song select", {
        {"left stick", "browse the sets"},
        {"D-PAD up / down", "pick the difficulty"},
        {"CROSS", "play"},
        {"SQUARE", "mods"},
        {"TRIANGLE", "get maps"},
        {"L1", "search"},
        {"R1", "change the sort, favourites last"},
        {"L2", "online leaderboard"},
        {"R2", "watch your best replay"},
        {"L3", "favourite the set"},
        {"R3  twice", "delete the set"},
        {"OPTIONS", "settings"},
        {"TOUCHPAD", "skins"},
        {"CIRCLE", "main menu"},
    }},
    {"pause and results", {
        {"CROSS", "continue / retry"},
        {"TRIANGLE", "retry from the pause menu"},
        {"CIRCLE", "back to song select"},
        {"D-PAD left / right", "this map's offset, while paused"},
        {"SQUARE", "watch the replay, on results"},
    }},
    {"taiko and mania", {
        {"taiko: L2  R2  DOWN  CROSS", "don (red)"},
        {"taiko: L1  R1  LEFT  CIRCLE", "kat (blue)"},
        {"mania 4K", "L2  L1  R1  R2"},
        {"mania 4K, also", "LEFT  DOWN  CROSS  CIRCLE"},
        {"mania 7K", "L2  L1  LEFT  CROSS  CIRCLE  R1  R2"},
        {"mania other", "the shoulders outside in, face buttons in the middle"},
    }},
};
#define HELP_PAGES (int)(sizeof(help_pages) / sizeof(help_pages[0]))

static screen help_return = SCREEN_MENU;

static void help_open(screen back)
{
    app.help_page = 0;
    help_return = back;
    app.scr = SCREEN_HELP;
}

static void help_frame(const pad_state *pad)
{
    if (pad->pressed & PAD_CROSS || pad->nav_x > 0)
        app.help_page++;
    if (pad->nav_x < 0 && app.help_page > 0)
        app.help_page--;
    if (app.help_page >= HELP_PAGES || pad->pressed & PAD_CIRCLE) {
        if (!app.onboarded) {
            app.onboarded = true;
            settings_save();
        }
        app.scr = help_return;
    }
}

static void render_help(void)
{
    const struct help_page *p = &help_pages[app.help_page < HELP_PAGES ? app.help_page : HELP_PAGES - 1];
    draw_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0.03f, 0.04f, 0.09f, 0.85f));
    draw_text(GFX_W / 2, 90, 30, gfx_rgba(0.5f, 0.9f, 0.95f, 1), ALIGN_CENTRE, app.onboarded ? "controls" : "welcome to ProjectCadence");
    draw_text(GFX_W / 2, 140, 56, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, p->title);
    for (int i = 0; i < 14 && p->rows[i][0]; i++) {
        float y = 250 + i * 50;
        draw_rect(360, y - 6, 1200, 44, gfx_rgba(1, 1, 1, i % 2 ? 0.04f : 0.08f));
        draw_text(900, y, 26, gfx_rgba(1, 0.85f, 0.4f, 1), ALIGN_RIGHT, p->rows[i][0]);
        draw_text(940, y, 26, gfx_rgba(1, 1, 1, 0.95f), ALIGN_LEFT, p->rows[i][1]);
    }
    /* page dots, and how to move on */
    for (int i = 0; i < HELP_PAGES; i++)
        draw_rect(GFX_W / 2 - HELP_PAGES * 20 + i * 40 + 8, GFX_H - 120, 24, 8,
                  gfx_rgba(1, 1, 1, i == app.help_page ? 0.95f : 0.25f));
    draw_text(GFX_W / 2, GFX_H - 90, 24, gfx_rgba(1, 1, 1, 0.7f), ALIGN_CENTRE,
              app.help_page == HELP_PAGES - 1 ? "CROSS done" : "CROSS next   CIRCLE skip");
}

/* ---- main menu ---- */

enum { MENU_PLAY, MENU_MAPS, MENU_SKINS, MENU_SETTINGS, MENU_COUNT };

static void menu_enter(void)
{
    app.scr = SCREEN_MENU;
    if (!app.logo_tex) {
        int w, h, n;
        unsigned char *px = stbi_load("/app0/sce_sys/icon0.png", &w, &h, &n, 4);
        if (px) {
            app.logo_tex = gfx_texture(w, h, px);
            stbi_image_free(px);
        }
    }
}

static void menu_frame(const pad_state *pad)
{
    app.menu_sel = (app.menu_sel + pad->nav_y + MENU_COUNT) % MENU_COUNT;

    /* something playing behind the menu: whatever song select is on */
    if (app.songs.count > 0 && app.previewed != app.sel) {
        const song_entry *e = &app.songs.items[app.sel];
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

    update_info up;
    update_status(&up);
    if (pad->pressed & PAD_TRIANGLE && (up.state == UPDATE_AVAILABLE || up.state == UPDATE_FAILED))
        update_install();

    if (!(pad->pressed & PAD_CROSS))
        return;
    switch (app.menu_sel) {
    case MENU_PLAY: app.scr = SCREEN_SELECT; break;
    case MENU_MAPS: mapdl_ui_open(); app.scr = SCREEN_DOWNLOAD; break;
    case MENU_SKINS: skin_ui_open(false); app.scr = SCREEN_SKINS; break;
    case MENU_SETTINGS: app.set_sel = 0; app.scr = SCREEN_SETTINGS; break;
    }
}

static void render_menu(void)
{
    draw_background(0.55f);
    draw_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0.03f, 0.04f, 0.09f, 0.35f));

    /* the logo breathes, a little faster than resting */
    float t = (float)(wall_ms() / 1000.0);
    float pulse = 1 + 0.03f * sinf(t * 2 * 3.14159f * 1.2f);
    float size = 420 * pulse, cx = 620, cy = GFX_H / 2;
    arc(cx, cy, size * 0.62f, 6, 0, 1, gfx_rgba(1, 0.47f, 0.63f, 0.35f));
    if (app.logo_tex) {
        gfx_bind(app.logo_tex);
        gfx_push(&(gfx_inst){cx, cy, size / 2, size / 2, 0, gfx_next_z(), gfx_rgba(1, 1, 1, 1), GK_TEX, {0, 0, 1, 1}});
        gfx_bind(0);
    }
    draw_text(cx, cy + size / 2 + 50, 48, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, "ProjectCadence");

    static const char *items[MENU_COUNT] = {"play", "get maps", "skins", "settings"};
    for (int i = 0; i < MENU_COUNT; i++) {
        bool on = i == app.menu_sel;
        float x = on ? 1100 : 1140, y = 330 + i * 110;
        draw_rect(x, y, GFX_W - x, 90, on ? gfx_rgba(1, 1, 1, 0.92f) : gfx_rgba(0, 0, 0, 0.55f));
        draw_rect(x, y, 10, 90, gfx_rgba(0.5f, 0.9f, 0.95f, on ? 1 : 0.5f));
        draw_text(x + 50, y + 26, 40, on ? gfx_rgba(0.08f, 0.08f, 0.12f, 1) : gfx_rgba(1, 1, 1, 0.95f), ALIGN_LEFT, items[i]);
    }

    if (app.songs.count > 0) {
        const osu5_peek_info *in = &app.songs.items[app.sel].info;
        char buf[300];
        snprintf(buf, sizeof(buf), "now playing: %.100s - %.100s", in->artist, in->title);
        draw_text(GFX_W - 30, 20, 22, gfx_rgba(1, 1, 1, 0.75f), ALIGN_RIGHT, buf);
    }
    draw_text(GFX_W - 30, GFX_H - 60, 22, gfx_rgba(1, 1, 1, 0.5f), ALIGN_RIGHT, CADENCE_VERSION);

    /* update banner along the top */
    update_info up;
    update_status(&up);
    char ub[200] = "";
    if (up.state == UPDATE_AVAILABLE)
        snprintf(ub, sizeof(ub), "ProjectCadence %s is out (you have %s)   TRIANGLE update now", up.latest, CADENCE_VERSION);
    else if (up.state == UPDATE_INSTALLING)
        snprintf(ub, sizeof(ub), "updating to %s... %.1f MB", up.latest, up.bytes / 1048576.0);
    else if (up.state == UPDATE_DONE || up.state == UPDATE_FAILED)
        snprintf(ub, sizeof(ub), "%s%s", up.msg, up.state == UPDATE_FAILED ? "   TRIANGLE try again" : "");
    if (ub[0]) {
        draw_rect(0, 60, GFX_W, 56, up.state == UPDATE_FAILED ? gfx_rgba(0.6f, 0.15f, 0.2f, 0.9f) : gfx_rgba(0.1f, 0.45f, 0.55f, 0.9f));
        draw_text(GFX_W / 2, 74, 28, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, ub);
    }
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
        notify("ProjectCadence: no controller");
    if (!audio_init(user))
        notify("ProjectCadence: audio out failed, playing silent");
    LOG("input/audio done");

    LOG("gc check: %d collections", osu5_gc_check());

    settings_load();
    audio_music_volume(app.music_vol);
    scores_load();
    online_init();
    update_check();
    stars_init();
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
    restore_selection();
    if (app.scr == SCREEN_SELECT) {
        menu_enter();
        if (!app.onboarded)
            help_open(SCREEN_MENU);
    }

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
            video_tick(app.play_time, true);
            /* this map's own offset, on top of the global one */
            if (pad.dpad_x) {
                app.map_offset += pad.dpad_x * 5;
                offsets_set(app.playing->file, app.map_offset);
            }
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
            if (pad.pressed & PAD_CROSS) {
                /* after a replay, retry means playing it yourself */
                if (app.watching) {
                    app.watching = false;
                    app.mods = app.saved_mods;
                }
                start_song(app.playing);
            } else if (pad.pressed & PAD_SQUARE && app.last_when && !app.watching) {
                watch_replay(app.playing, app.last_when);
            } else if (pad.pressed & PAD_CIRCLE) {
                back_to_select();
            }
            break;
        case SCREEN_SKINS:
            if (skin_ui_frame(&pad)) {
                app.scr = SCREEN_SELECT;
                if (!app.onboarded) {
                    menu_enter();
                    help_open(SCREEN_MENU);
                }
            }
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
        case SCREEN_WIZARD:
            wizard_frame(&pad);
            break;
        case SCREEN_MENU:
            menu_frame(&pad);
            break;
        case SCREEN_HELP:
            help_frame(&pad);
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
            {
                char ob[64];
                snprintf(ob, sizeof(ob), "map offset %+d ms   (LEFT/RIGHT)", app.map_offset);
                draw_text(GFX_W / 2, 640, 26, gfx_rgba(1, 0.85f, 0.4f, 1), ALIGN_CENTRE, ob);
            }
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
        case SCREEN_WIZARD:
            render_wizard();
            break;
        case SCREEN_MENU:
            render_menu();
            break;
        case SCREEN_HELP:
            if (help_return == SCREEN_MENU)
                render_menu();
            else
                draw_background(0.7f);
            render_help();
            break;
        }
        gfx_end();
    }
}
