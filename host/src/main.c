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
#include "web.h"
#include "bweb.h"
#include "collections.h"
#include "osr.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_WRITE
#include "../third_party/stb_image.h"

#include <dirent.h>
#include <sys/stat.h>
#include <math.h>
#include <stdarg.h>
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
    SCREEN_SETUP,
    SCREEN_RANKINGS,
    SCREEN_PROFILE,
    SCREEN_CHAT,
    SCREEN_SPECTATE,
    SCREEN_IMPORTS,
    SCREEN_SETINFO,
    SCREEN_READER,
} screen;

static struct {
    screen scr;
    song_list all;        /* everything in Songs/ */
    song_list songs;      /* what song select shows: filtered and sorted */
    char filter[64];
    int sort;
    int mode_pick;          /* 0 osu!, 1 taiko, 2 catch, 3 mania */
    float star_lo, star_hi; /* song select's star range, hi 0 = no top */
    bool converts;          /* osu! maps show up, converted, in the other modes */
    bool searching;
    char set_query[48];   /* settings search */
    kbd set_kbd;
    bool set_typing;
    int board;           /* song select: 0 your scores, 1 the online board */
    int board_src;        /* which online board: 0 ProjectCadence, 1 osu.ppy.sh, 2 the bancho server */
    bool bweb_waiting;    /* a server replay is downloading to be watched */
    int rp_from;          /* ...from 0 the bancho server, 1 ours */
    int coll_view;        /* song select shows only this collection, -1 everything */
    int hud_mode;         /* 0 always, 1 only in breaks, 2 never */
    bool key_overlay, live_board, classic_score;
    int map_objects;      /* for classic scoring */
    int key_count[4], key_prev;
    float da[4];          /* Difficulty Adjust: cs, ar, od, hp; -1 the map's own */
    double rate_custom;   /* the speed mods' rate, 0 for 1.5 / 0.75 */
    int mod_custom;       /* the customise panel's row in mod select, -1 closed */
    char set_entry[64], set_entry_keep[64]; /* settings' account fields */
    kbd set_entry_kbd;
    int set_entering;     /* 0 no, 1 clan, 2 old password, 3 new password, 4 delete */
    double clan_leave_armed;
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
    bool simple_ui; /* the plain look from before the lazer restyle */
    bool videos;
    int map_offset;       /* per map, from offsets.txt */
    bool fav_only;
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
} app = {.previewed = -1, .scores_for = -1, .new_place = -1, .mp_quick = -1, .coll_view = -1, .key_overlay = true, .live_board = true,
         .da = {-1, -1, -1, -1}, .mod_custom = -1};

/* what an osu! map gets converted to when another mode is picked */
static int conv_of(const song_entry *e)
{
    return e->info.mode == 0 && app.converts ? app.mode_pick : 0;
}

/* the mode a difficulty plays in right now: its own, or what it's converted to */
static int play_mode(const song_entry *e)
{
    int c = conv_of(e);
    return c ? c : e->info.mode;
}

static const char *base_name(const char *path)
{
    const char *b = strrchr(path, '/');
    return b ? b + 1 : path;
}

/* in overlays.inc, used before it */
static void chat_open(screen back);
static void rankings_open(screen back);
static void profile_lookup(screen back);
static bool coll_open(void);
static void coll_frame(const pad_state *pad);
static void render_coll(void);
static int coll_sel;
static void imports_open(void);
static void setinfo_open(int set_id);
static void reader_open(screen back);
static bool spec_live;
static int spec_target;
static void spectate_pull(void);
static void spectate_back(void);
static bool host_tools_open(void);
static void host_tools_frame(const pad_state *pad);
static void render_host_tools(void);
static int host_sel;

/* star lookups carry the convert in bits 28-29, so each mode gets its own rating */
static int star_flags(const song_entry *e, int mods)
{
    return stars_mods(mods) | conv_of(e) << 28;
}

/* scores of a convert live apart from the map's own */
static const char *score_path(const song_entry *e)
{
    static char buf[2][800];
    static int flip;
    int c = conv_of(e);
    if (!c)
        return e->file;
    flip ^= 1;
    snprintf(buf[flip], sizeof(buf[flip]), "%s#%d", e->file, c);
    return buf[flip];
}

static void help_open(screen back);
int sceUserServiceGetUserName(int32_t user, char *name, size_t size);
static void mp_report(bool failed, bool done);
static void menu_enter(void);
static bool setup_active; /* the first-run setup is open underneath */
static void setup_open(void);
static void want_converts(void);
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

/* toasts: slide in top right, go away by themselves */
static struct {
    char msg[128];
    double at;
} toasts[4];
static int ntoasts;

static void toast(const char *fmt, ...)
{
    if (ntoasts == 4) {
        memmove(toasts, toasts + 1, sizeof(toasts[0]) * 3);
        ntoasts--;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(toasts[ntoasts].msg, sizeof(toasts[0].msg), fmt, ap);
    va_end(ap);
    toasts[ntoasts++].at = wall_ms();
}

static void render_toasts(void)
{
    const double life = 3500, slide = 250;
    double now = wall_ms();
    int keep = 0;
    for (int i = 0; i < ntoasts; i++)
        if (now - toasts[i].at < life + slide)
            toasts[keep++] = toasts[i];
    ntoasts = keep;
    float y = 70;
    for (int i = ntoasts - 1; i >= 0; i--) {
        double age = now - toasts[i].at;
        float in = (float)fmin(1, age / slide), out = (float)fmin(1, fmax(0, (age - life) / slide));
        float k = in * in * (3 - 2 * in) * (1 - out);
        float w = 440, x = GFX_W - 20 - w * k;
        ui_round_rect(x, y, w, 64, 10, ui_hex(0x2A2833, 0.96f * k));
        ui_round_rect(x, y, 56, 64, 10, ui_hex(0x3A3747, k));
        ui_disc(x + 28, y + 32, 11, gfx_rgba(1, 1, 1, 0.9f * k));
        draw_text(x + 28, y + 21, 16, ui_hex(0x3A3747, k), ALIGN_CENTRE, "i");
        char m[128];
        snprintf(m, sizeof(m), "%s", toasts[i].msg);
        ui_fit(m, 18, w - 80);
        draw_text(x + 70, y + 21, 18, gfx_rgba(1, 1, 1, k), ALIGN_LEFT, m);
        y += 72;
    }
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
    app.converts = true;
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
            else if (!strcmp(key, "mode")) app.mode_pick = (int)v & 3;
            else if (!strcmp(key, "star_lo")) app.star_lo = v;
            else if (!strcmp(key, "star_hi")) app.star_hi = v;
            else if (!strcmp(key, "converts")) app.converts = v != 0;
            else if (!strcmp(key, "hud")) app.hud_mode = (int)v >= 0 && (int)v <= 2 ? (int)v : 0;
            else if (!strcmp(key, "key_overlay")) app.key_overlay = v != 0;
            else if (!strcmp(key, "live_board")) app.live_board = v != 0;
            else if (!strcmp(key, "classic_score")) app.classic_score = v != 0;
            else if (!strcmp(key, "storyboard")) app.storyboards = v != 0;
            else if (!strcmp(key, "simple_ui")) app.simple_ui = v != 0;
            else if (!strcmp(key, "video")) app.videos = v != 0;
            else if (!strcmp(key, "onboarded")) app.onboarded = v != 0;
            else if (!strcmp(key, "boards")) app.board_src = v >= 0 && v <= 2 ? (int)v : 0;
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
    fprintf(f, "offset %d\nsensitivity %.2f\nmusic %.2f\neffects %.2f\ndim %.2f\ncursor %.2f\nmods %d\nsort %d\nstoryboard %d\nvideo %d\nonboarded %d\nboards %d\nsimple_ui %d\nmode %d\nstar_lo %.1f\nstar_hi %.1f\nconverts %d\n"
               "hud %d\nkey_overlay %d\nlive_board %d\nclassic_score %d\n",
            app.offset_ms, app.sensitivity, app.music_vol, app.fx_vol, app.bg_dim, app.cursor_scale, app.mods & ~OSU5_MOD_AUTO,
            app.sort, app.storyboards, app.videos, app.onboarded, app.board_src, app.simple_ui, app.mode_pick, app.star_lo, app.star_hi, app.converts,
            app.hud_mode, app.key_overlay, app.live_board, app.classic_score);
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
    /* settings and fun; none of these go on the online boards */
    {OSU5_MOD_DIFFADJUST, "DA", "Difficulty Adjust", "set CS, AR, OD and HP yourself (OPTIONS)"},
    {OSU5_MOD_BARRELROLL, "BR", "Barrel Roll", "the playfield slowly turns"},
    {OSU5_MOD_TRACEABLE, "TC", "Traceable", "only the approach circles show"},
    {OSU5_MOD_NOSCOPE, "NS", "No Scope", "no cursor"},
    {OSU5_MOD_WINDUP, "WU", "Wind Up", "the song speeds up as it goes"},
};
/* mods that keep a play off the online boards */
#define UNRANKED_MODS (OSU5_MOD_DIFFADJUST | OSU5_MOD_BARRELROLL | OSU5_MOD_TRACEABLE | OSU5_MOD_NOSCOPE | OSU5_MOD_WINDUP | OSU5_MOD_AUTO)
#define MOD_COUNT (int)(sizeof(mod_list) / sizeof(mod_list[0]))
#define MOD_COLS 4

static double mod_rate(int mods)
{
    bool fast = mods & (OSU5_MOD_DOUBLETIME | OSU5_MOD_NIGHTCORE), slow = mods & (OSU5_MOD_HALFTIME | OSU5_MOD_DAYCORE);
    if ((fast && app.rate_custom > 1) || (slow && app.rate_custom > 0 && app.rate_custom < 1))
        return app.rate_custom;
    return fast ? 1.5 : slow ? 0.75 : 1.0;
}

/* what the core gets: Nightcore and Daycore are its speed mods as well */
static int core_mods(int mods)
{
    if (mods & OSU5_MOD_NIGHTCORE) mods |= OSU5_MOD_DOUBLETIME;
    if (mods & OSU5_MOD_DAYCORE) mods |= OSU5_MOD_HALFTIME;
    return mods & 0xFFFF;
}

/* Difficulty Adjust and the custom speed, handed over before a load */
static void push_mod_settings(void)
{
    bool da = app.mods & OSU5_MOD_DIFFADJUST;
    osu5_mod_settings(da ? app.da[0] : -1, da ? app.da[1] : -1, da ? app.da[2] : -1, da ? app.da[3] : -1,
                      mod_rate(app.mods) != 1.5 && mod_rate(app.mods) != 0.75 && mod_rate(app.mods) != 1 ? mod_rate(app.mods) : 0);
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
    app.nscores = scores_for(score_path(&app.songs.items[app.sel]), app.scores, SHOWN_SCORES);
}

static bool start_song(const song_entry *e)
{
    snprintf(app.status, sizeof(app.status), "loading...");
    preview_cancel();
    if (app.sb) {
        sb_free(app.sb);
        app.sb = NULL;
    }

    osu5_convert(conv_of(e));
    osu5_set_mods(core_mods(app.mods));
    push_mod_settings();
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
    bool kept_pitch = rate != 1 && !(app.mods & (OSU5_MOD_NIGHTCORE | OSU5_MOD_DAYCORE | OSU5_MOD_WINDUP)) &&
                      audio_stretch(&music, rate, &stretched);
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
    {
        int st5[5] = {0};
        app.map_objects = osu5_map_stats(e->file, st5) == 0 ? st5[0] + st5[1] + st5[2] : 0;
        memset(app.key_count, 0, sizeof(app.key_count));
        app.key_prev = 0;
    }
    if (bweb_on() || (app.mp_on && app.mp_kind == MP_CADENCE)) {
        char what[200];
        snprintf(what, sizeof(what), "%.60s - %.70s [%.50s]", e->info.artist, e->info.title, e->info.version);
        mp_status(app.watching ? 6 : 2, what, map_md5(e->file), app.mods, play_mode(e), e->info.beatmap_id);
    }
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

static int play_retries; /* for the pause screen */

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
    if (bweb_on() || (app.mp_on && app.mp_kind == MP_CADENCE))
        mp_status(0, "", "", 0, app.mode_pick, 0);
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
    if (spec_target)
        spectate_back();
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
        /* spectating: their newest frames first, and out if they left the map */
        if (spec_target) {
            spectate_pull();
            if (!spec_live) {
                back_to_select();
                return;
            }
        }
        /* the recorded calls, at their own times, up to now */
        while (app.rp_pos < app.watch.count && app.watch.frames[app.rp_pos].t <= t) {
            const replay_frame *f = &app.watch.frames[app.rp_pos++];
            osu5_update(f->t, f->x, f->y, f->keys);
            app.cursor.x = f->x;
            app.cursor.y = f->y;
            app.cursor.keys = f->keys;
        }
    } else {
        if (app.ruleset == 0)
            input_to_play(pad, &app.cursor, dt, app.sensitivity);
        else
            app.cursor.keys = ruleset_keys(pad);
        osu5_update(t, app.cursor.x, app.cursor.y, app.cursor.keys);
        if (!(app.mods & OSU5_MOD_AUTO))
            replay_push(&app.rec, t, app.cursor.x, app.cursor.y, app.cursor.keys);
        if (app.mp_on && app.mp_kind == MP_CADENCE && app.mps.spectators > 0 && !(app.mods & OSU5_MOD_AUTO))
            mp_frames_push(t, app.cursor.x, app.cursor.y, app.cursor.keys);
    }

    /* wind up: half as fast again by the end */
    if (app.mods & OSU5_MOD_WINDUP) {
        static double set_to;
        double r = mod_rate(app.mods) * (1 + 0.5 * fmin(1, fmax(0, app.last_hud.progress)));
        if (fabs(r - set_to) > 0.004) {
            audio_music_rate(r);
            set_to = r;
        }
    }

    /* presses for the key overlay */
    {
        int down = app.cursor.keys & ~app.key_prev;
        for (int k = 0; k < 4; k++)
            if (down & (1 << k))
                app.key_count[k]++;
        app.key_prev = app.cursor.keys;
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
                app.new_place = scores_add(score_path(app.playing), &app.last_hud, app.mods, app.last_pp, &app.last_when);
                if (!replay_save(&app.rec, app.last_when))
                    app.last_when = 0;
                const osu5_hud *h = &app.last_hud;
                if (!(app.mods & UNRANKED_MODS) &&
                    (app.rate_custom == 0 || !(app.mods & (OSU5_MOD_DOUBLETIME | OSU5_MOD_NIGHTCORE | OSU5_MOD_HALFTIME | OSU5_MOD_DAYCORE))))
                account_submit(map_md5(app.playing->file), app.playing->info.beatmap_id, app.ruleset, h->score, h->accuracy,
                               h->max_combo, app.mods, app.last_pp, h->rank, h->great, h->ok, h->meh, h->miss, &app.rec);
                if (bweb_on() && !(app.mods & UNRANKED_MODS) &&
                    (app.rate_custom == 0 || !(app.mods & (OSU5_MOD_DOUBLETIME | OSU5_MOD_NIGHTCORE | OSU5_MOD_HALFTIME | OSU5_MOD_DAYCORE)))) {
                    bweb_play bp;
                    memset(&bp, 0, sizeof(bp));
                    snprintf(bp.md5, sizeof(bp.md5), "%s", map_md5(app.playing->file));
                    snprintf(bp.osu_name, sizeof(bp.osu_name), "%s", base_name(app.playing->file));
                    bp.mode = play_mode(app.playing);
                    bp.n300 = h->great;
                    bp.n100 = h->ok;
                    bp.n50 = h->meh;
                    bp.miss = h->miss;
                    bp.score = h->score;
                    bp.max_combo = h->max_combo;
                    bp.perfect = h->miss == 0;
                    bp.passed = true;
                    bp.rank = h->rank;
                    bp.mods = app.mods;
                    bp.play_ms = (int)app.play_time;
                    bp.rp = &app.rec;
                    bweb_submit(&bp);
                }
            }
            app.scr = SCREEN_RESULTS;
        }
    }
}

