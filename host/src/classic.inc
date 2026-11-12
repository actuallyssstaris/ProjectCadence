/* The simplified look: main's screens as they were before the lazer restyle,
 * picked with the interface setting. Input is shared with the new screens except
 * where the layouts need their own (menu, mod grid). Included from main.c. */

enum { CMENU_PLAY, CMENU_MULTI, CMENU_MAPS, CMENU_SKINS, CMENU_SETTINGS, CMENU_COUNT };
static int cmenu_sel;

static void render_select_classic(void)
{
    draw_background(0.75f);
    char buf[300];

    /* search and sort, top right */
    if (app.filter[0] || app.searching)
        snprintf(buf, sizeof(buf), "search: %s%s   sort: %s", app.filter, app.searching ? "_" : "", sort_label());
    else
        snprintf(buf, sizeof(buf), "TRIANGLE search   sort: %s", sort_label());
    draw_rect(0, 0, GFX_W, 52, gfx_rgba(0, 0, 0, 0.6f));
    draw_text(GFX_W - 30, 12, 26, gfx_rgba(1, 1, 1, 0.9f), ALIGN_RIGHT, buf);
    draw_text(40, 12, 28, gfx_rgba(0.5f, 0.9f, 0.95f, 1), ALIGN_LEFT, "ProjectCadence");

    if (app.songs.count == 0 && app.all.count > 0) {
        snprintf(buf, sizeof(buf), "nothing matches \"%s\"", app.filter);
        draw_text(GFX_W / 2, 400, 40, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, buf);
        if (app.searching)
            kbd_render(&app.search);
        return;
    }

    if (app.songs.count == 0) {
        draw_text(GFX_W / 2, 400, 48, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, "no beatmaps yet");
        draw_text(GFX_W / 2, 480, 28, gfx_rgba(1, 1, 1, 0.8f), ALIGN_CENTRE, "press OPTIONS, then get more maps");
        draw_text(GFX_W / 2, 525, 28, gfx_rgba(1, 1, 1, 0.8f), ALIGN_CENTRE, "or copy extracted .osz folders to " SONGS_DIR);
        draw_text(GFX_W / 2, 570, 28, gfx_rgba(1, 1, 1, 0.8f), ALIGN_CENTRE, "and press CROSS to rescan");
        draw_text(40, GFX_H - 60, 22, gfx_rgba(1, 1, 1, 0.7f), ALIGN_LEFT,
                  "OPTIONS get maps, skins and settings");
        return;
    }

    /* One panel per set like osu! does; the selected set opens into its
     * difficulties under a header. Scrolled so the selection sits mid screen. */
    enum { ROW_SET, ROW_HEADER, ROW_DIFF };
    static struct { int kind, index; } *rows;
    static int rows_cap;
    if (rows_cap < app.songs.count + 1) {
        void *grown = realloc(rows, sizeof(*rows) * (size_t)(app.songs.count + 1));
        if (!grown)
            return;
        rows = grown;
        rows_cap = app.songs.count + 1;
    }
    int nrows = 0, sel_row = 0;
    int open = set_start(app.sel);
    for (int s = 0; s < app.songs.count; s = set_end(s)) {
        if (s != open) {
            rows[nrows].kind = ROW_SET;
            rows[nrows++].index = s;
            continue;
        }
        rows[nrows].kind = ROW_HEADER;
        rows[nrows++].index = s;
        for (int i = s; i < set_end(s); i++) {
            if (i == app.sel)
                sel_row = nrows;
            rows[nrows].kind = ROW_DIFF;
            rows[nrows++].index = i;
        }
    }

    const float row = 74;
    app.sel_scroll += (sel_row - app.sel_scroll) * 0.25f;
    if (fabsf(sel_row - app.sel_scroll) > 30)
        app.sel_scroll = (float)sel_row; /* big jumps: don't fly past hundreds of rows */
    int first = (int)app.sel_scroll - 8;
    for (int r = first < 0 ? 0 : first; r < nrows && r < app.sel_scroll + 9; r++) {
        float y = GFX_H / 2 - row / 2 + (r - app.sel_scroll) * row;
        const song_entry *e = &app.songs.items[rows[r].index];
        const osu5_peek_info *in = &e->info;

        if (rows[r].kind == ROW_DIFF) {
            bool on = r == sel_row;
            float x = on ? 940 : 990;
            float st = stars_get(e->file, 0);
            draw_rect(x, y, GFX_W - x, row - 6, on ? gfx_rgba(1, 1, 1, 0.92f) : gfx_rgba(0.08f, 0.1f, 0.18f, 0.8f));
            /* the rating's colour down the left edge */
            draw_rect(x, y, 8, row - 6, st >= 0 ? star_colour(st, 1) : gfx_rgba(0.5f, 0.5f, 0.5f, 1));
            uint32_t tc = on ? gfx_rgba(0.1f, 0.1f, 0.15f, 1) : gfx_rgba(1, 1, 1, 0.95f);
            float tx = x + 28;
            if (st >= 0)
                tx += star_pill(tx, y + 10, 30, st) + 14;
            if (in->mode != 0) {
                /* which game this difficulty is */
                char tag[16];
                if (in->mode == 3)
                    snprintf(tag, sizeof(tag), "%dK", (int)lroundf(in->cs));
                else
                    snprintf(tag, sizeof(tag), "%s", in->mode == 1 ? "TAIKO" : "CATCH");
                float tw = draw_text_width(16, tag) + 20;
                uint32_t tagc = in->mode == 1 ? gfx_rgba(0.92f, 0.3f, 0.3f, 1) : in->mode == 2 ? gfx_rgba(0.3f, 0.75f, 0.45f, 1)
                                                                               : gfx_rgba(0.6f, 0.45f, 1, 1);
                draw_rect(tx, y + 12, tw, 26, tagc);
                draw_text(tx + tw / 2, y + 17, 16, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, tag);
                tx += tw + 12;
            }
            snprintf(buf, sizeof(buf), "%.80s", in->version);
            draw_text(tx, y + 14, 24, tc, ALIGN_LEFT, buf);
            if (in->mode == 3)
                snprintf(buf, sizeof(buf), "%d keys  OD %.1f  HP %.1f", (int)lroundf(in->cs), in->od, in->hp);
            else if (in->mode == 1)
                snprintf(buf, sizeof(buf), "OD %.1f  HP %.1f", in->od, in->hp);
            else
                snprintf(buf, sizeof(buf), "CS %.1f  AR %.1f  OD %.1f  HP %.1f", in->cs, in->ar, in->od, in->hp);
            draw_text(x + 28, y + 46, 16, tc, ALIGN_LEFT, buf);
            continue;
        }

        /* a set: closed panel, or the header above the open one's difficulties */
        bool header = rows[r].kind == ROW_HEADER;
        float x = header ? 900 : 960;
        int n = set_end(rows[r].index) - rows[r].index;
        float th = row - 6, tw = th * 16 / 9;
        draw_rect(x, y, GFX_W - x, th, header ? gfx_rgba(1, 0.8f, 0.35f, 0.92f) : gfx_rgba(0, 0, 0, 0.6f));
        thumb_box(set_thumb(e), x, y, tw, th, header ? 1 : 0.75f);
        x += tw;
        uint32_t tc = header ? gfx_rgba(0.15f, 0.1f, 0.02f, 1) : gfx_rgba(1, 1, 1, 0.95f);
        snprintf(buf, sizeof(buf), "%.90s", in->title);
        draw_text(x + 20, y + 8, 26, tc, ALIGN_LEFT, buf);
        if (e->video) {
            draw_rect(GFX_W - 120, y + 10, 96, 26, gfx_rgba(1, 0.47f, 0.63f, 0.95f));
            draw_text(GFX_W - 72, y + 15, 16, gfx_rgba(0.1f, 0.05f, 0.08f, 1), ALIGN_CENTRE, "VIDEO");
        }
        /* star range of the set, once every difficulty in it is rated */
        float lo = 1e9f, hi = -1;
        for (int i = rows[r].index; i < rows[r].index + n; i++) {
            float st = stars_get(app.songs.items[i].file, 0);
            if (st < 0) {
                hi = -1;
                break;
            }
            lo = fminf(lo, st);
            hi = fmaxf(hi, st);
        }
        char range[32] = "";
        if (hi >= 0)
            snprintf(range, sizeof(range), n > 1 ? "  //  %.1f-%.1f*" : "  //  %.2f*", lo, hi);
        snprintf(buf, sizeof(buf), "%.60s  //  %.40s  //  %d %s%s", in->artist, in->creator, n, n == 1 ? "difficulty" : "difficulties", range);
        draw_text(x + 20, y + 42, 18, tc, ALIGN_LEFT, buf);
    }

    const osu5_peek_info *in = &app.songs.items[app.sel].info;
    draw_rect(40, 60, 800, 260, gfx_rgba(0, 0, 0, 0.6f));
    draw_text(60, 80, 34, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, in->title);
    draw_text(60, 126, 26, gfx_rgba(1, 1, 1, 0.85f), ALIGN_LEFT, in->artist);
    snprintf(buf, sizeof(buf), "[%.90s] mapped by %.60s", in->version, in->creator);
    draw_text(60, 166, 22, gfx_rgba(1, 1, 1, 0.75f), ALIGN_LEFT, buf);
    const char *selfile = app.songs.items[app.sel].file;
    float st = stars_get(selfile, 0), mst = stars_mods(app.mods) ? stars_get(selfile, stars_mods(app.mods)) : st;
    /* rating (with mods when they change it), then the four numbers as bars */
    float bx = 60;
    if (mst >= 0)
        bx += star_pill(60, 204, 38, mst) + 20;
    else if (st >= 0)
        bx += star_pill(60, 204, 38, st) + 20;
    float bw = (820 - bx - 3 * 16) / 4;
    stat_bar(bx, 204, bw, "CS", in->cs, 10);
    stat_bar(bx + (bw + 16), 204, bw, "AR", in->ar, 10);
    stat_bar(bx + (bw + 16) * 2, 204, bw, "OD", in->od, 10);
    stat_bar(bx + (bw + 16) * 3, 204, bw, "HP", in->hp, 10);

    char tags[64];
    mod_tags(app.mods, tags, sizeof(tags));
    snprintf(buf, sizeof(buf), "%s%soffset %+dms", tags, tags[0] ? "   " : "", app.offset_ms);
    draw_text(60, 262, 24, gfx_rgba(1, 0.85f, 0.4f, 1), ALIGN_LEFT, buf);

    draw_rect(40, 340, 800, 70 + SHOWN_SCORES * 64, gfx_rgba(0, 0, 0, 0.5f));
    if (app.board && (app.board_src == 0 || !online_enabled())) {
        render_global(&app.songs.items[app.sel]);
        goto scores_done;
    }
    if (app.board) {
        render_online(&app.songs.items[app.sel]);
        goto scores_done;
    }

    /* local scores for this difficulty */
    draw_text(60, 356, 24, gfx_rgba(1, 1, 1, 0.8f), ALIGN_LEFT, app.nscores ? "your best" : "no scores yet");
    if (app.nscores && replay_exists(app.scores[0].when))
        draw_text(820, 356, 20, gfx_rgba(1, 1, 1, 0.6f), ALIGN_RIGHT, "R2 watch replay");
    for (int i = 0; i < app.nscores; i++) {
        const score_rec *r = &app.scores[i];
        float y = 400 + i * 64;
        draw_rank(r->rank, 95, y + 26, 44, true, 1);
        snprintf(buf, sizeof(buf), "%lld", (long long)r->score);
        draw_text(150, y + 6, 28, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, buf);
        if (r->pp > 0) {
            snprintf(buf, sizeof(buf), "%.0fpp", r->pp);
            draw_text(820, y + 10, 26, gfx_rgba(1, 0.85f, 0.4f, 1), ALIGN_RIGHT, buf);
        }
        mod_tags(r->mods, tags, sizeof(tags));
        snprintf(buf, sizeof(buf), "%.2f%%  %dx  %s", r->accuracy * 100, r->max_combo, tags);
        draw_text(150, y + 38, 18, gfx_rgba(1, 1, 1, 0.7f), ALIGN_LEFT, buf);
    }
    draw_text(820, 356 + (app.nscores && replay_exists(app.scores[0].when) ? 24 : 0), 18,
              gfx_rgba(1, 1, 1, 0.5f), ALIGN_RIGHT, app.board_src && online_enabled() ? "L2 osu.ppy.sh" : "L2 global");
scores_done:;

    mapdl_status dl;
    mapdl_progress(&dl);
    if (dl.current) {
        snprintf(buf, sizeof(buf), "downloading a map, %.1f MB%s", dl.bytes / 1048576.0, dl.queued ? ", more waiting" : "");
        draw_text(40, GFX_H - 100, 22, gfx_rgba(0.5f, 0.85f, 1, 1), ALIGN_LEFT, buf);
    }


    if (app.status[0])
        draw_text(GFX_W / 2, GFX_H - 150, 26, gfx_rgba(1, 0.5f, 0.5f, 1), ALIGN_CENTRE, app.status);
    if (app.searching)
        kbd_render(&app.search);
}

