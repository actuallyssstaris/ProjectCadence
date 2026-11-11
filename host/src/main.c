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
#include "mp.h"
#include "account.h"
#include "md5.h"
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
#include "ui.h"

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
#include <strings.h>
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
    SCREEN_MP,
    SCREEN_ACCOUNT,
} screen;

static struct {
    screen scr;
    song_list all;        /* everything in Songs/ */
    song_list songs;      /* what song select shows: filtered and sorted */
    char filter[64];
    int sort;
    bool searching;
    int board;            /* song select: 0 your scores, 1 the online board */
    int board_src;        /* which online board: 0 ProjectCadence, 1 osu.ppy.sh */
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

    /* multiplayer */
    bool mp_on;           /* connected or connecting */
    int mp_kind;          /* MP_CADENCE / MP_BANCHO */
    char mp_rooms_url[160], mp_bancho[96], mp_user[32], mp_pass[64];
    mp_state mps;         /* this frame's copy */
    int mp_sel, mp_cfg;   /* list cursor; field on the connect form */
    kbd mp_kbd;
    bool mp_typing;
    bool mp_pick;         /* song select is choosing the room's map */
    bool mp_playing, mp_reported, mp_ready;
    int mp_round_seen, mp_saved_mods;
    double mp_sent;
    char mp_map_wanted[33]; /* the room map we last asked the downloader for */
    int mp_type_for;      /* what the keyboard is filling in */
    char mp_line[124], mp_pw[40];
    int mp_join_id;
    int mp_quick;         /* open quick-lines list: the row, -1 closed */
    bool mp_mods;         /* the mods screen is choosing our own, for a free-mod room */
    char ps_name[32];
    char acc_name[24], acc_pass[64];
    int acc_row;
    bool acc_typing;
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
} app = {.previewed = -1, .scores_for = -1, .new_place = -1, .mp_quick = -1};

static void help_open(screen back);
int sceUserServiceGetUserName(int32_t user, char *name, size_t size);
static void mp_report(bool failed, bool done);
static void menu_enter(void);
static void mp_board(float x, float y, float w, bool compact);
static const char *map_md5(const char *file);

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
            else if (!strcmp(key, "boards")) app.board_src = v != 0;
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
    fprintf(f, "offset %d\nsensitivity %.2f\nmusic %.2f\neffects %.2f\ndim %.2f\ncursor %.2f\nmods %d\nsort %d\nstoryboard %d\nvideo %d\nonboarded %d\nboards %d\n",
            app.offset_ms, app.sensitivity, app.music_vol, app.fx_vol, app.bg_dim, app.cursor_scale, app.mods & ~OSU5_MOD_AUTO,
            app.sort, app.storyboards, app.videos, app.onboarded, app.board_src);
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

static int play_retries; /* shown on the pause screen, like lazer's "retry count" */

static void back_to_select(void)
{
    play_retries = 0;
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
    if (app.mp_playing) {
        /* leaving a room's round early counts as giving up on it */
        if (!app.mp_reported)
            mp_report(true, true);
        app.mp_playing = false;
        app.mods = app.mp_saved_mods;
        app.scr = SCREEN_MP;
    }
}

static double game_time(void) { return audio_music_time() + app.offset_ms + app.map_offset; }

/* taiko: bits 0-1 don (left, right), 2-3 kat. mania: one bit a column, laid out
 * so the shoulders take the outer columns and the face buttons the middle */
