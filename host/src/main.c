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
#include "sce.h"
#include "skin.h"
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

typedef enum { SCREEN_SELECT, SCREEN_PLAY, SCREEN_PAUSE, SCREEN_FAILED, SCREEN_RESULTS } screen;

static struct {
    screen scr;
    song_list songs;
    int sel;
    float sel_scroll;
    int mods;

    int offset_ms;      /* added to the audio clock, positive = notes later */
    float sensitivity;
    float music_vol, fx_vol;

    int bg_tex;
    int bg_w, bg_h;

    double play_time;
    double fail_time;
    double finish_seen;
    osu5_hud last_hud;
    play_input cursor;

    char status[256];
} app;

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

static void settings_load(void)
{
    app.offset_ms = 0;
    app.sensitivity = 1.0f;
    app.music_vol = 0.7f;
    app.fx_vol = 0.8f;

    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f)
        return;
    char key[64];
    float v;
    while (fscanf(f, "%63s %f", key, &v) == 2) {
        if (!strcmp(key, "offset")) app.offset_ms = (int)v;
        else if (!strcmp(key, "sensitivity")) app.sensitivity = v;
        else if (!strcmp(key, "music")) app.music_vol = v;
        else if (!strcmp(key, "effects")) app.fx_vol = v;
    }
    fclose(f);
}

static void settings_save(void)
{
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f)
        return;
    fprintf(f, "offset %d\nsensitivity %.2f\nmusic %.2f\neffects %.2f\n", app.offset_ms, app.sensitivity, app.music_vol, app.fx_vol);
    fclose(f);
}

static void load_background(const char *path)
{
    if (app.bg_tex) {
        gfx_texture_free(app.bg_tex);
        app.bg_tex = 0;
    }

    int w, h, n;
    unsigned char *px = stbi_load(path, &w, &h, &n, 4);
    if (!px)
        return;

    /* big backgrounds just waste memory, halve until it fits 2048 */
    while (w > 2048 || h > 2048) {
        int nw = w / 2, nh = h / 2;
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++)
                memcpy(px + (y * nw + x) * 4, px + ((y * 2) * w + x * 2) * 4, 4);
        w = nw;
        h = nh;
    }

    app.bg_tex = gfx_texture(w, h, px);
    app.bg_w = w;
    app.bg_h = h;
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

static bool start_song(const song_entry *e)
{
    snprintf(app.status, sizeof(app.status), "loading...");

    osu5_set_mods(app.mods);
    if (osu5_load(e->file) != 0) {
        char err[200];
        osu5_string(OSU5_STR_ERROR, err, sizeof(err));
        snprintf(app.status, sizeof(app.status), "couldn't load map: %s", err);
        LOG("%s", app.status);
        return false;
    }

    char path[768];
    snprintf(path, sizeof(path), "%s/%s", e->dir, e->info.audio);
    sound_buf music;
    if (!audio_decode_file(path, &music)) {
        snprintf(app.status, sizeof(app.status), "couldn't decode %s", e->info.audio);
        LOG("%s", app.status);
        osu5_unload();
        return false;
    }

    if (e->info.background[0]) {
        snprintf(path, sizeof(path), "%s/%s", e->dir, e->info.background);
        load_background(path);
    }

    audio_music_set(&music);
    audio_music_volume(app.music_vol);
    audio_music_play(osu5_start_time());
    LOG("play %s from %.0f ms", e->file, osu5_start_time());

    app.cursor.x = 256;
    app.cursor.y = 192;
    app.finish_seen = 0;
    app.status[0] = 0;
    app.scr = SCREEN_PLAY;
    return true;
}

static void back_to_select(void)
{
    audio_music_stop();
    osu5_unload();
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
        if (app.finish_seen == 0)
            app.finish_seen = wall_ms();
        else if (wall_ms() - app.finish_seen > 1500)
            app.scr = SCREEN_RESULTS;
    }
}

static void render_play(void)
{
    double t = app.play_time;
    int np = 0;
    int n = osu5_frame(t, cmds, MAX_CMDS, points, MAX_POINTS, &np);

    draw_background(app.last_hud.state == OSU5_STATE_BREAK ? 0.5f : 0.8f);
    draw_gameplay(cmds, n, points, np);
    draw_hud(&app.last_hud, t);
}

static const char *rank_name(int r)
{
    static const char *names[] = {"D", "C", "B", "A", "S", "S", "SS", "SS", "F"};
    return r >= 0 && r <= 8 ? names[r] : "?";
}