static void render_mods_classic(void)
{
    char buf[128];
    draw_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0, 0, 0, 0.7f));
    draw_text(GFX_W / 2, 110, 52, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, "mods");

    /* one colour per row, so the kinds read at a glance */
    static const float row_col[4][3] = {{0.5f, 0.9f, 0.5f}, {1, 0.45f, 0.45f}, {1, 0.75f, 0.3f}, {0.5f, 0.75f, 1}};
    const float w = 400, h = 150, x0 = (GFX_W - MOD_COLS * w) / 2, y0 = 190;
    for (int i = 0; i < MOD_COUNT; i++) {
        float x = x0 + (i % MOD_COLS) * w, y = y0 + (i / MOD_COLS) * h;
        const float *rc = row_col[(i / MOD_COLS) % 4];
        bool on = app.mods & mod_list[i].bit, cur = i == app.mod_sel;
        draw_rect(x + 10, y + 10, w - 20, h - 20,
                  on ? gfx_rgba(rc[0], rc[1], rc[2], cur ? 1 : 0.85f) : gfx_rgba(1, 1, 1, cur ? 0.3f : 0.08f));
        if (cur)
            draw_rect(x + 10, y + h - 16, w - 20, 6, gfx_rgba(1, 1, 1, 0.9f));
        uint32_t tc = on ? gfx_rgba(0.08f, 0.06f, 0.06f, 1) : gfx_rgba(1, 1, 1, 1);
        draw_text(x + 30, y + 30, 38, on ? tc : gfx_rgba(rc[0], rc[1], rc[2], 1), ALIGN_LEFT, mod_list[i].tag);
        draw_text(x + 110, y + 38, 26, tc, ALIGN_LEFT, mod_list[i].name);
        draw_text(x + 30, y + 92, 16, tc, ALIGN_LEFT, mod_list[i].what);
    }

    int rows = (MOD_COUNT + MOD_COLS - 1) / MOD_COLS;
    snprintf(buf, sizeof(buf), "score x%.2f", mod_multiplier(app.mods));
    draw_text(GFX_W / 2, y0 + rows * h + 30, 34, gfx_rgba(1, 0.85f, 0.4f, 1), ALIGN_CENTRE, buf);
}