/* lazer's "classic" display score, per ruleset, from the standardised one */
static int64_t classic_score(int64_t standardised)
{
    double n = app.map_objects > 0 ? app.map_objects : 1;
    double scale;
    switch (app.ruleset) {
    case 1: scale = n * 1109 + 100000; break;
    case 2: scale = n * n * 21.62 + 100000; break;
    case 3: return standardised;
    default: scale = n * n * 32.57 + 100000; break;
    }
    return (int64_t)llround(scale * (double)standardised / 1000000.0);
}

/* the keys down the right edge, lit while held, with how many times each went down */
static void draw_key_overlay(void)
{
    static const char *const osu_names[2] = {"K1", "K2"}, *const taiko_names[4] = {"D", "D", "K", "K"};
    int n = app.ruleset == 1 ? 4 : app.ruleset == 0 ? 2 : 0;
    char buf[16];
    for (int k = 0; k < n; k++) {
        float x = GFX_W - 90, y = GFX_H / 2 - n * 34 + k * 68;
        bool held = app.cursor.keys & (1 << k);
        uint32_t c = app.ruleset == 1 ? (k < 2 ? ui_hex(0xEB452B, 1) : ui_hex(0x438EAC, 1)) : ui_hex(0xFFCC22, 1);
        ui_round_rect(x, y, 62, 58, 10, held ? c : gfx_rgba(0, 0, 0, 0.45f));
        ui_round_rect(x, y, 62, 58, 10, gfx_rgba(1, 1, 1, held ? 0 : 0.08f));
        draw_text(x + 31, y + 6, 18, gfx_rgba(1, 1, 1, 0.9f), ALIGN_CENTRE, app.ruleset == 1 ? taiko_names[k] : osu_names[k]);
        snprintf(buf, sizeof(buf), "%d", app.key_count[k]);
        draw_text(x + 31, y + 30, 16, gfx_rgba(1, 1, 1, 0.75f), ALIGN_CENTRE, buf);
    }
}

/* the local top five for this map with you slotted in, like lazer's in-game board */
static void draw_live_board(int64_t now)
{
    if (!app.nscores)
        return;
    struct { int64_t score; const char *who; bool me; } rows[6];
    int n = 0;
    for (int i = 0; i < app.nscores && n < 5; i++) {
        rows[n].score = app.classic_score ? classic_score(app.scores[i].score) : app.scores[i].score;
        rows[n].who = "your best";
        rows[n].me = false;
        n++;
    }
    rows[n].score = now;
    rows[n].who = app.watching ? "replay" : "you";
    rows[n].me = true;
    n++;
    for (int i = 1; i < n; i++)
        for (int k = i; k > 0 && rows[k].score > rows[k - 1].score; k--) {
            typeof(rows[0]) t = rows[k];
            rows[k] = rows[k - 1];
            rows[k - 1] = t;
        }
    char buf[48];
    for (int i = 0; i < n; i++) {
        float y = GFX_H / 2 - n * 26 + i * 52;
        ui_round_rect(20, y, 220, 46, 8, rows[i].me ? ui_hex(0x66CCFF, 0.35f) : gfx_rgba(0, 0, 0, 0.35f));
        snprintf(buf, sizeof(buf), "%d  %s", i + 1, rows[i].who);
        draw_text(34, y + 4, 16, gfx_rgba(1, 1, 1, 0.8f), ALIGN_LEFT, buf);
        snprintf(buf, sizeof(buf), "%lld", (long long)rows[i].score);
        draw_text(34, y + 22, 18, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, buf);
    }
}

