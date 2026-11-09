#include "skin_ui.h"
#include "../include/osu5.h"
#include "draw.h"
#include "gfx.h"
#include "log.h"
#include "sce.h"
#include "skin.h"
#include "skin_img.h"
#include "skins.h"

#include <stdio.h>
#include <string.h>

#define MAX_ROWS 1200

/* official skin first, the GitHub picks, then o!rdr's list by popularity, then
 * folders the player copied in, and the drawn look last */
enum { ROW_CATALOG, ROW_REMOTE, ROW_LOCAL, ROW_BUILTIN };

typedef struct {
    char id[96];
    int kind;
    int index; /* into skin_catalog or the o!rdr list */
} row;

static struct {
    bool first_run;
    int sel;
    float scroll;
    row rows[MAX_ROWS];
    int nrows;
    int remote_state; /* what the rows were built with */
    skin_job job;
    row job_row;
    char note[160];
} ui;

void skin_apply(const char *id)
{
    char dir[256];
    if (!strcmp(id, "builtin")) {
        skin_images_load(NULL);
        skin_load("/data/cadence/skin"); /* hitsounds dropped in by hand, if any */
    } else {
        skins_path(id, dir, sizeof(dir));
        skin_images_load(dir);
        skin_load(dir);
    }
    osu5_set_skin_colours(skin_img.combo_colours, skin_img.ncombo);
}

static bool listed(const char *id)
{
    for (int i = 0; i < ui.nrows; i++)
        if (!strcmp(ui.rows[i].id, id))
            return true;
    return false;
}

static void add(const char *id, int kind, int index)
{
    if (ui.nrows >= MAX_ROWS - 1 || listed(id))
        return;
    row *r = &ui.rows[ui.nrows++];
    snprintf(r->id, sizeof(r->id), "%s", id);
    r->kind = kind;
    r->index = index;
}

static void build_rows(void)
{
    char keep[96];
    snprintf(keep, sizeof(keep), "%s", ui.nrows ? ui.rows[ui.sel].id : skins_active());

    ui.nrows = 0;
    for (int i = 0; i < skin_catalog_count; i++)
        add(skin_catalog[i].id, ROW_CATALOG, i);

    const skin_remote *rs;
    int n = skins_remote(&rs);
    for (int i = 0; i < n; i++)
        add(rs[i].id, ROW_REMOTE, i);

    static char local[64][64];
    int nl = skins_local(local, 64);
    for (int i = 0; i < nl; i++)
        add(local[i], ROW_LOCAL, -1);

    add("builtin", ROW_BUILTIN, -1);
    ui.remote_state = skins_remote_state();

    ui.sel = 0;
    for (int i = 0; i < ui.nrows; i++)
        if (!strcmp(ui.rows[i].id, keep))
            ui.sel = i;
}

void skin_ui_open(bool first_run)
{
    ui.first_run = first_run;
    ui.note[0] = 0;
    ui.nrows = 0;
    skins_remote_load();
    build_rows();
    ui.scroll = (float)ui.sel;
}

static const char *row_name(const row *r)
{
    const skin_remote *rs;
    switch (r->kind) {
    case ROW_CATALOG: return skin_catalog[r->index].name;
    case ROW_REMOTE: return skins_remote(&rs) > r->index ? rs[r->index].name : r->id;
    case ROW_BUILTIN: return "the built-in look";
    default: return r->id;
    }
}

static bool installed(const row *r)
{
    return r->kind == ROW_LOCAL || r->kind == ROW_BUILTIN || skins_installed(r->id);
}

static void use(const row *r)
{
    skins_set_active(r->id);
    skin_apply(r->id);
    snprintf(ui.note, sizeof(ui.note), "now using %.120s", row_name(r));
}

static bool download(const row *r, bool force)
{
    const skin_remote *rs;
    bool ok = false;
    if (r->kind == ROW_CATALOG)
        ok = skins_install(r->index, &ui.job, force);
    else if (r->kind == ROW_REMOTE && skins_remote(&rs) > r->index)
        ok = skins_install_remote(&rs[r->index], &ui.job);
    if (ok)
        ui.job_row = *r;
    return ok;
}

bool skin_ui_frame(const pad_state *pad)
{
    /* the o!rdr list showed up (or failed) since the rows were built */
    if (skins_remote_state() != ui.remote_state)
        build_rows();

    if (ui.job.state == 1)
        return false; /* downloading, hold still */

    if (ui.job.state == 2) {
        ui.job.state = 0;
        use(&ui.job_row);
        if (ui.first_run)
            return true;
    } else if (ui.job.state == 3) {
        ui.job.state = 0;
        snprintf(ui.note, sizeof(ui.note), "%s", ui.job.msg);
    }

    ui.sel += pad->nav_y;
    if (pad->pressed & PAD_R1) ui.sel += 20;
    if (pad->pressed & PAD_L1) ui.sel -= 20;
    if (ui.sel < 0) ui.sel = 0;
    if (ui.sel >= ui.nrows) ui.sel = ui.nrows - 1;

    if (pad->pressed & PAD_CIRCLE) {
        if (ui.first_run && !skins_chosen())
            skins_set_active("builtin");
        return true;
    }

    const row *r = &ui.rows[ui.sel];
    if (pad->pressed & PAD_CROSS) {
        if (installed(r)) {
            use(r);
            if (ui.first_run)
                return true;
        } else if (!download(r, false)) {
            snprintf(ui.note, sizeof(ui.note), "%s", ui.job.msg[0] ? ui.job.msg : "couldn't start the download");
        }
    }

    /* fetch it again, over what's there */
    if ((pad->pressed & PAD_TRIANGLE) && (r->kind == ROW_CATALOG || r->kind == ROW_REMOTE))
        download(r, true);
    return false;
}