static void render_settings_classic(void)
{
    char val[64];
    draw_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0, 0, 0, 0.7f));
    draw_text(160, 90, 52, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, "settings");

    for (int i = 0; i < SET_COUNT; i++) {
        const char *name = "";
        val[0] = 0;
        switch (i) {
        case SET_MUSIC: name = "music volume"; snprintf(val, sizeof(val), "%d%%", (int)lroundf(app.music_vol * 100)); break;
        case SET_EFFECTS: name = "effects volume"; snprintf(val, sizeof(val), "%d%%", (int)lroundf(app.fx_vol * 100)); break;
        case SET_OFFSET: name = "audio offset"; snprintf(val, sizeof(val), "%+d ms", app.offset_ms); break;
        case SET_SENS: name = "left stick reach"; snprintf(val, sizeof(val), "%.2fx", app.sensitivity); break;
        case SET_DIM: name = "background dim in play"; snprintf(val, sizeof(val), "%d%%", (int)lroundf(app.bg_dim * 100)); break;
        case SET_CURSOR: name = "cursor size"; snprintf(val, sizeof(val), "%.1fx", app.cursor_scale); break;
        case SET_STORYBOARD: name = "storyboards"; snprintf(val, sizeof(val), "%s", app.storyboards ? "on" : "off"); break;
        case SET_VIDEO: name = "video backgrounds (mp4)"; snprintf(val, sizeof(val), "%s", app.videos ? "on" : "off"); break;
        case SET_WIZARD: name = "find my offset"; snprintf(val, sizeof(val), "CROSS"); break;
        case SET_HELP: name = "show the controls"; snprintf(val, sizeof(val), "CROSS"); break;
        case SET_ACCOUNT:
            name = "ProjectCadence account";
            snprintf(val, sizeof(val), "%s", account_signed_in() ? account_name() : "sign in");
            break;
        case SET_BOARD:
            name = "online leaderboard in song select";
            snprintf(val, sizeof(val), "%s", !app.board_src ? "ProjectCadence"
                     : online_enabled() ? "osu.ppy.sh (official)" : "osu.ppy.sh (needs osuapi.txt)");
            break;
        case SET_RESCAN: name = "rescan the Songs folder"; snprintf(val, sizeof(val), "CROSS"); break;
        case SET_SKIN: name = "skin"; snprintf(val, sizeof(val), "%.40s", skins_active()); break;
        case SET_UPDATE: name = "check for updates"; snprintf(val, sizeof(val), "CROSS"); break;
        case SET_SETUP: name = "run setup wizard"; snprintf(val, sizeof(val), "CROSS"); break;
        case SET_CONVERTS: name = "show converted maps"; snprintf(val, sizeof(val), "%s", app.converts ? "on" : "off"); break;
        case SET_STARS_LO: name = "song select: lowest stars"; snprintf(val, sizeof(val), "%.1f*", app.star_lo); break;
        case SET_STARS_HI: name = "song select: highest stars"; snprintf(val, sizeof(val), app.star_hi > 0 ? "%.1f*" : "no limit", app.star_hi); break;
        case SET_LOOK: name = "interface"; snprintf(val, sizeof(val), "%s", app.simple_ui ? "simplified" : "lazer"); break;
        case SET_HUD: name = "hud"; snprintf(val, sizeof(val), "%s", app.hud_mode == 0 ? "always" : app.hud_mode == 1 ? "only in breaks" : "never"); break;
        case SET_KEYS: name = "key overlay"; snprintf(val, sizeof(val), "%s", app.key_overlay ? "on" : "off"); break;
        case SET_LIVE: name = "leaderboard while playing"; snprintf(val, sizeof(val), "%s", app.live_board ? "on" : "off"); break;
        case SET_CLASSIC: name = "score display"; snprintf(val, sizeof(val), "%s", app.classic_score ? "classic" : "standardised"); break;
        case SET_FPS: name = "show frame time"; snprintf(val, sizeof(val), "%s", app.show_fps ? "on" : "off"); break;
        case SET_CLAN: name = "clan"; snprintf(val, sizeof(val), "%s", account_clan()[0] ? account_clan() : "CROSS"); break;
        case SET_PASSWORD: name = "change password"; snprintf(val, sizeof(val), "CROSS"); break;
        case SET_DELETE: name = "delete account"; snprintf(val, sizeof(val), "CROSS"); break;
        case SET_REPORT: name = "report a problem"; snprintf(val, sizeof(val), "CROSS"); break;
        }
        float y = 150 + i * 54;
        bool on = i == app.set_sel;
        draw_rect(160, y, 1600, 48, on ? gfx_rgba(1, 1, 1, 0.92f) : gfx_rgba(1, 1, 1, 0.08f));
        uint32_t tc = on ? gfx_rgba(0.1f, 0.1f, 0.15f, 1) : gfx_rgba(1, 1, 1, 1);
        draw_text(190, y + 10, 26, tc, ALIGN_LEFT, name);
        draw_text(1730, y + 10, 26, tc, ALIGN_RIGHT, val);
    }

    const char *hint = "";
    switch (app.set_sel) {
    case SET_OFFSET: hint = "positive if you hit late everywhere; a map's own offset is D-PAD left/right while paused"; break;
    case SET_SENS: hint = "how far a full stick tilt reaches; above 1 you can reach the edge before full tilt"; break;
    case SET_RESCAN: hint = "after copying maps over FTP"; break;
    default: break;
    }
    draw_text(160, 150 + SET_COUNT * 54 + 6, 22, gfx_rgba(1, 1, 1, 0.7f), ALIGN_LEFT, hint);
    if (app.status[0])
        draw_text(160, 150 + SET_COUNT * 54 + 38, 24, gfx_rgba(1, 0.85f, 0.4f, 1), ALIGN_LEFT, app.status);
}