static void render_play(void)
{
    double t = app.play_time;
    int np = 0;
    int n = osu5_frame(t, cmds, MAX_CMDS, points, MAX_POINTS, &np);
    if (app.ruleset == 0 && app.mods & (OSU5_MOD_BARRELROLL | OSU5_MOD_TRACEABLE | OSU5_MOD_NOSCOPE)) {
        /* barrel roll: half a turn a minute round the playfield's middle */
        double a = app.mods & OSU5_MOD_BARRELROLL ? t * 0.5 * 2 * 3.14159265 / 60000.0 : 0;
        float ca = (float)cos(a), sa = (float)sin(a);
        int k = 0;
        for (int i = 0; i < n; i++) {
            osu5_draw c = cmds[i];
            if (app.mods & OSU5_MOD_TRACEABLE && c.kind == OSU5_DRAW_CIRCLE)
                continue;
            if (app.mods & OSU5_MOD_NOSCOPE && c.kind == OSU5_DRAW_CURSOR)
                continue;
            if (a != 0 && c.kind >= OSU5_DRAW_CIRCLE && c.kind <= OSU5_DRAW_CURSOR) {
                float dx = c.x - 256, dy = c.y - 192;
                c.x = 256 + dx * ca - dy * sa;
                c.y = 192 + dx * sa + dy * ca;
            }
            cmds[k++] = c;
        }
        n = k;
        if (a != 0)
            for (int i = 0; i < np; i++) {
                float dx = points[i * 2] - 256, dy = points[i * 2 + 1] - 192;
                points[i * 2] = 256 + dx * ca - dy * sa;
                points[i * 2 + 1] = 192 + dx * sa + dy * ca;
            }
    }

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
    {
        /* lazer's hud visibility, and classic scoring if picked */
        bool show = app.hud_mode == 0 || (app.hud_mode == 1 && app.last_hud.state == OSU5_STATE_BREAK);
        osu5_hud h = app.last_hud;
        if (app.classic_score)
            h.score = classic_score(h.score);
        if (show)
            draw_hud(&h, t);
        if (show && app.key_overlay)
            draw_key_overlay();
        if (show && app.live_board && !app.mp_playing)
            draw_live_board(h.score);
    }

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
    {
        /* the picked mode, then the star range (unrated maps stay in) */
        int n = 0;
        for (int i = 0; i < app.songs.count; i++) {
            const song_entry *e = &app.songs.items[i];
            bool convert = app.converts && app.mode_pick && e->info.mode == 0;
            if ((e->info.mode & 3) != app.mode_pick && !convert)
                continue;
            float st = stars_get(e->file, star_flags(e, 0));
            if (st >= 0 && (st < app.star_lo || (app.star_hi > 0 && st > app.star_hi)))
                continue;
            app.songs.items[n++] = *e;
        }
        app.songs.count = n;
    }
    if (app.coll_view >= 0) {
        int n = 0;
        for (int i = 0; i < app.songs.count; i++)
            if (coll_has(app.coll_view, app.songs.items[i].file))
                app.songs.items[n++] = app.songs.items[i];
        app.songs.count = n;
    }
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


static const char *const mode_names[4] = {"osu!", "osu!taiko", "osu!catch", "osu!mania"};

/* toolbar mode; song select only lists maps for it */
static void set_mode(int m)
{
    app.mode_pick = (m + 4) % 4;
    mapdl_ui_set_mode(app.mode_pick);
    app.scores_for = -1;
    want_converts();
    apply_view();
    settings_save();
    toast("playing %s maps", mode_names[app.mode_pick]);
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

/* converts get their own ratings, queued behind everything else */
static void want_converts(void)
{
    if (!app.converts || !app.mode_pick)
        return;
    for (int i = 0; i < app.all.count; i++)
        if (app.all.items[i].info.mode == 0)
            stars_want(app.all.items[i].file, app.mode_pick << 28, false);
}

static void rescan(void)
{
    songs_scan(SONGS_DIR, &app.all);
    /* everything gets rated in the background, what's on screen first */
    for (int i = 0; i < app.all.count; i++)
        stars_want(app.all.items[i].file, 0, false);
    want_converts();
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

/* song select options list */
enum { OPT_RANDOM, OPT_SORT, OPT_SHOW, OPT_FAV, OPT_COLLECT, OPT_SETINFO, OPT_IMPORTS, OPT_DELETE, OPT_MAPS, OPT_SKINS,
       OPT_SETTINGS, OPT_COUNT };
static int sel_opt = -1; /* -1 closed */
static double delete_armed;

static void random_set(void)
{
    if (app.songs.count > 0)
        app.sel = set_start(rand() % app.songs.count);
}

static void sel_opts_frame(const pad_state *pad)
{
    sel_opt = (sel_opt + pad->nav_y + OPT_COUNT) % OPT_COUNT;
    if (pad->pressed & (PAD_CIRCLE | PAD_OPTIONS)) {
        sel_opt = -1;
        return;
    }
    if (!(pad->pressed & PAD_CROSS))
        return;
    bool have = app.songs.count > 0;
    switch (sel_opt) {
    case OPT_RANDOM:
        random_set();
        sel_opt = -1;
        break;
    case OPT_SORT:
        app.sort = (app.sort + 1) % (SORT_COUNT + 1);
        apply_view();
        settings_save();
        break;
    case OPT_SHOW:
        /* everything, then each collection in turn */
        app.coll_view = app.coll_view + 1 < coll_count() ? app.coll_view + 1 : -1;
        apply_view();
        break;
    case OPT_COLLECT:
        if (have) {
            sel_opt = -1;
            coll_sel = 0;
        }
        break;
    case OPT_SETINFO:
        if (have) {
            sel_opt = -1;
            setinfo_open(app.songs.items[app.sel].info.set_id);
        }
        break;
    case OPT_IMPORTS:
        sel_opt = -1;
        imports_open();
        break;
    case OPT_FAV:
        if (have) {
            bool on = fav_toggle(app.songs.items[app.sel].dir);
            toast(on ? "added to favourites" : "removed from favourites");
            if (app.sort == SORT_COUNT)
                apply_view();
        }
        break;
    case OPT_DELETE:
        if (!have)
            break;
        if (wall_ms() - delete_armed < 3000) {
            char dir[512];
            snprintf(dir, sizeof(dir), "%s", app.songs.items[app.sel].dir);
            preview_cancel();
            audio_music_stop();
            bool ok = remove_tree(dir);
            LOG("deleted %s: %s", dir, ok ? "ok" : "failed");
            toast(ok ? "set deleted" : "couldn't delete everything");
            delete_armed = 0;
            sel_opt = -1;
            rescan();
        } else {
            delete_armed = wall_ms();
        }
        break;
    case OPT_MAPS:
        sel_opt = -1;
        mapdl_ui_open();
        app.scr = SCREEN_DOWNLOAD;
        break;
    case OPT_SKINS:
        sel_opt = -1;
        skin_ui_open(false);
        app.scr = SCREEN_SKINS;
        break;
    case OPT_SETTINGS:
        sel_opt = -1;
        app.set_sel = 0;
        app.scr = SCREEN_SETTINGS;
        break;
    }
}

static void render_sel_opts(void)
{
    char buf[96];
    const float w = 560, row = 56, h = 100 + OPT_COUNT * row, x = (GFX_W - w) / 2, y = (GFX_H - h) / 2;
    draw_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0, 0, 0, 0.55f));
    ui_round_rect(x, y, w, h, 18, ui_hex(0x22222A, 0.98f));
    draw_text(x + 30, y + 24, 30, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, "options");
    bool fav = app.songs.count > 0 && fav_is(app.songs.items[app.sel].dir);
    for (int i = 0; i < OPT_COUNT; i++) {
        float ry = y + 84 + i * row;
        bool on = i == sel_opt;
        const char *label = "";
        switch (i) {
        case OPT_RANDOM: label = "random set"; break;
        case OPT_SORT: snprintf(buf, sizeof(buf), "sort by: %s", sort_label()); label = buf; break;
        case OPT_FAV: label = fav ? "remove from favourites" : "add to favourites"; break;
        case OPT_SHOW:
            snprintf(buf, sizeof(buf), "show: %s", app.coll_view < 0 ? "every map" : coll_name(app.coll_view));
            label = buf;
            break;
        case OPT_COLLECT: label = "add to a collection"; break;
        case OPT_SETINFO: label = "set info and comments"; break;
        case OPT_IMPORTS: label = "imported replays (.osr)"; break;
        case OPT_DELETE: label = wall_ms() - delete_armed < 3000 ? "press CROSS again to delete" : "delete this set"; break;
        case OPT_MAPS: label = "get more maps"; break;
        case OPT_SKINS: label = "skins"; break;
        case OPT_SETTINGS: label = "settings"; break;
        }
        uint32_t bg = i == OPT_DELETE && on ? ui_hex(0xAA1B27, 1) : on ? ui_hex(0x3D3D4A, 1) : ui_hex(0x2C2C35, 1);
        ui_round_rect(x + 20, ry, w - 40, row - 8, 10, bg);
        draw_text(x + 44, ry + 14, 24, gfx_rgba(1, 1, 1, on ? 1 : 0.8f), ALIGN_LEFT, label);
    }
    draw_text(GFX_W / 2, y + h + 16, 20, gfx_rgba(1, 1, 1, 0.7f), ALIGN_CENTRE, "CROSS pick   CIRCLE close");
}

static void select_frame(const pad_state *pad)
{
    if (sel_opt >= 0) {
        sel_opts_frame(pad);
        return;
    }
    if (coll_open()) {
        coll_frame(pad);
        return;
    }
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
    /* L1 / R1: mode */
    if (pad->pressed & (PAD_L1 | PAD_R1)) {
        set_mode(app.mode_pick + (pad->pressed & PAD_R1 ? 1 : -1));
        return;
    }
    if (pad->pressed & PAD_TRIANGLE) {
        app.search.text = app.filter;
        app.search.cap = sizeof(app.filter);
        app.searching = true;
        return;
    }
    if (pad->pressed & PAD_L2)
        app.board = !app.board; /* yours <-> the online board picked in settings */
    if (pad->pressed & PAD_R3) {
        random_set();
        return;
    }
    if (pad->pressed & PAD_OPTIONS) {
        sel_opt = 0;
        delete_armed = 0;
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
        stars_want(app.songs.items[i].file, star_flags(&app.songs.items[i], 0), true);
    if (stars_mods(app.mods))
        stars_want(app.songs.items[app.sel].file, star_flags(&app.songs.items[app.sel], app.mods), true);
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
    else if (pad->pressed & PAD_R2 && app.board && app.board_src == 0) {
        static global_board gb;
        if (!app.bweb_waiting && account_board(map_md5(e->file), &gb) == ONLINE_READY && gb.count > 0 && gb.s[0].replay) {
            account_replay_get(map_md5(e->file), gb.s[0].name, play_mode(e), e->file);
            app.bweb_waiting = true;
            app.rp_from = 1;
            app.watch.mods = gb.s[0].mods;
        }
    } else if (pad->pressed & PAD_R2 && app.board && app.board_src == 2) {
        bweb_board b;
        if (!app.bweb_waiting && bweb_board_for(map_md5(e->file), base_name(e->file), e->info.set_id, play_mode(e), &b) == ONLINE_READY &&
            b.count > 0) {
            bweb_replay_get(b.s[0].id, play_mode(e), e->file);
            app.bweb_waiting = true;
            app.rp_from = 0;
            app.watch.mods = b.s[0].mods;
        }
    } else if (pad->pressed & PAD_R2 && app.nscores > 0 && replay_exists(app.scores[0].when))
        watch_replay(e, app.scores[0].when);

    /* bancho replay arrived */
    if (app.bweb_waiting) {
        int mods = app.watch.mods;
        int st = app.rp_from ? account_replay_take(&app.watch) : bweb_replay_take(&app.watch);
        if (st == ONLINE_READY) {
            app.bweb_waiting = false;
            app.watch.mods = mods;
            app.saved_mods = app.mods;
            app.mods = mods;
            app.watching = true;
            if (!start_song(e)) {
                app.watching = false;
                app.mods = app.saved_mods;
            }
        } else if (st != ONLINE_LOADING) {
            app.bweb_waiting = false;
            toast("couldn't get that replay from the server");
        }
    }
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
    if (app.bweb_waiting)
        head = "getting the replay...";
    float hw = draw_text(60, 356, 24, gfx_rgba(1, 1, 1, 0.8f), ALIGN_LEFT, head);
    if (st == ONLINE_READY && b.status != -99) {
        static const char *const names[] = {"not on osu.ppy.sh", "graveyard", "wip", "pending", "ranked", "approved", "qualified", "loved"};
        int k = b.status + 3;
        if (k >= 0 && k < 8)
            ui_pill(60 + hw + 14, 360, 14, b.status == 1 || b.status == 2 ? ui_hex(0x88DD44, 1) : b.status == 4 ? ui_hex(0xFF66AA, 1)
                                                                                              : gfx_rgba(1, 1, 1, 0.25f),
                    gfx_rgba(0.1f, 0.1f, 0.1f, 1), names[k]);
    }
    draw_text(820, 356, 18, gfx_rgba(1, 1, 1, 0.5f), ALIGN_RIGHT,
              st == ONLINE_READY && b.count && b.s[0].replay ? "R2 watch #1   L2 yours" : "L2 yours");
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

/* the bancho server's board, osu! stable's own call */
static void render_bancho(const song_entry *e)
{
    static bweb_board b;
    char buf[160];
    int st = bweb_board_for(map_md5(e->file), base_name(e->file), e->info.set_id, play_mode(e), &b);
    const char *head = "bancho server top scores";
    if (st == ONLINE_OFF)
        head = "connect to a bancho server in multiplayer first";
    else if (st == ONLINE_LOADING)
        head = "loading...";
    else if (st == ONLINE_FAILED)
        head = "the bancho server didn't answer";
    else if (b.status < 0 || (b.status == 0 && !b.count))
        head = "this difficulty isn't on the server";
    else if (!b.count)
        head = "no scores yet: be the first";
    if (app.bweb_waiting)
        head = "getting the replay...";
    draw_text(60, 356, 24, gfx_rgba(1, 1, 1, 0.8f), ALIGN_LEFT, head);
    draw_text(820, 356, 18, gfx_rgba(1, 1, 1, 0.5f), ALIGN_RIGHT, st == ONLINE_READY && b.count ? "R2 watch #1   L2 yours" : "L2 yours");
    if (st != ONLINE_READY)
        return;

    int rows = SHOWN_SCORES * 64 / 40;
    for (int i = 0; i < b.count && i < rows; i++) {
        const bweb_score *s = &b.s[i];
        float y = 396 + i * 40;
        if (b.has_mine && b.mine.id == s->id)
            draw_rect(50, y, 780, 38, gfx_rgba(0.5f, 0.9f, 0.95f, 0.15f));
        snprintf(buf, sizeof(buf), "%d", i + 1);
        draw_text(90, y + 6, 22, gfx_rgba(1, 1, 1, 0.6f), ALIGN_RIGHT, buf);
        draw_rank(s->rank, 125, y + 18, 28, true, 1);
        draw_text(160, y + 6, 22, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, s->user);
        char tags[64];
        mod_tags(s->mods, tags, sizeof(tags));
        snprintf(buf, sizeof(buf), "%.2f%%  %dx  %s", s->acc * 100, s->combo, tags);
        draw_text(400, y + 9, 18, gfx_rgba(1, 1, 1, 0.7f), ALIGN_LEFT, buf);
        snprintf(buf, sizeof(buf), "%lld", s->score);
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
    snprintf(buf, sizeof(buf), "sort by  %s   (OPTIONS)", sort_label());
    draw_text(GFX_W - 860, 86, 20, ui_hex(UI_YELLOW, 1), ALIGN_LEFT, buf);
    if (app.star_hi > 0)
        snprintf(buf, sizeof(buf), "%s   %.1f-%.1f*", mode_names[app.mode_pick], app.star_lo, app.star_hi);
    else if (app.star_lo > 0)
        snprintf(buf, sizeof(buf), "%s   %.1f*+", mode_names[app.mode_pick], app.star_lo);
    else
        snprintf(buf, sizeof(buf), "%s", mode_names[app.mode_pick]);
    ui_pill(GFX_W - 560, 82, 16, ui_hex(0x66CCFF, 1), ui_hex(0x222222, 1), buf);
    snprintf(buf, sizeof(buf), "%d maps", app.songs.count);
    draw_text(GFX_W - 40, 86, 20, gfx_rgba(1, 1, 1, 0.6f), ALIGN_RIGHT, buf);

    if (app.songs.count == 0 && app.all.count > 0 && app.filter[0]) {
        snprintf(buf, sizeof(buf), "nothing matches \"%s\"", app.filter);
        draw_text(GFX_W / 2, 400, 40, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, buf);
        if (app.searching)
            kbd_render(&app.search);
        return;
    }

    if (app.songs.count == 0) {
        if (app.all.count > 0) {
            snprintf(buf, sizeof(buf), "no %s maps here", mode_names[app.mode_pick]);
            draw_text(GFX_W / 2, 400, 48, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, buf);
            draw_text(GFX_W / 2, 480, 28, gfx_rgba(1, 1, 1, 0.8f), ALIGN_CENTRE, "flick the right stick left or right to change the mode");
            ui_footer(NULL, "CIRCLE");
            return;
        }
        draw_text(GFX_W / 2, 400, 48, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, "no beatmaps yet");
        draw_text(GFX_W / 2, 480, 28, gfx_rgba(1, 1, 1, 0.8f), ALIGN_CENTRE, "press OPTIONS, then get more maps");
        draw_text(GFX_W / 2, 525, 28, gfx_rgba(1, 1, 1, 0.8f), ALIGN_CENTRE, "or copy extracted .osz folders to " SONGS_DIR);
        draw_text(GFX_W / 2, 570, 28, gfx_rgba(1, 1, 1, 0.8f), ALIGN_CENTRE, "and press CROSS to rescan");
        draw_text(40, GFX_H - 60, 22, gfx_rgba(1, 1, 1, 0.7f), ALIGN_LEFT,
                  "OPTIONS get maps, skins and settings");
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
            /* difficulty row, tinted with its star colour, brighter when picked */
            bool on = r == sel_row;
            float x = (on ? 960 : 1000) + curve;
            float st = stars_get(e->file, star_flags(e, 0));
            uint32_t sc = st >= 0 ? star_colour(st, 1) : gfx_rgba(0.5f, 0.5f, 0.5f, 1);
            if (on)
                ui_round_rect(x - 3, y - 3, GFX_W - x + 30, row, 14, gfx_rgba(1, 1, 1, 0.9f));
            ui_round_rect(x, y, GFX_W - x + 30, row - 6, 12, ui_mix(ui_hex(0x222222, 0.95f), sc, on ? 0.55f : 0.22f));
            ui_disc(x + 22, y + (row - 6) / 2, 8, gfx_rgba(1, 1, 1, on ? 0.95f : 0.6f));
            uint32_t tc = gfx_rgba(1, 1, 1, on ? 1 : 0.9f);
            float tx = x + 44;
            if (in->mode != 0 || conv_of(e)) {
                char tag[16];
                if (conv_of(e))
                    snprintf(tag, sizeof(tag), "convert");
                else if (in->mode == 3)
                    snprintf(tag, sizeof(tag), "%dK", (int)lroundf(in->cs));
                else
                    snprintf(tag, sizeof(tag), "%s", in->mode == 1 ? "taiko" : "catch");
                tx += ui_pill(tx, y + 10, 14, gfx_rgba(0, 0, 0, 0.4f), gfx_rgba(1, 1, 1, 0.9f), tag) + 10;
            }
            float nw = draw_text(tx, y + 10, 22, tc, ALIGN_LEFT, in->version);
            snprintf(buf, sizeof(buf), "mapped by %.40s", in->creator);
            draw_text(tx + nw + 10, y + 14, 16, gfx_rgba(1, 1, 1, 0.65f), ALIGN_LEFT, buf);
            float px = x + 44;
            if (st >= 0)
                px += star_pill(px, y + 40, 24, st) + 10;
            /* a star per whole point, like the panel's star row */
            int whole = st >= 0 ? (int)st : 0;
            for (int k = 0; k < 10; k++)
                ui_disc(px + 8 + k * 16, y + 52, 4.5f, k < whole ? gfx_rgba(1, 1, 1, 0.85f) : gfx_rgba(1, 1, 1, 0.18f));
            continue;
        }

        /* a set: background as the cover, fading to dark */
        bool header = rows[r].kind == ROW_HEADER;
        float x = (header ? 920 : 980) + curve;
        int n = set_end(rows[r].index) - rows[r].index;
        float th = row - 6, pw = GFX_W - x + 30;
        if (header)
            ui_round_rect(x - 3, y - 3, pw, th + 6, 14, gfx_rgba(1, 1, 1, 0.85f));
        ui_round_rect(x, y, pw, th, 12, ui_hex(0x1A1A1A, 0.95f));
        int tex = set_thumb(e);
        if (tex) {
            float cw = pw * 0.55f;
            gfx_bind(tex);
            gfx_push(&(gfx_inst){x + cw / 2, y + th / 2, cw / 2, th / 2, 0, gfx_next_z(), gfx_rgba(1, 1, 1, header ? 0.9f : 0.6f), GK_TEX,
                                 {0, 0.25f, 1, 0.75f}});
            gfx_bind(0);
            for (int k = 0; k < 8; k++) /* the fade, in bands */
                draw_rect(x + cw * (0.3f + k * 0.0875f), y, cw * 0.0875f + 1, th, ui_hex(0x1A1A1A, 0.12f + k * 0.11f));
        }
        uint32_t tc = gfx_rgba(1, 1, 1, header ? 1 : 0.92f);
        snprintf(buf, sizeof(buf), "%.90s", in->title);
        draw_text(x + 24, y + 6, 24, tc, ALIGN_LEFT, buf);
        snprintf(buf, sizeof(buf), "%.60s", in->artist);
        draw_text(x + 24, y + 34, 16, gfx_rgba(1, 1, 1, 0.8f), ALIGN_LEFT, buf);
        if (e->video)
            ui_pill(GFX_W - 110, y + 8, 12, ui_hex(UI_PINK, 1), gfx_rgba(1, 1, 1, 1), "VIDEO");
        /* a dot per difficulty in its star colour, once they're rated */
        float dx = x + 24;
        for (int i = rows[r].index; i < rows[r].index + n && i < rows[r].index + 16; i++) {
            float st = stars_get(app.songs.items[i].file, star_flags(&app.songs.items[i], 0));
            ui_round_rect(dx, y + 56, 8, 14, 4, st >= 0 ? star_colour(st, 1) : gfx_rgba(1, 1, 1, 0.3f));
            dx += 11;
        }
        snprintf(buf, sizeof(buf), "%d %s  //  %.40s", n, n == 1 ? "difficulty" : "difficulties", in->creator);
        draw_text(dx + 8, y + 54, 15, gfx_rgba(1, 1, 1, 0.6f), ALIGN_LEFT, buf);
    }

    const osu5_peek_info *in = &app.songs.items[app.sel].info;
    const char *selfile = app.songs.items[app.sel].file;
    /* the wedge: title and artist, the map's numbers in pills, the
     * difficulty under its star pill, then a column per stat with a bar on top */
    ui_shear_rect(-60, 0, 995, 318, 46, ui_hex(0x1C1C1C, 0.86f));
    {
        static const char *modes[4] = {"osu!", "taiko", "catch", "mania"};
        ui_pill(28, 16, 13, ui_hex(0x555555, 1), gfx_rgba(1, 1, 1, 0.9f), modes[in->mode & 3]);
    }
    snprintf(buf, sizeof(buf), "%s", in->title);
    ui_fit(buf, 36, 860);
    draw_text(28, 44, 36, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, buf);
    snprintf(buf, sizeof(buf), "%s", in->artist);
    ui_fit(buf, 19, 860);
    draw_text(28, 90, 19, gfx_rgba(1, 1, 1, 0.85f), ALIGN_LEFT, buf);

    /* length, bpm and object counts need the whole map read, so only once the
     * selection has sat still for a moment */
    static char stats_for[512];
    static int stats[5] = {-1};
    static double still_since;
    static int last_sel = -1;
    if (app.sel != last_sel) {
        last_sel = app.sel;
        still_since = wall_ms();
    }
    if (strcmp(stats_for, selfile) && wall_ms() - still_since > 250) {
        snprintf(stats_for, sizeof(stats_for), "%s", selfile);
        if (osu5_map_stats(selfile, stats) != 0)
            stats[0] = -1;
    }
    bool have_stats = !strcmp(stats_for, selfile) && stats[0] >= 0;
    {
        float px = 28, py = 124;
        char pill[48];
        if (have_stats) {
            int sec = stats[3] / 1000;
            snprintf(pill, sizeof(pill), "length  %d:%02d", sec / 60, sec % 60);
            px += ui_pill(px, py, 16, gfx_rgba(0, 0, 0, 0.45f), gfx_rgba(1, 1, 1, 0.9f), pill) + 8;
            snprintf(pill, sizeof(pill), "bpm  %d", stats[4]);
            px += ui_pill(px, py, 16, gfx_rgba(0, 0, 0, 0.45f), gfx_rgba(1, 1, 1, 0.9f), pill) + 8;
        }
        bool fav = fav_is(app.songs.items[app.sel].dir);
        px += ui_pill(px, py, 16, fav ? ui_hex(UI_PINK, 1) : gfx_rgba(0, 0, 0, 0.45f), gfx_rgba(1, 1, 1, 0.9f),
                      fav ? "favourite" : "not favourite") + 8;
        snprintf(pill, sizeof(pill), "%d %s", app.nscores, app.nscores == 1 ? "score" : "scores");
        px += ui_pill(px, py, 16, gfx_rgba(0, 0, 0, 0.45f), gfx_rgba(1, 1, 1, 0.9f), pill) + 8;
        snprintf(pill, sizeof(pill), "offset %+dms", app.offset_ms);
        ui_pill(px, py, 16, gfx_rgba(0, 0, 0, 0.45f), ui_hex(UI_YELLOW, 1), pill);
    }

    const song_entry *sele = &app.songs.items[app.sel];
    float st = stars_get(selfile, star_flags(sele, 0)), mst = stars_mods(app.mods) ? stars_get(selfile, star_flags(sele, app.mods)) : st;
    float shown = mst >= 0 ? mst : st;
    uint32_t dcol = shown >= 0 ? star_colour(shown, 1) : ui_hex(0xAAAAAA, 1);
    {
        float x = 28;
        if (shown >= 0)
            x += star_pill(28, 172, 30, shown) + 12;
        snprintf(buf, sizeof(buf), "%s", in->version);
        ui_fit(buf, 20, 420);
        x += draw_text(x, 176, 20, dcol, ALIGN_LEFT, buf) + 8;
        x += draw_text(x, 178, 17, gfx_rgba(1, 1, 1, 0.6f), ALIGN_LEFT, "mapped by ") ;
        snprintf(buf, sizeof(buf), "%s", in->creator);
        ui_fit(buf, 17, 880 - x);
        draw_text(x, 178, 17, dcol, ALIGN_LEFT, buf);
    }

    /* stat columns: object counts on the left, the four settings on the right */
    {
        char v[24];
        struct { const char *label; float value, max; bool count; } cols[7] = {
            {"circles", (float)stats[0], 0, true}, {"sliders", (float)stats[1], 0, true}, {"spinners", (float)stats[2], 0, true},
            {in->mode == 3 ? "keys" : "circle size", in->cs, 10, false}, {"approach rate", in->ar, 10, false},
            {"accuracy", in->od, 10, false}, {"hp drain", in->hp, 10, false},
        };
        int total = have_stats ? stats[0] + stats[1] + stats[2] : 0;
        for (int i = 0; i < 7; i++) {
            float cw = 112, x = 28 + i * (cw + 8) + (i >= 3 ? 30 : 0), y = 226;
            if (in->mode == 1 && (i == 3 || i == 4))
                continue; /* taiko has no circle size or approach */
            float k = cols[i].count ? (total ? cols[i].value / total : 0) : cols[i].value / cols[i].max;
            if (k > 1) k = 1;
            if (k < 0) k = 0;
            ui_round_rect(x, y, cw, 5, 2.5f, gfx_rgba(1, 1, 1, 0.12f));
            ui_round_rect(x, y, fmaxf(cw * k, 5), 5, 2.5f, dcol);
            draw_text(x, y + 12, 15, gfx_rgba(1, 1, 1, 0.75f), ALIGN_LEFT, cols[i].label);
            if (cols[i].count)
                snprintf(v, sizeof(v), "%s", have_stats ? "" : "-");
            if (cols[i].count && have_stats)
                snprintf(v, sizeof(v), "%d", (int)cols[i].value);
            else if (!cols[i].count)
                snprintf(v, sizeof(v), "%g", roundf(cols[i].value * 10) / 10);
            draw_text(x, y + 34, 22, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, v);
        }
    }

    char tags[64];
    mod_tags(app.mods, tags, sizeof(tags));
    if (tags[0])
        ui_pill(760, 16, 13, ui_hex(0x8866EE, 1), gfx_rgba(1, 1, 1, 1), tags);

    ui_round_rect(40, 330, 800, 80 + SHOWN_SCORES * 64, 16, gfx_rgba(0.05f, 0.05f, 0.07f, 0.75f));
    if (app.board && app.board_src == 2) {
        render_bancho(&app.songs.items[app.sel]);
        goto scores_done;
    }
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
              gfx_rgba(1, 1, 1, 0.5f), ALIGN_RIGHT, app.board_src == 2 ? "L2 bancho" : app.board_src && online_enabled() ? "L2 osu.ppy.sh" : "L2 global");
scores_done:;

    /* footer: 84 tall strip, the wide pink back button,
     * then dark tiles that stand a little proud of it with a colour stripe each */
    {
        const float fh = 84, fy = GFX_H - fh, lean = 14;
        draw_rect(0, fy + 14, GFX_W, fh - 14, gfx_rgba(0.12f, 0.12f, 0.14f, 0.95f));
        ui_shear_rect(21, fy + 18, 330, 62, lean, ui_hex(0xDD4499, 1));
        ui_arrow_left(130, fy + 49, 20, gfx_rgba(1, 1, 1, 1));
        draw_text(196, fy + 34, 24, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, "back");

        static const struct { const char *label, *button; uint32_t colour; } tiles[] = {
            {"mods", "SQUARE", 0xB2E05C}, {"random", "R3", UI_ORANGE}, {"search", "TRIANGLE", UI_BLUE}, {"options", "OPTIONS", 0x8866EE},
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
            case 1: /* a die */
                ui_round_rect(cx - 11, fy + 11, 22, 22, 5, ic);
                ui_disc(cx - 5, fy + 17, 2.5f, gfx_rgba(0.22f, 0.22f, 0.26f, 1));
                ui_disc(cx + 5, fy + 27, 2.5f, gfx_rgba(0.22f, 0.22f, 0.26f, 1));
                ui_disc(cx, fy + 22, 2.5f, gfx_rgba(0.22f, 0.22f, 0.26f, 1));
                break;
            case 2:
                ui_search_icon(cx, fy + 22, 9, ic);
                break;
            default: ui_gear_icon(cx, fy + 22, 24, ic); break;
            }
            draw_text(cx, fy + 40, 20, gfx_rgba(1, 1, 1, 0.95f), ALIGN_CENTRE, tiles[i].label);
            draw_text(tx + 150 - lean - 8, fy + 6, 11, gfx_rgba(1, 1, 1, 0.4f), ALIGN_RIGHT, tiles[i].button);
            tx += 156;
        }
        draw_text(GFX_W - 330, fy + 36, 20, gfx_rgba(1, 1, 1, 0.55f), ALIGN_RIGHT, "L1 / R1 mode   L2 board   R2 replay   CROSS play");
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
    {"conversion", 0x8866EE, 2, {11, 16}},
    {"fun", 0xFF66AA, 4, {17, 18, 19, 20}},
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

/* customise: speed for the speed mods, CS / AR / OD / HP for Difficulty Adjust */
enum { CUST_RATE, CUST_CS, CUST_AR, CUST_OD, CUST_HP, CUST_ROWS };
static const char *const cust_names[CUST_ROWS] = {"speed", "circle size", "approach rate", "overall difficulty", "hp drain"};

static void custom_frame(const pad_state *pad)
{
    app.mod_custom = (app.mod_custom + pad->nav_y + CUST_ROWS) % CUST_ROWS;
    int d = pad->nav_x;
    if (d && app.mod_custom == CUST_RATE) {
        bool slow = app.mods & (OSU5_MOD_HALFTIME | OSU5_MOD_DAYCORE);
        double r = app.rate_custom > 0 ? app.rate_custom : slow ? 0.75 : 1.5;
        r += d * 0.05;
        r = slow ? fmin(0.99, fmax(0.5, r)) : fmin(2.0, fmax(1.01, r));
        app.rate_custom = r;
    } else if (d) {
        float *v = &app.da[app.mod_custom - CUST_CS];
        float nv = (*v < 0 ? 5 : *v) + d * 0.1f;
        *v = nv < 0 ? 0 : nv > 11 ? 11 : nv;
    }
    /* TRIANGLE puts the row back to the map's own value */
    if (pad->pressed & PAD_TRIANGLE) {
        if (app.mod_custom == CUST_RATE)
            app.rate_custom = 0;
        else
            app.da[app.mod_custom - CUST_CS] = -1;
    }
    if (pad->pressed & (PAD_CIRCLE | PAD_OPTIONS))
        app.mod_custom = -1;
}

static void render_custom(void)
{
    char buf[64];
    const float w = 620, row = 64, h = 110 + CUST_ROWS * row, x = (GFX_W - w) / 2, y = (GFX_H - h) / 2;
    draw_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0, 0, 0, 0.5f));
    ui_round_rect(x, y, w, h, 18, ui_hex(0x2D372E, 0.98f));
    draw_text(x + 30, y + 24, 30, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, "customise");
    for (int i = 0; i < CUST_ROWS; i++) {
        float ry = y + 86 + i * row;
        bool on = i == app.mod_custom;
        bool live = i == CUST_RATE ? (app.mods & (OSU5_MOD_DOUBLETIME | OSU5_MOD_NIGHTCORE | OSU5_MOD_HALFTIME | OSU5_MOD_DAYCORE)) != 0
                                   : (app.mods & OSU5_MOD_DIFFADJUST) != 0;
        ui_round_rect(x + 20, ry, w - 40, row - 8, 10, on ? ui_hex(0x455346, 1) : ui_hex(0x3C4D3B, 1));
        draw_text(x + 44, ry + 15, 22, gfx_rgba(1, 1, 1, live ? 0.95f : 0.4f), ALIGN_LEFT, cust_names[i]);
        if (i == CUST_RATE)
            snprintf(buf, sizeof(buf), app.rate_custom > 0 ? "%.2fx" : "default", app.rate_custom);
        else
            snprintf(buf, sizeof(buf), app.da[i - CUST_CS] >= 0 ? "%.1f" : "map's", app.da[i - CUST_CS]);
        draw_text(x + w - 44, ry + 15, 22, live ? ui_hex(UI_YELLOW, 1) : gfx_rgba(1, 1, 1, 0.4f), ALIGN_RIGHT, buf);
    }
    draw_text(GFX_W / 2, y + h + 16, 20, gfx_rgba(1, 1, 1, 0.7f), ALIGN_CENTRE,
              "LEFT / RIGHT change   TRIANGLE reset   CIRCLE done   (speed needs DT / HT, the rest Difficulty Adjust)");
}

static void mods_frame(const pad_state *pad)
{
    if (app.mod_custom >= 0) {
        custom_frame(pad);
        return;
    }
    if (pad->pressed & PAD_OPTIONS) {
        app.mod_custom = 0;
        return;
    }
    if (app.simple_ui) {
        /* the plain look lays the mods out four to a row in list order */
        int rows = (MOD_COUNT + MOD_COLS - 1) / MOD_COLS;
        int r = app.mod_sel / MOD_COLS, c = app.mod_sel % MOD_COLS;
        r = (r + pad->nav_y + rows) % rows;
        c = (c + pad->nav_x + MOD_COLS) % MOD_COLS;
        app.mod_sel = r * MOD_COLS + c;
        if (app.mod_sel >= MOD_COUNT)
            app.mod_sel = MOD_COUNT - 1;
    } else {
        int c, r;
        mod_where(app.mod_sel, &c, &r);
        c = (c + pad->nav_x + MOD_NCOLS) % MOD_NCOLS;
        if (r >= mod_cols[c].n)
            r = mod_cols[c].n - 1;
        r = (r + pad->nav_y + mod_cols[c].n) % mod_cols[c].n;
        app.mod_sel = mod_cols[c].mods[r];
    }

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
    draw_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0.03f, 0.04f, 0.03f, 0.82f));

    /* banner across the top */
    ui_round_rect(92, 55, GFX_W - 184, 92, 12, ui_hex(0x2F472F, 0.97f));
    draw_text(150, 72, 26, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, "mod select");
    draw_text(150, 108, 17, gfx_rgba(1, 1, 1, 0.7f), ALIGN_LEFT,
              "mods change how a map plays; some change the score you get, others are just for fun");

    /* leaning columns: everything slides left the lower it sits */
    const float k = 0.16f, gap = 16, cw = (GFX_W - 260 - (MOD_NCOLS - 1) * gap) / MOD_NCOLS, top = 200, hh = 56, bottom = 930, rh = 62;
    float x0 = (GFX_W - MOD_NCOLS * cw - (MOD_NCOLS - 1) * gap) / 2 + 60;
    for (int c = 0; c < MOD_NCOLS; c++) {
        float x = x0 + c * (cw + gap);
        uint32_t col = mod_cols[c].colour;
        float bh = bottom - top;
        ui_shear_rect(x - k * bh, top, cw + k * bh, bh, k * bh, ui_hex(0x2D372E, 0.97f));
        ui_shear_rect(x - k * hh, top, cw + k * hh, hh, k * hh, ui_hex(col, 1));
        /* "difficulty reduction": first word plain, the rest bold-ish, dark on the colour */
        const char *t = mod_cols[c].title, *sp = strchr(t, ' ');
        char first[32];
        snprintf(first, sizeof(first), "%.*s", sp ? (int)(sp - t) : (int)strlen(t), t);
        float w = draw_text(x + 18, top + 14, 23, ui_hex(0x1A1A1A, 0.75f), ALIGN_LEFT, first);
        if (sp)
            draw_text(x + 18 + w, top + 14, 23, ui_hex(0x111111, 1), ALIGN_LEFT, sp);

        for (int r = 0; r < mod_cols[c].n; r++) {
            int i = mod_cols[c].mods[r];
            float y = top + hh + 12 + r * (rh + 8);
            float ix = x + 14 - k * (y + rh - top);
            bool on = app.mods & mod_list[i].bit, cur = i == app.mod_sel;
            uint32_t bg = on ? ui_hex(col, 1) : ui_hex(0x455346, 1);
            if (cur)
                ui_shear_rect(ix - 4, y - 4, cw - 20 + k * rh + 8, rh + 8, k * (rh + 8), gfx_rgba(1, 1, 1, 0.9f));
            ui_shear_rect(ix, y, cw - 20 + k * rh, rh, k * rh, bg);
            /* the icon block on the left, darker */
            ui_shear_rect(ix, y, 70 + k * rh, rh, k * rh, on ? gfx_rgba(0, 0, 0, 0.18f) : ui_hex(0x3C4D3B, 1));
            uint32_t tc = on ? ui_hex(0x111111, 1) : gfx_rgba(1, 1, 1, 1);
            draw_text(ix + 40 + k * rh / 2, y + 19, 19, on ? tc : ui_hex(col, 1), ALIGN_CENTRE, mod_list[i].tag);
            draw_text(ix + 92 + k * rh / 2, y + 7, 21, tc, ALIGN_LEFT, mod_list[i].name);
            snprintf(buf, sizeof(buf), "%s", mod_list[i].what);
            ui_fit(buf, 14, cw - 120);
            draw_text(ix + 92 + k * rh / 2, y + 35, 14, on ? ui_hex(0x111111, 0.8f) : gfx_rgba(1, 1, 1, 0.65f), ALIGN_LEFT, buf);
        }
    }

    /* the footer, solid so song select's doesn't show through */
    const float fh = 84, fy = GFX_H - fh, lean = 14;
    draw_rect(0, fy, GFX_W, fh, ui_hex(0x131812, 1));
    ui_shear_rect(21, fy + 12, 330, 62, lean, ui_hex(0xDD4499, 1));
    ui_arrow_left(130, fy + 43, 20, gfx_rgba(1, 1, 1, 1));
    draw_text(196, fy + 28, 24, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, "back");
    /* the mods tile, lit since we're in it */
    ui_shear_rect(400, fy - 6, 150, fh + 6, lean, ui_hex(0xB2E05C, 1));
    draw_rect(463, fy + 12, 22, 3, ui_hex(0x222222, 1));
    draw_rect(465, fy + 22, 22, 3, ui_hex(0x222222, 1));
    draw_text(475, fy + 36, 20, ui_hex(0x222222, 1), ALIGN_CENTRE, "mods");
    ui_shear_rect(570, fy + 12, 300, 62, lean, ui_hex(0x2D372E, 1));
    draw_text(720, fy + 22, 22, gfx_rgba(1, 1, 1, 0.9f), ALIGN_CENTRE, "deselect all");
    draw_text(720, fy + 50, 12, gfx_rgba(1, 1, 1, 0.5f), ALIGN_CENTRE, "TRIANGLE");

    float bx = GFX_W - 860;
    ui_shear_rect(bx, fy + 12, 560, 62, lean, ui_hex(0x2D372E, 1));
    if (app.songs.count > 0) {
        const song_entry *e = &app.songs.items[app.sel];
        float st = stars_get(e->file, star_flags(e, app.mods));
        if (st < 0)
            st = stars_get(e->file, star_flags(e, 0));
        if (st >= 0)
            star_pill(bx + 30, fy + 28, 30, st);
        const osu5_peek_info *in = &e->info;
        static const char *lab[4] = {"CS", "AR", "OD", "HP"};
        float v[4] = {in->cs, in->ar, in->od, in->hp};
        for (int q = 0; q < 4; q++) {
            draw_text(bx + 260 + q * 72, fy + 18, 18, gfx_rgba(1, 1, 1, 0.95f), ALIGN_CENTRE, lab[q]);
            snprintf(buf, sizeof(buf), "%.1f", v[q]);
            draw_text(bx + 260 + q * 72, fy + 44, 18, gfx_rgba(1, 1, 1, 0.8f), ALIGN_CENTRE, buf);
        }
    }
    ui_shear_rect(GFX_W - 280, fy + 12, 250, 62, lean, ui_hex(0x2D372E, 1));
    snprintf(buf, sizeof(buf), "%.2fx", mod_multiplier(app.mods));
    draw_text(GFX_W - 220, fy + 30, 20, gfx_rgba(1, 1, 1, 0.75f), ALIGN_CENTRE, "score");
    draw_text(GFX_W - 110, fy + 28, 26, ui_hex(UI_YELLOW, 1), ALIGN_CENTRE, buf);
}

