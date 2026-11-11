/* "get maps" screen: search the mirror, queue sets, watch them come in */
#include "mapdl_ui.h"
#include "draw.h"
#include "gfx.h"
#include "kbd.h"
#include "mapdl.h"
#include "sce.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>

static struct {
    char query[64];
    int page;
    int sel;
    float scroll;
    bool typing;
    kbd keys;
    mapdl_search res;
    char note[128];
} ui;

static void run_search(void)
{
    ui.sel = 0;
    ui.scroll = 0;
    if (!mapdl_find(ui.query, ui.page))
        snprintf(ui.note, sizeof(ui.note), "no network");
    else
        ui.note[0] = 0;
}

void mapdl_ui_open(void)
{
    ui.typing = false;
    ui.keys.text = ui.query;
    ui.keys.cap = sizeof(ui.query);
    mapdl_results(&ui.res);
    if (ui.res.state == 0) {
        ui.page = 0;
        run_search();
    }
}

bool mapdl_ui_frame(const pad_state *pad)
{
    mapdl_results(&ui.res);

    if (ui.typing) {
        int k = kbd_frame(&ui.keys, pad);
        if (k != KBD_TYPING)
            ui.typing = false;
        if (k == KBD_DONE) {
            ui.page = 0;
            run_search();
        }
        return false;
    }

    if (pad->pressed & PAD_CIRCLE)
        return true;
    if (pad->pressed & PAD_TRIANGLE) {
        ui.typing = true;
        return false;
    }
    if (pad->pressed & PAD_R1 && ui.res.state == 2 && ui.res.count == MAPDL_RESULTS) {
        ui.page++;
        run_search();
    }
    if (pad->pressed & PAD_L1 && ui.page > 0) {
        ui.page--;
        run_search();
    }

    if (ui.res.state != 2 || ui.res.count == 0)
        return false;

    ui.sel += pad->nav_x + pad->nav_y * 3; /* three cards a row */
    if (ui.sel < 0) ui.sel = 0;
    if (ui.sel >= ui.res.count) ui.sel = ui.res.count - 1;

    if (pad->pressed & PAD_CROSS) {
        const mapdl_set *s = &ui.res.sets[ui.sel];
        if (mapdl_state(s->id) == 0) {
            if (mapdl_queue(s))
                snprintf(ui.note, sizeof(ui.note), "queued %.90s", s->title);
            else
                snprintf(ui.note, sizeof(ui.note), "queue is full, give it a moment");
        }
    }
    return false;
}

