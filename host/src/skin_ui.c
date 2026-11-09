#include "skin_ui.h"
#include "draw.h"
#include "gfx.h"
#include "log.h"
#include "sce.h"
#include "skin.h"
#include "skin_img.h"
#include "skins.h"

#include <stdio.h>
#include <string.h>

#define MAX_ROWS 48

typedef struct {
    char id[64];
    int catalog; /* index into skin_catalog, -1 for a folder the player added */
} row;

static struct {
    bool first_run;
    int sel;
    float scroll;
    row rows[MAX_ROWS];
    int nrows;
    skin_job job;
    int job_row;
    char note[160];
} ui;

void skin_apply(const char *id)
{
    char dir[256];
    if (!strcmp(id, "builtin")) {
        skin_images_load(NULL);
        skin_load("/data/osu5/skin"); /* hitsounds dropped in by hand, if any */
        return;
    }
    skins_path(id, dir, sizeof(dir));
    skin_images_load(dir);
    skin_load(dir);
}

static void build_rows(void)
{
    ui.nrows = 0;
    for (int i = 0; i < skin_catalog_count && ui.nrows < MAX_ROWS; i++) {
        snprintf(ui.rows[ui.nrows].id, 64, "%s", skin_catalog[i].id);
        ui.rows[ui.nrows++].catalog = i;
    }

    char local[32][64];
    int n = skins_local(local, 32);
    for (int i = 0; i < n && ui.nrows < MAX_ROWS - 1; i++) {
        bool known = false;
        for (int c = 0; c < skin_catalog_count; c++)
            known |= !strcmp(local[i], skin_catalog[c].id);
        if (!known) {
            snprintf(ui.rows[ui.nrows].id, 64, "%s", local[i]);
            ui.rows[ui.nrows++].catalog = -1;
        }
    }

    snprintf(ui.rows[ui.nrows].id, 64, "builtin");
    ui.rows[ui.nrows++].catalog = -1;
}

void skin_ui_open(bool first_run)
{
    ui.first_run = first_run;
    ui.note[0] = 0;
    build_rows();
    ui.sel = 0;
    for (int i = 0; i < ui.nrows; i++) {
        if (!strcmp(ui.rows[i].id, skins_active()))
            ui.sel = i;
    }
    ui.scroll = (float)ui.sel;
}

static void use(const row *r)
{
    skins_set_active(r->id);
    skin_apply(r->id);
    snprintf(ui.note, sizeof(ui.note), "now using %s",
             r->catalog >= 0 ? skin_catalog[r->catalog].name : !strcmp(r->id, "builtin") ? "the built-in look" : r->id);
}

bool skin_ui_frame(const pad_state *pad)
{
    if (ui.job.state == 1)
        return false; /* downloading, hold still */

    if (ui.job.state == 2) {
        ui.job.state = 0;
        use(&ui.rows[ui.job_row]);
        if (ui.first_run)
            return true;
    } else if (ui.job.state == 3) {
        ui.job.state = 0;
        snprintf(ui.note, sizeof(ui.note), "%s", ui.job.msg);
    }

    if (pad->pressed & PAD_DOWN) ui.sel++;
    if (pad->pressed & PAD_UP) ui.sel--;
    if (ui.sel < 0) ui.sel = 0;
    if (ui.sel >= ui.nrows) ui.sel = ui.nrows - 1;

    if (pad->pressed & PAD_CIRCLE) {
        if (ui.first_run && !skins_chosen())
            skins_set_active("builtin");
        return true;
    }

    if (pad->pressed & PAD_CROSS) {
        const row *r = &ui.rows[ui.sel];
        bool have = r->catalog < 0 || skins_installed(r->id);
        if (have) {
            use(r);
            if (ui.first_run)
                return true;
        } else if (skins_install(r->catalog, &ui.job)) {
            ui.job_row = ui.sel;
        } else {
            snprintf(ui.note, sizeof(ui.note), "%s", ui.job.msg[0] ? ui.job.msg : "couldn't start the download");
        }
    }

    /* re-pull a catalog skin that's already there */
    if ((pad->pressed & PAD_TRIANGLE) && ui.rows[ui.sel].catalog >= 0 && skins_install(ui.rows[ui.sel].catalog, &ui.job))
        ui.job_row = ui.sel;
    return false;
}