static void render_overlay_classic(const char *title, uint32_t colour, const char *hint)
{
    draw_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0, 0, 0, 0.6f));
    draw_text(GFX_W / 2, 380, 96, colour, ALIGN_CENTRE, title);
    draw_text(GFX_W / 2, 560, 30, gfx_rgba(1, 1, 1, 0.85f), ALIGN_CENTRE, hint);
}

static void render_results_classic(void)
{
    const osu5_hud *h = &app.last_hud;
    char buf[128];

    draw_background(0.7f);
    draw_rect(260, 140, 1400, 800, gfx_rgba(0, 0, 0, 0.65f));

    char title[200];
    osu5_string(OSU5_STR_TITLE, title, sizeof(title));
    draw_text(GFX_W / 2, 170, 40, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, title);

    /* accuracy ring: hits split by judgement around the rank, accuracy as the inner arc */
    float cx = 560, cy = 520, R = 230;
    int total = h->great + h->ok + h->meh + h->miss;
    arc(cx, cy, R, 26, 0, 1, gfx_rgba(1, 1, 1, 0.08f));
    if (total > 0) {
        const int counts[4] = {h->great, h->ok, h->meh, h->miss};
        const uint32_t cols[4] = {gfx_rgba(0.4f, 0.8f, 1, 1), gfx_rgba(0.5f, 1, 0.5f, 1), gfx_rgba(1, 0.85f, 0.35f, 1),
                                  gfx_rgba(1, 0.35f, 0.35f, 1)};
        float at = 0;
        for (int i = 0; i < 4; i++) {
            float span = (float)counts[i] / total;
            arc(cx, cy, R, 26, at, at + span, cols[i]);
            at += span;
        }
    }
    arc(cx, cy, R - 34, 8, 0, h->accuracy, gfx_rgba(1, 1, 1, 0.9f));
    draw_rank(h->rank, cx, cy, 240, false, 1);

    float x = 900, y = 230;
    snprintf(buf, sizeof(buf), "%08lld", (long long)h->score);
    draw_text(x, y, 72, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, buf);
    snprintf(buf, sizeof(buf), "%.0fpp", app.last_pp);
    draw_text(1620, y, 72, gfx_rgba(1, 0.85f, 0.4f, 1), ALIGN_RIGHT, buf);
    snprintf(buf, sizeof(buf), "%.2f%%   %dx   UR %.1f   %.2f*", h->accuracy * 100, h->max_combo, h->unstable_rate, osu5_play_stars());
    draw_text(x, y + 100, 32, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, buf);
    snprintf(buf, sizeof(buf), "300 %d   100 %d   50 %d   miss %d", h->great, h->ok, h->meh, h->miss);
    draw_text(x, y + 150, 28, gfx_rgba(1, 1, 1, 0.85f), ALIGN_LEFT, buf);

    char tags[64];
    mod_tags(app.mods, tags, sizeof(tags));
    if (tags[0])
        draw_text(x, y + 200, 28, gfx_rgba(1, 0.85f, 0.4f, 1), ALIGN_LEFT, tags);
    if (app.watching)
        draw_text(x, y + 250, 26, gfx_rgba(1, 1, 1, 0.6f), ALIGN_LEFT, "watched a replay");
    else if (app.new_place == 0)
        draw_text(x, y + 250, 32, gfx_rgba(0.5f, 1, 0.6f, 1), ALIGN_LEFT, "new personal best!");
    else if (app.mods & OSU5_MOD_AUTO)
        draw_text(x, y + 250, 26, gfx_rgba(1, 1, 1, 0.6f), ALIGN_LEFT, "auto plays aren't saved");

    /* hit offsets: where the taps landed against the beat */
    static float errs[8192];
    int ne = osu5_hit_errors(errs, 8192);
    enum { BINS = 41 };
    int bins[BINS] = {0}, top = 1;
    float sum = 0;
    for (int i = 0; i < ne; i++) {
        int b = (int)lroundf(errs[i] / 5) + BINS / 2; /* 5 ms a bin, +-100 ms */
        if (b >= 0 && b < BINS && ++bins[b] > top)
            top = bins[b];
        sum += errs[i];
    }
    float gx = 900, gy = 560, gw = 340, gh = 150;
    draw_rect(gx, gy, gw, gh, gfx_rgba(0, 0, 0, 0.35f));
    draw_rect(gx + gw / 2 - 1, gy, 2, gh, gfx_rgba(1, 1, 1, 0.35f));
    for (int b = 0; b < BINS; b++) {
        float bh = (gh - 10) * bins[b] / top;
        float off = fabsf((b - BINS / 2) * 5.0f);
        uint32_t c = off < 20 ? gfx_rgba(0.4f, 0.8f, 1, 0.9f) : off < 50 ? gfx_rgba(0.5f, 1, 0.5f, 0.9f) : gfx_rgba(1, 0.85f, 0.35f, 0.9f);
        draw_rect(gx + b * gw / BINS + 1, gy + gh - bh, gw / BINS - 2, bh, c);
    }
    snprintf(buf, sizeof(buf), "hits: %+.1f ms on average", ne ? sum / ne : 0);
    draw_text(gx, gy + gh + 10, 20, gfx_rgba(1, 1, 1, 0.7f), ALIGN_LEFT, buf);

    /* accuracy through the map */
    float ax = 1280, aw = 340;
    draw_rect(ax, gy, aw, gh, gfx_rgba(0, 0, 0, 0.35f));
    float lo = 1;
    for (int i = 0; i < app.acc_n; i++)
        lo = fminf(lo, app.acc[i]);
    lo = fminf(lo, 0.9f);
    for (int i = 1; i < app.acc_n; i++) {
        float x0 = ax + aw * (i - 1) / (ACC_SAMPLES - 1), x1 = ax + aw * i / (ACC_SAMPLES - 1);
        float y0 = gy + gh - 5 - (gh - 10) * (app.acc[i - 1] - lo) / (1 - lo);
        float y1 = gy + gh - 5 - (gh - 10) * (app.acc[i] - lo) / (1 - lo);
        gfx_bind(0);
        gfx_push(&(gfx_inst){x0, y0, 2, 2, 0, gfx_next_z(), gfx_rgba(1, 0.85f, 0.4f, 1), GK_CAPSULE, {x1, y1, 0, 0}});
    }
    snprintf(buf, sizeof(buf), "accuracy, %.0f%% to 100%%", lo * 100);
    draw_text(ax, gy + gh + 10, 20, gfx_rgba(1, 1, 1, 0.7f), ALIGN_LEFT, buf);

    if (app.mp_playing)
        mp_board(1680, 140, 230, true);
}

