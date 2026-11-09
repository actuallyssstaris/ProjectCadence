#ifndef OSU5_SKIN_IMG_H
#define OSU5_SKIN_IMG_H

#include <stdbool.h>
#include <stdint.h>

/* tex 0 = the skin doesn't have it; w/h are in SD pixels (an @2x file counts half) */
typedef struct {
    int tex;
    float w, h;
} skin_sprite;

typedef struct {
    bool on; /* gameplay art present (hitcircle); the HUD bits are used whenever loaded */
    skin_sprite circle, overlay, approach, digit[10];
    skin_sprite cursor, ball, follow, reverse, tick, point;
    skin_sprite hit0, hit50, hit100, hit300;
    float overlap; /* HitCircleOverlap, SD pixels */

    /* HUD: score-*, the combo font (ComboPrefix, score by default), health bar */
    skin_sprite score[10], score_comma, score_dot, score_percent, score_x;
    skin_sprite combo[10], combo_x;
    float score_overlap, combo_overlap;
    skin_sprite bar_bg, bar_colour;

    /* spinner, new style (top/bottom/middle) or old (circle/background/metre) */
    skin_sprite sp_glow, sp_bottom, sp_top, sp_middle, sp_middle2;
    skin_sprite sp_circle, sp_background, sp_metre;
    skin_sprite sp_approach, sp_clear;

    /* ranking letters in OSU5_RANK_* order (F has none) */
    skin_sprite rank[8], rank_small[8];

    /* skin.ini [Colours]; 0xAARRGGBB */
    uint32_t combo_colours[8];
    int ncombo;
    bool has_border, has_track;
    uint32_t border, track;
} skin_images;

extern skin_images skin_img;

/* NULL or a folder without images = the drawn look */
void skin_images_load(const char *dir);

#endif