void skin_ui_render(void)
{
    char buf[256];
    uint32_t white = gfx_rgba(1, 1, 1, 1), dim = gfx_rgba(1, 1, 1, 0.65f);

    draw_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0, 0, 0, 0.55f));
    draw_text(120, 70, 52, white, ALIGN_LEFT, ui.first_run ? "welcome! pick a skin to start" : "skins");
    draw_text(120, 140, 24, dim, ALIGN_LEFT,
              ui.first_run ? "you can change it later from song select (press the touchpad)" : "the one you pick is used right away");

    const float top = 210, h = 86;
    ui.scroll += (ui.sel - ui.scroll) * 0.25f;
    float first = ui.scroll - 3;
    for (int i = 0; i < ui.nrows; i++) {
        float y = top + (i - (first < 0 ? 0 : first)) * h;
        if (y < top - 1 || y > GFX_H - 380)
            continue;
        const row *r = &ui.rows[i];
        bool on = i == ui.sel;
        draw_rect(120, y, 1100, h - 10, on ? gfx_rgba(1, 1, 1, 0.92f) : gfx_rgba(0, 0, 0, 0.5f));
        uint32_t tc = on ? gfx_rgba(0.1f, 0.1f, 0.15f, 1) : white;
        uint32_t sc = on ? gfx_rgba(0.1f, 0.1f, 0.15f, 0.7f) : dim;

        const char *state;
        if (!strcmp(r->id, skins_active()))
            state = "in use";
        else if (r->catalog < 0 || skins_installed(r->id))
            state = "installed";
        else
            state = "download";

        if (r->catalog >= 0) {
            const skin_entry *e = &skin_catalog[r->catalog];
            draw_text(145, y + 12, 28, tc, ALIGN_LEFT, e->name);
            snprintf(buf, sizeof(buf), "by %s - %s", e->author, e->licence);
            draw_text(145, y + 48, 18, sc, ALIGN_LEFT, buf);
        } else if (!strcmp(r->id, "builtin")) {
            draw_text(145, y + 12, 28, tc, ALIGN_LEFT, "built-in look");
            draw_text(145, y + 48, 18, sc, ALIGN_LEFT, "no download, plain shapes");
        } else {
            draw_text(145, y + 12, 28, tc, ALIGN_LEFT, r->id);
            draw_text(145, y + 48, 18, sc, ALIGN_LEFT, "added by you");
        }
        draw_text(1195, y + 26, 24, tc, ALIGN_RIGHT, state);
    }

    if (ui.job.state == 1) {
        const skin_entry *e = &skin_catalog[ui.rows[ui.job_row].catalog];
        snprintf(buf, sizeof(buf), "%s: %s %.1f MB", e->name, ui.job.msg, ui.job.bytes / 1048576.0);
        if (ui.job.total > 1)
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " (%d/%d files)", ui.job.done, ui.job.total);
        draw_text(120, GFX_H - 280, 28, white, ALIGN_LEFT, buf);
    } else if (ui.note[0]) {
        draw_text(120, GFX_H - 280, 28, gfx_rgba(1, 0.85f, 0.4f, 1), ALIGN_LEFT, ui.note);
    }

    if (ui.rows[ui.sel].catalog == 0)
        draw_text(120, GFX_H - 236, 20, dim, ALIGN_LEFT, "osu! classic skin by ppy, CC BY-NC 4.0, fetched from github.com/ppy/osu-resources");

    draw_text(120, GFX_H - 200, 22, dim, ALIGN_LEFT, "more skins: osuskins.net, skins.osuck.net, the osu! forums");
    draw_text(120, GFX_H - 168, 22, dim, ALIGN_LEFT, "unzip the .osk into /data/osu5/skins/<name>/ over FTP and it shows up here");
    draw_text(120, GFX_H - 110, 26, white, ALIGN_LEFT,
              ui.first_run ? "CROSS pick   CIRCLE skip (built-in look)" : "CROSS use / download   TRIANGLE re-download   CIRCLE back");
}