static void render_mp_classic(void)
{
    draw_background(0.75f);
    draw_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0.03f, 0.04f, 0.09f, 0.55f));
    const mp_state *s = &app.mps;
    bool bancho = app.mp_kind == MP_BANCHO;
    char buf[300];
    draw_text(120, 70, 52, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, "multiplayer");

    if (!app.mp_on || s->conn == MP_FAILED || s->conn == MP_OFF) {
        const char *labels[5] = {"server", bancho ? "bancho address" : "room server", "name", "password", "connect"};
        char vals[5][170];
        snprintf(vals[0], sizeof(vals[0]), "%s", bancho ? "bancho server (bancho.py)" : "ProjectCadence rooms");
        snprintf(vals[1], sizeof(vals[1]), "%s", bancho ? (app.mp_bancho[0] ? app.mp_bancho : "e.g. example.com") : app.mp_rooms_url);
        snprintf(vals[2], sizeof(vals[2]), "%s", app.mp_user);
        size_t pl = strlen(app.mp_pass);
        memset(vals[3], '*', pl < 20 ? pl : 20);
        vals[3][pl < 20 ? pl : 20] = 0;
        vals[4][0] = 0;
        for (int i = 0; i < 5; i++) {
            if (i == 3 && !bancho)
                continue;
            float y = 200 + i * 90;
            bool on = i == app.mp_cfg;
            draw_rect(120, y, 1680, 76, on ? gfx_rgba(1, 1, 1, 0.92f) : gfx_rgba(1, 1, 1, 0.08f));
            uint32_t tc = on ? gfx_rgba(0.08f, 0.08f, 0.12f, 1) : gfx_rgba(1, 1, 1, 1);
            draw_text(150, y + 22, 30, tc, ALIGN_LEFT, labels[i]);
            draw_text(1770, y + 22, 30, tc, ALIGN_RIGHT, vals[i]);
        }
        const char *err = s->error[0] ? s->error : app.status;
        if (err[0])
            draw_text(120, 680, 26, gfx_rgba(1, 0.55f, 0.5f, 1), ALIGN_LEFT, err);
        if (bancho)
            draw_text(120, 730, 22, gfx_rgba(1, 1, 1, 0.6f), ALIGN_LEFT,
                      "your account on that server; the password is kept on this console only");
        if (app.mp_typing)
            kbd_render(&app.mp_kbd);
        return;
    }

    if (s->conn == MP_CONNECTING) {
        draw_text(GFX_W / 2, 480, 40, gfx_rgba(1, 1, 1, 0.9f), ALIGN_CENTRE, "connecting...");
        return;
    }

    if (s->online > 0)
        snprintf(buf, sizeof(buf), "%s as %s   -   %d online", bancho ? app.mp_bancho : "ProjectCadence rooms", app.mp_user,
                 s->online);
    else
        snprintf(buf, sizeof(buf), "%s as %s", bancho ? app.mp_bancho : "ProjectCadence rooms", app.mp_user);
    draw_text(GFX_W - 120, 90, 24, gfx_rgba(0.5f, 0.9f, 0.95f, 1), ALIGN_RIGHT, buf);

    if (!s->in_room) {
        int rows = 2 + s->nrooms;
        for (int i = 0; i < rows && i < 9; i++) {
            float y = 190 + i * 88;
            bool on = i == app.mp_sel;
            draw_rect(120, y, 1680, 76, on ? gfx_rgba(1, 1, 1, 0.92f) : gfx_rgba(0, 0, 0, 0.5f));
            uint32_t tc = on ? gfx_rgba(0.08f, 0.08f, 0.12f, 1) : gfx_rgba(1, 1, 1, 1);
            if (i < 2) {
                draw_text(150, y + 22, 30, tc, ALIGN_LEFT, i == 0 ? "+  make a room" : "+  make a private room (password)");
                continue;
            }
            const mp_room_brief *r = &s->rooms[i - 2];
            draw_text(150, y + 10, 28, tc, ALIGN_LEFT, r->name);
            if (r->locked)
                draw_text(1770, y + 22, 24, on ? gfx_rgba(0.6f, 0.3f, 0.1f, 1) : gfx_rgba(1, 0.75f, 0.4f, 1), ALIGN_RIGHT, "password");
            snprintf(buf, sizeof(buf), "%d playing here%s   %.80s", r->players, r->playing ? ", mid round" : "", r->map);
            draw_text(150, y + 46, 18, tc, ALIGN_LEFT, buf);
        }
        if (s->nrooms == 0)
            draw_text(120, 300, 24, gfx_rgba(1, 1, 1, 0.6f), ALIGN_LEFT, "no rooms open yet");
        if (s->error[0])
            draw_text(120, GFX_H - 120, 24, gfx_rgba(1, 0.55f, 0.5f, 1), ALIGN_LEFT, s->error);
        return;
    }

    /* the room */
    draw_text(120, 150, 36, gfx_rgba(1, 0.85f, 0.4f, 1), ALIGN_LEFT, s->room_name);
    bool have = mp_find_map(s) >= 0;
    if (s->map_title[0])
        snprintf(buf, sizeof(buf), "%.140s%s", s->map_title, have ? "" : "   (downloading...)");
    else
        snprintf(buf, sizeof(buf), "%s", s->i_host ? "pick a map with SQUARE" : "the host is picking a map");
    draw_rect(120, 210, 1100, 70, gfx_rgba(0, 0, 0, 0.5f));
    draw_text(140, 230, 28, gfx_rgba(1, 1, 1, 0.95f), ALIGN_LEFT, buf);
    char tags[64];
    mod_tags(s->mods, tags, sizeof(tags));
    if (tags[0])
        draw_text(1200, 232, 26, gfx_rgba(1, 0.85f, 0.4f, 1), ALIGN_RIGHT, tags);

    if (s->state == MP_RESULTS) {
        draw_text(120, 310, 30, gfx_rgba(1, 1, 1, 0.9f), ALIGN_LEFT, "last round");
        mp_board(120, 350, 1100, false);
    } else {
        for (int i = 0; i < s->nmembers; i++) {
            const mp_member *m = &s->members[i];
            float y = 310 + i * 60;
            draw_rect(120, y, 1100, 52, m->me ? gfx_rgba(0.5f, 0.9f, 0.95f, 0.2f) : gfx_rgba(0, 0, 0, 0.45f));
            if (s->teams) /* the team's colour down the left */
                draw_rect(120, y, 10, 52, m->team ? gfx_rgba(0.35f, 0.6f, 1, 1) : gfx_rgba(1, 0.35f, 0.35f, 1));
            char mt[48] = "";
            if (s->freemod && m->mods)
                mod_tags(m->mods, mt, sizeof(mt));
            snprintf(buf, sizeof(buf), "%s%s%s%s", m->name, m->host ? "   (host)" : "", mt[0] ? "   " : "", mt);
            draw_text(140, y + 12, 26, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, buf);
            const char *tag = s->state == MP_PLAYING ? (m->done ? "done" : "playing") : !m->has_map ? "no map yet" : m->ready ? "ready" : "";
            draw_text(1200, y + 14, 24, m->ready ? gfx_rgba(0.5f, 1, 0.6f, 1) : gfx_rgba(1, 0.75f, 0.4f, 1), ALIGN_RIGHT, tag);
        }
    }

    /* the room's rules, and what this player can do here */
    snprintf(buf, sizeof(buf), "%s   %s", s->teams ? "teams: red vs blue" : "everyone for themselves",
             s->freemod ? "free mods" : "host's mods");
    draw_text(1280, 160, 24, gfx_rgba(0.5f, 0.9f, 0.95f, 1), ALIGN_LEFT, buf);
    const char *acts[8];
    int na = 0;
    acts[na++] = app.mp_ready ? "CROSS  not ready" : "CROSS  ready";
    if (s->i_host) {
        acts[na++] = "SQUARE  pick the map";
        acts[na++] = "TRIANGLE  start";
        acts[na++] = "OPTIONS  room settings";
    } else if (s->freemod) {
        acts[na++] = "SQUARE  your mods";
    }
    if (s->teams)
        acts[na++] = "L2  switch team";
    acts[na++] = "R1 type   L1 quick chat";
    acts[na++] = "CIRCLE  leave";
    float hy = 200;
    draw_rect(1280, hy, 520, 16 + na * 40, gfx_rgba(0, 0, 0, 0.5f));
    for (int i = 0; i < na; i++)
        draw_text(1300, hy + 12 + i * 40, 22, gfx_rgba(1, 1, 1, 0.9f), ALIGN_LEFT, acts[i]);

    /* chat, newest at the bottom */
    float cy = hy + 36 + na * 40, ch = GFX_H - 130 - cy;
    draw_rect(1280, cy, 520, ch, gfx_rgba(0, 0, 0, 0.5f));
    int lines = (int)(ch - 20) / 30;
    for (int i = 0; i < lines && i < s->nchat; i++) {
        int k = s->nchat - 1 - i;
        float ly = cy + ch - 40 - i * 30;
        if (s->chat[k].from[0])
            snprintf(buf, sizeof(buf), "%.16s: %.60s", s->chat[k].from, s->chat[k].text);
        else
            snprintf(buf, sizeof(buf), "%.70s", s->chat[k].text);
        draw_text(1295, ly, 18, s->chat[k].from[0] ? gfx_rgba(1, 1, 1, 0.92f) : gfx_rgba(1, 0.85f, 0.4f, 0.9f), ALIGN_LEFT, buf);
    }

    if (app.mp_quick >= 0) {
        draw_rect(700, 300, 520, 30 + QUICK_COUNT * 52, gfx_rgba(0.05f, 0.06f, 0.1f, 0.95f));
        for (int i = 0; i < QUICK_COUNT; i++) {
            float qy = 315 + i * 52;
            if (i == app.mp_quick)
                draw_rect(710, qy - 4, 500, 46, gfx_rgba(1, 1, 1, 0.9f));
            draw_text(730, qy + 6, 26, i == app.mp_quick ? gfx_rgba(0.08f, 0.08f, 0.12f, 1) : gfx_rgba(1, 1, 1, 0.9f),
                      ALIGN_LEFT, quick_lines[i]);
        }
    }
    if (app.mp_typing)
        kbd_render(&app.mp_kbd);
    if (s->state == MP_PLAYING && s->go_at > 0) {
        double left = s->go_at - wall_ms();
        if (left > 0) {
            snprintf(buf, sizeof(buf), "starting in %.0f", ceil(left / 1000));
            draw_text(GFX_W / 2, 900, 44, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, buf);
        }
    }
    if (s->error[0])
        draw_text(120, GFX_H - 100, 24, gfx_rgba(1, 0.55f, 0.5f, 1), ALIGN_LEFT, s->error);
}