static void render_select(void)
{
    draw_background(0.75f);

    if (app.songs.count == 0) {
        draw_text(GFX_W / 2, 420, 48, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, "no beatmaps found");
        draw_text(GFX_W / 2, 500, 28, gfx_rgba(1, 1, 1, 0.8f), ALIGN_CENTRE, "copy extracted .osz folders to " SONGS_DIR);
        draw_text(GFX_W / 2, 545, 28, gfx_rgba(1, 1, 1, 0.8f), ALIGN_CENTRE, "then press CROSS to rescan");
        return;
    }

    /* list, scrolled so the selection sits mid screen */
    const float row = 74;
    app.sel_scroll += (app.sel - app.sel_scroll) * 0.25f;
    int first = (int)app.sel_scroll - 8;
    for (int i = first < 0 ? 0 : first; i < app.songs.count && i < app.sel_scroll + 9; i++) {
        float y = GFX_H / 2 - row / 2 + (i - app.sel_scroll) * row;
        const osu5_peek_info *in = &app.songs.items[i].info;
        bool on = i == app.sel;
        float x = on ? 900 : 960;

        draw_rect(x, y, GFX_W - x, row - 6, on ? gfx_rgba(1, 1, 1, 0.9f) : gfx_rgba(0, 0, 0, 0.55f));
        uint32_t tc = on ? gfx_rgba(0.1f, 0.1f, 0.15f, 1) : gfx_rgba(1, 1, 1, 0.95f);
        char line[300];
        snprintf(line, sizeof(line), "%s - %s", in->artist, in->title);
        draw_text(x + 20, y + 8, 26, tc, ALIGN_LEFT, line);
        snprintf(line, sizeof(line), "[%s]", in->version);
        draw_text(x + 20, y + 40, 20, tc, ALIGN_LEFT, line);
    }

    const osu5_peek_info *in = &app.songs.items[app.sel].info;
    char buf[300];
    draw_rect(40, 60, 800, 260, gfx_rgba(0, 0, 0, 0.6f));
    draw_text(60, 80, 34, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, in->title);
    draw_text(60, 126, 26, gfx_rgba(1, 1, 1, 0.85f), ALIGN_LEFT, in->artist);
    snprintf(buf, sizeof(buf), "[%s] mapped by %s", in->version, in->creator);
    draw_text(60, 166, 22, gfx_rgba(1, 1, 1, 0.75f), ALIGN_LEFT, buf);
    snprintf(buf, sizeof(buf), "CS %.1f  AR %.1f  OD %.1f  HP %.1f", in->cs, in->ar, in->od, in->hp);
    draw_text(60, 214, 26, gfx_rgba(1, 1, 1, 0.95f), ALIGN_LEFT, buf);
    snprintf(buf, sizeof(buf), "%s%s  offset %+dms", app.mods & OSU5_MOD_AUTO ? "AUTO " : "", app.mods & OSU5_MOD_NOFAIL ? "NF " : "",
             app.offset_ms);
    draw_text(60, 262, 24, gfx_rgba(1, 0.85f, 0.4f, 1), ALIGN_LEFT, buf);

    draw_text(40, GFX_H - 60, 22, gfx_rgba(1, 1, 1, 0.7f), ALIGN_LEFT,
              "CROSS play   TRIANGLE auto   SQUARE no fail   L1/R1 offset");

    if (app.status[0])
        draw_text(GFX_W / 2, 360, 26, gfx_rgba(1, 0.5f, 0.5f, 1), ALIGN_CENTRE, app.status);
}

static void select_frame(const pad_state *pad)
{
    if (app.songs.count == 0) {
        if (pad->pressed & PAD_CROSS)
            songs_scan(SONGS_DIR, &app.songs);
        return;
    }

    int prev = app.sel;
    static double hold_since;
    static int hold_dir;

    int dir = 0;
    if (pad->held & PAD_DOWN || pad->ly > 0.6f) dir = 1;
    if (pad->held & PAD_UP || pad->ly < -0.6f) dir = -1;

    if (dir != hold_dir) {
        hold_dir = dir;
        hold_since = wall_ms();
        app.sel += dir;
    } else if (dir != 0 && wall_ms() - hold_since > 350) {
        app.sel += dir; /* key repeat */
        hold_since = wall_ms() - 280;
    }
    if (pad->pressed & PAD_RIGHT) app.sel += 10;
    if (pad->pressed & PAD_LEFT) app.sel -= 10;

    if (app.sel < 0) app.sel = 0;
    if (app.sel >= app.songs.count) app.sel = app.songs.count - 1;

    if (pad->pressed & PAD_TRIANGLE) app.mods ^= OSU5_MOD_AUTO;
    if (pad->pressed & PAD_SQUARE) app.mods ^= OSU5_MOD_NOFAIL;
    if (pad->pressed & PAD_L1) { app.offset_ms -= 5; settings_save(); }
    if (pad->pressed & PAD_R1) { app.offset_ms += 5; settings_save(); }

    const song_entry *e = &app.songs.items[app.sel];

    if (app.sel != prev) {
        app.status[0] = 0;
        if (prev < 0 || prev >= app.songs.count || strcmp(app.songs.items[prev].dir, e->dir) != 0) {
            char path[768];
            snprintf(path, sizeof(path), "%s/%s", e->dir, e->info.background);
            load_background(path);
        }
    }

    if (pad->pressed & PAD_CROSS)
        start_song(e);
}

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

    draw_text(520, 330, 260, gfx_rgba(1, 0.85f, 0.35f, 1), ALIGN_CENTRE, rank_name(h->rank));

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

    settings_load();
    LOG("settings read");
    skin_load(SKIN_DIR);
    LOG("skin loaded");
    songs_scan(SONGS_DIR, &app.songs);
    LOG("%d maps", app.songs.count);
    if (app.songs.count > 0) {
        char path[768];
        snprintf(path, sizeof(path), "%s/%s", app.songs.items[0].dir, app.songs.items[0].info.background);
        load_background(path);
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
                start_song(&app.songs.items[app.sel]);
            }
            break;
        case SCREEN_FAILED: {
            /* slow the music down to nothing, like stable's fail */
            double k = (wall_ms() - app.fail_time) / 1500.0;
            audio_music_volume(app.music_vol * (float)fmax(0, 1 - k));
            if (k > 1) audio_music_stop();
            if (pad.pressed & PAD_CROSS) start_song(&app.songs.items[app.sel]);
            else if (pad.pressed & PAD_CIRCLE) back_to_select();
            break;
        }
        case SCREEN_RESULTS:
            if (pad.pressed & PAD_CROSS) start_song(&app.songs.items[app.sel]);
            else if (pad.pressed & PAD_CIRCLE) back_to_select();
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
        }
        gfx_end();
    }
}
