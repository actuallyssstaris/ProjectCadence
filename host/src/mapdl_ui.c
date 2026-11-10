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

    draw_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0.05f, 0.05f, 0.07f, 0.8f));
    /* header band with the search field in it */
    draw_rect(80, 30, GFX_W - 160, 190, gfx_rgba(0.12f, 0.12f, 0.16f, 0.95f));
    draw_text(120, 50, 30, white, ALIGN_LEFT, "beatmap listing");
    ui_round_rect(120, 140, 1100, 64, 12, gfx_rgba(0, 0, 0, ui.typing ? 0.55f : 0.4f));
    if (ui.query[0] || ui.typing)
        snprintf(buf, sizeof(buf), "%s%s", ui.query, ui.typing ? "_" : "");
    else
        snprintf(buf, sizeof(buf), "newest ranked maps (TRIANGLE to search)");
    draw_text(140, 156, 32, ui.query[0] || ui.typing ? white : dim, ALIGN_LEFT, buf);

    /* download progress, top right */
    mapdl_status st;
    mapdl_progress(&st);
    if (st.current) {
        snprintf(buf, sizeof(buf), "downloading %.1f MB", st.bytes / 1048576.0);
        if (st.queued)
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ", %d waiting", st.queued);
        draw_text(GFX_W - 120, 70, 26, gfx_rgba(0.5f, 0.85f, 1, 1), ALIGN_RIGHT, buf);
    }
    if (st.msg[0])
        draw_text(GFX_W - 120, 110, 22, dim, ALIGN_RIGHT, st.msg);

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
            if (y < top - 1 || y > GFX_H - (ui.typing ? 640 : 250))
                continue;
            const mapdl_set *s = &ui.res.sets[i];
            bool on = i == ui.sel && !ui.typing;
            if (on)
                ui_round_rect(x - 4, y - 4, cw + 8, ch + 8, 14, gfx_rgba(1, 1, 1, 0.9f));
            ui_round_rect(x, y, cw, ch, 10, gfx_rgba(0.13f, 0.14f, 0.18f, 0.97f));
            /* a coloured block where the cover sits, its colour from the star rating */
            ui_round_rect(x, y, ch, ch, 10, gfx_rgba(0.22f, 0.24f, 0.32f, 1));
            ui_triangles(x, y, ch, ch, gfx_rgba(0.3f, 0.32f, 0.42f, 0.6f), 0, (unsigned)s->id, 6);

            snprintf(buf, sizeof(buf), "%.40s", s->title);
            draw_text(x + ch + 16, y + 10, 22, white, ALIGN_LEFT, buf);
            snprintf(buf, sizeof(buf), "by %.40s", s->artist);
            draw_text(x + ch + 16, y + 38, 16, dim, ALIGN_LEFT, buf);
            snprintf(buf, sizeof(buf), "mapped by %.30s", s->creator);
            draw_text(x + ch + 16, y + 60, 14, gfx_rgba(1, 1, 1, 0.5f), ALIGN_LEFT, buf);

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
        draw_text(GFX_W - 120, 170, 22, dim, ALIGN_RIGHT, buf);
    }

    if (ui.note[0])
        draw_text(120, GFX_H - 150, 24, gfx_rgba(1, 0.85f, 0.4f, 1), ALIGN_LEFT, ui.note);
    draw_text(120, GFX_H - 110, 22, dim, ALIGN_LEFT, "maps come from catboy.best, a public mirror of osu.ppy.sh");

    if (ui.typing)
        kbd_render(&ui.keys);
    else
        draw_text(120, GFX_H - 70, 26, white, ALIGN_LEFT, "CROSS download   TRIANGLE search   L1/R1 page   CIRCLE back");
}