static void render_account_classic(void)
{
    char msg[128], buf[160];
    int st = account_state(msg, sizeof(msg));
    draw_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0, 0, 0, 0.7f));
    draw_text(160, 90, 52, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, "ProjectCadence account");
    if (account_signed_in())
        snprintf(buf, sizeof(buf), "signed in as %s: your best scores go to the global board", account_name());
    else
        snprintf(buf, sizeof(buf), "an account puts your scores on the global board and your name on you in rooms");
    draw_text(160, 160, 24, gfx_rgba(0.5f, 0.9f, 0.95f, 1), ALIGN_LEFT, buf);

    static const char *labels[ACC_ROWS] = {"name", "password", "sign in", "make a new account", "sign out"};
    int rows = account_signed_in() ? ACC_ROWS : ACC_ROWS - 1;
    for (int i = 0; i < rows; i++) {
        float y = 230 + i * 90;
        bool on = i == app.acc_row;
        draw_rect(160, y, 1600, 76, on ? gfx_rgba(1, 1, 1, 0.92f) : gfx_rgba(1, 1, 1, 0.08f));
        uint32_t tc = on ? gfx_rgba(0.08f, 0.08f, 0.12f, 1) : gfx_rgba(1, 1, 1, 1);
        draw_text(190, y + 22, 30, tc, ALIGN_LEFT, labels[i]);
        if (i == ACC_NAME)
            draw_text(1730, y + 22, 30, tc, ALIGN_RIGHT, app.acc_name);
        if (i == ACC_PASS) {
            char stars[24];
            size_t n = strlen(app.acc_pass);
            memset(stars, '*', n < 20 ? n : 20);
            stars[n < 20 ? n : 20] = 0;
            draw_text(1730, y + 22, 30, tc, ALIGN_RIGHT, stars);
        }
    }
    const char *line = st == ACC_BUSY ? "talking to the server..." : st == ACC_FAILED ? msg : st == ACC_OK ? "done" : "";
    draw_text(160, 230 + rows * 90 + 20, 26, st == ACC_FAILED ? gfx_rgba(1, 0.55f, 0.5f, 1) : gfx_rgba(0.5f, 1, 0.6f, 1),
              ALIGN_LEFT, line);
    if (app.acc_typing)
        kbd_render(&app.mp_kbd);
}