/* ---- settings ---- */

/* grouped the way the screen shows them */
enum {
    SET_SETUP, SET_HELP, SET_UPDATE, SET_LOOK,  /* general */
    SET_SKIN,                                   /* skin */
    SET_SENS, SET_CURSOR,                       /* input */
    SET_DIM, SET_STORYBOARD, SET_VIDEO, SET_HUD, SET_KEYS, SET_LIVE, SET_CLASSIC, SET_STARS_LO, SET_STARS_HI, SET_CONVERTS, /* gameplay */
    SET_MUSIC, SET_EFFECTS, SET_OFFSET, SET_WIZARD, /* audio */
    SET_ACCOUNT, SET_CLAN, SET_PASSWORD, SET_DELETE, SET_BOARD, /* online */
    SET_RESCAN,                                 /* maintenance */
    SET_COUNT
};

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* what the search box looks through: each row's name, its section, and a few
 * words someone might type instead */
static const char *const set_words[SET_COUNT] = {
    [SET_SETUP] = "general run setup wizard first-run onboarding",
    [SET_HELP] = "general show the controls buttons help",
    [SET_UPDATE] = "general check for updates version",
    [SET_LOOK] = "general interface simplified lazer look ui",
    [SET_SKIN] = "skin current skin",
    [SET_SENS] = "input left stick reach sensitivity",
    [SET_CURSOR] = "input cursor size",
    [SET_DIM] = "gameplay background dim",
    [SET_STORYBOARD] = "gameplay storyboards",
    [SET_VIDEO] = "gameplay video backgrounds mp4",
    [SET_HUD] = "gameplay hud visibility interface",
    [SET_KEYS] = "gameplay key overlay counter",
    [SET_LIVE] = "gameplay leaderboard in game live",
    [SET_CLASSIC] = "gameplay score display classic standardised scoring",
    [SET_STARS_LO] = "gameplay song select lowest stars difficulty filter",
    [SET_STARS_HI] = "gameplay song select highest stars difficulty filter",
    [SET_CONVERTS] = "gameplay show converted maps converts taiko catch mania",
    [SET_MUSIC] = "audio music volume",
    [SET_EFFECTS] = "audio effects volume hitsounds",
    [SET_OFFSET] = "audio offset latency",
    [SET_WIZARD] = "audio find my offset calibrate",
    [SET_ACCOUNT] = "online ProjectCadence account sign in",
    [SET_CLAN] = "online clan tag join create leave",
    [SET_PASSWORD] = "online change password account",
    [SET_DELETE] = "online delete account remove",
    [SET_BOARD] = "online leaderboard scores osu.ppy.sh",
    [SET_RESCAN] = "maintenance rescan the songs folder ftp",
};

