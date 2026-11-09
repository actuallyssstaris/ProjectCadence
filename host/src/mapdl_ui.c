/* "get maps" screen: search the mirror, queue sets, watch them come in */
#include "mapdl_ui.h"
#include "draw.h"
#include "gfx.h"
#include "mapdl.h"
#include "sce.h"

#include <stdio.h>
#include <string.h>

static const char *keys[4] = {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm-.!"};
enum { KEY_SPACE, KEY_DEL, KEY_GO }; /* the wide bottom row */

static struct {
    char query[64];
    int page;
    int sel;
    float scroll;
    bool typing;
    int kx, ky; /* keyboard cursor, ky 4 = bottom row */
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
    mapdl_results(&ui.res);
    if (ui.res.state == 0) {
        ui.page = 0;
        run_search();
    }
}

static int wide_key(void) { return ui.kx * 3 / 10; }

static void type_key(void)
{
    size_t n = strlen(ui.query);
    if (ui.ky == 4) {
        if (wide_key() == KEY_DEL) {
            if (n) ui.query[n - 1] = 0;
        } else if (wide_key() == KEY_GO) {
            ui.typing = false;
            ui.page = 0;
            run_search();
        } else if (n + 1 < sizeof(ui.query)) {
            ui.query[n] = ' ';
            ui.query[n + 1] = 0;
        }
        return;
    }
    if (n + 1 < sizeof(ui.query)) {
        ui.query[n] = keys[ui.ky][ui.kx];
        ui.query[n + 1] = 0;
    }
}

static void keyboard_frame(const pad_state *pad)
{
    /* kx stays a column of the 10 wide rows; on the bottom row it picks one of
     * the three wide keys, so going up and down keeps roughly the same spot */
    ui.ky = (ui.ky + pad->nav_y + 5) % 5;
    if (pad->nav_x) {
        if (ui.ky == 4) {
            static const int centre[3] = {1, 5, 8};
            ui.kx = centre[(wide_key() + pad->nav_x + 3) % 3];
        } else {
            ui.kx = (ui.kx + pad->nav_x + 10) % 10;
        }
    }

    size_t n = strlen(ui.query);
    if (pad->pressed & PAD_CROSS)
        type_key();
    if (pad->pressed & PAD_SQUARE && n)
        ui.query[n - 1] = 0;
    if (pad->pressed & PAD_TRIANGLE && n + 1 < sizeof(ui.query)) {
        ui.query[n] = ' ';
        ui.query[n + 1] = 0;
    }
    if (pad->pressed & (PAD_OPTIONS | PAD_R2)) {
        ui.typing = false;
        ui.page = 0;
        run_search();
    }
    if (pad->pressed & PAD_CIRCLE)
        ui.typing = false;
}

bool mapdl_ui_frame(const pad_state *pad)
{
    mapdl_results(&ui.res);

    if (ui.typing) {
        keyboard_frame(pad);
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

static void render_keyboard(void)
{
    const float kw = 92, kh = 78, x0 = (GFX_W - kw * 10) / 2, y0 = GFX_H - 5 * kh - 120;
    draw_rect(x0 - 20, y0 - 20, kw * 10 + 40, kh * 5 + 40, gfx_rgba(0, 0, 0, 0.85f));
    char c[2] = {0, 0};
    for (int r = 0; r < 4; r++) {
        for (int k = 0; k < 10; k++) {
            bool on = !(ui.ky != r || ui.kx != k);
            float x = x0 + k * kw, y = y0 + r * kh;
            draw_rect(x + 4, y + 4, kw - 8, kh - 8, on ? gfx_rgba(1, 1, 1, 0.95f) : gfx_rgba(1, 1, 1, 0.12f));
            c[0] = keys[r][k];
            draw_text(x + kw / 2, y + kh / 2 - 18, 36, on ? gfx_rgba(0.1f, 0.1f, 0.15f, 1) : gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, c);
        }
    }
    static const char *wide[3] = {"space", "delete", "search"};
    for (int k = 0; k < 3; k++) {
        bool on = ui.ky == 4 && wide_key() == k;
        float w = kw * 10 / 3, x = x0 + k * w, y = y0 + 4 * kh;
        draw_rect(x + 4, y + 4, w - 8, kh - 8, on ? gfx_rgba(1, 1, 1, 0.95f) : gfx_rgba(1, 1, 1, 0.12f));
        draw_text(x + w / 2, y + kh / 2 - 15, 30, on ? gfx_rgba(0.1f, 0.1f, 0.15f, 1) : gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, wide[k]);
    }
    draw_text(GFX_W / 2, GFX_H - 90, 24, gfx_rgba(1, 1, 1, 0.75f), ALIGN_CENTRE,
              "CROSS type   SQUARE delete   TRIANGLE space   OPTIONS search   CIRCLE close");
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
            snprintf(buf, sizeof(buf), "mapped by %.40s   %d %s, up to %.2f stars", s->creator, s->diffs,
                     s->diffs == 1 ? "difficulty" : "difficulties", s->stars);
            draw_text(140, y + 44, 18, sc, ALIGN_LEFT, buf);

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
        render_keyboard();
    else
        draw_text(120, GFX_H - 70, 26, white, ALIGN_LEFT, "CROSS download   TRIANGLE search   L1/R1 page   CIRCLE back");
}