void mapdl_ui_render(void)
{
    char buf[300];
    uint32_t white = gfx_rgba(1, 1, 1, 1), dim = gfx_rgba(1, 1, 1, 0.65f);

    /* lazer's overlay: a dark full-width sheet, the header strip, then the search box */
    draw_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0.09f, 0.09f, 0.11f, 0.96f));
    draw_rect(0, 0, GFX_W, 96, gfx_rgba(0.15f, 0.15f, 0.19f, 1));
    ui_arrow_down(120, 48, 30, ui_hex(UI_LIME, 1));
    draw_text(150, 30, 32, white, ALIGN_LEFT, "beatmap listing");
    draw_rect(0, 96, GFX_W, 120, gfx_rgba(0.12f, 0.12f, 0.15f, 1));
    ui_round_rect(120, 124, 1240, 64, 8, gfx_rgba(0.06f, 0.06f, 0.08f, 1));
    if (ui.typing)
        ui_round_rect(120, 184, 1240, 4, 2, ui_hex(UI_YELLOW, 1));
    if (ui.query[0] || ui.typing)
        snprintf(buf, sizeof(buf), "%s%s", ui.query, ui.typing ? "_" : "");
    else
        snprintf(buf, sizeof(buf), "type in keywords...");
    draw_text(144, 141, 28, ui.query[0] || ui.typing ? white : gfx_rgba(1, 1, 1, 0.4f), ALIGN_LEFT, buf);
    ui_search_icon(1325, 156, 34, gfx_rgba(1, 1, 1, 0.75f));

    /* download progress, top right */
    mapdl_status st;
    mapdl_progress(&st);
    if (st.current) {
        snprintf(buf, sizeof(buf), "downloading %.1f MB", st.bytes / 1048576.0);
        if (st.queued)
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ", %d waiting", st.queued);
        draw_text(GFX_W - 120, 34, 24, ui_hex(UI_BLUE, 1), ALIGN_RIGHT, buf);
    }
    if (st.msg[0]) {
        snprintf(buf, sizeof(buf), "%s", st.msg);
        ui_fit(buf, 20, 420);
        draw_text(GFX_W - 120, 124, 20, dim, ALIGN_RIGHT, buf);
    }

    const float top = 240;
    if (ui.res.state == 1) {
        draw_text(120, top + 20, 30, dim, ALIGN_LEFT, "searching...");
    } else if (ui.res.state == 3 || (ui.res.state == 2 && ui.res.count == 0)) {
        draw_text(120, top + 20, 30, gfx_rgba(1, 0.6f, 0.5f, 1), ALIGN_LEFT, ui.res.msg);
    } else if (ui.res.state == 2) {
        /* cards three to a row, scrolled by row */
        const float gx = 120, cw = 548, ch = 120, gap = 18;
        int row = ui.sel / 3;
        ui.scroll += (row - ui.scroll) * 0.25f;
        float first = ui.scroll - 1;
        if (first < 0) first = 0;
        for (int i = 0; i < ui.res.count; i++) {
            float x = gx + (i % 3) * (cw + gap), y = top + (i / 3 - first) * (ch + gap);
            if (y < top - 1 || y > GFX_H - (ui.typing ? 640 : 270))
                continue;
            const mapdl_set *s = &ui.res.sets[i];
            bool on = i == ui.sel && !ui.typing;
            ui_shadow(x, y, cw, ch, 10);
            if (on)
                ui_round_rect(x - 3, y - 3, cw + 6, ch + 6, 13, gfx_rgba(1, 1, 1, 0.85f));
            ui_round_rect(x, y, cw, ch, 10, gfx_rgba(0.17f, 0.17f, 0.21f, 1));
            /* the cover goes here once we fetch them; until then a tint from the set id */
            static const uint32_t tints[] = {UI_PURPLE, UI_PURPLE_DARK, UI_BLUE, UI_PINK, UI_ORANGE, UI_GREEN};
            ui_round_rect(x, y, ch, ch, 10, ui_mix(gfx_rgba(0.2f, 0.2f, 0.25f, 1), ui_hex(tints[s->id % 6], 1), 0.25f));
            ui_play_icon(x + ch / 2, y + ch / 2, 34, gfx_rgba(1, 1, 1, on ? 0.9f : 0.35f));

            float tx = x + ch + 16, tw = cw - ch - 30 - (s->video ? 76 : 0);
            snprintf(buf, sizeof(buf), "%s", s->title);
            ui_fit(buf, 22, tw);
            draw_text(tx, y + 10, 22, white, ALIGN_LEFT, buf);
            snprintf(buf, sizeof(buf), "by %s", s->artist);
            ui_fit(buf, 16, cw - ch - 30);
            draw_text(tx, y + 38, 16, dim, ALIGN_LEFT, buf);
            snprintf(buf, sizeof(buf), "mapped by %s", s->creator);
            ui_fit(buf, 14, cw - ch - 30);
            draw_text(tx, y + 60, 14, gfx_rgba(1, 1, 1, 0.5f), ALIGN_LEFT, buf);

            /* status pill, then the difficulty count and top rating */
            float pw = draw_text_width(13, s->status) + 18;
            ui_round_rect(x + ch + 16, y + 86, pw, 22, 11, ui_hex(UI_LIME, 1));
            draw_text(x + ch + 16 + pw / 2, y + 89, 13, gfx_rgba(0.08f, 0.08f, 0.08f, 1), ALIGN_CENTRE, s->status);
            snprintf(buf, sizeof(buf), "%d diff%s  up to %.2f*", s->diffs, s->diffs == 1 ? "" : "s", s->stars);
            draw_text(x + ch + 28 + pw, y + 89, 14, dim, ALIGN_LEFT, buf);
            if (s->video) {
                ui_round_rect(x + cw - 76, y + 10, 64, 22, 11, ui_hex(UI_PINK, 1));
                draw_text(x + cw - 44, y + 13, 13, white, ALIGN_CENTRE, "VIDEO");
            }

            int state = mapdl_state(s->id);
            const char *label = state == 2 ? "installed" : state == 1 ? (st.current == s->id ? "downloading" : "queued") : "";
            if (label[0])
                draw_text(x + cw - 14, y + 88, 15, ui_hex(state == 2 ? UI_LIME : UI_BLUE, 1), ALIGN_RIGHT, label);
        }
        snprintf(buf, sizeof(buf), "page %d", ui.page + 1);
        draw_text(GFX_W - 120, 158, 22, dim, ALIGN_RIGHT, buf);
    }

    /* footer strip, like the song select's */
    draw_rect(0, GFX_H - 90, GFX_W, 90, gfx_rgba(0.12f, 0.12f, 0.15f, 1));
    if (ui.note[0])
        draw_text(120, GFX_H - 130, 22, ui_hex(UI_YELLOW, 1), ALIGN_LEFT, ui.note);
    draw_text(GFX_W - 120, GFX_H - 56, 18, gfx_rgba(1, 1, 1, 0.4f), ALIGN_RIGHT, "maps from catboy.best, a mirror of osu.ppy.sh");

    if (ui.typing)
        kbd_render(&ui.keys);
    else
        draw_text(120, GFX_H - 58, 22, dim, ALIGN_LEFT, "CROSS download   TRIANGLE search   L1/R1 page   CIRCLE back");
}
