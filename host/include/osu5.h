/* C side of core/Native/Exports.cs. Struct layouts must match core/Play/DrawList.cs. */
#ifndef OSU5_H
#define OSU5_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    OSU5_DRAW_CIRCLE = 1,
    OSU5_DRAW_APPROACH,
    OSU5_DRAW_SLIDER_BODY,
    OSU5_DRAW_SLIDER_BALL,
    OSU5_DRAW_FOLLOW_CIRCLE,
    OSU5_DRAW_TICK,
    OSU5_DRAW_REVERSE,
    OSU5_DRAW_SPINNER,
    OSU5_DRAW_JUDGEMENT,
    OSU5_DRAW_FOLLOW_POINT,
    OSU5_DRAW_CURSOR,
};

/* HitResult values, only the ones the host shows */
enum {
    OSU5_RESULT_MISS = 1,
    OSU5_RESULT_MEH = 2,
    OSU5_RESULT_OK = 3,
    OSU5_RESULT_GREAT = 4,
};

enum {
    OSU5_STATE_INTRO = 0,
    OSU5_STATE_PLAYING,
    OSU5_STATE_BREAK,
    OSU5_STATE_FAILED,
    OSU5_STATE_FINISHED,
};

enum { OSU5_RANK_D, OSU5_RANK_C, OSU5_RANK_B, OSU5_RANK_A, OSU5_RANK_S, OSU5_RANK_SH, OSU5_RANK_X, OSU5_RANK_XH, OSU5_RANK_F };

enum {
    OSU5_MOD_AUTO = 1,
    OSU5_MOD_NOFAIL = 2,
    OSU5_MOD_HIDDEN = 4,
    OSU5_MOD_HARDROCK = 8,
    OSU5_MOD_EASY = 16,
    OSU5_MOD_DOUBLETIME = 32,
    OSU5_MOD_HALFTIME = 64,
};

enum {
    OSU5_SOUND_HIT = 1,
    OSU5_SOUND_SLIDER_TICK,
    OSU5_SOUND_SLIDER_SLIDE,
    OSU5_SOUND_SPINNER_BONUS,
    OSU5_SOUND_COMBO_BREAK,
};

/* hitsound bits */
enum { OSU5_HS_NORMAL = 1, OSU5_HS_WHISTLE = 2, OSU5_HS_FINISH = 4, OSU5_HS_CLAP = 8 };

typedef struct {
    int32_t kind;
    uint32_t colour; /* 0xAARRGGBB */
    int32_t number;
    int32_t flags;
    float x, y;
    float radius;
    float alpha;
    float p0, p1;
    int32_t path_start;
    int32_t path_count;
} osu5_draw;

typedef struct {
    int64_t score;
    int32_t combo;
    int32_t max_combo;
    float accuracy;
    float health;
    int32_t great, ok, meh, miss;
    int32_t state;
    int32_t rank;
    float progress;
    float unstable_rate;
    double skip_target;
} osu5_hud;

typedef struct {
    int32_t kind;
    int32_t sounds;
    int32_t bank;
    int32_t add_bank;
    int32_t volume;
    int32_t custom;
    float pan;
} osu5_sound;

typedef struct {
    int32_t mode;
    int32_t preview_time;
    int32_t set_id;
    float cs, ar, od, hp;
    char title[128];
    char artist[128];
    char version[96];
    char creator[64];
    char audio[128];
    char background[128];
} osu5_peek_info;

enum { OSU5_STR_TITLE, OSU5_STR_ARTIST, OSU5_STR_VERSION, OSU5_STR_CREATOR, OSU5_STR_AUDIO, OSU5_STR_BACKGROUND, OSU5_STR_ERROR };

int osu5_gc_check(void);
float osu5_play_stars(void); /* the loaded map, with its mods */
float osu5_pp(void);         /* the current score */
float osu5_stars(const char *path, int mods); /* -1 on failure; safe off the main thread */ /* forces a collection, returns the gen0 count */
int osu5_load(const char *path);
void osu5_unload(void);
void osu5_set_mods(int flags); /* applies from the next osu5_load */
void osu5_set_skin_colours(const uint32_t *argb, int n);
double osu5_start_time(void);
void osu5_update(double time, float x, float y, int keys);
int osu5_frame(double time, osu5_draw *cmds, int max_cmds, float *points, int max_points, int *point_count);
void osu5_get_hud(double time, osu5_hud *hud);
int osu5_poll_sound(osu5_sound *ev);
int osu5_string(int id, char *buf, int len);
void osu5_difficulty(float *out4);
int osu5_peek(const char *path, osu5_peek_info *info);

#ifdef __cplusplus
}
#endif

#endif