static void menu_frame_classic(const pad_state *pad)
{
    cmenu_sel = (cmenu_sel + pad->nav_y + CMENU_COUNT) % CMENU_COUNT;

    /* something playing behind the menu: whatever song select is on */
    if (app.songs.count > 0 && app.previewed != app.sel) {
        const song_entry *e = &app.songs.items[app.sel];
        char audio[768] = "", bg[768] = "";
        songs_file(e->dir, e->info.audio, audio, sizeof(audio));
        if (!songs_file(e->dir, e->info.background, bg, sizeof(bg)))
            bg[0] = 0;
        preview_request(audio, bg, e->info.preview_time);
        app.previewed = app.sel;
    }
    unsigned char *px;
    int w, h;
    char path[768];
    if (preview_take_bg(&px, &w, &h, path, sizeof(path))) {
        set_background(px, w, h, path);
        if (px)
            stbi_image_free(px);
    }
    preview_tick();

    update_info up;
    update_status(&up);
    if (pad->pressed & PAD_TRIANGLE && (up.state == UPDATE_AVAILABLE || up.state == UPDATE_FAILED))
        update_install();

    if (!(pad->pressed & PAD_CROSS))
        return;
    switch (cmenu_sel) {
    case CMENU_PLAY: app.scr = SCREEN_SELECT; break;
    case CMENU_MAPS: mapdl_ui_open(); app.scr = SCREEN_DOWNLOAD; break;
    case CMENU_SKINS: skin_ui_open(false); app.scr = SCREEN_SKINS; break;
    case CMENU_SETTINGS: app.set_sel = 0; app.scr = SCREEN_SETTINGS; break;
    case CMENU_MULTI: mp_open(); break;
    }
}