static bool set_shown(int i)
{
    if (app.simple_ui || !app.set_query[0])
        return true;
    /* every word typed has to be in there somewhere */
    const char *q = app.set_query;
    while (*q) {
        while (*q == ' ')
            q++;
        size_t n = strcspn(q, " ");
        if (!n)
            break;
        bool hit = false;
        for (const char *s = set_words[i]; *s && !hit; s++)
            hit = !strncasecmp(s, q, n);
        if (!hit)
            return false;
        q += n;
    }
    return true;
}

/* first shown row from i on, going in dir; i itself if nothing is */
static int set_step(int i, int dir)
{
    for (int k = 1; k <= SET_COUNT; k++) {
        int j = ((i + dir * k) % SET_COUNT + SET_COUNT) % SET_COUNT;
        if (set_shown(j))
            return j;
    }
    return i;
}

static void entry_open(int what)
{
    app.set_entry[0] = 0;
    app.set_entry_kbd.text = app.set_entry;
    app.set_entry_kbd.cap = sizeof(app.set_entry);
    app.set_entering = what;
}

/* what was typed into an account field */
static void entry_done(void)
{
    char *t = app.set_entry;
    switch (app.set_entering) {
    case 1: {
        /* "TAG" joins, "TAG Some Name" starts one */
        char *space = strchr(t, ' ');
        if (space) {
            *space = 0;
            account_clan_create(t, space + 1);
        } else if (t[0]) {
            account_clan_join(t);
        }
        break;
    }
    case 2:
        snprintf(app.set_entry_keep, sizeof(app.set_entry_keep), "%s", t);
        entry_open(3);
        return;
    case 3:
        account_password(app.set_entry_keep, t);
        break;
    case 4:
        account_delete(t);
        break;
    }
    memset(app.set_entry_keep, 0, sizeof(app.set_entry_keep));
    memset(app.set_entry, 0, sizeof(app.set_entry));
    app.set_entering = 0;
}