void skin_ui_render(void)
{
    char buf[256];
    uint32_t white = gfx_rgba(1, 1, 1, 1), dim = gfx_rgba(1, 1, 1, 0.65f);
    const skin_remote *rs;
    int nr = skins_remote(&rs);

    draw_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0, 0, 0, 0.55f));
    draw_text(120, 70, 52, white, ALIGN_LEFT, ui.first_run ? "welcome! pick a skin to start" : "skins");
    draw_text(120, 140, 24, dim, ALIGN_LEFT,
              ui.first_run ? "you can change it later from song select (press the touchpad)" : "the one you pick is used right away");
    snprintf(buf, sizeof(buf), "%d / %d", ui.sel + 1, ui.nrows);
    draw_text(1220, 140, 24, dim, ALIGN_RIGHT, buf);

    const float top = 210, h = 86;
    ui.scroll += (ui.sel - ui.scroll) * 0.25f;
    if (ui.sel - ui.scroll > 30 || ui.scroll - ui.sel > 30)
        ui.scroll = (float)ui.sel;
    float first = ui.scroll - 3;
    if (first < 0)
        first = 0;
    for (int i = (int)first; i < ui.nrows; i++) {
        float y = top + (i - first) * h;
        if (y > GFX_H - 380)
            break;
        if (y < top - 1)
            continue;
        const row *r = &ui.rows[i];
        bool on = i == ui.sel;
        draw_rect(120, y, 1100, h - 10, on ? gfx_rgba(1, 1, 1, 0.92f) : gfx_rgba(0, 0, 0, 0.5f));
        uint32_t tc = on ? gfx_rgba(0.1f, 0.1f, 0.15f, 1) : white;
        uint32_t sc = on ? gfx_rgba(0.1f, 0.1f, 0.15f, 0.7f) : dim;

        const char *state = !strcmp(r->id, skins_active()) ? "in use" : installed(r) ? "installed" : "download";

        switch (r->kind) {
        case ROW_CATALOG: {
            const skin_entry *e = &skin_catalog[r->index];
            draw_text(145, y + 12, 28, tc, ALIGN_LEFT, e->name);
            snprintf(buf, sizeof(buf), "by %s - %s", e->author, e->licence);
            draw_text(145, y + 48, 18, sc, ALIGN_LEFT, buf);
            break;
        }
        case ROW_REMOTE:
            if (r->index < nr) {
                draw_text(145, y + 12, 28, tc, ALIGN_LEFT, rs[r->index].name);
                snprintf(buf, sizeof(buf), "by %.50s - via o!rdr, used in %d renders", rs[r->index].author, rs[r->index].used);
                draw_text(145, y + 48, 18, sc, ALIGN_LEFT, buf);
            }
            break;
        case ROW_BUILTIN:
            draw_text(145, y + 12, 28, tc, ALIGN_LEFT, "built-in look");
            draw_text(145, y + 48, 18, sc, ALIGN_LEFT, "no download, plain shapes");
            break;
        default:
            draw_text(145, y + 12, 28, tc, ALIGN_LEFT, r->id);
            draw_text(145, y + 48, 18, sc, ALIGN_LEFT, "added by you");
            break;
        }
        draw_text(1195, y + 26, 24, tc, ALIGN_RIGHT, state);
    }

    if (ui.job.state == 1) {
        snprintf(buf, sizeof(buf), "%.80s: %s %.1f MB", row_name(&ui.job_row), ui.job.msg, ui.job.bytes / 1048576.0);
        if (ui.job.total > 1)
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " (%d/%d files)", ui.job.done, ui.job.total);
        draw_text(120, GFX_H - 280, 28, white, ALIGN_LEFT, buf);
    } else if (ui.note[0]) {
        draw_text(120, GFX_H - 280, 28, gfx_rgba(1, 0.85f, 0.4f, 1), ALIGN_LEFT, ui.note);
    } else if (ui.remote_state == 1) {
        draw_text(120, GFX_H - 280, 24, dim, ALIGN_LEFT, "getting the o!rdr skin list...");
    } else if (ui.remote_state == 3) {
        draw_text(120, GFX_H - 280, 24, dim, ALIGN_LEFT, "couldn't get the o!rdr skin list (offline?)");
    }

    const row *cur = &ui.rows[ui.sel];
    if (cur->kind == ROW_CATALOG && cur->index == 0)
        draw_text(120, GFX_H - 236, 20, dim, ALIGN_LEFT, "osu! classic skin by ppy, CC BY-NC 4.0, fetched from github.com/ppy/osu-resources");
    else if (cur->kind == ROW_REMOTE)
        draw_text(120, GFX_H - 236, 20, dim, ALIGN_LEFT, "skins from o!rdr (ordr.issou.best), made and shared by their authors");

    draw_text(120, GFX_H - 168, 22, dim, ALIGN_LEFT, "or unzip any .osk into /data/cadence/skins/<name>/ over FTP and it shows up here");
    draw_text(120, GFX_H - 110, 26, white, ALIGN_LEFT,
              ui.first_run ? "CROSS pick   L1/R1 page   CIRCLE skip (built-in look)"
                           : "CROSS use / download   TRIANGLE re-download   L1/R1 page   CIRCLE back");
}
