/* "get maps" screen: search the mirror, queue sets, watch them come in */
#include "mapdl_ui.h"
#include "draw.h"
#include "gfx.h"
#include "kbd.h"
#include "mapdl.h"
#include "sce.h"

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

    ui.sel += pad->nav_y + pad->nav_x * 8;
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

    draw_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0, 0, 0, 0.6f));
    draw_text(120, 60, 52, white, ALIGN_LEFT, "get maps");

    /* search box */
    draw_rect(120, 140, 1100, 64, gfx_rgba(1, 1, 1, ui.typing ? 0.25f : 0.12f));
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

    const float top = 240, h = 80;
    if (ui.res.state == 1) {
        draw_text(120, top + 20, 30, dim, ALIGN_LEFT, "searching...");
    } else if (ui.res.state == 3 || (ui.res.state == 2 && ui.res.count == 0)) {
        draw_text(120, top + 20, 30, gfx_rgba(1, 0.6f, 0.5f, 1), ALIGN_LEFT, ui.res.msg);
    } else if (ui.res.state == 2) {
        ui.scroll += (ui.sel - ui.scroll) * 0.25f;
        float first = ui.scroll - 3;
        if (first < 0) first = 0;
        for (int i = 0; i < ui.res.count; i++) {
            float y = top + (i - first) * h;
            if (y < top - 1 || y > GFX_H - (ui.typing ? 640 : 230))
                continue;
            const mapdl_set *s = &ui.res.sets[i];
            bool on = i == ui.sel && !ui.typing;
            draw_rect(120, y, 1680, h - 8, on ? gfx_rgba(1, 1, 1, 0.92f) : gfx_rgba(0, 0, 0, 0.5f));
            uint32_t tc = on ? gfx_rgba(0.1f, 0.1f, 0.15f, 1) : white;
            uint32_t sc = on ? gfx_rgba(0.1f, 0.1f, 0.15f, 0.7f) : dim;

            snprintf(buf, sizeof(buf), "%.60s - %.80s", s->artist, s->title);
            draw_text(140, y + 10, 26, tc, ALIGN_LEFT, buf);
            snprintf(buf, sizeof(buf), "mapped by %.40s   %d %s, up to %.2f stars   %s", s->creator, s->diffs,
                     s->diffs == 1 ? "difficulty" : "difficulties", s->stars, s->status);
            draw_text(140, y + 44, 18, sc, ALIGN_LEFT, buf);
            if (s->video) {
                draw_rect(1500, y + 22, 120, 32, gfx_rgba(1, 0.47f, 0.63f, 0.95f));
                draw_text(1560, y + 28, 20, gfx_rgba(0.1f, 0.05f, 0.08f, 1), ALIGN_CENTRE, "VIDEO");
            }

            int state = mapdl_state(s->id);
            const char *label = state == 2 ? "installed" : state == 1 ? (st.current == s->id ? "downloading" : "queued") : "get";
            draw_text(1780, y + 24, 24, tc, ALIGN_RIGHT, label);
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