static void settings_frame(const pad_state *pad)
{
    if (app.set_entering) {
        int k = kbd_frame(&app.set_entry_kbd, pad);
        if (k == KBD_DONE)
            entry_done();
        else if (k == KBD_CANCEL)
            app.set_entering = 0;
        return;
    }
    /* typing in the search box: rows filter as the text changes */
    if (app.set_typing) {
        int k = kbd_frame(&app.set_kbd, pad);
        if (!set_shown(app.set_sel))
            app.set_sel = set_step(app.set_sel, 1);
        if (k != KBD_TYPING)
            app.set_typing = false;
        return;
    }
    if (!app.simple_ui && pad->pressed & PAD_SQUARE) {
        app.set_kbd.text = app.set_query;
        app.set_kbd.cap = sizeof(app.set_query);
        app.set_typing = true;
        return;
    }
    if (!app.simple_ui && pad->pressed & PAD_TRIANGLE)
        app.set_query[0] = 0;
    if (pad->nav_y)
        app.set_sel = set_step(app.set_sel, pad->nav_y > 0 ? 1 : -1);
    else if (!set_shown(app.set_sel))
        app.set_sel = set_step(app.set_sel, 1);
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
    case SET_HUD:
        if (d || pad->pressed & PAD_CROSS)
            app.hud_mode = (app.hud_mode + (d < 0 ? 2 : 1)) % 3;
        break;
    case SET_KEYS:
        if (d || pad->pressed & PAD_CROSS)
            app.key_overlay = !app.key_overlay;
        break;
    case SET_LIVE:
        if (d || pad->pressed & PAD_CROSS)
            app.live_board = !app.live_board;
        break;
    case SET_CLASSIC:
        if (d || pad->pressed & PAD_CROSS)
            app.classic_score = !app.classic_score;
        break;
    case SET_SKIN:
        if (pad->pressed & PAD_CROSS) {
            skin_ui_open(false);
            app.scr = SCREEN_SKINS;
        }
        break;
    case SET_CONVERTS:
        if (d || pad->pressed & PAD_CROSS) {
            app.converts = !app.converts;
            want_converts();
            apply_view();
        }
        break;
    case SET_STARS_LO:
        if (d) {
            app.star_lo = clampf(app.star_lo + d * 0.5f, 0, 15);
            apply_view();
        }
        break;
    case SET_STARS_HI:
        if (d) {
            /* 0 means no top; stepping down from 0 wraps to 15 */
            app.star_hi = app.star_hi + d * 0.5f;
            if (app.star_hi < 0) app.star_hi = 15;
            if (app.star_hi > 15) app.star_hi = 0;
            apply_view();
        }
        break;
    case SET_SETUP:
        if (pad->pressed & PAD_CROSS) {
            setup_open();
            return;
        }
        break;
    case SET_LOOK:
        if (d || pad->pressed & PAD_CROSS) {
            app.simple_ui = !app.simple_ui;
            mapdl_ui_set_simple(app.simple_ui);
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
            app.board_src = (app.board_src + (d < 0 ? 2 : 1)) % 3;
        break;
    case SET_CLAN:
        if (!(pad->pressed & PAD_CROSS) || !account_signed_in())
            break;
        if (!account_clan()[0]) {
            entry_open(1);
        } else if (wall_ms() - app.clan_leave_armed < 3000) {
            account_clan_leave();
            app.clan_leave_armed = 0;
        } else {
            app.clan_leave_armed = wall_ms();
        }
        break;
    case SET_PASSWORD:
        if (pad->pressed & PAD_CROSS && account_signed_in())
            entry_open(2);
        break;
    case SET_DELETE:
        if (pad->pressed & PAD_CROSS && account_signed_in())
            entry_open(4);
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
        {SET_SETUP, "general"}, {SET_SKIN, "skin"}, {SET_SENS, "input"}, {SET_DIM, "gameplay"},
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
    float ys[SET_COUNT], heads[8], y = 250;
    int shown = 0;
    for (int i = 0, s = 0; i < SET_COUNT; i++) {
        bool head = s < nsec && i == sections[s].first;
        if (head)
            heads[s++] = -1;
        ys[i] = -1;
        if (!set_shown(i))
            continue;
        /* a section's header only when something under it is left */
        if (s > 0 && heads[s - 1] < 0) {
            y += head_h;
            heads[s - 1] = y;
        }
        ys[i] = y;
        y += row_h;
        shown++;
    }
    static float scroll;
    float want = shown ? ys[app.set_sel] - GFX_H * 0.45f : 0;
    if (want < 0) want = 0;
    if (want > y - GFX_H + 120) want = fmaxf(0, y - GFX_H + 120);
    scroll = ui_approach(scroll, want, 0.25f);

    draw_text(panel_x + 50, 40 - scroll, 52, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, "settings");
    draw_text(panel_x + 52, 104 - scroll, 22, gfx_rgba(1, 1, 1, 0.6f), ALIGN_LEFT, "how ProjectCadence plays, sounds and looks");

    /* the search box under the title */
    {
        float bx = panel_x + 40, by = 150 - scroll, bw = panel_w - 80;
        ui_round_rect(bx, by, bw, 54, 27, app.set_typing ? gfx_rgba(1, 1, 1, 0.16f) : gfx_rgba(0, 0, 0, 0.3f));
        ui_search_icon(bx + 34, by + 27, 11, gfx_rgba(1, 1, 1, 0.7f));
        if (app.set_query[0])
            draw_text(bx + 64, by + 13, 24, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, app.set_query);
        else
            draw_text(bx + 64, by + 13, 24, gfx_rgba(1, 1, 1, 0.4f), ALIGN_LEFT, "type to search  (SQUARE)");
    }
    if (!shown)
        draw_text(panel_x + panel_w / 2, 300, 26, gfx_rgba(1, 1, 1, 0.6f), ALIGN_CENTRE, "no settings match that");

    for (int s = 0; s < nsec; s++)
        if (heads[s] >= 0)
            draw_text(panel_x + 50, heads[s] - head_h + 22 - scroll, 32, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, sections[s].name);

    for (int i = 0; i < SET_COUNT; i++) {
        if (ys[i] < 0)
            continue;
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
        case SET_HUD: {
            static const char *const modes[3] = {"always", "only in breaks", "never"};
            name = "hud";
            snprintf(val, sizeof(val), "%s", modes[app.hud_mode]);
            break;
        }
        case SET_KEYS: name = "key overlay"; kind = 1; flag = app.key_overlay; break;
        case SET_LIVE: name = "leaderboard while playing"; kind = 1; flag = app.live_board; break;
        case SET_CLASSIC: name = "score display"; snprintf(val, sizeof(val), "%s", app.classic_score ? "classic" : "standardised"); break;
        case SET_WIZARD: name = "find my offset"; kind = 3; break;
        case SET_HELP: name = "show the controls"; kind = 3; break;
        case SET_ACCOUNT:
            name = "ProjectCadence account";
            snprintf(val, sizeof(val), "%s", account_signed_in() ? account_name() : "sign in");
            break;
        case SET_CLAN:
            name = "clan";
            if (!account_signed_in())
                snprintf(val, sizeof(val), "sign in first");
            else if (!account_clan()[0])
                snprintf(val, sizeof(val), "join or start one");
            else
                snprintf(val, sizeof(val), wall_ms() - app.clan_leave_armed < 3000 ? "CROSS again to leave" : "[%s]", account_clan());
            break;
        case SET_PASSWORD: name = "change password"; kind = account_signed_in() ? 3 : 0; break;
        case SET_DELETE: name = "delete account"; kind = account_signed_in() ? 3 : 0; break;
        case SET_BOARD:
            name = "online leaderboard";
            snprintf(val, sizeof(val), "%s", app.board_src == 0 ? "ProjectCadence"
                     : app.board_src == 2 ? (bweb_on() ? "bancho server" : "bancho server (connect in multiplayer)")
                     : online_enabled() ? "osu.ppy.sh" : "osu.ppy.sh (needs osuapi.txt)");
            break;
        case SET_RESCAN: name = "rescan the Songs folder"; kind = 3; break;
        case SET_SKIN: name = "current skin"; snprintf(val, sizeof(val), "%.40s", skins_active()); break;
        case SET_SETUP: name = "run setup wizard"; kind = 3; break;
        case SET_CONVERTS: name = "show converted maps"; kind = 1; flag = app.converts; break;
        case SET_STARS_LO: name = "song select: lowest stars"; snprintf(val, sizeof(val), "%.1f*", app.star_lo); break;
        case SET_STARS_HI:
            name = "song select: highest stars";
            snprintf(val, sizeof(val), app.star_hi > 0 ? "%.1f*" : "no limit", app.star_hi);
            break;
        case SET_LOOK: name = "interface"; snprintf(val, sizeof(val), "%s", app.simple_ui ? "simplified" : "lazer"); break;
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
    case SET_CLAN: hint = "type a tag to join, or a tag and a name to start one: CDN Cadence Crew"; break;
    case SET_DELETE: hint = "scores, replays and your clan place go too; asks for your password"; break;
    default: break;
    }
    float hx = panel_x + panel_w + 50;
    if (hint[0])
        draw_text(hx, GFX_H - 160, 22, gfx_rgba(1, 1, 1, 0.8f), ALIGN_LEFT, hint);
    if (app.status[0])
        draw_text(hx, GFX_H - 120, 24, ui_hex(UI_YELLOW, 1), ALIGN_LEFT, app.status);
    draw_text(GFX_W - 40, GFX_H - 60, 20, gfx_rgba(1, 1, 1, 0.55f), ALIGN_RIGHT,
              "LEFT / RIGHT change   CROSS pick   SQUARE search   TRIANGLE clear   CIRCLE back");
    if (app.set_typing)
        kbd_render(&app.set_kbd);
    if (app.set_entering) {
        static const char *const ask[] = {"", "clan tag (and a name to start one)", "current password", "new password", "password, to delete the account"};
        draw_text(GFX_W / 2, 120, 26, ui_hex(UI_YELLOW, 1), ALIGN_CENTRE, ask[app.set_entering]);
        kbd_render(&app.set_entry_kbd);
    }
}

/* ---- pause / fail / results ---- */

/* pause and fail: a yellow heading over a stack of wide buttons, each with its pad button */
typedef struct {
    const char *label, *button;
    uint32_t colour;
} overlay_button;

/* pause and fail: a spaced-out yellow title, full-width leaning
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

/* one stats cell: a dark label bar over the number */
static void stat_cell(float x, float y, float w, const char *label, uint32_t lc, const char *value)
{
    ui_round_rect(x, y, w, 20, 10, gfx_rgba(0, 0, 0, 0.35f));
    draw_text(x + w / 2, y + 2, 13, lc, ALIGN_CENTRE, label);
    draw_text(x + w / 2, y + 24, 24, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, value);
}

static void render_results(void)
{
    const osu5_hud *h = &app.last_hud;
    char buf[128];

    draw_background(0.6f);
    draw_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0, 0, 0, 0.35f));

    /* score card: player on top, then the panel with the ring */
    const float pw = 475, px = (GFX_W - pw) / 2, cxm = GFX_W / 2;
    ui_round_rect(px, 70, pw, 900, 20, ui_hex(0x3A3A3A, 0.96f));
    ui_round_rect(cxm - 52, 40, 104, 104, 16, ui_hex(0x555555, 1));
    {
        const char *who = account_signed_in() ? account_name() : app.ps_name[0] ? app.ps_name : "guest";
        char ini[2] = {who[0] >= 'a' && who[0] <= 'z' ? (char)(who[0] - 32) : who[0], 0};
        draw_text(cxm, 66, 52, gfx_rgba(1, 1, 1, 0.85f), ALIGN_CENTRE, ini);
        draw_text(cxm, 150, 18, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, app.watching ? "replay" : app.mods & OSU5_MOD_AUTO ? "auto" : who);
    }
    ui_round_rect(px, 180, pw, 790, 20, ui_hex(0x444444, 1));

    char title[200];
    osu5_string(OSU5_STR_TITLE, title, sizeof(title));
    ui_fit(title, 22, pw - 40);
    draw_text(cxm, 200, 22, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, title);
    if (app.playing) {
        snprintf(buf, sizeof(buf), "%s", app.playing->info.artist);
        ui_fit(buf, 15, pw - 40);
        draw_text(cxm, 228, 15, gfx_rgba(1, 1, 1, 0.8f), ALIGN_CENTRE, buf);
    }

    /* the ring: accuracy as a thick band, the rank thresholds as a thin one inside */
    float cy = 430, R = 150;
    arc(cxm, cy, R, 30, 0, 1, gfx_rgba(0, 0, 0, 0.25f));
    arc(cxm, cy, R, 30, 0, h->accuracy, ui_mix(ui_hex(0x66FFCC, 1), ui_hex(0x88B300, 1), 1 - h->accuracy));
    static const struct { float from, to; uint32_t c; const char *tag; } bands[] = {
        {0, 0.7f, 0xFF5A5A, "D"}, {0.7f, 0.8f, 0xFF8E5D, "C"}, {0.8f, 0.9f, 0xFFCC22, "B"},
        {0.9f, 0.95f, 0x88DA20, "A"}, {0.95f, 0.995f, 0x02B5C3, "S"}, {0.995f, 1, 0xDE31AE, "SS"},
    };
    for (int i = 0; i < 6; i++) {
        arc(cxm, cy, R - 34, 5, bands[i].from + 0.003f, bands[i].to - 0.003f, ui_hex(bands[i].c, 1));
        /* the grade's tag just outside the ring, at the middle of its band */
        float mid = (bands[i].from + bands[i].to) / 2 * 6.2831853f - 1.5707963f;
        float tx = cxm + cosf(mid) * (R + 34), ty = cy + sinf(mid) * (R + 34);
        float tw = draw_text_width(13, bands[i].tag) + 16;
        ui_pill(tx - tw / 2, ty - 10, 13, ui_hex(bands[i].c, 1), ui_hex(0x222222, 1), bands[i].tag);
    }
    draw_rank(h->rank, cxm, cy, 170, false, 1);

    snprintf(buf, sizeof(buf), "%lld", (long long)h->score);
    {
        /* grouped in threes */
        char sp[48];
        int n = (int)strlen(buf), o = 0;
        for (int i = 0; i < n; i++) {
            if (i && (n - i) % 3 == 0)
                sp[o++] = ' ';
            sp[o++] = buf[i];
        }
        sp[o] = 0;
        draw_text(cxm, 620, 56, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, sp);
    }

    /* stars and mods, then the difficulty */
    {
        snprintf(buf, sizeof(buf), "%.2f*", osu5_play_stars());
        char tags[64];
        mod_tags(app.mods, tags, sizeof(tags));
        float sw = draw_text_width(15, buf) + 18, tw = tags[0] ? draw_text_width(15, tags) + 18 : 0;
        float x = cxm - (sw + (tw ? tw + 8 : 0)) / 2;
        ui_pill(x, 690, 15, star_colour(osu5_play_stars(), 1), ui_hex(0x222222, 1), buf);
        if (tags[0])
            ui_pill(x + sw + 8, 690, 15, ui_hex(0x8866EE, 1), gfx_rgba(1, 1, 1, 1), tags);
        if (app.playing) {
            snprintf(buf, sizeof(buf), "%s", app.playing->info.version);
            ui_fit(buf, 18, pw - 40);
            draw_text(cxm, 724, 18, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, buf);
            snprintf(buf, sizeof(buf), "mapped by %s", app.playing->info.creator);
            ui_fit(buf, 13, pw - 40);
            draw_text(cxm, 748, 13, gfx_rgba(1, 1, 1, 0.65f), ALIGN_CENTRE, buf);
        }
    }

    /* the statistics grid */
    {
        float gx = px + 18, gw = pw - 36, y = 780;
        float w3 = (gw - 16) / 3;
        snprintf(buf, sizeof(buf), "%.2f%%", h->accuracy * 100);
        stat_cell(gx, y, w3, "ACCURACY", gfx_rgba(1, 1, 1, 0.9f), buf);
        snprintf(buf, sizeof(buf), "%dx", h->max_combo);
        stat_cell(gx + w3 + 8, y, w3, "MAX COMBO", gfx_rgba(1, 1, 1, 0.9f), buf);
        snprintf(buf, sizeof(buf), "%.0f", app.last_pp);
        stat_cell(gx + 2 * (w3 + 8), y, w3, "PP", gfx_rgba(1, 1, 1, 0.55f), buf);
        float w4 = (gw - 24) / 4;
        const int counts[4] = {h->great, h->ok, h->meh, h->miss};
        static const char *names[4] = {"GREAT", "OK", "MEH", "MISS"};
        static const uint32_t cols[4] = {0x66CCFF, 0xB3D944, 0xFFCC22, 0xED1121};
        for (int i = 0; i < 4; i++) {
            snprintf(buf, sizeof(buf), "%d", counts[i]);
            stat_cell(gx + i * (w4 + 8), y + 64, w4, names[i], ui_hex(cols[i], 1), buf);
        }
        const char *line = app.watching ? "watched a replay" : app.new_place == 0 ? "new personal best!" :
                           app.mods & OSU5_MOD_AUTO ? "auto plays aren't saved" : "";
        draw_text(cxm, y + 140, 15, app.new_place == 0 && !app.watching ? ui_hex(0xB3FF66, 1) : gfx_rgba(1, 1, 1, 0.6f),
                  ALIGN_CENTRE, line);
    }

    /* the statistics side: where the hits landed and the accuracy through the map */
    {
        const float sx = 1260, sw = 600;
        ui_round_rect(sx, 180, sw, 520, 20, ui_hex(0x3A3A3A, 0.92f));
        draw_text(sx + 30, 200, 20, gfx_rgba(1, 1, 1, 0.9f), ALIGN_LEFT, "timing distribution");
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
        float gx = sx + 30, gy = 240, gw = sw - 60, gh = 160;
        draw_rect(gx + gw / 2 - 1, gy, 2, gh, gfx_rgba(1, 1, 1, 0.3f));
        for (int b = 0; b < BINS; b++) {
            float bh = (gh - 6) * bins[b] / top;
            float off = fabsf((b - BINS / 2) * 5.0f);
            uint32_t c = ui_hex(off < 20 ? 0x66CCFF : off < 50 ? 0xB3D944 : 0xFFCC22, 0.9f);
            ui_round_rect(gx + b * gw / BINS + 1, gy + gh - bh, gw / BINS - 2, bh, 2, c);
        }
        snprintf(buf, sizeof(buf), "%+.1f ms average     UR %.1f", ne ? sum / ne : 0, h->unstable_rate);
        draw_text(gx, gy + gh + 10, 17, gfx_rgba(1, 1, 1, 0.7f), ALIGN_LEFT, buf);

        float ay = gy + gh + 60, ah = 140;
        draw_text(sx + 30, ay - 34, 20, gfx_rgba(1, 1, 1, 0.9f), ALIGN_LEFT, "accuracy");
        float lo = 1;
        for (int i = 0; i < app.acc_n; i++)
            lo = fminf(lo, app.acc[i]);
        lo = fminf(lo, 0.9f);
        for (int i = 1; i < app.acc_n; i++) {
            float x0 = gx + gw * (i - 1) / (ACC_SAMPLES - 1), x1 = gx + gw * i / (ACC_SAMPLES - 1);
            float y0 = ay + ah - 5 - (ah - 10) * (app.acc[i - 1] - lo) / (1 - lo);
            float y1 = ay + ah - 5 - (ah - 10) * (app.acc[i] - lo) / (1 - lo);
            gfx_bind(0);
            gfx_push(&(gfx_inst){x0, y0, 2, 2, 0, gfx_next_z(), ui_hex(UI_YELLOW, 1), GK_CAPSULE, {x1, y1, 0, 0}});
        }
        snprintf(buf, sizeof(buf), "%.0f%% to 100%%", lo * 100);
        draw_text(gx + gw, ay - 32, 15, gfx_rgba(1, 1, 1, 0.5f), ALIGN_RIGHT, buf);
    }

    if (app.mp_playing)
        mp_board(60, 180, 500, true);

    /* footer: back, the big retry button, and the replay one */
    ui_footer(NULL, "CIRCLE");
    if (!app.mp_playing) {
        float bw = 520, bx = cxm - bw / 2, by = GFX_H - 66;
        ui_round_rect(bx, by, bw, 50, 10, ui_hex(0x88B300, 1));
        draw_text(cxm, by + 13, 22, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, app.watching ? "play it yourself  (CROSS)" : "retry  (CROSS)");
        if (app.last_when && !app.watching) {
            ui_round_rect(bx + bw + 14, by, 200, 50, 10, ui_hex(0x555555, 1));
            draw_text(bx + bw + 114, by + 14, 20, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, "replay  (SQUARE)");
        }
    }
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
    /* the host's room settings take the pad while they're open */
    if (host_tools_open()) {
        host_tools_frame(pad);
        return;
    }
    if (s->i_host && pad->pressed & PAD_OPTIONS && s->state != MP_PLAYING) {
        host_sel = 0;
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
 * bar on the left when it's the one selected */
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

/* full-width button: purple, or another colour, text centred */
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

    /* the room: participants, beatmap and chat in three
     * columns, and the big ready button in the footer */
    ui_crumb(40, 40, 30, "multiplayer", s->room_name, ui_hex(UI_YELLOW, 1));
    /* the room's own card across the top, same as its lounge card */
    {
        const mp_member *host = NULL;
        for (int i = 0; i < s->nmembers; i++)
            if (s->members[i].host)
                host = &s->members[i];
        uint32_t state = s->state == MP_PLAYING ? 0x8866EE : 0xB3FF66;
        ui_round_rect(40, 96, GFX_W - 80, 112, 10, ui_plum(0.16f, 0.97f));
        ui_triangles(GFX_W / 2, 96, GFX_W / 2 - 40, 112, ui_plum(0.22f, 0.6f), wall_ms(), 5, 10);
        ui_round_rect(40, 96, 10, 112, 5, ui_hex(state, 1));
        ui_pill(66, 108, 13, ui_hex(state, 1), ui_hex(0x222222, 1), s->state == MP_PLAYING ? "playing" : "open");
        draw_text(66, 134, 26, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, s->room_name);
        if (host) {
            float w = draw_text(66, 170, 16, gfx_rgba(1, 1, 1, 0.55f), ALIGN_LEFT, "hosted by ");
            draw_text(66 + w, 170, 16, ui_hex(0x8FC3E8, 1), ALIGN_LEFT, host->name);
        }
        snprintf(buf, sizeof(buf), "%d %s", s->nmembers, s->nmembers == 1 ? "player" : "players");
        float ww = draw_text_width(17, buf) + 44;
        ui_round_rect(GFX_W - 64 - ww, 131, ww, 42, 21, gfx_rgba(0, 0, 0, 0.45f));
        draw_text(GFX_W - 64 - ww / 2, 141, 17, gfx_rgba(1, 1, 1, 0.9f), ALIGN_CENTRE, buf);
    }
    const float top = 226, colh = GFX_H - 96 - top - 10;
    int ready = 0;
    for (int i = 0; i < s->nmembers; i++)
        ready += s->members[i].ready;

    /* participants */
    const float lx = 40, lw = 600;
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
    const float mx = 660, mw = 600;
    draw_text(mx, top, 22, gfx_rgba(1, 1, 1, 0.9f), ALIGN_LEFT, "beatmap queue");
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
    draw_text(mx, top + 190, 22, gfx_rgba(1, 1, 1, 0.9f), ALIGN_LEFT, s->freemod ? "extra mods (free)" : "extra mods");
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
        snprintf(hints, sizeof(hints), "SQUARE map   TRIANGLE start   OPTIONS room settings%s", s->teams ? "   L2 team" : "");
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
        app.scr = setup_active ? SCREEN_SETUP : SCREEN_SETTINGS;
}

