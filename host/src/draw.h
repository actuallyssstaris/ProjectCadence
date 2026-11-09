#ifndef DRAW_H
#define DRAW_H

#include "../include/osu5.h"
#include <stdbool.h>
#include <stdint.h>

enum { ALIGN_LEFT, ALIGN_CENTRE, ALIGN_RIGHT };

void draw_init(void);

void draw_rect(float x, float y, float w, float h, uint32_t rgba);
void draw_image(int tex, float x, float y, float w, float h, uint32_t rgba);
float draw_text(float x, float y, float size, uint32_t rgba, int align, const char *s);
float draw_text_width(float size, const char *s);

/* playfield <-> screen */
void draw_set_playfield(float scale, float ox, float oy);
void draw_to_screen(float px, float py, float *sx, float *sy);

void draw_gameplay(const osu5_draw *cmds, int n, const float *points, int npoints);
void draw_hud(const osu5_hud *hud, double time);
void draw_set_cursor_scale(float s);

/* the skin's score / combo font, when it has one; scale per SD pixel, y = top */
bool draw_skin_font(bool combo);
float draw_skin_text(float x, float y, float scale, bool combo, int align, const char *s, float alpha);

/* ranking letter from the skin, or a tinted text letter */
void draw_rank(int rank, float cx, float cy, float height, bool small, float alpha);

#endif