static int ruleset_keys(const pad_state *pad)
{
    uint32_t held = pad->held;
    if (app.ruleset == 2) {
        /* catch: stick or d-pad to move, any face or shoulder button to dash */
        int k = 0;
        if (pad->lx < -0.3f || held & PAD_LEFT) k |= 1;
        if (pad->lx > 0.3f || held & PAD_RIGHT) k |= 2;
        if (held & (PAD_CROSS | PAD_SQUARE | PAD_CIRCLE | PAD_R1 | PAD_R2 | PAD_L1 | PAD_L2)) k |= 4;
        return k;
    }
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
            app.cursor.keys = ruleset_keys(pad);
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

    /* the room sees our score about once a second */
    if (app.mp_playing && !app.mp_reported && wall_ms() - app.mp_sent > 1000) {
        app.mp_sent = wall_ms();
        mp_report(false, false);
    }

    if (app.last_hud.state == OSU5_STATE_FAILED) {
        app.fail_time = wall_ms();
        app.scr = SCREEN_FAILED;
        if (app.mp_playing && !app.mp_reported)
            mp_report(true, true);
    } else if (app.last_hud.state == OSU5_STATE_FINISHED) {
        if (app.mp_playing && !app.mp_reported)
            mp_report(false, true);
        if (app.finish_seen == 0) {
            app.finish_seen = wall_ms();
        } else if (wall_ms() - app.finish_seen > 1500) {
            app.last_pp = osu5_pp();
            if (!(app.mods & OSU5_MOD_AUTO) && !app.watching && app.playing) {
                app.new_place = scores_add(app.playing->file, &app.last_hud, app.mods, app.last_pp, &app.last_when);
                if (!replay_save(&app.rec, app.last_when))
                    app.last_when = 0;
                const osu5_hud *h = &app.last_hud;
                account_submit(map_md5(app.playing->file), app.playing->info.beatmap_id, app.ruleset, h->score, h->accuracy,
                               h->max_combo, app.mods, app.last_pp, h->rank, h->great, h->ok, h->meh, h->miss);
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
    if (app.mp_playing)
        mp_board(20, 200, 300, true);
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
        app.scr = app.mp_pick ? SCREEN_MP : SCREEN_MENU;
        app.mp_pick = false;
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
    if (pad->pressed & PAD_L2)
        app.board = !app.board; /* yours <-> the online board picked in settings */
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

    if (pad->pressed & PAD_CROSS && app.mp_pick) {
        /* choosing for the room: send it over and go back there */
        char md5[33] = "", title[200];
        md5_file(e->file, md5);
        snprintf(title, sizeof(title), "%.60s - %.80s [%.50s]", e->info.artist, e->info.title, e->info.version);
        mp_set_map(e->info.beatmap_id, e->info.set_id, e->info.mode, md5, title, app.mods);
        app.mp_pick = false;
        app.mp_ready = false;
        app.scr = SCREEN_MP;
    } else if (pad->pressed & PAD_CROSS)
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

/* a map's md5, worked out once per file */
static const char *map_md5(const char *file)
{
    static char for_file[768], md5[33];
    if (strcmp(file, for_file)) {
        snprintf(for_file, sizeof(for_file), "%s", file);
        if (!md5_file(file, md5))
            md5[0] = 0;
    }
    return md5;
}

/* ProjectCadence's own top 50 for the difficulty */
static void render_global(const song_entry *e)
{
    static global_board b;
    char buf[160];
    int st = account_board(map_md5(e->file), &b);
    const char *head = "ProjectCadence top scores";
    if (st == ONLINE_LOADING)
        head = "loading...";
    else if (st == ONLINE_FAILED)
        head = "couldn't reach the ProjectCadence server";
    else if (b.count == 0)
        head = account_signed_in() ? "no scores yet: be the first" : "no scores yet; sign in from settings to post yours";
    draw_text(60, 356, 24, gfx_rgba(1, 1, 1, 0.8f), ALIGN_LEFT, head);
    draw_text(820, 356, 18, gfx_rgba(1, 1, 1, 0.5f), ALIGN_RIGHT, "L2 yours");
    if (st != ONLINE_READY)
        return;

    int rows = SHOWN_SCORES * 64 / 40;
    for (int i = 0; i < b.count && i < rows; i++) {
        const global_score *s = &b.s[i];
        float y = 396 + i * 40;
        bool me = account_signed_in() && !strcasecmp(s->name, account_name());
        if (me)
            draw_rect(50, y, 780, 38, gfx_rgba(0.5f, 0.9f, 0.95f, 0.15f));
        snprintf(buf, sizeof(buf), "%d", i + 1);
        draw_text(90, y + 6, 22, gfx_rgba(1, 1, 1, 0.6f), ALIGN_RIGHT, buf);
        draw_rank(s->rank, 125, y + 18, 28, true, 1);
        draw_text(160, y + 6, 22, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, s->name);
        char tags[64];
        mod_tags(s->mods, tags, sizeof(tags));
        snprintf(buf, sizeof(buf), "%.2f%%  %dx  %s", s->acc * 100, s->combo, tags);
        draw_text(400, y + 9, 18, gfx_rgba(1, 1, 1, 0.7f), ALIGN_LEFT, buf);
        snprintf(buf, sizeof(buf), "%lld", s->score);
        draw_text(820, y + 6, 22, gfx_rgba(1, 0.85f, 0.4f, 1), ALIGN_RIGHT, buf);
    }
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
    draw_text(820, 356, 18, gfx_rgba(1, 1, 1, 0.5f), ALIGN_RIGHT, "L2 yours");
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
    draw_background(0.65f);
    char buf[300];

    /* filter box, top right: search field over the sort tabs */
    ui_round_rect(GFX_W - 900, 14, 880, 110, 14, gfx_rgba(0.08f, 0.08f, 0.1f, 0.82f));
    ui_round_rect(GFX_W - 880, 28, 840, 46, 10, gfx_rgba(1, 1, 1, app.searching ? 0.16f : 0.08f));
    if (app.filter[0] || app.searching)
        snprintf(buf, sizeof(buf), "%s%s", app.filter, app.searching ? "_" : "");
    else
        snprintf(buf, sizeof(buf), "type to search  (L1)");
    draw_text(GFX_W - 860, 38, 24, gfx_rgba(1, 1, 1, app.filter[0] || app.searching ? 1 : 0.45f), ALIGN_LEFT, buf);
    snprintf(buf, sizeof(buf), "sort by  %s   (R1)", sort_label());
    draw_text(GFX_W - 860, 86, 20, ui_hex(UI_YELLOW, 1), ALIGN_LEFT, buf);
    snprintf(buf, sizeof(buf), "%d maps", app.songs.count);
    draw_text(GFX_W - 40, 86, 20, gfx_rgba(1, 1, 1, 0.6f), ALIGN_RIGHT, buf);

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

    const float row = 80;
    app.sel_scroll += (sel_row - app.sel_scroll) * 0.25f;
    if (fabsf(sel_row - app.sel_scroll) > 30)
        app.sel_scroll = (float)sel_row; /* big jumps: don't fly past hundreds of rows */
    int first = (int)app.sel_scroll - 8;
    for (int r = first < 0 ? 0 : first; r < nrows && r < app.sel_scroll + 9; r++) {
        float y = GFX_H / 2 - row / 2 + (r - app.sel_scroll) * row;
        /* text draws over every box, so a row half under the footer or the
         * filter box would show through: leave it out */
        if (y + row - 6 > GFX_H - 84 || y < 130)
            continue;
        const song_entry *e = &app.songs.items[rows[r].index];
        const osu5_peek_info *in = &e->info;
        /* the list bows away from the screen edge, further from the middle */
        float off = (y + row / 2 - GFX_H / 2) / (GFX_H / 2);
        float curve = off * off * 110;

        if (rows[r].kind == ROW_DIFF) {
            bool on = r == sel_row;
            float x = (on ? 960 : 1000) + curve;
            float st = stars_get(e->file, 0);
            uint32_t sc = st >= 0 ? star_colour(st, 1) : gfx_rgba(0.5f, 0.5f, 0.5f, 1);
            if (on)
                ui_round_rect(x - 4, y - 4, GFX_W - x + 30, row + 2, 16, ui_mix(sc, gfx_rgba(1, 1, 1, 1), 0.3f));
            ui_round_rect(x, y, GFX_W - x + 30, row - 6, 12, on ? gfx_rgba(0.16f, 0.16f, 0.2f, 0.97f) : gfx_rgba(0.1f, 0.1f, 0.13f, 0.85f));
            /* the rating's colour down the left edge */
            ui_round_rect(x, y, 14, row - 6, 7, sc);
            uint32_t tc = gfx_rgba(1, 1, 1, on ? 1 : 0.9f);
            float tx = x + 28;
            if (st >= 0)
                tx += star_pill(tx, y + 10, 30, st) + 14;
            if (in->mode != 0) {
                /* which game this difficulty is */
                char tag[16];
                if (in->mode == 3)
                    snprintf(tag, sizeof(tag), "%dK", (int)lroundf(in->cs));
                else
                    snprintf(tag, sizeof(tag), "%s", in->mode == 1 ? "TAIKO" : "CATCH");
                float tw = draw_text_width(16, tag) + 20;
                uint32_t tagc = in->mode == 1 ? gfx_rgba(0.92f, 0.3f, 0.3f, 1) : in->mode == 2 ? gfx_rgba(0.3f, 0.75f, 0.45f, 1)
                                                                               : gfx_rgba(0.6f, 0.45f, 1, 1);
                draw_rect(tx, y + 12, tw, 26, tagc);
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
        float x = (header ? 920 : 980) + curve;
        int n = set_end(rows[r].index) - rows[r].index;
        float th = row - 6, tw = th * 16 / 9;
        if (header)
            ui_round_rect(x - 4, y - 4, GFX_W - x + 30, th + 8, 16, ui_hex(UI_YELLOW, 0.9f));
        ui_round_rect(x, y, GFX_W - x + 30, th, 12, gfx_rgba(0.07f, 0.07f, 0.09f, header ? 0.95f : 0.8f));
        thumb_box(set_thumb(e), x + 6, y + 6, tw - 12, th - 12, header ? 1 : 0.7f);
        x += tw;
        uint32_t tc = gfx_rgba(1, 1, 1, header ? 1 : 0.9f);
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
    /* the wedge: a leaning dark panel, top left, with the map's details */
    ui_shear_rect(-60, 0, 960, 300, 50, gfx_rgba(0.05f, 0.05f, 0.07f, 0.88f));
    draw_text(40, 26, 40, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, in->title);
    draw_text(40, 78, 26, gfx_rgba(1, 1, 1, 0.85f), ALIGN_LEFT, in->artist);
    snprintf(buf, sizeof(buf), "%.90s   mapped by %.60s", in->version, in->creator);
    draw_text(40, 122, 22, ui_hex(UI_YELLOW, 0.95f), ALIGN_LEFT, buf);
    const char *selfile = app.songs.items[app.sel].file;
    float st = stars_get(selfile, 0), mst = stars_mods(app.mods) ? stars_get(selfile, stars_mods(app.mods)) : st;
    /* rating (with mods when they change it), then the four numbers as bars */
    float bx = 40;
    if (mst >= 0)
        bx += star_pill(40, 170, 38, mst) + 20;
    else if (st >= 0)
        bx += star_pill(40, 170, 38, st) + 20;
    float bw = (820 - bx - 3 * 16) / 4;
    stat_bar(bx, 170, bw, "CS", in->cs, 10);
    stat_bar(bx + (bw + 16), 170, bw, "AR", in->ar, 10);
    stat_bar(bx + (bw + 16) * 2, 170, bw, "OD", in->od, 10);
    stat_bar(bx + (bw + 16) * 3, 170, bw, "HP", in->hp, 10);

    char tags[64];
    mod_tags(app.mods, tags, sizeof(tags));
    snprintf(buf, sizeof(buf), "%s%soffset %+dms", tags, tags[0] ? "   " : "", app.offset_ms);
    draw_text(40, 236, 24, ui_hex(UI_YELLOW, 1), ALIGN_LEFT, buf);

    ui_round_rect(40, 330, 800, 80 + SHOWN_SCORES * 64, 16, gfx_rgba(0.05f, 0.05f, 0.07f, 0.75f));
    if (app.board && (app.board_src == 0 || !online_enabled())) {
        render_global(&app.songs.items[app.sel]);
        goto scores_done;
    }
    if (app.board) {
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
    draw_text(820, 356 + (app.nscores && replay_exists(app.scores[0].when) ? 24 : 0), 18,
              gfx_rgba(1, 1, 1, 0.5f), ALIGN_RIGHT, app.board_src && online_enabled() ? "L2 osu.ppy.sh" : "L2 global");
scores_done:;

    /* footer, lazer's measurements: an 84 tall strip, the wide pink back button,
     * then dark tiles that stand a little proud of it with a colour stripe each */
    {
        const float fh = 84, fy = GFX_H - fh, lean = 14;
        draw_rect(0, fy + 14, GFX_W, fh - 14, gfx_rgba(0.12f, 0.12f, 0.14f, 0.95f));
        ui_shear_rect(21, fy + 18, 330, 62, lean, ui_hex(0xDD4499, 1));
        ui_arrow_left(130, fy + 49, 20, gfx_rgba(1, 1, 1, 1));
        draw_text(196, fy + 34, 24, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, "back");

        static const struct { const char *label, *button; uint32_t colour; } tiles[] = {
            {"mods", "SQUARE", 0xB2E05C}, {"get maps", "TRIANGLE", UI_BLUE}, {"skins", "TOUCHPAD", UI_ORANGE}, {"options", "OPTIONS", 0x8866EE},
        };
        float tx = 400;
        for (int i = 0; i < 4; i++) {
            ui_shear_rect(tx, fy, 150, fh, lean, gfx_rgba(0.22f, 0.22f, 0.26f, 0.97f));
            ui_shear_rect(tx + 4, fy + fh - 6, 150 - 8 - lean / 2, 6, 0, ui_hex(tiles[i].colour, 1));
            float cx = tx + 75;
            uint32_t ic = ui_hex(tiles[i].colour, 1);
            switch (i) {
            case 0: /* two arrows passing, the mods glyph */
                draw_rect(cx - 12, fy + 17, 22, 3, ic);
                ui_play_icon(cx + 12, fy + 18.5f, 10, ic);
                draw_rect(cx - 10, fy + 27, 22, 3, ic);
                ui_arrow_left(cx - 12, fy + 28.5f, 10, ic);
                break;
            case 1:
                draw_rect(cx - 2, fy + 10, 4, 12, ic);
                ui_arrow_down(cx, fy + 25, 14, ic);
                break;
            case 2:
                arc(cx, fy + 22, 9, 3, 0, 1, ic);
                ui_disc(cx, fy + 22, 3.5f, ic);
                break;
            default: ui_gear_icon(cx, fy + 22, 24, ic); break;
            }
            draw_text(cx, fy + 40, 20, gfx_rgba(1, 1, 1, 0.95f), ALIGN_CENTRE, tiles[i].label);
            draw_text(tx + 150 - lean - 8, fy + 6, 11, gfx_rgba(1, 1, 1, 0.4f), ALIGN_RIGHT, tiles[i].button);
            tx += 156;
        }
        draw_text(GFX_W - 330, fy + 36, 20, gfx_rgba(1, 1, 1, 0.55f), ALIGN_RIGHT, "L2 board   R2 replay   CROSS play");
        /* our logo peeking in from the corner, like the menu's */
        if (app.logo_tex) {
            float s = 230;
            ui_disc(GFX_W - 100, GFX_H - 30, s / 2 + 10, gfx_rgba(1, 1, 1, 1));
            gfx_bind(app.logo_tex);
            gfx_push(&(gfx_inst){GFX_W - 100, GFX_H - 30, s / 2, s / 2, 0, gfx_next_z(), gfx_rgba(1, 1, 1, 1), GK_TEX, {0, 0, 1, 1}});
            gfx_bind(0);
        }
    }

    mapdl_status dl;
    mapdl_progress(&dl);
    if (dl.current) {
        snprintf(buf, sizeof(buf), "downloading a map, %.1f MB%s", dl.bytes / 1048576.0, dl.queued ? ", more waiting" : "");
        draw_text(40, GFX_H - 112, 22, ui_hex(UI_BLUE, 1), ALIGN_LEFT, buf);
    }


    if (app.status[0])
        draw_text(GFX_W / 2, GFX_H - 150, 26, gfx_rgba(1, 0.5f, 0.5f, 1), ALIGN_CENTRE, app.status);
    if (app.searching)
        kbd_render(&app.search);
}

/* ---- mod select ---- */

/* mod select is laid out in columns by kind; these are indices into mod_list */
static const struct {
    const char *title;
    uint32_t colour;
    int n, mods[8];
} mod_cols[] = {
    {"difficulty reduction", 0xB2E05C, 4, {0, 1, 2, 3}},
    {"difficulty increase", 0xFF6666, 7, {4, 5, 6, 7, 8, 9, 10}},
    {"automation", 0x66CCFF, 4, {12, 13, 14, 15}},
    {"conversion", 0x8866EE, 1, {11}},
};
#define MOD_NCOLS (int)(sizeof(mod_cols) / sizeof(mod_cols[0]))

static void mod_where(int sel, int *col, int *row)
{
    for (int c = 0; c < MOD_NCOLS; c++)
        for (int r = 0; r < mod_cols[c].n; r++)
            if (mod_cols[c].mods[r] == sel) {
                *col = c;
                *row = r;
                return;
            }
    *col = *row = 0;
}

static void mods_frame(const pad_state *pad)
{
    int c, r;
    mod_where(app.mod_sel, &c, &r);
    c = (c + pad->nav_x + MOD_NCOLS) % MOD_NCOLS;
    if (r >= mod_cols[c].n)
        r = mod_cols[c].n - 1;
    r = (r + pad->nav_y + mod_cols[c].n) % mod_cols[c].n;
    app.mod_sel = mod_cols[c].mods[r];

    if (pad->pressed & PAD_CROSS)
        toggle_mod(mod_list[app.mod_sel].bit);
    if (pad->pressed & PAD_TRIANGLE)
        app.mods = 0;
    if (pad->pressed & (PAD_CIRCLE | PAD_SQUARE)) {
        settings_save();
        app.scr = SCREEN_SELECT;
        if (app.mp_mods) {
            /* a free-mod room: these are ours for the next round */
            mp_my_mods(app.mods);
            app.mp_mods = false;
            app.scr = SCREEN_MP;
        }
    }
}

static void render_mods(void)
{
    char buf[128];
    draw_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0.04f, 0.05f, 0.04f, 0.8f));

    /* banner across the top */
    ui_round_rect(80, 60, GFX_W - 160, 90, 12, gfx_rgba(0.14f, 0.2f, 0.12f, 0.95f));
    draw_text(130, 76, 26, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, "mod select");
    draw_text(130, 112, 18, gfx_rgba(1, 1, 1, 0.7f), ALIGN_LEFT,
              "mods change how a map plays; some change the score you get, others are just for fun");

    /* one leaning column per kind, a coloured header on each */
    const float cw = 420, gap = 22, lean = 24, top = 190, hh = 52, rh = 84;
    float x0 = (GFX_W - MOD_NCOLS * cw - (MOD_NCOLS - 1) * gap) / 2;
    for (int c = 0; c < MOD_NCOLS; c++) {
        float x = x0 + c * (cw + gap);
        uint32_t col = mod_cols[c].colour;
        ui_shear_rect(x, top, cw, hh, lean / 2, ui_hex(col, 1));
        draw_text(x + 30, top + 13, 22, gfx_rgba(0.08f, 0.08f, 0.08f, 1), ALIGN_LEFT, mod_cols[c].title);
        ui_round_rect(x, top + hh, cw, 700 - hh, 12, gfx_rgba(0.14f, 0.17f, 0.13f, 0.95f));

        for (int r = 0; r < mod_cols[c].n; r++) {
            int i = mod_cols[c].mods[r];
            float y = top + hh + 14 + r * rh;
            bool on = app.mods & mod_list[i].bit, cur = i == app.mod_sel;
            float ix = x + 16;
            uint32_t bg = on ? ui_hex(col, 1) : gfx_rgba(0.2f, 0.24f, 0.19f, 1);
            if (cur)
                ui_round_rect(ix - 4, y - 4, cw - 24, rh - 6, 12, gfx_rgba(1, 1, 1, 0.9f));
            ui_round_rect(ix, y, cw - 32, rh - 14, 10, bg);
            /* the tag sits in a darker square, the way the icons do */
            ui_round_rect(ix + 8, y + 8, 56, rh - 30, 8, on ? gfx_rgba(0, 0, 0, 0.25f) : gfx_rgba(0, 0, 0, 0.3f));
            uint32_t tc = on ? gfx_rgba(0.08f, 0.08f, 0.08f, 1) : gfx_rgba(1, 1, 1, 1);
            draw_text(ix + 36, y + 22, 20, on ? tc : ui_hex(col, 1), ALIGN_CENTRE, mod_list[i].tag);
            draw_text(ix + 80, y + 10, 22, tc, ALIGN_LEFT, mod_list[i].name);
            draw_text(ix + 80, y + 40, 15, on ? gfx_rgba(0.1f, 0.1f, 0.1f, 0.85f) : gfx_rgba(1, 1, 1, 0.65f), ALIGN_LEFT,
                      mod_list[i].what);
        }
    }

    /* footer: back and clear on the left, the map's numbers with these mods on the right */
    const float fy = GFX_H - 96;
    ui_shear_rect(20, fy, 300, 76, 18, ui_hex(UI_PINK, 1));
    ui_arrow_left(80, fy + 38, 22, gfx_rgba(1, 1, 1, 1));
    draw_text(190, fy + 18, 26, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, "back");
    draw_text(190, fy + 50, 14, gfx_rgba(1, 1, 1, 0.7f), ALIGN_CENTRE, "CIRCLE");
    ui_shear_rect(340, fy, 300, 76, 18, gfx_rgba(0.18f, 0.2f, 0.18f, 0.95f));
    draw_text(490, fy + 18, 24, gfx_rgba(1, 1, 1, 0.9f), ALIGN_CENTRE, "deselect all");
    draw_text(490, fy + 50, 14, gfx_rgba(1, 1, 1, 0.6f), ALIGN_CENTRE, "TRIANGLE");

    float bx = GFX_W - 760;
    ui_shear_rect(bx, fy, 520, 76, 18, gfx_rgba(0.12f, 0.14f, 0.12f, 0.95f));
    if (app.songs.count > 0) {
        const song_entry *e = &app.songs.items[app.sel];
        float st = stars_get(e->file, stars_mods(app.mods));
        if (st < 0)
            st = stars_get(e->file, 0);
        if (st >= 0)
            star_pill(bx + 30, fy + 20, 36, st);
        const osu5_peek_info *in = &e->info;
        static const char *lab[4] = {"CS", "AR", "OD", "HP"};
        float v[4] = {in->cs, in->ar, in->od, in->hp};
        for (int k = 0; k < 4; k++) {
            draw_text(bx + 230 + k * 70, fy + 12, 18, gfx_rgba(1, 1, 1, 0.9f), ALIGN_CENTRE, lab[k]);
            snprintf(buf, sizeof(buf), "%.1f", v[k]);
            draw_text(bx + 230 + k * 70, fy + 40, 18, gfx_rgba(1, 1, 1, 0.75f), ALIGN_CENTRE, buf);
        }
    }
    ui_shear_rect(GFX_W - 230, fy, 210, 76, 18, gfx_rgba(0.12f, 0.14f, 0.12f, 0.95f));
    snprintf(buf, sizeof(buf), "%.2fx", mod_multiplier(app.mods));
    draw_text(GFX_W - 125, fy + 14, 20, gfx_rgba(1, 1, 1, 0.6f), ALIGN_CENTRE, "score");
    draw_text(GFX_W - 125, fy + 38, 26, ui_hex(UI_YELLOW, 1), ALIGN_CENTRE, buf);
}

/* ---- settings ---- */

/* grouped the way the screen shows them */
enum {
    SET_HELP, SET_UPDATE,                       /* general */
    SET_SKIN,                                   /* skin */
    SET_SENS, SET_CURSOR,                       /* input */
    SET_DIM, SET_STORYBOARD, SET_VIDEO,         /* gameplay */
    SET_MUSIC, SET_EFFECTS, SET_OFFSET, SET_WIZARD, /* audio */
    SET_ACCOUNT, SET_BOARD,                     /* online */
    SET_RESCAN,                                 /* maintenance */
    SET_COUNT
};

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
    case SET_SKIN:
        if (pad->pressed & PAD_CROSS) {
            skin_ui_open(false);
            app.scr = SCREEN_SKINS;
        }
        break;
    case SET_UPDATE:
        if (pad->pressed & PAD_CROSS) {
            update_check();
            snprintf(app.status, sizeof(app.status), "checking for a new version...");
        }
        break;
    case SET_HELP:
        if (pad->pressed & PAD_CROSS) {
            help_open(SCREEN_SETTINGS);
            return;
        }
        break;
    case SET_ACCOUNT:
        if (pad->pressed & PAD_CROSS) {
            app.acc_row = 0;
            app.scr = SCREEN_ACCOUNT;
            return;
        }
        break;
    case SET_BOARD:
        if (d || pad->pressed & PAD_CROSS)
            app.board_src = !app.board_src;
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
    static const struct { int first; const char *name; } sections[] = {
        {SET_HELP, "general"}, {SET_SKIN, "skin"}, {SET_SENS, "input"}, {SET_DIM, "gameplay"},
        {SET_MUSIC, "audio"}, {SET_ACCOUNT, "online"}, {SET_RESCAN, "maintenance"},
    };
    const int nsec = sizeof(sections) / sizeof(sections[0]);
    int cur = 0;
    for (int s = 0; s < nsec; s++)
        if (app.set_sel >= sections[s].first)
            cur = s;

    char val[64];
    draw_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0, 0, 0, 0.45f));

    /* sidebar of sections, then the panel the rows live on */
    const float side_w = 220, panel_x = side_w, panel_w = 900;
    draw_rect(0, 0, side_w, GFX_H, gfx_rgba(0.1f, 0.09f, 0.12f, 0.97f));
    draw_rect(panel_x, 0, panel_w, GFX_H, gfx_rgba(0.16f, 0.14f, 0.2f, 0.97f));
    for (int s = 0; s < nsec; s++) {
        float y = 120 + s * 64;
        bool on = s == cur;
        if (on)
            ui_round_rect(14, y - 6, 6, 44, 3, ui_hex(UI_PURPLE, 1));
        draw_text(40, y, 26, gfx_rgba(1, 1, 1, on ? 1 : 0.55f), ALIGN_LEFT, sections[s].name);
    }

    /* rows with a header above each section; scrolled to keep the selection in view */
    const float row_h = 58, head_h = 70;
    float ys[SET_COUNT], y = 190;
    for (int i = 0, s = 0; i < SET_COUNT; i++) {
        if (s < nsec && i == sections[s].first) {
            y += head_h;
            s++;
        }
        ys[i] = y;
        y += row_h;
    }
    static float scroll;
    float want = ys[app.set_sel] - GFX_H * 0.45f;
    if (want < 0) want = 0;
    if (want > y - GFX_H + 120) want = fmaxf(0, y - GFX_H + 120);
    scroll = ui_approach(scroll, want, 0.25f);

    draw_text(panel_x + 50, 40 - scroll, 52, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, "settings");
    draw_text(panel_x + 52, 104 - scroll, 22, gfx_rgba(1, 1, 1, 0.6f), ALIGN_LEFT, "how ProjectCadence plays, sounds and looks");

    for (int s = 0; s < nsec; s++)
        draw_text(panel_x + 50, ys[sections[s].first] - head_h + 22 - scroll, 32, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, sections[s].name);

    for (int i = 0; i < SET_COUNT; i++) {
        float ry = ys[i] - scroll;
        if (ry < -row_h || ry > GFX_H)
            continue;
        const char *name = "";
        int kind = 0; /* 0 text, 1 toggle, 2 slider, 3 button */
        float level = 0;
        bool flag = false;
        val[0] = 0;
        switch (i) {
        case SET_MUSIC: name = "music volume"; kind = 2; level = app.music_vol; snprintf(val, sizeof(val), "%d%%", (int)lroundf(app.music_vol * 100)); break;
        case SET_EFFECTS: name = "effects volume"; kind = 2; level = app.fx_vol; snprintf(val, sizeof(val), "%d%%", (int)lroundf(app.fx_vol * 100)); break;
        case SET_OFFSET: name = "audio offset"; snprintf(val, sizeof(val), "%+d ms", app.offset_ms); break;
        case SET_SENS: name = "left stick reach"; snprintf(val, sizeof(val), "%.2fx", app.sensitivity); break;
        case SET_DIM: name = "background dim in play"; kind = 2; level = app.bg_dim; snprintf(val, sizeof(val), "%d%%", (int)lroundf(app.bg_dim * 100)); break;
        case SET_CURSOR: name = "cursor size"; snprintf(val, sizeof(val), "%.1fx", app.cursor_scale); break;
        case SET_STORYBOARD: name = "storyboards"; kind = 1; flag = app.storyboards; break;
        case SET_VIDEO: name = "video backgrounds (mp4)"; kind = 1; flag = app.videos; break;
        case SET_WIZARD: name = "find my offset"; kind = 3; break;
        case SET_HELP: name = "show the controls"; kind = 3; break;
        case SET_ACCOUNT:
            name = "ProjectCadence account";
            snprintf(val, sizeof(val), "%s", account_signed_in() ? account_name() : "sign in");
            break;
        case SET_BOARD:
            name = "online leaderboard";
            snprintf(val, sizeof(val), "%s", !app.board_src ? "ProjectCadence"
                     : online_enabled() ? "osu.ppy.sh" : "osu.ppy.sh (needs osuapi.txt)");
            break;
        case SET_RESCAN: name = "rescan the Songs folder"; kind = 3; break;
        case SET_SKIN: name = "current skin"; snprintf(val, sizeof(val), "%.40s", skins_active()); break;
        case SET_UPDATE: {
            update_info up;
            update_status(&up);
            name = "check for updates";
            kind = up.state == UPDATE_AVAILABLE ? 0 : 3;
            if (kind == 0)
                snprintf(val, sizeof(val), "%s is out", up.latest);
            break;
        }
        }
        bool on = i == app.set_sel;
        float x = panel_x + 40, w = panel_w - 80;
        if (kind == 3) {
            /* actions are full-width coloured buttons */
            ui_round_rect(x, ry + 4, w, row_h - 10, 10, on ? ui_mix(ui_hex(UI_PURPLE, 1), gfx_rgba(1, 1, 1, 1), 0.2f) : ui_hex(UI_PURPLE, 0.85f));
            draw_text(x + w / 2, ry + 15, 24, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, name);
            continue;
        }
        if (on)
            ui_round_rect(x, ry, w, row_h - 4, 10, gfx_rgba(1, 1, 1, 0.1f));
        draw_text(x + 20, ry + 14, 24, gfx_rgba(1, 1, 1, on ? 1 : 0.85f), ALIGN_LEFT, name);
        float vx = x + w - 20;
        if (kind == 1) {
            uint32_t tc = flag ? ui_hex(UI_PURPLE, 1) : gfx_rgba(1, 1, 1, 0.12f);
            ui_round_rect(vx - 64, ry + 13, 64, 28, 14, tc);
            ui_disc(flag ? vx - 14 : vx - 50, ry + 27, 10, gfx_rgba(1, 1, 1, flag ? 1 : 0.6f));
        } else if (kind == 2) {
            float bw = 260;
            ui_round_rect(vx - bw, ry + 24, bw, 8, 4, gfx_rgba(1, 1, 1, 0.15f));
            ui_round_rect(vx - bw, ry + 24, bw * clampf(level, 0, 1), 8, 4, ui_hex(UI_PURPLE, 1));
            ui_disc(vx - bw + bw * clampf(level, 0, 1), ry + 28, on ? 11 : 8, gfx_rgba(1, 1, 1, 1));
            draw_text(vx - bw - 20, ry + 16, 20, gfx_rgba(1, 1, 1, 0.75f), ALIGN_RIGHT, val);
        } else {
            draw_text(vx, ry + 14, 24, on ? ui_hex(UI_YELLOW, 1) : gfx_rgba(1, 1, 1, 0.7f), ALIGN_RIGHT, val);
        }
    }

    /* hints for the selected row down the right */
    const char *hint = "";
    switch (app.set_sel) {
    case SET_OFFSET: hint = "positive if you hit late everywhere; L1/R1 in song select change it by 5 too"; break;
    case SET_SENS: hint = "how far a full stick tilt reaches; above 1 you can reach the edge before full tilt"; break;
    case SET_RESCAN: hint = "after copying maps over FTP"; break;
    default: break;
    }
    float hx = panel_x + panel_w + 50;
    if (hint[0])
        draw_text(hx, GFX_H - 160, 22, gfx_rgba(1, 1, 1, 0.8f), ALIGN_LEFT, hint);
    if (app.status[0])
        draw_text(hx, GFX_H - 120, 24, ui_hex(UI_YELLOW, 1), ALIGN_LEFT, app.status);
    draw_text(GFX_W - 40, GFX_H - 60, 20, gfx_rgba(1, 1, 1, 0.55f), ALIGN_RIGHT, "LEFT / RIGHT change   CROSS pick   CIRCLE back");
}

/* ---- pause / fail / results ---- */

/* pause and fail: a yellow heading over a stack of wide buttons, each with its pad button */
typedef struct {
    const char *label, *button;
    uint32_t colour;
} overlay_button;

/* lazer's pause and fail dialog: a spaced-out yellow title, full-width leaning
 * buttons with triangles drifting in them, and the attempt's numbers underneath */
static void render_overlay(const char *title, const overlay_button *b, int n)
{
    bool failed = !strcmp(title, "failed");
    draw_rect(0, 0, GFX_W, GFX_H, failed ? gfx_rgba(0.12f, 0, 0, 0.82f) : gfx_rgba(0, 0, 0, 0.75f));

    /* letter by letter so it can breathe like the original's */
    float tw = 0, gap = 14, size = 52;
    char ch[2] = {0, 0};
    for (const char *p = title; *p; p++) {
        ch[0] = *p;
        tw += draw_text_width(size, ch) + gap;
    }
    float tx = (GFX_W - tw + gap) / 2;
    for (const char *p = title; *p; p++) {
        ch[0] = *p;
        tx += draw_text(tx, 162, size, ui_hex(UI_YELLOW, 1), ALIGN_LEFT, ch) + gap;
    }

    const float w = 1443, h = 111, lean = 19, x = (GFX_W - w) / 2;
    float y = failed ? 300 : 369;
    for (int i = 0; i < n; i++) {
        uint32_t c = ui_hex(b[i].colour, 1);
        ui_shear_rect(x, y, w, h, lean, c);
        ui_triangles(x + 40, y, w - 80, h, ui_mix(c, gfx_rgba(1, 1, 1, 1), 0.18f), wall_ms(), (unsigned)i * 31 + 7, 22);
        draw_text(GFX_W / 2, y + h / 2 - 17, 34, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, b[i].label);
        draw_text(x + w - 60, y + h / 2 - 10, 18, gfx_rgba(1, 1, 1, 0.6f), ALIGN_RIGHT, b[i].button);
        y += h + 4;
    }

    char line[64];
    const osu5_hud *hd = &app.last_hud;
    float iy = 860;
    snprintf(line, sizeof(line), "retry count: %d", play_retries);
    draw_text(GFX_W / 2, iy, 24, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, line);
    snprintf(line, sizeof(line), "progress: %d%%", (int)(hd->progress * 100));
    draw_text(GFX_W / 2, iy + 30, 24, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, line);
    snprintf(line, sizeof(line), "accuracy: %.2f%%", hd->accuracy * 100);
    draw_text(GFX_W / 2, iy + 60, 24, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, line);
}

static void render_results(void)
{
    const osu5_hud *h = &app.last_hud;
    char buf[128];

    draw_background(0.7f);
    ui_shadow(260, 140, 1400, 800, 24);
    ui_round_rect(260, 140, 1400, 800, 24, gfx_rgba(0.1f, 0.09f, 0.13f, 0.92f));
    /* the left card holds the ring, like a score panel */
    ui_round_rect(300, 240, 520, 660, 20, gfx_rgba(0.16f, 0.14f, 0.2f, 1));
    ui_triangles(300, 240, 520, 660, gfx_rgba(0.2f, 0.17f, 0.26f, 0.5f), wall_ms(), 3, 14);

    char title[200];
    osu5_string(OSU5_STR_TITLE, title, sizeof(title));
    draw_text(GFX_W / 2, 170, 40, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, title);

    /* accuracy ring: hits split by judgement around the rank, accuracy as the inner arc */
    float cx = 560, cy = 540, R = 210;
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

    if (app.mp_playing)
        mp_board(1680, 140, 230, true);
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

/* ---- multiplayer ---- */

#define MP_FILE DATA_DIR "/multiplayer.txt"
#define CADENCE_ROOMS "https://projectcadence-rooms.le-bg-de-genshin123.workers.dev"

static void mp_load_cfg(void)
{
    app.mp_kind = MP_CADENCE;
    snprintf(app.mp_rooms_url, sizeof(app.mp_rooms_url), "%s", CADENCE_ROOMS);
    app.mp_bancho[0] = app.mp_pass[0] = 0;
    snprintf(app.mp_user, sizeof(app.mp_user), "%s", app.ps_name[0] ? app.ps_name : "player");
    FILE *f = fopen(MP_FILE, "r");
    if (!f)
        return;
    char line[200];
    char *fields[] = {NULL, app.mp_rooms_url, app.mp_bancho, app.mp_user, app.mp_pass};
    size_t caps[] = {0, sizeof(app.mp_rooms_url), sizeof(app.mp_bancho), sizeof(app.mp_user), sizeof(app.mp_pass)};
    for (int i = 0; i < 5 && fgets(line, sizeof(line), f); i++) {
        line[strcspn(line, "\r\n")] = 0;
        if (i == 0)
            app.mp_kind = atoi(line) == MP_BANCHO ? MP_BANCHO : MP_CADENCE;
        else if (line[0] || i == 4)
            snprintf(fields[i], caps[i], "%s", line);
    }
    fclose(f);
}

static void mp_save_cfg(void)
{
    FILE *f = fopen(MP_FILE, "w");
    if (!f)
        return;
    fprintf(f, "%d\n%s\n%s\n%s\n%s\n", app.mp_kind, app.mp_rooms_url, app.mp_bancho, app.mp_user, app.mp_pass);
    fclose(f);
}

/* what the on-screen keyboard is filling in on the multiplayer screen */
enum { TYPE_CFG, TYPE_CHAT, TYPE_NEW_PW, TYPE_JOIN_PW };

static void mp_type(int what, char *buf, size_t cap)
{
    app.mp_type_for = what;
    app.mp_kbd.text = buf;
    app.mp_kbd.cap = cap;
    app.mp_typing = true;
}

/* the lines L1 offers, for when typing is too slow */
static const char *const quick_lines[] = {
    "gl hf", "gg", "ready?", "one more?", "wait for me", "nice one", "pick something harder", "brb",
};
#define QUICK_COUNT (int)(sizeof(quick_lines) / sizeof(quick_lines[0]))

static void mp_open(void)
{
    if (!app.mp_on)
        mp_load_cfg();
    app.mp_sel = 0;
    app.scr = SCREEN_MP;
}

static void mp_report(bool failed, bool done)
{
    const osu5_hud *h = &app.last_hud;
    mp_score(h->score, h->combo, h->max_combo, h->accuracy, h->great, h->ok, h->meh, h->miss, failed, done);
    if (done)
        app.mp_reported = true;
}

/* the room's map in our library, -1 if we don't have it */
static int mp_find_map(const mp_state *s)
{
    static char cached_for[48];
    static int cached = -1, cached_count = -1;
    char key[48];
    snprintf(key, sizeof(key), "%d:%s", s->map_id, s->map_md5);
    if (!strcmp(key, cached_for) && cached_count == app.all.count)
        return cached;
    snprintf(cached_for, sizeof(cached_for), "%s", key);
    cached_count = app.all.count;
    cached = -1;
    for (int i = 0; i < app.all.count && cached < 0; i++)
        if (s->map_id > 0 && app.all.items[i].info.beatmap_id == s->map_id)
            cached = i;
    /* maps that never got an id: hash the difficulties of the right set */
    for (int i = 0; i < app.all.count && cached < 0 && s->map_md5[0]; i++) {
        const song_entry *e = &app.all.items[i];
        if (s->map_set > 0 && e->info.set_id != s->map_set)
            continue;
        if (s->map_set <= 0 && s->map_id > 0)
            break;
        char md5[33];
        if (md5_file(e->file, md5) && !strcmp(md5, s->map_md5))
            cached = i;
    }
    return cached;
}

static void mp_tick(void)
{
    if (!app.mp_on)
        return;
    mp_snapshot(&app.mps);
    const mp_state *s = &app.mps;

    /* maps the room wants that we don't have come in through the downloader */
    mapdl_status dl;
    mapdl_progress(&dl);
    if (dl.installed != app.seen_installs && !dl.current) {
        app.seen_installs = dl.installed;
        rescan();
    }
    if (s->in_room && (s->map_id > 0 || s->map_md5[0]) && mp_find_map(s) < 0) {
        char key[33];
        snprintf(key, sizeof(key), "%d:%.24s", s->map_id, s->map_md5);
        if (strcmp(key, app.mp_map_wanted)) {
            snprintf(app.mp_map_wanted, sizeof(app.mp_map_wanted), "%s", key);
            mapdl_set want;
            memset(&want, 0, sizeof(want));
            want.id = s->map_set;
            want.beatmap = s->map_id;
            snprintf(want.title, sizeof(want.title), "%.120s", s->map_title);
            mapdl_queue(&want);
            app.mp_ready = false;
            mp_ready(false, false);
        }
    }

    /* a round starting */
    if (s->in_room && s->state == MP_PLAYING && s->round != app.mp_round_seen && s->go_at > 0 && wall_ms() >= s->go_at &&
        (app.scr == SCREEN_MP || app.scr == SCREEN_SELECT)) {
        app.mp_round_seen = s->round;
        app.mp_pick = false;
        int idx = mp_find_map(s);
        if (idx >= 0) {
            app.mp_saved_mods = app.mods;
            /* free mods: everyone keeps their own */
            if (!s->freemod)
                app.mods = s->mods;
            app.mp_playing = true;
            app.mp_reported = false;
            app.mp_sent = 0;
            if (!start_song(&app.all.items[idx])) {
                app.mp_playing = false;
                app.mods = app.mp_saved_mods;
                mp_score(0, 0, 0, 0, 0, 0, 0, 0, true, true);
                app.scr = SCREEN_MP;
            }
        } else {
            mp_score(0, 0, 0, 0, 0, 0, 0, 0, true, true); /* sit this one out */
        }
    }
}

static void mp_frame(const pad_state *pad)
{
    const mp_state *s = &app.mps;
    if (app.mp_typing) {
        int k = kbd_frame(&app.mp_kbd, pad);
        if (k == KBD_TYPING)
            return;
        app.mp_typing = false;
        bool done = k == KBD_DONE;
        switch (app.mp_type_for) {
        case TYPE_CFG:
            mp_save_cfg();
            break;
        case TYPE_CHAT:
            if (done && app.mp_line[0])
                mp_chat(app.mp_line);
            app.mp_line[0] = 0;
            break;
        case TYPE_NEW_PW:
            if (done) {
                char name[64];
                snprintf(name, sizeof(name), "%.24s's room", app.mp_user);
                mp_create(name, app.mp_pw);
            }
            break;
        case TYPE_JOIN_PW:
            if (done)
                mp_join(app.mp_join_id, app.mp_pw);
            break;
        }
        return;
    }
    bool bancho = app.mp_kind == MP_BANCHO;

    if (!app.mp_on || s->conn == MP_FAILED || s->conn == MP_OFF) {
        /* the connect form: server, address, name, password, connect */
        if (app.mp_on && (s->conn == MP_FAILED || s->conn == MP_OFF))
            app.mp_on = false;
        int rows = 5;
        app.mp_cfg = (app.mp_cfg + pad->nav_y + rows) % rows;
        if (!bancho && app.mp_cfg == 3)
            app.mp_cfg = pad->nav_y < 0 ? 2 : 4;
        if (app.mp_cfg == 0 && (pad->nav_x || pad->pressed & PAD_CROSS)) {
            app.mp_kind = bancho ? MP_CADENCE : MP_BANCHO;
            mp_save_cfg();
        } else if (pad->pressed & PAD_CROSS && app.mp_cfg >= 1 && app.mp_cfg <= 3) {
            char *field = app.mp_cfg == 1 ? (bancho ? app.mp_bancho : app.mp_rooms_url) : app.mp_cfg == 2 ? app.mp_user : app.mp_pass;
            size_t cap = app.mp_cfg == 1 ? (bancho ? sizeof(app.mp_bancho) : sizeof(app.mp_rooms_url))
                       : app.mp_cfg == 2 ? sizeof(app.mp_user) : sizeof(app.mp_pass);
            mp_type(TYPE_CFG, field, cap);
        } else if (pad->pressed & PAD_CROSS && app.mp_cfg == 4) {
            if (bancho && (!app.mp_bancho[0] || !app.mp_pass[0])) {
                snprintf(app.status, sizeof(app.status), "a bancho server needs its address and your password");
            } else {
                app.status[0] = 0;
                mp_connect(app.mp_kind, bancho ? app.mp_bancho : app.mp_rooms_url, app.mp_user, app.mp_pass);
                app.mp_on = true;
                app.mp_sel = 0;
                app.mp_round_seen = 0;
                app.mp_map_wanted[0] = 0;
                mp_snapshot(&app.mps);
            }
        }
        if (pad->pressed & PAD_CIRCLE)
            menu_enter();
        return;
    }

    if (s->conn == MP_CONNECTING) {
        if (pad->pressed & PAD_CIRCLE) {
            mp_disconnect();
            app.mp_on = false;
        }
        return;
    }

    if (!s->in_room) {
        /* make a room, make a private one, then the open rooms */
        int rows = 2 + s->nrooms;
        app.mp_sel = (app.mp_sel + pad->nav_y + rows) % rows;
        if (pad->pressed & PAD_CROSS) {
            app.mp_ready = false;
            app.mp_pw[0] = 0;
            if (app.mp_sel == 0) {
                char name[64];
                snprintf(name, sizeof(name), "%.24s's room", app.mp_user);
                mp_create(name, "");
            } else if (app.mp_sel == 1) {
                mp_type(TYPE_NEW_PW, app.mp_pw, sizeof(app.mp_pw));
            } else {
                const mp_room_brief *r = &s->rooms[app.mp_sel - 2];
                app.mp_join_id = r->id;
                if (r->locked)
                    mp_type(TYPE_JOIN_PW, app.mp_pw, sizeof(app.mp_pw));
                else
                    mp_join(r->id, "");
            }
        }
        if (pad->pressed & PAD_TRIANGLE)
            mp_refresh();
        if (pad->pressed & PAD_CIRCLE) {
            mp_disconnect();
            app.mp_on = false;
        }
        return;
    }

    /* in a room; the quick lines list takes the pad while it's open */
    if (app.mp_quick >= 0) {
        app.mp_quick = (app.mp_quick + pad->nav_y + QUICK_COUNT) % QUICK_COUNT;
        if (pad->pressed & PAD_CROSS) {
            mp_chat(quick_lines[app.mp_quick]);
            app.mp_quick = -1;
        } else if (pad->pressed & (PAD_CIRCLE | PAD_L1)) {
            app.mp_quick = -1;
        }
        return;
    }
    bool have = mp_find_map(s) >= 0;
    bool between = s->state != MP_PLAYING;
    if (pad->pressed & PAD_CROSS && between) {
        app.mp_ready = !app.mp_ready && have;
        mp_ready(app.mp_ready, have);
    }
    if (pad->pressed & PAD_SQUARE && between) {
        if (s->i_host) {
            app.mp_pick = true;
            app.scr = SCREEN_SELECT;
        } else if (s->freemod) {
            /* your own mods, back here when done */
            app.mp_mods = true;
            app.mod_sel = 0;
            app.scr = SCREEN_MODS;
        }
    }
    if (s->i_host && pad->pressed & PAD_TRIANGLE && between)
        mp_start();
    if (s->i_host && pad->pressed & PAD_R2 && between)
        mp_settings(!s->teams, s->freemod);
    if (s->i_host && pad->pressed & PAD_L3 && between)
        mp_settings(s->teams, !s->freemod);
    if (s->teams && pad->pressed & PAD_L2 && between) {
        int mine = 0;
        for (int i = 0; i < s->nmembers; i++)
            if (s->members[i].me) mine = s->members[i].team;
        mp_team(!mine);
    }
    if (pad->pressed & PAD_R1)
        mp_type(TYPE_CHAT, app.mp_line, sizeof(app.mp_line));
    if (pad->pressed & PAD_L1)
        app.mp_quick = 0;
    if (pad->pressed & PAD_CIRCLE) {
        mp_leave();
        app.mp_ready = false;
        app.mp_sel = 0;
    }
}

static int by_score(const void *a, const void *b)
{
    const mp_member *x = a, *y = b;
    return y->score > x->score ? 1 : y->score < x->score ? -1 : 0;
}

/* everyone's score, best first; used in play and on results */
static void mp_board(float x, float y, float w, bool compact)
{
    mp_member m[MP_MAX_MEMBERS];
    int n = app.mps.nmembers;
    memcpy(m, app.mps.members, sizeof(m[0]) * (size_t)n);
    qsort(m, (size_t)n, sizeof(m[0]), by_score);
    char buf[96];
    float rh = compact ? 46 : 64;
    draw_rect(x, y, w, 12 + n * rh, gfx_rgba(0, 0, 0, 0.55f));
    for (int i = 0; i < n; i++) {
        float ry = y + 6 + i * rh;
        if (m[i].me)
            draw_rect(x, ry, w, rh - 4, gfx_rgba(0.5f, 0.9f, 0.95f, 0.18f));
        snprintf(buf, sizeof(buf), "%d  %.20s", i + 1, m[i].name);
        draw_text(x + 12, ry + 6, compact ? 20 : 24, m[i].failed ? gfx_rgba(1, 0.5f, 0.5f, 0.9f) : gfx_rgba(1, 1, 1, 0.95f),
                  ALIGN_LEFT, buf);
        if (!compact)
            snprintf(buf, sizeof(buf), "%d   %.2f%%   %dx", m[i].score, m[i].acc * 100, m[i].max_combo);
        else
            snprintf(buf, sizeof(buf), "%d   %dx", m[i].score, m[i].combo);
        draw_text(x + 12, ry + (compact ? 26 : 34), compact ? 16 : 20, gfx_rgba(1, 0.85f, 0.4f, 0.9f), ALIGN_LEFT, buf);
    }
}

/* a settings-style row: label left, value right, a lighter box and a purple
 * bar on the left when it's the one selected (lazer's settings rows) */
static void form_row(float x, float y, float w, const char *label, const char *value, bool on)
{
    ui_round_rect(x, y, w, 64, 8, on ? ui_hex(0x46444F, 1) : ui_hex(0x2C2A35, 0.96f));
    if (on)
        ui_round_rect(x, y + 12, 5, 40, 2.5f, ui_hex(0x7F5CEA, 1));
    draw_text(x + 26, y + 19, 24, gfx_rgba(1, 1, 1, 0.95f), ALIGN_LEFT, label);
    if (value && value[0]) {
        char v[200];
        snprintf(v, sizeof(v), "%s", value);
        ui_fit(v, 22, w * 0.6f);
        draw_text(x + w - 26, y + 20, 22, gfx_rgba(1, 1, 1, 0.7f), ALIGN_RIGHT, v);
    }
}

/* lazer's full-width action button: purple, or another colour, text centred */
static void form_button(float x, float y, float w, const char *label, uint32_t colour, bool on)
{
    uint32_t c = ui_hex(colour, 1);
    if (on) {
        c = ui_mix(c, gfx_rgba(1, 1, 1, 1), 0.18f);
        ui_round_rect(x - 3, y - 3, w + 6, 64, 10, gfx_rgba(1, 1, 1, 0.3f));
    }
    ui_round_rect(x, y, w, 58, 8, c);
    draw_text(x + w / 2, y + 16, 24, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, label);
}

static void render_mp(void)
{
    draw_background(0.75f);
    draw_rect(0, 0, GFX_W, GFX_H, ui_plum(0.07f, 0.72f));
    const mp_state *s = &app.mps;
    bool bancho = app.mp_kind == MP_BANCHO;
    char buf[300];

    if (!app.mp_on || s->conn == MP_FAILED || s->conn == MP_OFF) {
        /* connecting: a settings panel in the middle */
        ui_crumb(40, 40, 30, "multiplayer", "connect", ui_hex(UI_YELLOW, 1));
        const float px = 460, pw = 1000;
        ui_round_rect(px, 130, pw, 640, 12, ui_hex(0x302E39, 0.97f));
        draw_text(px + 40, 160, 34, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, "server");
        draw_text(px + 40, 204, 20, gfx_rgba(1, 1, 1, 0.55f), ALIGN_LEFT,
                  bancho ? "your account on that server; the password stays on this console" : "rooms run by ProjectCadence, no account needed");
        const char *labels[4] = {"server", bancho ? "bancho address" : "room server", "name", "password"};
        char vals[4][170];
        snprintf(vals[0], sizeof(vals[0]), "%s", bancho ? "bancho server (bancho.py)" : "ProjectCadence rooms");
        snprintf(vals[1], sizeof(vals[1]), "%s", bancho ? (app.mp_bancho[0] ? app.mp_bancho : "e.g. example.com") : app.mp_rooms_url);
        snprintf(vals[2], sizeof(vals[2]), "%s", app.mp_user);
        size_t pl = strlen(app.mp_pass);
        memset(vals[3], '*', pl < 20 ? pl : 20);
        vals[3][pl < 20 ? pl : 20] = 0;
        float y = 260;
        for (int i = 0; i < 4; i++) {
            if (i == 3 && !bancho)
                continue;
            form_row(px + 30, y, pw - 60, labels[i], vals[i], i == app.mp_cfg);
            y += 74;
        }
        form_button(px + 30, y + 16, pw - 60, "connect", 0x6644CC, app.mp_cfg == 4);
        const char *err = s->error[0] ? s->error : app.status;
        if (err[0])
            draw_text(px + 40, y + 100, 22, ui_hex(0xFF6666, 1), ALIGN_LEFT, err);
        ui_footer("CROSS change", "CIRCLE");
        if (app.mp_typing)
            kbd_render(&app.mp_kbd);
        return;
    }

    if (s->conn == MP_CONNECTING) {
        ui_crumb(40, 40, 30, "multiplayer", "connecting", ui_hex(UI_YELLOW, 1));
        draw_text(GFX_W / 2, 500, 32, gfx_rgba(1, 1, 1, 0.8f), ALIGN_CENTRE, "connecting...");
        return;
    }

    if (s->online > 0)
        snprintf(buf, sizeof(buf), "%s as %s  -  %d online", bancho ? app.mp_bancho : "ProjectCadence rooms", app.mp_user, s->online);
    else
        snprintf(buf, sizeof(buf), "%s as %s", bancho ? app.mp_bancho : "ProjectCadence rooms", app.mp_user);
    draw_text(GFX_W - 40, 46, 18, gfx_rgba(1, 1, 1, 0.5f), ALIGN_RIGHT, buf);

    if (!s->in_room) {
        /* the lounge: one wide card per room, create buttons top left */
        ui_crumb(40, 40, 30, "multiplayer", "lounge", ui_hex(UI_YELLOW, 1));
        const float cx = 225, cw = 1470, ch = 106, gap = 10;
        for (int b = 0; b < 2; b++) {
            bool on = app.mp_sel == b;
            float bx = 40 + b * 290;
            if (on)
                ui_round_rect(bx - 3, 97, 276, 76, 10, gfx_rgba(1, 1, 1, 0.3f));
            ui_round_rect(bx, 100, 270, 70, 8, on ? ui_mix(ui_hex(0x5E3FBA, 1), gfx_rgba(1, 1, 1, 1), 0.2f) : ui_hex(0x5E3FBA, 1));
            draw_text(bx + 135, 122, 21, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, b == 0 ? "create room" : "create private room");
        }

        static float scroll;
        float want = app.mp_sel >= 2 ? (app.mp_sel - 2) * (ch + gap) - 3 * (ch + gap) : 0;
        scroll = ui_approach(scroll, want < 0 ? 0 : want, 0.25f);
        for (int i = 0; i < s->nrooms; i++) {
            const mp_room_brief *r = &s->rooms[i];
            float y = 200 + i * (ch + gap) - scroll;
            if (y < 190 || y + ch > GFX_H - 90)
                continue;
            bool on = app.mp_sel == i + 2;
            uint32_t state = r->playing ? 0x8866EE : r->locked ? UI_PINK : 0xB3FF66;
            if (on)
                ui_round_rect(cx - 3, y - 3, cw + 6, ch + 6, 13, gfx_rgba(1, 1, 1, 0.85f));
            ui_round_rect(cx, y, cw, ch, 10, ui_plum(0.16f, 0.97f));
            ui_triangles(cx + cw * 0.5f, y, cw * 0.5f, ch, ui_plum(0.22f, 0.6f), wall_ms(), (unsigned)i + 11, 8);
            ui_round_rect(cx, y, 10, ch, 5, ui_hex(state, 1));

            const char *st = r->playing ? "playing" : r->locked ? "open (private)" : "open";
            ui_pill(cx + 26, y + 10, 13, ui_hex(state, 1), ui_hex(0x222222, 1), st);
            snprintf(buf, sizeof(buf), "%s", r->name);
            ui_fit(buf, 24, cw * 0.5f);
            draw_text(cx + 26, y + 36, 24, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, buf);
            if (r->map[0]) {
                float w = draw_text(cx + 26, y + 70, 15, ui_hex(0xB3FF66, 1), ALIGN_LEFT, "currently playing ");
                snprintf(buf, sizeof(buf), "%s", r->map);
                ui_fit(buf, 15, cw * 0.6f - w);
                draw_text(cx + 26 + w, y + 70, 15, gfx_rgba(1, 1, 1, 0.7f), ALIGN_LEFT, buf);
            }
            /* players, on the right in a dark pill */
            snprintf(buf, sizeof(buf), "%d %s", r->players, r->players == 1 ? "player" : "players");
            float ww = draw_text_width(17, buf) + 44;
            ui_round_rect(cx + cw - ww - 24, y + ch / 2 - 21, ww, 42, 21, gfx_rgba(0, 0, 0, 0.45f));
            draw_text(cx + cw - 24 - ww / 2, y + ch / 2 - 11, 17, gfx_rgba(1, 1, 1, 0.9f), ALIGN_CENTRE, buf);
        }
        if (s->nrooms == 0)
            draw_text(GFX_W / 2, 420, 26, gfx_rgba(1, 1, 1, 0.5f), ALIGN_CENTRE, "no rooms open yet: make one");
        if (s->error[0])
            draw_text(cx, GFX_H - 130, 22, ui_hex(0xFF6666, 1), ALIGN_LEFT, s->error);
        ui_footer("CROSS join / create", "CIRCLE");
        return;
    }

    /* the room, laid out like lazer's: participants, beatmap and chat in three
     * columns, and the big ready button in the footer */
    ui_crumb(40, 40, 30, "multiplayer", s->room_name, ui_hex(UI_YELLOW, 1));
    const float top = 110, colh = GFX_H - 96 - top - 20;
    int ready = 0;
    for (int i = 0; i < s->nmembers; i++)
        ready += s->members[i].ready;

    /* participants */
    const float lx = 40, lw = 560;
    snprintf(buf, sizeof(buf), "participants (%d)", s->nmembers);
    draw_text(lx, top, 22, gfx_rgba(1, 1, 1, 0.9f), ALIGN_LEFT, buf);
    ui_round_rect(lx, top + 40, lw, colh - 40, 10, ui_plum(0.13f, 0.9f));
    if (s->state == MP_RESULTS) {
        mp_board(lx + 10, top + 50, lw - 20, true);
    } else {
        for (int i = 0; i < s->nmembers; i++) {
            const mp_member *m = &s->members[i];
            float y = top + 52 + i * 72;
            bool mods = s->freemod && m->mods;
            ui_round_rect(lx + 12, y, lw - 24, 64, 8, m->me ? ui_plum(0.26f, 1) : ui_plum(0.2f, 1));
            if (s->teams)
                ui_round_rect(lx + 12, y, 8, 64, 4, m->team ? ui_hex(0x4D8DFF, 1) : ui_hex(0xFF5A5A, 1));
            /* an initial in a disc stands in for the avatar */
            ui_disc(lx + 58, y + 32, 22, ui_plum(0.35f, 1));
            char ini[2] = {m->name[0] >= 'a' && m->name[0] <= 'z' ? (char)(m->name[0] - 32) : m->name[0], 0};
            draw_text(lx + 58, y + 19, 22, gfx_rgba(1, 1, 1, 0.9f), ALIGN_CENTRE, ini);
            float nw = draw_text(lx + 94, y + (mods ? 9 : 19), 22, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, m->name);
            if (m->host)
                ui_pill(lx + 104 + nw, y + (mods ? 10 : 20), 12, ui_hex(UI_YELLOW, 1), ui_hex(0x222222, 1), "host");
            if (mods) {
                char mt[48] = "";
                mod_tags(m->mods, mt, sizeof(mt));
                draw_text(lx + 94, y + 38, 15, ui_hex(UI_YELLOW, 1), ALIGN_LEFT, mt);
            }
            const char *tag;
            uint32_t tc;
            if (s->state == MP_PLAYING) {
                tag = m->done ? "results" : "playing";
                tc = m->done ? 0x66CCFF : 0x8866EE;
            } else if (!m->has_map) {
                tag = "no map";
                tc = 0xEEAA00;
            } else if (m->ready) {
                tag = "ready";
                tc = 0x88B300;
            } else {
                tag = "idle";
                tc = 0x555555;
            }
            float tw = draw_text_width(14, tag) + 17;
            ui_pill(lx + lw - 30 - tw, y + 20, 14, ui_hex(tc, 1), gfx_rgba(1, 1, 1, 1), tag);
        }
    }

    /* beatmap, mods and the room's rules */
    const float mx = 640, mw = 600;
    draw_text(mx, top, 22, gfx_rgba(1, 1, 1, 0.9f), ALIGN_LEFT, "beatmap");
    bool have = mp_find_map(s) >= 0;
    ui_round_rect(mx, top + 40, mw, 120, 10, ui_plum(0.2f, 0.97f));
    ui_round_rect(mx, top + 40, 120, 120, 10, ui_mix(ui_plum(0.25f, 1), ui_hex(0x6644CC, 1), 0.35f));
    ui_play_icon(mx + 60, top + 100, 34, gfx_rgba(1, 1, 1, 0.5f));
    if (s->map_title[0]) {
        snprintf(buf, sizeof(buf), "%s", s->map_title);
        ui_fit(buf, 22, mw - 150);
        draw_text(mx + 140, top + 64, 22, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, buf);
        if (!have)
            ui_pill(mx + 140, top + 110, 13, ui_hex(UI_BLUE, 1), ui_hex(0x222222, 1), "downloading");
    } else {
        draw_text(mx + 140, top + 86, 20, gfx_rgba(1, 1, 1, 0.6f), ALIGN_LEFT,
                  s->i_host ? "pick a map with SQUARE" : "the host is picking a map");
    }
    draw_text(mx, top + 190, 22, gfx_rgba(1, 1, 1, 0.9f), ALIGN_LEFT, s->freemod ? "free mods" : "required mods");
    char tags[64];
    mod_tags(s->mods, tags, sizeof(tags));
    if (tags[0]) {
        /* one pill per tag */
        float tx = mx;
        for (char *t = tags; *t;) {
            char one[8];
            size_t k = 0;
            while (*t && *t != ' ' && k < sizeof(one) - 1)
                one[k++] = *t++;
            one[k] = 0;
            while (*t == ' ')
                t++;
            if (k)
                tx += ui_pill(tx, top + 228, 16, ui_hex(0x8866EE, 1), gfx_rgba(1, 1, 1, 1), one) + 8;
        }
    } else {
        draw_text(mx, top + 230, 18, gfx_rgba(1, 1, 1, 0.45f), ALIGN_LEFT, "none");
    }
    draw_text(mx, top + 290, 22, gfx_rgba(1, 1, 1, 0.9f), ALIGN_LEFT, "room");
    float rx = mx;
    rx += ui_pill(rx, top + 328, 15, ui_plum(0.3f, 1), gfx_rgba(1, 1, 1, 1), s->teams ? "team versus" : "head to head") + 8;
    ui_pill(rx, top + 328, 15, ui_plum(0.3f, 1), gfx_rgba(1, 1, 1, 1), s->freemod ? "free mods" : "host's mods");

    /* chat, newest at the bottom */
    const float cx = 1280, cw = 600;
    draw_text(cx, top, 22, gfx_rgba(1, 1, 1, 0.9f), ALIGN_LEFT, "chat");
    float cy = top + 40, chh = colh - 40;
    ui_round_rect(cx, cy, cw, chh, 10, ui_plum(0.13f, 0.9f));
    ui_round_rect(cx + 12, cy + chh - 52, cw - 24, 40, 8, ui_plum(0.2f, 1));
    draw_text(cx + 26, cy + chh - 44, 17, gfx_rgba(1, 1, 1, 0.45f), ALIGN_LEFT, "R1 type a message   L1 quick chat");
    int lines = (int)(chh - 80) / 28;
    for (int i = 0; i < lines && i < s->nchat; i++) {
        int k = s->nchat - 1 - i;
        float ly = cy + chh - 92 - i * 28;
        if (s->chat[k].from[0]) {
            float w = draw_text(cx + 20, ly, 17, ui_hex(0x8FC3E8, 1), ALIGN_LEFT, s->chat[k].from);
            snprintf(buf, sizeof(buf), "%s", s->chat[k].text);
            ui_fit(buf, 17, cw - 50 - w);
            draw_text(cx + 30 + w, ly, 17, gfx_rgba(1, 1, 1, 0.92f), ALIGN_LEFT, buf);
        } else {
            snprintf(buf, sizeof(buf), "%s", s->chat[k].text);
            ui_fit(buf, 17, cw - 40);
            draw_text(cx + 20, ly, 17, ui_hex(UI_YELLOW, 0.85f), ALIGN_LEFT, buf);
        }
    }

    /* footer: back, what this player can change, and the big ready button */
    char hints[160];
    if (s->i_host)
        snprintf(hints, sizeof(hints), "SQUARE map   R2 %s   L3 %s%s", s->teams ? "no teams" : "teams",
                 s->freemod ? "host's mods" : "free mods", s->teams ? "   L2 team" : "");
    else
        snprintf(hints, sizeof(hints), "%s%s", s->freemod ? "SQUARE your mods" : "", s->teams ? "   L2 team" : "");
    ui_footer(hints, "CIRCLE");
    {
        bool all = ready == s->nmembers && s->nmembers > 0;
        char label[64];
        uint32_t c;
        if (s->i_host && all) {
            snprintf(label, sizeof(label), "start match  (TRIANGLE)");
            c = 0x88B300;
        } else if (app.mp_ready) {
            snprintf(label, sizeof(label), "ready %d/%d  (CROSS to cancel)", ready, s->nmembers);
            c = 0x88B300;
        } else {
            snprintf(label, sizeof(label), "ready  (CROSS)");
            c = 0xFFCC22;
        }
        float bw = 560, bx = GFX_W - bw - 40, by = GFX_H - 70;
        ui_shear_rect(bx, by, bw, 58, 14, ui_hex(c, 1));
        draw_text(bx + bw / 2, by + 15, 24, c == 0xFFCC22 ? ui_hex(0x222222, 1) : gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, label);
    }

    if (app.mp_quick >= 0) {
        ui_round_rect(700, 300, 520, 30 + QUICK_COUNT * 52, 12, ui_plum(0.1f, 0.97f));
        for (int i = 0; i < QUICK_COUNT; i++) {
            float qy = 315 + i * 52;
            if (i == app.mp_quick)
                ui_round_rect(710, qy - 4, 500, 46, 8, ui_hex(0x7F5CEA, 1));
            draw_text(730, qy + 6, 24, gfx_rgba(1, 1, 1, 0.95f), ALIGN_LEFT, quick_lines[i]);
        }
    }
    if (app.mp_typing)
        kbd_render(&app.mp_kbd);
    if (s->state == MP_PLAYING && s->go_at > 0) {
        double left = s->go_at - wall_ms();
        if (left > 0) {
            snprintf(buf, sizeof(buf), "starting in %.0f", ceil(left / 1000));
            draw_text(GFX_W / 2, 880, 44, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, buf);
        }
    }
    if (s->error[0])
        draw_text(40, GFX_H - 130, 22, ui_hex(0xFF6666, 1), ALIGN_LEFT, s->error);
}

/* ---- account: sign in or make one, for global scores and your name in rooms ---- */

enum { ACC_NAME, ACC_PASS, ACC_SIGN_IN, ACC_CREATE, ACC_SIGN_OUT, ACC_ROWS };

static void account_frame(const pad_state *pad)
{
    if (app.acc_typing) {
        int k = kbd_frame(&app.mp_kbd, pad);
        if (k != KBD_TYPING)
            app.acc_typing = false;
        return;
    }
    int rows = account_signed_in() ? ACC_ROWS : ACC_ROWS - 1;
    app.acc_row = (app.acc_row + pad->nav_y + rows) % rows;
    bool busy = account_state(NULL, 0) == ACC_BUSY;
    if (pad->pressed & PAD_CROSS && !busy) {
        switch (app.acc_row) {
        case ACC_NAME:
        case ACC_PASS:
            app.mp_kbd.text = app.acc_row == ACC_NAME ? app.acc_name : app.acc_pass;
            app.mp_kbd.cap = app.acc_row == ACC_NAME ? sizeof(app.acc_name) : sizeof(app.acc_pass);
            app.acc_typing = true;
            break;
        case ACC_SIGN_IN:
        case ACC_CREATE:
            account_sign_in(app.acc_name, app.acc_pass, app.acc_row == ACC_CREATE);
            break;
        case ACC_SIGN_OUT:
            account_sign_out();
            app.acc_row = 0;
            break;
        }
    }
    /* signed in: the password has done its job */
    if (account_state(NULL, 0) == ACC_OK && app.acc_pass[0])
        memset(app.acc_pass, 0, sizeof(app.acc_pass));
    if (pad->pressed & PAD_CIRCLE)
        app.scr = SCREEN_SETTINGS;
}

static void render_account(void)
{
    char msg[128], buf[160];
    int st = account_state(msg, sizeof(msg));
    draw_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0, 0, 0, 0.7f));
    /* lazer's sign-in panel, centred: a heading, two fields and the buttons */
    const float pw = 760, px = (GFX_W - pw) / 2, py = 170;
    int rows = account_signed_in() ? ACC_ROWS : ACC_ROWS - 1;
    ui_round_rect(px, py, pw, 250 + rows * 76, 14, ui_hex(0x302E39, 0.98f));
    draw_text(GFX_W / 2, py + 34, 34, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, account_signed_in() ? "signed in" : "account");
    if (account_signed_in())
        snprintf(buf, sizeof(buf), "as %s: your best scores go to the global board", account_name());
    else
        snprintf(buf, sizeof(buf), "puts your scores on the global board and your name in rooms");
    draw_text(GFX_W / 2, py + 84, 19, gfx_rgba(1, 1, 1, 0.55f), ALIGN_CENTRE, buf);

    char stars[24];
    size_t n = strlen(app.acc_pass);
    memset(stars, '*', n < 20 ? n : 20);
    stars[n < 20 ? n : 20] = 0;
    float y = py + 140;
    form_row(px + 40, y, pw - 80, "username", app.acc_name, app.acc_row == ACC_NAME);
    form_row(px + 40, y + 76, pw - 80, "password", stars, app.acc_row == ACC_PASS);
    form_button(px + 40, y + 170, pw - 80, "sign in", 0x6644CC, app.acc_row == ACC_SIGN_IN);
    form_button(px + 40, y + 240, pw - 80, "make a new account", 0x555555, app.acc_row == ACC_CREATE);
    if (rows == ACC_ROWS)
        form_button(px + 40, y + 310, pw - 80, "sign out", 0xAA1B27, app.acc_row == ACC_SIGN_OUT);

    const char *line = st == ACC_BUSY ? "talking to the server..." : st == ACC_FAILED ? msg : st == ACC_OK ? "done" : "";
    draw_text(GFX_W / 2, py + 250 + rows * 76 + 24, 22, st == ACC_FAILED ? ui_hex(0xFF6666, 1) : ui_hex(0xB3FF66, 1), ALIGN_CENTRE, line);
    ui_footer("CROSS edit / choose", "CIRCLE");
    if (app.acc_typing)
        kbd_render(&app.mp_kbd);
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
    {"multiplayer", {
        {"main menu", "multiplayer, then connect"},
        {"server", "ProjectCadence rooms, or a bancho.py server"},
        {"CROSS on a room", "join it, or make your own at the top"},
        {"CROSS in a room", "ready / not ready"},
        {"SQUARE, host", "pick the map (and its mods) in song select"},
        {"TRIANGLE, host", "start once everyone's ready"},
        {"CIRCLE", "leave the room"},
        {"missing maps", "download by themselves"},
    }},
    {"taiko, catch and mania", {
        {"catch: left stick or D-PAD", "move"},
        {"catch: any face button or shoulder", "dash"},
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

/* lazer's two rows: settings | play, skins, get maps, quit at the top, and play
 * opening back | solo, multi. The first entry of each row sits left of the logo */
enum { MB_SETTINGS, MB_PLAY, MB_SKINS, MB_MAPS, MB_QUIT, MB_BACK, MB_SOLO, MB_MULTI };
static const int menu_top[] = {MB_SETTINGS, MB_PLAY, MB_SKINS, MB_MAPS, MB_QUIT};
static const int menu_play[] = {MB_BACK, MB_SOLO, MB_MULTI};
static int menu_level; /* 0 top, 1 play */
int sceSystemServiceLoadExec(const char *path, char *const argv[]);

static int menu_row(const int **ids)
{
    if (menu_level) {
        *ids = menu_play;
        return (int)(sizeof(menu_play) / sizeof(menu_play[0]));
    }
    *ids = menu_top;
    return (int)(sizeof(menu_top) / sizeof(menu_top[0]));
}

static void menu_enter(void)
{
    app.scr = SCREEN_MENU;
    if (!app.logo_tex) {
        int w, h, n;
        unsigned char *px = stbi_load("/app0/sce_sys/icon0.png", &w, &h, &n, 4);
        app.menu_sel = 1; /* start on play, like lazer */
        if (px) {
            /* the icon is square; lazer's logo is round. Shrink the art into a
             * disc filled with the icon's own background so nothing gets cut */
            int N = w < h ? w : h;
            unsigned char *out = malloc((size_t)N * N * 4);
            if (out) {
                const unsigned char *bg = px + (2 * w + 2) * 4;
                for (int y = 0; y < N; y++) {
                    for (int x = 0; x < N; x++) {
                        float u = (x + 0.5f) / N - 0.5f, v = (y + 0.5f) / N - 0.5f;
                        float edge = (0.5f - sqrtf(u * u + v * v)) * N;
                        int sx = (int)((u / 0.84f + 0.5f) * w), sy = (int)((v / 0.84f + 0.5f) * h);
                        const unsigned char *src = sx >= 0 && sy >= 0 && sx < w && sy < h ? px + (sy * w + sx) * 4 : bg;
                        unsigned char *d = out + (y * N + x) * 4;
                        memcpy(d, src, 3);
                        d[3] = (unsigned char)(fminf(fmaxf(edge + 0.5f, 0), 1) * src[3]);
                    }
                }
                app.logo_tex = gfx_texture(N, N, out);
                free(out);
            }
            stbi_image_free(px);
        }
    }
}

static void menu_frame(const pad_state *pad)
{
    {
        const int *ids;
        int n = menu_row(&ids);
        app.menu_sel = (app.menu_sel + pad->nav_x + pad->nav_y + n) % n;
    }

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

    const int *ids;
    int n = menu_row(&ids);
    if (pad->pressed & PAD_CIRCLE && menu_level) {
        menu_level = 0;
        app.menu_sel = 1;
        return;
    }
    if (!(pad->pressed & PAD_CROSS))
        return;
    switch (ids[app.menu_sel < n ? app.menu_sel : 0]) {
    case MB_PLAY: menu_level = 1; app.menu_sel = 1; break;
    case MB_BACK: menu_level = 0; app.menu_sel = 1; break;
    case MB_SOLO: app.scr = SCREEN_SELECT; break;
    case MB_MULTI: mp_open(); break;
    case MB_MAPS: mapdl_ui_open(); app.scr = SCREEN_DOWNLOAD; break;
    case MB_SKINS: skin_ui_open(false); app.scr = SCREEN_SKINS; break;
    case MB_SETTINGS: app.set_sel = 0; app.scr = SCREEN_SETTINGS; break;
    case MB_QUIT: settings_save(); sceSystemServiceLoadExec("exit", NULL); break;
    }
}

static void render_menu(void)
{
    draw_background(0.45f);
    double now = wall_ms();
    float t = (float)(now / 1000.0);

    /* toolbar along the top */
    draw_rect(0, 0, GFX_W, 56, ui_hex(0x191919, 0.96f));
    draw_text(28, 14, 26, gfx_rgba(1, 1, 1, 0.95f), ALIGN_LEFT, "ProjectCadence");
    {
        char who[96];
        const char *name = account_signed_in() ? account_name() : app.ps_name[0] ? app.ps_name : "guest";
        int n = app.mp_on && app.mps.online > 0 ? app.mps.online
              : mp_peek_online(app.mp_kind, app.mp_kind == MP_BANCHO ? app.mp_bancho : app.mp_rooms_url);
        if (n >= 0)
            snprintf(who, sizeof(who), "%s    %d online", name, n);
        else
            snprintf(who, sizeof(who), "%s", name);
        draw_text(GFX_W - 28, 16, 22, gfx_rgba(1, 1, 1, 0.8f), ALIGN_RIGHT, who);
    }

    /* the button strip through the middle, the logo sitting on its left end;
     * sizes from lazer at 1080p: 133 tall, 181 wide, 25 of lean */
    const float bar_h = 133, bar_y = GFX_H / 2 - bar_h / 2, lean = 25, base_w = 181;
    float pulse = 1 + 0.025f * sinf(t * 2 * 3.14159f * 1.2f);
    float lx = 652, ly = GFX_H / 2, lsize = 284 * pulse;
    draw_rect(0, bar_y, GFX_W, bar_h, ui_hex(0x323232, 0.92f));

    static const char *names[] = {"settings", "play", "skins", "get maps", "quit", "back", "solo", "multi"};
    static const uint32_t cols[] = {UI_GREY, UI_PURPLE, UI_ORANGE, UI_LIME, 0xEE3399, 0x4A4AAB, 0x6644CC, 0x5E3FBA};
    static float widths[8];
    const int *ids;
    int n = menu_row(&ids);
    if (app.menu_sel >= n)
        app.menu_sel = 1;
    float x = 0;
    for (int k = 0; k < n; k++) {
        int i = ids[k];
        bool on = k == app.menu_sel;
        if (widths[i] <= 0)
            widths[i] = base_w;
        widths[i] = ui_approach(widths[i], on ? base_w * 1.5f : base_w, 0.25f);
        float w = widths[i];
        if (k == 0)
            x = lx - lsize * 0.5f - w - 20; /* grows to the left, away from the logo */
        else if (k == 1)
            x = lx + lsize * 0.5f - 5; /* the rest start at the logo's right edge */
        uint32_t c = ui_hex(cols[i], 1);
        if (on)
            c = ui_mix(c, gfx_rgba(1, 1, 1, 1), 0.12f);
        ui_shear_rect(x, bar_y, w + lean, bar_h, lean, c);

        /* the left one runs in under the logo, so centre on the part you can see */
        float vis_r = k == 0 ? lx - lsize * 0.5f - 14 : x + w + lean;
        float icx = (x + vis_r) / 2, icy = bar_y + bar_h * 0.44f - (on ? 5 * fabsf(sinf(t * 3.14159f * 1.2f)) : 0);
        uint32_t ic = gfx_rgba(1, 1, 1, 1);
        switch (i) {
        case MB_PLAY:
            arc(icx, icy, 20, 3, 0, 1, ic);
            ui_play_icon(icx + 1, icy, 18, ic);
            break;
        case MB_SOLO:
            arc(icx, icy, 20, 3, 0, 1, ic);
            ui_disc(icx, icy - 5, 6, ic);
            ui_round_rect(icx - 10, icy + 4, 20, 9, 4, ic);
            break;
        case MB_MULTI:
            arc(icx, icy, 20, 3, 0, 1, ic);
            draw_rect(icx - 20, icy - 1.5f, 40, 3, ic);
            arc(icx, icy, 9, 3, 0, 1, ic);
            break;
        case MB_MAPS:
            draw_rect(icx - 4, icy - 20, 8, 22, ic);
            ui_arrow_down(icx, icy + 8, 26, ic);
            draw_rect(icx - 18, icy + 18, 36, 5, ic);
            break;
        case MB_SKINS:
            arc(icx, icy, 20, 3, 0, 1, ic);
            ui_disc(icx, icy, 7, ic);
            break;
        case MB_QUIT:
        case MB_BACK:
            arc(icx, icy, 20, 3, 0, 1, ic);
            if (i == MB_BACK) {
                ui_arrow_left(icx - 3, icy, 14, ic);
                draw_rect(icx - 2, icy - 1.5f, 12, 3, ic);
            } else {
                gfx_bind(0);
                gfx_push(&(gfx_inst){icx, icy, 10, 1.5f, 0.785f, gfx_next_z(), ic, GK_RECT, {0}});
                gfx_push(&(gfx_inst){icx, icy, 10, 1.5f, -0.785f, gfx_next_z(), ic, GK_RECT, {0}});
            }
            break;
        case MB_SETTINGS: ui_gear_icon(icx, icy, 44, ic); break;
        }
        draw_text(icx, bar_y + bar_h - 40, 20, gfx_rgba(1, 1, 1, 0.95f), ALIGN_CENTRE, names[i]);
        x += w;
    }

    /* the logo, breathing; a flash ring on every beat */
    float ring = fmodf(t * 1.2f, 1.0f);
    arc(lx, ly, lsize * 0.5f + ring * 60, 4, 0, 1, gfx_rgba(1, 1, 1, 0.35f * (1 - ring)));
    for (int i = 3; i >= 1; i--)
        ui_disc(lx, ly + 6, lsize * 0.5f + 14 + i * 8, gfx_rgba(0, 0, 0, 0.08f));
    ui_disc(lx, ly, lsize * 0.5f + 14, gfx_rgba(1, 1, 1, 1));
    if (app.logo_tex) {
        gfx_bind(app.logo_tex);
        gfx_push(&(gfx_inst){lx, ly, lsize / 2, lsize / 2, 0, gfx_next_z(), gfx_rgba(1, 1, 1, 1), GK_TEX, {0, 0, 1, 1}});
        gfx_bind(0);
    }

    if (app.songs.count > 0) {
        const osu5_peek_info *in = &app.songs.items[app.sel].info;
        char buf[300];
        /* now playing, top right: title over artist */
        snprintf(buf, sizeof(buf), "%.100s", in->title);
        draw_text(GFX_W - 30, 68, 28, gfx_rgba(1, 1, 1, 0.95f), ALIGN_RIGHT, buf);
        snprintf(buf, sizeof(buf), "%.100s", in->artist);
        draw_text(GFX_W - 30, 102, 20, gfx_rgba(1, 1, 1, 0.75f), ALIGN_RIGHT, buf);
    }
    draw_text(GFX_W - 30, GFX_H - 50, 20, gfx_rgba(1, 1, 1, 0.5f), ALIGN_RIGHT, CADENCE_VERSION);

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
    if (sceUserServiceGetUserName(user, app.ps_name, sizeof(app.ps_name)) != 0)
        app.ps_name[0] = 0;
    LOG("user %d", user);

    if (!gfx_init())
        die("couldn't start Vulkan");
    draw_init();
    ui_init();
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
    mp_load_cfg();
    account_init(CADENCE_ROOMS);
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
        mp_tick();

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
            } else if (pad.pressed & PAD_TRIANGLE && !app.mp_playing) {
                play_retries++;
                start_song(app.playing);
            }
            break;
        case SCREEN_FAILED: {
            /* fade the music out, like stable's fail */
            double k = (wall_ms() - app.fail_time) / 1500.0;
            audio_music_volume(app.music_vol * (float)fmax(0, 1 - k));
            if (k > 1) audio_music_stop();
            if (pad.pressed & PAD_CROSS && !app.mp_playing) {
                play_retries++;
                start_song(app.playing);
            }
            else if (pad.pressed & (PAD_CIRCLE | PAD_CROSS)) back_to_select();
            break;
        }
        case SCREEN_RESULTS:
            if (app.mp_playing) {
                /* a room round: back to the room, no retries */
                if (pad.pressed & (PAD_CIRCLE | PAD_CROSS))
                    back_to_select();
            } else if (pad.pressed & PAD_CROSS) {
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
        case SCREEN_ACCOUNT:
            account_frame(&pad);
            break;
        case SCREEN_MP:
            mp_frame(&pad);
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
            {
                static const overlay_button pb[] = {
                    {"continue", "CROSS", 0x88B300}, {"retry", "TRIANGLE", 0xEEAA00}, {"quit", "CIRCLE", 0xAA1B27},
                };
                render_overlay("pause", pb, 3);
            }
            {
                char ob[64];
                snprintf(ob, sizeof(ob), "map offset %+d ms   (LEFT/RIGHT)", app.map_offset);
                draw_text(GFX_W / 2, 770, 22, gfx_rgba(1, 1, 1, 0.55f), ALIGN_CENTRE, ob);
            }
            break;
        case SCREEN_FAILED:
            render_play();
            {
                static const overlay_button fb[] = {{"retry", "CROSS", 0xEEAA00}, {"quit", "CIRCLE", 0xAA1B27}};
                render_overlay("failed", fb, 2);
            }
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
        case SCREEN_ACCOUNT:
            draw_background(0.7f);
            render_account();
            break;
        case SCREEN_MP:
            render_mp();
            break;
        }
        gfx_end();
    }
}