static void render_account(void)
{
    char msg[128], buf[160];
    int st = account_state(msg, sizeof(msg));
    draw_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0, 0, 0, 0.7f));
    /* sign-in panel: a heading, two fields and the buttons */
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
        {"TRIANGLE", "search"},
        {"L1 / R1", "change the mode (osu!, taiko, catch, mania)"},
        {"L2", "your scores / online leaderboard"},
        {"R2", "watch the top replay"},
        {"R3", "random set"},
        {"OPTIONS", "sort, favourite, delete, get maps, skins, settings"},
        {"CIRCLE", "main menu"},
    }},
    {"pause and results", {
        {"CROSS", "continue / retry"},
        {"TRIANGLE", "retry from the pause menu"},
        {"CIRCLE", "back to song select"},
        {"D-PAD left / right", "this map's offset, while paused"},
        {"SQUARE", "watch the replay, on results"},
    }},
    {"main menu and online", {
        {"L1 / R1", "change the mode"},
        {"SQUARE", "chat"},
        {"R2", "rankings"},
        {"L2", "look a player up"},
        {"in a room: CROSS", "ready / not ready"},
        {"in a room: SQUARE", "pick the map (host) or your mods (free mods)"},
        {"in a room: TRIANGLE", "start, for the host"},
        {"in a room: OPTIONS", "room settings, for the host"},
        {"in a room: R1 / L1", "type in chat / quick lines"},
        {"in a room: CIRCLE", "leave"},
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
    draw_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0.05f, 0.04f, 0.08f, 0.85f));
    /* the header and panel the setup wizard uses */
    draw_rect(0, 0, GFX_W, 110, ui_hex(0x1E1A2B, 1));
    ui_round_rect(60, 34, 8, 44, 4, ui_hex(0x8866EE, 1));
    draw_text(86, 24, 34, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, "controls");
    draw_text(86, 66, 20, gfx_rgba(1, 1, 1, 0.6f), ALIGN_LEFT, "what each button does on a controller");
    for (int i = 0; i < HELP_PAGES; i++) {
        float x = GFX_W - 80 - (HELP_PAGES - 1 - i) * 190;
        ui_round_rect(x - 170, 52, 170, 6, 3, i <= app.help_page ? ui_hex(0x8866EE, 1) : gfx_rgba(1, 1, 1, 0.15f));
        draw_text(x - 85, 66, 15, gfx_rgba(1, 1, 1, i == app.help_page ? 1 : 0.45f), ALIGN_CENTRE, help_pages[i].title);
    }

    const float pw = 1120, px = (GFX_W - pw) / 2, py = 150;
    ui_round_rect(px, py, pw, GFX_H - 96 - 30 - py, 20, ui_hex(0x16131F, 0.98f));
    draw_text(px + 60, py + 40, 40, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, p->title);
    for (int i = 0; i < 14 && p->rows[i][0]; i++) {
        float y = py + 110 + i * 50;
        ui_round_rect(px + 60, y, pw - 120, 42, 8, ui_hex(0x221E2E, 1));
        ui_pill(px + 74, y + 8, 15, ui_hex(0x8866EE, 1), gfx_rgba(1, 1, 1, 1), p->rows[i][0]);
        draw_text(px + 470, y + 9, 21, gfx_rgba(1, 1, 1, 0.9f), ALIGN_LEFT, p->rows[i][1]);
    }

    ui_footer("LEFT / RIGHT page", NULL);
    const char *next = app.help_page == HELP_PAGES - 1 ? "done" : "next";
    float bw = 360, bx = GFX_W - bw - 50, by = GFX_H - 70;
    ui_shear_rect(bx, by, bw, 58, 14, ui_hex(0x6644CC, 1));
    char lab[48];
    snprintf(lab, sizeof(lab), "%s  (CROSS)", next);
    draw_text(bx + bw / 2, by + 15, 24, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, lab);
    draw_text(330, GFX_H - 50, 18, gfx_rgba(1, 1, 1, 0.5f), ALIGN_LEFT, "CIRCLE skip");
}

/* ---- main menu ---- */

/* two rows: settings | play, skins, get maps, quit at the top, and play
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
        app.menu_sel = 1;
        if (px) {
            /* the icon is square; the logo is round. Shrink the art into a
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
    if (pad->pressed & (PAD_L1 | PAD_R1))
        set_mode(app.mode_pick + (pad->pressed & PAD_R1 ? 1 : -1));
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

    /* the toolbar's overlays: chat, rankings, looking a player up, the reader */
    if (pad->pressed & PAD_OPTIONS) {
        reader_open(SCREEN_MENU);
        return;
    }
    if (pad->pressed & PAD_SQUARE) {
        chat_open(SCREEN_MENU);
        return;
    }
    if (pad->pressed & PAD_R2) {
        rankings_open(SCREEN_MENU);
        return;
    }
    if (pad->pressed & PAD_L2) {
        profile_lookup(SCREEN_MENU);
        return;
    }

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

    /* toolbar: settings and home, then the four modes
     * with the current map's lit, the player on the right */
    draw_rect(0, 0, GFX_W, 56, ui_hex(0x191919, 0.96f));
    ui_gear_icon(40, 28, 30, gfx_rgba(1, 1, 1, 0.85f));
    /* home: a roof over a box */
    {
        uint32_t hc = gfx_rgba(1, 1, 1, 0.85f);
        gfx_bind(0);
        gfx_push(&(gfx_inst){111, 22, 10, 1.6f, -0.7f, gfx_next_z(), hc, GK_RECT, {0}});
        gfx_push(&(gfx_inst){125, 22, 10, 1.6f, 0.7f, gfx_next_z(), hc, GK_RECT, {0}});
        draw_rect(108, 26, 2.5f, 13, hc);
        draw_rect(126, 26, 2.5f, 13, hc);
        draw_rect(108, 37, 20.5f, 2.5f, hc);
    }
    {
        int mode = app.mode_pick;
        ui_round_rect(160, 0, 230, 56, 0, ui_hex(0x222222, 1));
        for (int m = 0; m < 4; m++) {
            float cx = 190 + m * 56, cy = 26;
            bool on = m == mode;
            uint32_t c = on ? ui_hex(0x66FFCC, 1) : gfx_rgba(1, 1, 1, 0.8f);
            arc(cx, cy, 14, 2.5f, 0, 1, c);
            switch (m) {
            case 0: ui_disc(cx, cy, 6, c); break;
            case 1: draw_rect(cx - 1, cy - 9, 2.5f, 18, c); arc(cx, cy, 7, 2.5f, 0, 1, c); break;
            case 2:
                ui_disc(cx - 5, cy - 4, 2.5f, c);
                ui_disc(cx + 5, cy - 4, 2.5f, c);
                ui_disc(cx, cy + 5, 2.5f, c);
                break;
            default:
                for (int b = -1; b <= 1; b++)
                    draw_rect(cx + b * 6 - 1.2f, cy - 8 + (b == 0 ? 4 : 0), 2.5f, b == 0 ? 12 : 16, c);
                break;
            }
            if (on)
                ui_round_rect(cx - 12, 50, 24, 4, 2, gfx_rgba(1, 1, 1, 0.95f));
        }
    }
    {
        char who[96];
        const char *name = account_signed_in() ? account_name() : app.ps_name[0] ? app.ps_name : "guest";
        int n = app.mp_on && app.mps.online > 0 ? app.mps.online
              : mp_peek_online(app.mp_kind, app.mp_kind == MP_BANCHO ? app.mp_bancho : app.mp_rooms_url);
        float rx = GFX_W - 28;
        if (n >= 0) {
            snprintf(who, sizeof(who), "%d online", n);
            rx -= draw_text(rx, 18, 18, gfx_rgba(1, 1, 1, 0.55f), ALIGN_RIGHT, who) + 24;
        }
        /* avatar square with the initial, then the name, like the toolbar's user button */
        ui_round_rect(rx - 40, 8, 40, 40, 6, ui_hex(0x555555, 1));
        char ini[2] = {name[0] >= 'a' && name[0] <= 'z' ? (char)(name[0] - 32) : name[0], 0};
        draw_text(rx - 20, 15, 22, gfx_rgba(1, 1, 1, 0.9f), ALIGN_CENTRE, ini);
        rx -= 52 + draw_text(rx - 52, 16, 22, gfx_rgba(1, 1, 1, 0.9f), ALIGN_RIGHT, name) + 40;
        /* the overlay buttons left of the user, with the pad button under each */
        static const char *const tb[4][2] = {{"news", "OPTIONS"}, {"players", "L2"}, {"rankings", "R2"}, {"chat", "SQUARE"}};
        for (int i = 3; i >= 0; i--) {
            float w = draw_text(rx, 10, 20, gfx_rgba(1, 1, 1, 0.85f), ALIGN_RIGHT, tb[i][0]);
            draw_text(rx - w / 2, 34, 13, gfx_rgba(1, 1, 1, 0.4f), ALIGN_CENTRE, tb[i][1]);
            rx -= w + 34;
        }
    }

    /* the button strip through the middle, the logo sitting on its left end;
     * at 1080p: 133 tall, 181 wide, 25 of lean */
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
        draw_text(GFX_W - 30, 72, 28, gfx_rgba(1, 1, 1, 0.95f), ALIGN_RIGHT, buf);
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

/* ---- first-run setup: a step at a time in a centred panel,
 * "next (step)" in the footer, back to go a step back ---- */

enum { SETUP_WELCOME, SETUP_CONTROLS, SETUP_SKIN, SETUP_BEATMAPS, SETUP_BEHAVIOUR, SETUP_STEPS };
static const char *const setup_names[SETUP_STEPS] = {"welcome", "controls", "skin", "beatmaps", "behaviour"};
static int setup_step, setup_focus;


/* the step's own buttons, before next */
static int setup_actions(int step)
{
    switch (step) {
    case SETUP_SKIN: return 1;
    case SETUP_BEATMAPS: return 1;
    case SETUP_BEHAVIOUR: return 3; /* new look, simplified, account */
    default: return 0;
    }
}

static void setup_open(void)
{
    setup_step = 0;
    setup_focus = 0;
    setup_active = true;
    app.scr = SCREEN_SETUP;
}

static void setup_finish(void)
{
    setup_active = false;
    app.onboarded = true;
    if (!skins_chosen())
        skins_set_active("builtin");
    settings_save();
    menu_enter();
}

static void setup_frame(const pad_state *pad)
{
    int n = setup_actions(setup_step) + 1; /* + next */
    setup_focus = (setup_focus + pad->nav_y + n) % n;
    if (setup_step == SETUP_BEHAVIOUR && setup_focus < 2 && pad->nav_x)
        setup_focus = !setup_focus;

    if (pad->pressed & PAD_CIRCLE) {
        if (setup_step > 0) {
            setup_step--;
            setup_focus = setup_actions(setup_step);
        }
        return;
    }
    if (!(pad->pressed & PAD_CROSS))
        return;
    if (setup_focus == n - 1) {
        if (++setup_step >= SETUP_STEPS)
            setup_finish();
        else
            setup_focus = 0;
        return;
    }
    switch (setup_step) {
    case SETUP_SKIN:
        skin_ui_open(true);
        app.scr = SCREEN_SKINS;
        break;
    case SETUP_BEATMAPS:
        mapdl_ui_open();
        app.scr = SCREEN_DOWNLOAD;
        break;
    case SETUP_BEHAVIOUR:
        if (setup_focus < 2) {
            app.simple_ui = setup_focus == 1;
            mapdl_ui_set_simple(app.simple_ui);
        } else {
            app.acc_row = 0;
            app.scr = SCREEN_ACCOUNT;
        }
        break;
    }
}

