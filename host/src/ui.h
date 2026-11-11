/* Shapes the menus are built from: rounded boxes, sheared buttons, the drifting
 * triangles. Layout and colours lean on osu!lazer's (https://github.com/ppy/osu, MIT). */
#ifndef OSU5_UI_H
#define OSU5_UI_H

#include <stdbool.h>
#include <stdint.h>

/* lazer's palette */
#define UI_PINK 0xFF66AA
#define UI_BLUE 0x66CCFF
#define UI_YELLOW 0xFFCC22
#define UI_GREEN 0x88B300
#define UI_PURPLE 0x6644CC
#define UI_PURPLE_DARK 0x5E3FBA
#define UI_LIME 0xA5CC00
#define UI_ORANGE 0xEEAA00
#define UI_GREY 0x555555
#define UI_RED 0xEE3399

uint32_t ui_hex(uint32_t rgb, float a);
uint32_t ui_mix(uint32_t a, uint32_t b, float k); /* both rgba8 */

void ui_init(void);

void ui_round_rect(float x, float y, float w, float h, float r, uint32_t c);
/* a parallelogram leaning right by `lean` pixels at the top */
void ui_shear_rect(float x, float y, float w, float h, float lean, uint32_t c);
/* a soft dark shadow under a box */
void ui_shadow(float x, float y, float w, float h, float r);
/* triangles drifting up through a box, lazer's background motif */
void ui_triangles(float x, float y, float w, float h, uint32_t base, double time_ms, unsigned seed, int count);
/* a filled triangle pointing right, for play icons */
void ui_play_icon(float cx, float cy, float size, uint32_t c);
void ui_gear_icon(float cx, float cy, float size, uint32_t c);
void ui_arrow_down(float cx, float cy, float size, uint32_t c);
void ui_arrow_left(float cx, float cy, float size, uint32_t c);
void ui_disc(float cx, float cy, float r, uint32_t c);

/* time-based easing for screen-to-screen motion */
/* cuts s down with "..." until it fits; s needs 4 bytes spare */
void ui_fit(char *s, float size, float max_w);
void ui_search_icon(float cx, float cy, float size, uint32_t c);

float ui_approach(float cur, float target, float rate);

#endif
