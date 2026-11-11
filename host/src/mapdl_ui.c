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

/* lazer's status colours, text dark on top */
static uint32_t status_colour(const char *s)
{
    if (!strcmp(s, "ranked") || !strcmp(s, "approved"))
        return 0xB3FF66;
    if (!strcmp(s, "loved"))
        return 0xFF66AA;
    if (!strcmp(s, "qualified"))
        return 0x66CCFF;
    if (!strcmp(s, "graveyard"))
        return 0x111111;
    return 0xFFD966; /* pending, wip */
}

void mapdl_ui_render(void)
{
    char buf[300];
    uint32_t white = gfx_rgba(1, 1, 1, 1), dim = gfx_rgba(1, 1, 1, 0.65f);

    /* lazer's listing overlay, colours sampled off it: a centred blue-grey sheet
     * with the header, the search block, a sort strip, then the cards */
    const float px = 144, pw = GFX_W - 2 * px;
    draw_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0, 0, 0, 0.6f));
    draw_rect(px, 0, pw, GFX_H, ui_hex(0x22272A, 1));
    draw_rect(px, 0, pw, 96, ui_hex(0x20292E, 1));
    /* the listing glyph: a note in a box */
    float ix = px + 96;
    ui_round_rect(ix - 16, 30, 32, 36, 5, gfx_rgba(1, 1, 1, 0.95f));
    ui_round_rect(ix - 13, 33, 26, 30, 3, ui_hex(0x20292E, 1));
    ui_disc(ix - 3, 54, 5, gfx_rgba(1, 1, 1, 0.95f));
    draw_rect(ix + 1, 38, 3, 16, gfx_rgba(1, 1, 1, 0.95f));
    draw_text(ix + 40, 30, 30, white, ALIGN_LEFT, "beatmap listing");

    draw_rect(px, 96, pw, 130, ui_hex(0x141B23, 1));
    ui_round_rect(px + 70, 120, pw - 140, 54, 8, ui_hex(0x22272A, 1));
    if (ui.typing)
        ui_round_rect(px + 70, 170, pw - 140, 4, 2, ui_hex(UI_YELLOW, 1));
    if (ui.query[0] || ui.typing)
        snprintf(buf, sizeof(buf), "%s%s", ui.query, ui.typing ? "_" : "");
    else
        snprintf(buf, sizeof(buf), "type in keywords...");
    draw_text(px + 90, 133, 26, ui.query[0] || ui.typing ? white : gfx_rgba(1, 1, 1, 0.45f), ALIGN_LEFT, buf);
    ui_search_icon(px + pw - 100, 147, 32, white);
    draw_text(px + 70, 190, 18, gfx_rgba(1, 1, 1, 0.5f), ALIGN_LEFT, "mode");
    draw_text(px + 190, 190, 18, white, ALIGN_LEFT, "any");
    draw_text(px + 260, 190, 18, gfx_rgba(1, 1, 1, 0.5f), ALIGN_LEFT, "category");
    draw_text(px + 380, 190, 18, white, ALIGN_LEFT, ui.query[0] ? "any" : "ranked");

    /* sort strip */
    draw_rect(px, 226, pw, 54, ui_hex(0x2D3538, 1));
    draw_text(px + 30, 242, 18, dim, ALIGN_LEFT, "sort by");
    draw_text(px + 120, 242, 18, white, ALIGN_LEFT, ui.query[0] ? "relevance" : "ranked");
    snprintf(buf, sizeof(buf), "page %d", ui.page + 1);
    draw_text(px + pw - 30, 242, 18, dim, ALIGN_RIGHT, buf);

    /* download progress, in the header */
    mapdl_status st;
    mapdl_progress(&st);
    if (st.current) {
        snprintf(buf, sizeof(buf), "downloading %.1f MB", st.bytes / 1048576.0);
        if (st.queued)
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ", %d waiting", st.queued);
        draw_text(px + pw - 40, 26, 20, ui_hex(UI_BLUE, 1), ALIGN_RIGHT, buf);
    }
    if (st.msg[0]) {
        snprintf(buf, sizeof(buf), "%s", st.msg);
        ui_fit(buf, 16, 520);
        draw_text(px + pw - 40, 54, 16, dim, ALIGN_RIGHT, buf);
    }

    const float top = 300;
    if (ui.res.state == 1) {
        draw_text(GFX_W / 2, top + 120, 28, dim, ALIGN_CENTRE, "searching...");
    } else if (ui.res.state == 3 || (ui.res.state == 2 && ui.res.count == 0)) {
        draw_text(GFX_W / 2, top + 120, 28, gfx_rgba(1, 0.6f, 0.5f, 1), ALIGN_CENTRE, ui.res.msg);
    } else if (ui.res.state == 2) {
        /* three to a row, 485 x 112 like lazer's normal cards at 1080p */
        const float gap = 15, cw = 485, ch = 112, gx = (GFX_W - 3 * cw - 2 * gap) / 2;
        int row = ui.sel / 3;
        ui.scroll += (row - ui.scroll) * 0.25f;
        float first = ui.scroll - 1;
        if (first < 0) first = 0;
        for (int i = 0; i < ui.res.count; i++) {
            float x = gx + (i % 3) * (cw + gap), y = top + (i / 3 - first) * (ch + gap);
            if (y < top - 1 || y + ch > GFX_H - (ui.typing ? 560 : 80))
                continue;
            const mapdl_set *s = &ui.res.sets[i];
            bool on = i == ui.sel && !ui.typing;
            if (on)
                ui_round_rect(x - 3, y - 3, cw + 6, ch + 6, 13, gfx_rgba(1, 1, 1, 0.9f));
            ui_round_rect(x, y, cw, ch, 10, ui_hex(0x38424E, 1));
            /* the cover goes here once we fetch them; until then a tint from the set id */
            static const uint32_t tints[] = {UI_PURPLE, UI_PURPLE_DARK, UI_BLUE, UI_PINK, UI_ORANGE, UI_GREEN};
            ui_round_rect(x, y, 102, ch, 10, ui_mix(ui_hex(0x2A3238, 1), ui_hex(tints[s->id % 6], 1), 0.3f));
            ui_play_icon(x + 51, y + ch / 2, 30, gfx_rgba(1, 1, 1, on ? 0.9f : 0.3f));

            float tx = x + 116, tw = cw - 130 - (s->video ? 70 : 0);
            snprintf(buf, sizeof(buf), "%s", s->title);
            ui_fit(buf, 21, tw);
            draw_text(tx, y + 9, 21, white, ALIGN_LEFT, buf);
            snprintf(buf, sizeof(buf), "by %s", s->artist);
            ui_fit(buf, 16, cw - 130);
            draw_text(tx, y + 36, 16, white, ALIGN_LEFT, buf);
            float mw = draw_text(tx, y + 58, 13, dim, ALIGN_LEFT, "mapped by ");
            snprintf(buf, sizeof(buf), "%s", s->creator);
            ui_fit(buf, 13, cw - 130 - mw);
            draw_text(tx + mw, y + 58, 13, ui_hex(0x8FC3E8, 1), ALIGN_LEFT, buf);

            /* status pill, then a dot per difficulty in the top rating's colour */
            char up[24];
            size_t k = 0;
            for (; s->status[k] && k < sizeof(up) - 1; k++)
                up[k] = (char)(s->status[k] >= 'a' && s->status[k] <= 'z' ? s->status[k] - 32 : s->status[k]);
            up[k] = 0;
            uint32_t sc = status_colour(s->status);
            float pwid = ui_pill(tx, y + 82, 12, ui_hex(sc, 1), sc == 0x111111 ? dim : ui_hex(0x222222, 1), up);
            float dx = tx + pwid + 10;
            int dots = s->diffs < 12 ? s->diffs : 12;
            for (int d = 0; d < dots; d++)
                ui_round_rect(dx + d * 9, y + 86, 6, 13, 3, ui_hex(0xB3FF66, 1));
            snprintf(buf, sizeof(buf), "%.2f*", s->stars);
            draw_text(dx + dots * 9 + 6, y + 84, 14, dim, ALIGN_LEFT, buf);
            if (s->video)
                ui_pill(x + cw - 72, y + 10, 11, ui_hex(0x2D3538, 1), dim, "VIDEO");

            int state = mapdl_state(s->id);
            const char *label = state == 2 ? "installed" : state == 1 ? (st.current == s->id ? "downloading" : "queued") : "";
            if (label[0])
                draw_text(x + cw - 14, y + 84, 14, ui_hex(state == 2 ? 0xB3FF66 : UI_BLUE, 1), ALIGN_RIGHT, label);
        }
    }

    if (ui.note[0])
        draw_text(px + 70, GFX_H - 110, 20, ui_hex(UI_YELLOW, 1), ALIGN_LEFT, ui.note);
    ui_footer(ui.typing ? NULL : "CROSS download   TRIANGLE search   L1/R1 page", "CIRCLE");
    draw_text(GFX_W - 40, GFX_H - 50, 16, gfx_rgba(1, 1, 1, 0.4f), ALIGN_RIGHT, "maps from catboy.best, a mirror of osu.ppy.sh");
    if (ui.typing)
        kbd_render(&ui.keys);
}