static void setup_button(float x, float y, float w, const char *label, bool on, bool picked)
{
    uint32_t c = picked ? ui_hex(0x6644CC, 1) : ui_hex(0x2E2A3D, 1);
    if (on) {
        ui_round_rect(x - 3, y - 3, w + 6, 66, 11, gfx_rgba(1, 1, 1, 0.85f));
        c = ui_mix(c, gfx_rgba(1, 1, 1, 1), 0.12f);
    }
    ui_round_rect(x, y, w, 60, 9, c);
    draw_text(x + w / 2, y + 17, 23, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, label);
}

static void render_setup(void)
{
    char buf[200];
    render_menu();
    draw_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0.05f, 0.04f, 0.08f, 0.88f));

    /* header: title and what it's for, like the overlay headers */
    draw_rect(0, 0, GFX_W, 110, ui_hex(0x1E1A2B, 1));
    ui_round_rect(60, 34, 8, 44, 4, ui_hex(0x8866EE, 1));
    draw_text(86, 24, 34, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, "first-run setup");
    draw_text(86, 66, 20, gfx_rgba(1, 1, 1, 0.6f), ALIGN_LEFT, "set up ProjectCadence to suit you");
    /* where we are: a dot per step */
    for (int i = 0; i < SETUP_STEPS; i++) {
        float x = GFX_W - 80 - (SETUP_STEPS - 1 - i) * 150;
        bool here = i == setup_step;
        ui_round_rect(x - 130, 52, 130, 6, 3, i <= setup_step ? ui_hex(0x8866EE, 1) : gfx_rgba(1, 1, 1, 0.15f));
        draw_text(x - 65, 66, 16, gfx_rgba(1, 1, 1, here ? 1 : 0.45f), ALIGN_CENTRE, setup_names[i]);
    }

    /* the step's panel */
    const float pw = 1120, px = (GFX_W - pw) / 2, py = 150, ph = GFX_H - 96 - 30 - py;
    ui_round_rect(px, py, pw, ph, 20, ui_hex(0x16131F, 0.98f));
    float x = px + 60, y = py + 50, w = pw - 120;
    uint32_t text = gfx_rgba(1, 1, 1, 0.85f), dim = gfx_rgba(1, 1, 1, 0.55f);
    switch (setup_step) {
    case SETUP_WELCOME:
        draw_text(x, y, 44, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, "welcome");
        draw_text(x, y + 80, 24, text, ALIGN_LEFT, "Welcome to the first-run setup! It gets you playing in about a minute.");
        draw_text(x, y + 120, 24, text, ALIGN_LEFT, "ProjectCadence plays osu! beatmaps on your PS5: circles, taiko, catch and mania.");
        draw_text(x, y + 180, 22, dim, ALIGN_LEFT, "Everything here can be changed later in settings, where this guide lives too.");
        if (app.logo_tex) {
            float s = 260, lx = px + pw - 220, ly = py + ph - 200;
            ui_disc(lx, ly, s / 2 + 10, gfx_rgba(1, 1, 1, 1));
            gfx_bind(app.logo_tex);
            gfx_push(&(gfx_inst){lx, ly, s / 2, s / 2, 0, gfx_next_z(), gfx_rgba(1, 1, 1, 1), GK_TEX, {0, 0, 1, 1}});
            gfx_bind(0);
        }
        break;
    case SETUP_CONTROLS: {
        draw_text(x, y, 44, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, "controls");
        draw_text(x, y + 70, 22, dim, ALIGN_LEFT, "how a controller plays osu!; the full list is under settings, show the controls");
        const struct help_page *p = &help_pages[0];
        for (int i = 0; i < 14 && p->rows[i][0]; i++) {
            float ry = y + 130 + i * 52;
            ui_round_rect(x, ry, w, 44, 8, ui_hex(0x221E2E, 1));
            ui_pill(x + 14, ry + 9, 15, ui_hex(0x8866EE, 1), gfx_rgba(1, 1, 1, 1), p->rows[i][0]);
            draw_text(x + 330, ry + 10, 21, text, ALIGN_LEFT, p->rows[i][1]);
        }
        break;
    }
    case SETUP_SKIN:
        draw_text(x, y, 44, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, "skin");
        draw_text(x, y + 80, 24, text, ALIGN_LEFT, "A skin is the look of the circles, the cursor, numbers and sounds.");
        draw_text(x, y + 120, 22, dim, ALIGN_LEFT, "Pick one from the list, or stay with the plain built-in look.");
        snprintf(buf, sizeof(buf), "current skin:  %s", skins_chosen() ? skins_active() : "none yet (built-in)");
        draw_text(x, y + 200, 24, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, buf);
        setup_button(x, y + 260, w, "choose a skin", setup_focus == 0, false);
        break;
    case SETUP_BEATMAPS:
        draw_text(x, y, 44, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, "beatmaps");
        draw_text(x, y + 80, 24, text, ALIGN_LEFT, "You'll need some beatmaps to play. They download straight to the console.");
        draw_text(x, y + 120, 22, dim, ALIGN_LEFT, "Or copy .osz sets into /data/cadence/Songs over FTP and rescan in settings.");
        snprintf(buf, sizeof(buf), "%d difficulties installed", app.songs.count);
        draw_text(x, y + 200, 24, app.songs.count ? ui_hex(0xB3FF66, 1) : ui_hex(UI_YELLOW, 1), ALIGN_LEFT, buf);
        setup_button(x, y + 260, w, "get beatmaps", setup_focus == 0, false);
        break;
    case SETUP_BEHAVIOUR: {
        draw_text(x, y, 44, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, "behaviour");
        draw_text(x, y + 80, 24, text, ALIGN_LEFT, "Pick how the menus look. The new look follows osu!lazer;");
        draw_text(x, y + 120, 24, text, ALIGN_LEFT, "simplified is the plain one, big lists and fewer moving parts.");
        float hw = (w - 20) / 2;
        setup_button(x, y + 200, hw, "new look", setup_focus == 0, !app.simple_ui);
        setup_button(x + hw + 20, y + 200, hw, "simplified", setup_focus == 1, app.simple_ui);
        draw_text(x, y + 310, 22, dim, ALIGN_LEFT,
                  account_signed_in() ? "you're signed in, your best scores go on the global board"
                                      : "an account puts your scores on the global board (optional)");
        setup_button(x, y + 350, w, account_signed_in() ? "account" : "sign in or make an account", setup_focus == 2, false);
        break;
    }
    }

    /* footer: back and the leaning next button */
    ui_footer(NULL, setup_step ? "CIRCLE" : NULL);
    if (setup_step == SETUP_STEPS - 1)
        snprintf(buf, sizeof(buf), "finish");
    else if (setup_step == 0)
        snprintf(buf, sizeof(buf), "get started");
    else
        snprintf(buf, sizeof(buf), "next (%s)", setup_names[setup_step + 1]);
    bool on = setup_focus == setup_actions(setup_step);
    float bw = 420, bx = GFX_W - bw - 50, by = GFX_H - 70;
    if (on)
        ui_shear_rect(bx - 4, by - 4, bw + 8, 66, 16, gfx_rgba(1, 1, 1, 0.85f));
    ui_shear_rect(bx, by, bw, 58, 14, ui_hex(on ? 0x8866EE : 0x6644CC, 1));
    draw_text(bx + bw / 2, by + 15, 24, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, buf);
}

#include "classic.inc"
#include "overlays.inc"

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
    mapdl_ui_set_mode(app.mode_pick);
    mapdl_ui_set_simple(app.simple_ui);
    audio_music_volume(app.music_vol);
    scores_load();
    online_init();
    update_check();
    stars_init();
    LOG("settings read");
    skin_apply(skins_active());
    LOG("skin %s", skins_active());
    if (!skins_chosen() && app.onboarded) {
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
    if (account_signed_in() && app.mp_kind == MP_CADENCE) {
        mp_connect(MP_CADENCE, app.mp_rooms_url, app.mp_user[0] ? app.mp_user : account_name(), app.mp_pass);
        app.mp_on = true;
    }
    if (app.scr == SCREEN_SELECT) {
        menu_enter();
        if (!app.onboarded)
            setup_open();
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
            } else if (pad.pressed & PAD_TRIANGLE && app.rec.count && !app.watching) {
                const osu5_hud *h = &app.last_hud;
                osr_info in = {.mode = play_mode(app.playing), .n300 = h->great, .n100 = h->ok, .n50 = h->meh, .miss = h->miss,
                               .score = (int)h->score, .max_combo = h->max_combo, .perfect = h->miss == 0, .mods = app.mods,
                               .when = (long long)time(NULL) * 1000};
                snprintf(in.md5, sizeof(in.md5), "%s", map_md5(app.playing->file));
                snprintf(in.player, sizeof(in.player), "%s", account_signed_in() ? account_name() : app.ps_name[0] ? app.ps_name : "player");
                char title[200], name[256];
                snprintf(title, sizeof(title), "%.50s - %.60s [%.40s]", app.playing->info.artist, app.playing->info.title, app.playing->info.version);
                toast(osr_export(&in, &app.rec, title, name, sizeof(name)) ? "saved to exports: %s" : "couldn't save the replay%s", "");
            } else if (pad.pressed & PAD_CIRCLE) {
                back_to_select();
            }
            break;
        case SCREEN_SKINS:
            if (skin_ui_frame(&pad)) {
                app.scr = setup_active ? SCREEN_SETUP : SCREEN_SELECT;
                if (!app.onboarded && !setup_active) {
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
                app.scr = setup_active ? SCREEN_SETUP : SCREEN_SELECT;
            break;
        case SCREEN_WIZARD:
            wizard_frame(&pad);
            break;
        case SCREEN_MENU:
            if (app.simple_ui)
                menu_frame_classic(&pad);
            else
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
        case SCREEN_SETUP:
            setup_frame(&pad);
            break;
        case SCREEN_RANKINGS:
            rankings_frame(&pad);
            break;
        case SCREEN_PROFILE:
            profile_frame(&pad);
            break;
        case SCREEN_CHAT:
            chat_frame(&pad);
            break;
        case SCREEN_SPECTATE:
            spectate_frame(&pad);
            break;
        case SCREEN_IMPORTS:
            imports_frame(&pad);
            break;
        case SCREEN_SETINFO:
            setinfo_frame(&pad);
            break;
        case SCREEN_READER:
            reader_frame(&pad);
            break;
        }

        {
            /* a toast per finished download, whichever screen we're on */
            static int toasted;
            mapdl_status ts;
            mapdl_progress(&ts);
            if (ts.installed > toasted) {
                toast("%s", ts.msg[0] ? ts.msg : "download finished");
                toasted = ts.installed;
            }
            char am[128];
            int as = account_action(am, sizeof(am));
            if ((as == ACC_OK || as == ACC_FAILED) && am[0])
                toast("%s", am);
            static int sub_seen;
            char sm[128];
            int ss = bweb_submit_state(sm, sizeof(sm));
            if (ss != sub_seen) {
                if (ss >= 2)
                    toast("bancho: %s", sm);
                sub_seen = ss;
            }
        }
        gfx_begin(0.06f, 0.06f, 0.09f);
        switch (app.scr) {
        case SCREEN_SELECT:
            app.simple_ui ? render_select_classic() : render_select();
            if (sel_opt >= 0)
                render_sel_opts();
            if (coll_open())
                render_coll();
            break;
        case SCREEN_PLAY:
            render_play();
            break;
        case SCREEN_PAUSE:
            render_play();
            if (app.simple_ui)
                render_overlay_classic("paused", gfx_rgba(1, 1, 1, 1), "CROSS continue   TRIANGLE retry   CIRCLE quit");
            else {
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
                if (app.simple_ui)
                    render_overlay_classic("failed", gfx_rgba(1, 0.35f, 0.35f, 1), "CROSS retry   CIRCLE back");
                else
                    render_overlay("failed", fb, 2);
            }
            break;
        case SCREEN_RESULTS:
            app.simple_ui ? render_results_classic() : render_results();
            break;
        case SCREEN_SKINS:
            draw_background(0.7f);
            skin_ui_render();
            break;
        case SCREEN_MODS:
            if (app.simple_ui) {
                render_select_classic();
                render_mods_classic();
            } else {
                render_select();
                render_mods();
            }
            if (app.mod_custom >= 0)
                render_custom();
            break;
        case SCREEN_SETTINGS:
            draw_background(0.7f);
            app.simple_ui ? render_settings_classic() : render_settings();
            break;
        case SCREEN_DOWNLOAD:
            draw_background(0.7f);
            mapdl_ui_render();
            break;
        case SCREEN_WIZARD:
            render_wizard();
            break;
        case SCREEN_MENU:
            app.simple_ui ? render_menu_classic() : render_menu();
            break;
        case SCREEN_HELP:
            if (help_return == SCREEN_MENU)
                app.simple_ui ? render_menu_classic() : render_menu();
            else
                draw_background(0.7f);
            render_help();
            break;
        case SCREEN_ACCOUNT:
            draw_background(0.7f);
            app.simple_ui ? render_account_classic() : render_account();
            break;
        case SCREEN_MP:
            app.simple_ui ? render_mp_classic() : render_mp();
            if (host_tools_open())
                render_host_tools();
            break;
        case SCREEN_SETUP:
            render_setup();
            break;
        case SCREEN_RANKINGS:
            render_rankings();
            break;
        case SCREEN_PROFILE:
            render_profile();
            break;
        case SCREEN_CHAT:
            render_chat();
            break;
        case SCREEN_SPECTATE:
            render_spectate();
            break;
        case SCREEN_IMPORTS:
            render_imports();
            break;
        case SCREEN_SETINFO:
            render_setinfo();
            break;
        case SCREEN_READER:
            render_reader();
            break;
        }
        {
            /* screens fade in from black when they change, instead of cutting */
            static screen shown = SCREEN_MENU;
            static double since;
            if (app.scr != shown) {
                bool quiet = (shown == SCREEN_PLAY && app.scr == SCREEN_PAUSE) || (shown == SCREEN_PAUSE && app.scr == SCREEN_PLAY);
                shown = app.scr;
                since = quiet ? 0 : wall_ms();
            }
            float k = since > 0 ? 1 - (float)((wall_ms() - since) / 220.0) : 0;
            if (k > 0)
                draw_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0, 0, 0, 0.75f * k * k));
        }
        if (app.scr != SCREEN_PLAY)
            render_toasts();
        gfx_end();
    }
}