static void render_menu_classic(void)
{
    draw_background(0.55f);
    draw_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0.03f, 0.04f, 0.09f, 0.35f));

    /* the logo breathes, a little faster than resting */
    float t = (float)(wall_ms() / 1000.0);
    float pulse = 1 + 0.03f * sinf(t * 2 * 3.14159f * 1.2f);
    float size = 420 * pulse, cx = 620, cy = GFX_H / 2;
    arc(cx, cy, size * 0.62f, 6, 0, 1, gfx_rgba(1, 0.47f, 0.63f, 0.35f));
    if (app.logo_tex) {
        gfx_bind(app.logo_tex);
        gfx_push(&(gfx_inst){cx, cy, size / 2, size / 2, 0, gfx_next_z(), gfx_rgba(1, 1, 1, 1), GK_TEX, {0, 0, 1, 1}});
        gfx_bind(0);
    }
    draw_text(cx, cy + size / 2 + 50, 48, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, "ProjectCadence");

    static const char *items[CMENU_COUNT] = {"play", "multiplayer", "get maps", "skins", "settings"};
    for (int i = 0; i < CMENU_COUNT; i++) {
        bool on = i == cmenu_sel;
        float x = on ? 1100 : 1140, y = 330 + i * 110;
        draw_rect(x, y, GFX_W - x, 90, on ? gfx_rgba(1, 1, 1, 0.92f) : gfx_rgba(0, 0, 0, 0.55f));
        draw_rect(x, y, 10, 90, gfx_rgba(0.5f, 0.9f, 0.95f, on ? 1 : 0.5f));
        draw_text(x + 50, y + 26, 40, on ? gfx_rgba(0.08f, 0.08f, 0.12f, 1) : gfx_rgba(1, 1, 1, 0.95f), ALIGN_LEFT, items[i]);
        if (i == CMENU_MULTI) {
            /* who's around right now */
            int n = app.mp_on && app.mps.online > 0 ? app.mps.online
                  : mp_peek_online(app.mp_kind, app.mp_kind == MP_BANCHO ? app.mp_bancho : app.mp_rooms_url);
            if (n >= 0) {
                char cb[32];
                snprintf(cb, sizeof(cb), "%d online", n);
                draw_text(GFX_W - 60, y + 32, 26, on ? gfx_rgba(0.1f, 0.45f, 0.5f, 1) : gfx_rgba(0.5f, 0.9f, 0.95f, 1),
                          ALIGN_RIGHT, cb);
            }
        }
    }

    if (app.songs.count > 0) {
        const osu5_peek_info *in = &app.songs.items[app.sel].info;
        char buf[300];
        snprintf(buf, sizeof(buf), "now playing: %.100s - %.100s", in->artist, in->title);
        draw_text(GFX_W - 30, 20, 22, gfx_rgba(1, 1, 1, 0.75f), ALIGN_RIGHT, buf);
    }
    draw_text(GFX_W - 30, GFX_H - 60, 22, gfx_rgba(1, 1, 1, 0.5f), ALIGN_RIGHT, CADENCE_VERSION);

    /* update banner along the top */
    update_info up;
    update_status(&up);
    char ub[200] = "";
    if (up.state == UPDATE_AVAILABLE)
        snprintf(ub, sizeof(ub), "ProjectCadence %s is out (you have %s)   TRIANGLE update now", up.latest, CADENCE_VERSION);
    else if (up.state == UPDATE_INSTALLING)
        snprintf(ub, sizeof(ub), "updating to %s... %.1f MB", up.latest, up.bytes / 1048576.0);
    else if (up.state == UPDATE_DONE || up.state == UPDATE_FAILED)
        snprintf(ub, sizeof(ub), "%s%s", up.msg, up.state == UPDATE_FAILED ? "   TRIANGLE try again" : "");
    if (ub[0]) {
        draw_rect(0, 60, GFX_W, 56, up.state == UPDATE_FAILED ? gfx_rgba(0.6f, 0.15f, 0.2f, 0.9f) : gfx_rgba(0.1f, 0.45f, 0.55f, 0.9f));
        draw_text(GFX_W / 2, 74, 28, gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, ub);
    }
}

