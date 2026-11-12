/* ---- online overlays: rankings, player profiles and chat ----
 * Rankings and profiles read osu.ppy.sh (osuapi.txt keys); chat is the room
 * server's lobby channel, plus the room's own while you're in one. */

static screen ov_return = SCREEN_MENU;
static int rk_sel, rk_mode, rk_page;
static char pf_name[40];
static kbd pf_kbd;
static bool pf_typing;
static int pf_mode;
static screen pf_return = SCREEN_MENU;
static char ch_line[120];
static kbd ch_kbd;
static bool ch_typing;
static int ch_tab;      /* 0 lobby, 1 the room */
static float ch_scroll;

static const char *const ov_modes[4] = {"osu!", "osu!taiko", "osu!catch", "osu!mania"};

static void ov_mode_tabs(float x, float y, int mode)
{
    for (int m = 0; m < 4; m++) {
        bool on = m == mode;
        float w = draw_text(x, y, 22, gfx_rgba(1, 1, 1, on ? 1 : 0.55f), ALIGN_LEFT, ov_modes[m]);
        if (on)
            ui_round_rect(x, y + 34, w, 4, 2, ui_hex(0xFF66AA, 1));
        x += w + 36;
    }
}

static void group3(long long v, char *out, size_t n)
{
    char raw[32];
    snprintf(raw, sizeof(raw), "%lld", v);
    size_t len = strlen(raw), k = 0;
    for (size_t i = 0; i < len && k + 2 < n; i++) {
        if (i && (len - i) % 3 == 0)
            out[k++] = ',';
        out[k++] = raw[i];
    }
    out[k] = 0;
}

/* ---- rankings ---- */

static void rankings_open(screen back)
{
    ov_return = back;
    rk_mode = app.mode_pick;
    rk_page = 0;
    rk_sel = 0;
    web_rankings_get(rk_mode, rk_page);
    app.scr = SCREEN_RANKINGS;
}

static void profile_open(const char *who, int mode, screen back)
{
    snprintf(pf_name, sizeof(pf_name), "%s", who);
    pf_mode = mode;
    pf_return = back;
    web_user_get(pf_name, pf_mode);
    app.scr = SCREEN_PROFILE;
}

/* an empty profile with the keyboard up for a name */
static void profile_lookup(screen back)
{
    profile_open("", app.mode_pick, back);
    pf_kbd.text = pf_name;
    pf_kbd.cap = sizeof(pf_name);
    pf_typing = true;
}

static void rankings_frame(const pad_state *pad)
{
    web_rankings r;
    web_rankings_read(&r);
    int flick = rstick_flick(pad);
    if (pad->pressed & PAD_L1) flick = -1;
    if (pad->pressed & PAD_R1) flick = 1;
    if (flick == 1 || flick == -1) {
        rk_mode = (rk_mode + flick + 4) % 4;
        rk_page = rk_sel = 0;
        web_rankings_get(rk_mode, rk_page);
    }
    if (pad->nav_x && (rk_page + pad->nav_x) >= 0 && (rk_page + pad->nav_x) < 200) {
        rk_page += pad->nav_x;
        rk_sel = 0;
        web_rankings_get(rk_mode, rk_page);
    }
    if (r.count)
        rk_sel = (rk_sel + pad->nav_y + r.count) % r.count;
    if (pad->pressed & PAD_CROSS && r.state == WEB_READY && rk_sel < r.count) {
        char id[16];
        snprintf(id, sizeof(id), "%d", r.r[rk_sel].id);
        profile_open(id, rk_mode, SCREEN_RANKINGS);
        return;
    }
    if (pad->pressed & PAD_TRIANGLE)
        web_rankings_get(rk_mode, rk_page);
    if (pad->pressed & PAD_CIRCLE)
        app.scr = ov_return;
}

static void render_rankings(void)
{
    char buf[96];
    web_rankings r;
    web_rankings_read(&r);
    draw_background(0.85f);

    /* rankings sit on lazer's green-blue scheme */
    const float px = 144, pw = GFX_W - 2 * px;
    draw_rect(px, 0, pw, GFX_H, ui_hex(0x1C2125, 1));
    ui_header("rankings", "performance", ui_hex(0x66CCFF, 1));
    draw_rect(px, 110, pw, 70, ui_hex(0x242B30, 1));
    ov_mode_tabs(px + 50, 128, rk_mode);
    snprintf(buf, sizeof(buf), "page %d", rk_page + 1);
    draw_text(px + pw - 50, 132, 22, gfx_rgba(1, 1, 1, 0.6f), ALIGN_RIGHT, buf);

    /* column heads */
    float x = px + 40, w = pw - 80, y = 200;
    const float c_acc = x + w - 560, c_plays = x + w - 340, c_pp = x + w - 30;
    uint32_t head = gfx_rgba(1, 1, 1, 0.5f);
    draw_text(c_acc, y, 18, head, ALIGN_RIGHT, "accuracy");
    draw_text(c_plays, y, 18, head, ALIGN_RIGHT, "play count");
    draw_text(c_pp, y, 18, head, ALIGN_RIGHT, "performance");
    y += 36;

    if (r.state == WEB_LOADING) {
        draw_text(GFX_W / 2, 420, 26, gfx_rgba(1, 1, 1, 0.6f), ALIGN_CENTRE, "loading...");
    } else if (r.state != WEB_READY) {
        draw_text(GFX_W / 2, 400, 26, gfx_rgba(1, 1, 1, 0.75f), ALIGN_CENTRE,
                  online_enabled() ? "couldn't reach osu.ppy.sh, TRIANGLE tries again"
                                   : "rankings need osu! API keys in /data/cadence/osuapi.txt");
    } else {
        const float row_h = 48;
        int rows = (int)((GFX_H - 84 - 20 - y) / row_h);
        static float scroll;
        float want = rk_sel - rows / 2.0f;
        if (want > r.count - rows) want = (float)(r.count - rows);
        if (want < 0) want = 0;
        scroll = ui_approach(scroll, want, 0.3f);
        int first = (int)scroll;
        for (int i = first; i < r.count && i < first + rows + 1; i++) {
            float ry = y + (i - scroll) * row_h;
            if (ry < y - 4 || ry + row_h > GFX_H - 84)
                continue;
            const web_rank *e = &r.r[i];
            bool on = i == rk_sel;
            ui_round_rect(x, ry, w, row_h - 6, 8, on ? ui_hex(0x3A4A55, 1) : ui_hex(0x283035, 1));
            snprintf(buf, sizeof(buf), "#%d", e->rank);
            draw_text(x + 20, ry + 9, 22, gfx_rgba(1, 1, 1, 0.85f), ALIGN_LEFT, buf);
            ui_pill(x + 120, ry + 9, 16, ui_hex(0x445566, 1), gfx_rgba(1, 1, 1, 0.9f), e->country);
            draw_text(x + 190, ry + 9, 22, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, e->user);
            snprintf(buf, sizeof(buf), "%.2f%%", e->acc);
            draw_text(c_acc, ry + 9, 22, gfx_rgba(1, 1, 1, 0.75f), ALIGN_RIGHT, buf);
            group3(e->plays, buf, sizeof(buf));
            draw_text(c_plays, ry + 9, 22, gfx_rgba(1, 1, 1, 0.75f), ALIGN_RIGHT, buf);
            group3((long long)lroundf(e->pp), buf, sizeof(buf));
            strcat(buf, "pp");
            draw_text(c_pp, ry + 9, 22, gfx_rgba(1, 1, 1, 1), ALIGN_RIGHT, buf);
        }
    }
    ui_footer("L1 / R1  mode     LEFT / RIGHT  page     CROSS  profile", "CIRCLE");
}

/* ---- a player's profile ---- */

static void profile_frame(const pad_state *pad)
{
    if (pf_typing) {
        int k = kbd_frame(&pf_kbd, pad);
        if (k == KBD_DONE && pf_name[0])
            web_user_get(pf_name, pf_mode);
        if (k != KBD_TYPING)
            pf_typing = false;
        return;
    }
    int flick = rstick_flick(pad);
    if (pad->pressed & PAD_L1) flick = -1;
    if (pad->pressed & PAD_R1) flick = 1;
    if ((flick == 1 || flick == -1) && pf_name[0]) {
        pf_mode = (pf_mode + flick + 4) % 4;
        web_user_get(pf_name, pf_mode);
    }
    if (pad->pressed & PAD_SQUARE) {
        pf_kbd.text = pf_name;
        pf_kbd.cap = sizeof(pf_name);
        pf_typing = true;
        return;
    }
    if (pad->pressed & PAD_CIRCLE)
        app.scr = pf_return;
}

static void pf_stat(float x, float y, const char *label, const char *value, float size)
{
    draw_text(x, y, 18, gfx_rgba(1, 1, 1, 0.55f), ALIGN_LEFT, label);
    draw_text(x, y + 26, size, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, value);
}

static void render_profile(void)
{
    char buf[96], num[48];
    web_user u;
    web_user_read(&u);
    draw_background(0.85f);

    const float px = 144, pw = GFX_W - 2 * px;
    draw_rect(px, 0, pw, GFX_H, ui_hex(0x1F1D24, 1));
    ui_header("player info", ov_modes[pf_mode], ui_hex(0xFFCC22, 1));

    /* cover band with the avatar square over its bottom edge, then the name */
    draw_rect(px, 110, pw, 190, ui_hex(0x3D3550, 1));
    ui_triangles(px, 110, pw, 190, ui_hex(0x4A4060, 0.7f), wall_ms(), 7, 14);
    float ax = px + 60, ay = 200;
    ui_round_rect(ax, ay, 150, 150, 24, ui_hex(0x555066, 1));
    char ini[2] = {u.user[0] >= 'a' && u.user[0] <= 'z' ? (char)(u.user[0] - 32) : u.user[0], 0};
    draw_text(ax + 75, ay + 40, 64, gfx_rgba(1, 1, 1, 0.9f), ALIGN_CENTRE, ini);
    float nx = ax + 190;
    draw_text(nx, 230, 44, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, u.user[0] ? u.user : pf_name);
    float tx = nx;
    if (u.country[0])
        tx += ui_pill(tx, 300, 18, ui_hex(0x445566, 1), gfx_rgba(1, 1, 1, 0.9f), u.country) + 12;
    if (u.supporter)
        ui_pill(tx, 300, 18, ui_hex(0xFF66AA, 1), gfx_rgba(1, 1, 1, 1), "supporter");

    float y = 390, x = px + 60;
    if (u.state == WEB_LOADING) {
        draw_text(GFX_W / 2, 470, 26, gfx_rgba(1, 1, 1, 0.6f), ALIGN_CENTRE, "loading...");
    } else if (u.state != WEB_READY) {
        draw_text(GFX_W / 2, 470, 26, gfx_rgba(1, 1, 1, 0.75f), ALIGN_CENTRE,
                  !online_enabled() ? "profiles need osu! API keys in /data/cadence/osuapi.txt"
                  : pf_name[0]     ? "no such player, or osu.ppy.sh didn't answer"
                                   : "SQUARE to look someone up");
    } else {
        /* the big two: global and country ranking */
        snprintf(buf, sizeof(buf), u.rank ? "#%d" : "-", u.rank);
        if (u.rank) { group3(u.rank, num, sizeof(num)); snprintf(buf, sizeof(buf), "#%s", num); }
        pf_stat(x, y, "global ranking", buf, 40);
        if (u.country_rank) { group3(u.country_rank, num, sizeof(num)); snprintf(buf, sizeof(buf), "#%s", num); }
        else snprintf(buf, sizeof(buf), "-");
        pf_stat(x + 340, y, "country ranking", buf, 40);

        /* level: the number in a box and the progress bar */
        float lx = px + pw - 520;
        snprintf(buf, sizeof(buf), "%d", u.level);
        ui_round_rect(lx, y + 4, 70, 70, 12, ui_hex(0xFFCC22, 1));
        draw_text(lx + 35, y + 20, 30, ui_hex(0x1F1D24, 1), ALIGN_CENTRE, buf);
        ui_round_rect(lx + 90, y + 34, 360, 10, 5, gfx_rgba(1, 1, 1, 0.12f));
        ui_round_rect(lx + 90, y + 34, 360 * u.level_pct / 100.0f, 10, 5, ui_hex(0xFFCC22, 1));
        snprintf(buf, sizeof(buf), "%d%%", u.level_pct);
        draw_text(lx + 450, y + 52, 18, gfx_rgba(1, 1, 1, 0.6f), ALIGN_RIGHT, buf);

        /* the stat grid */
        y += 130;
        draw_rect(px + 40, y - 20, pw - 80, 2, gfx_rgba(1, 1, 1, 0.08f));
        float cw = (pw - 120) / 4;
        group3((long long)lroundf(u.pp), num, sizeof(num));
        pf_stat(x, y, "pp", num, 30);
        snprintf(buf, sizeof(buf), "%.2f%%", u.acc);
        pf_stat(x + cw, y, "hit accuracy", buf, 30);
        group3(u.plays, num, sizeof(num));
        pf_stat(x + 2 * cw, y, "play count", num, 30);
        snprintf(buf, sizeof(buf), "%dh", u.hours);
        pf_stat(x + 3 * cw, y, "play time", buf, 30);
        y += 100;
        group3(u.ranked_score, num, sizeof(num));
        pf_stat(x, y, "ranked score", num, 30);
        group3(u.total_score, num, sizeof(num));
        pf_stat(x + cw, y, "total score", num, 30);
        group3(u.max_combo, num, sizeof(num));
        pf_stat(x + 2 * cw, y, "maximum combo", num, 30);
        snprintf(buf, sizeof(buf), "%.10s", u.joined);
        pf_stat(x + 3 * cw, y, "joined", buf, 30);

        /* grade counts as lazer's badges */
        y += 120;
        static const struct { const char *g; uint32_t c; } grades[3] = {{"SS", 0xFFDD55}, {"S", 0xFFAA22}, {"A", 0x88DD44}};
        int counts[3] = {u.ss, u.s, u.a};
        for (int i = 0; i < 3; i++) {
            float gx = x + i * 200;
            ui_pill(gx, y, 24, ui_hex(grades[i].c, 1), ui_hex(0x1F1D24, 1), grades[i].g);
            group3(counts[i], num, sizeof(num));
            draw_text(gx + 70, y + 2, 26, gfx_rgba(1, 1, 1, 0.9f), ALIGN_LEFT, num);
        }
    }
    ui_footer("L1 / R1  mode     SQUARE  look someone up", "CIRCLE");
    if (pf_typing)
        kbd_render(&pf_kbd);
}

/* ---- chat ---- */

static void chat_open(screen back)
{
    ov_return = back;
    ch_tab = app.mps.in_room ? 1 : 0;
    ch_scroll = 0;
    /* the lobby lives on the room server: connect with the saved settings */
    if (!app.mp_on) {
        mp_load_cfg();
        if (app.mp_kind == MP_CADENCE) {
            mp_connect(MP_CADENCE, app.mp_rooms_url, app.mp_user, app.mp_pass);
            app.mp_on = true;
            mp_snapshot(&app.mps);
        }
    }
    app.scr = SCREEN_CHAT;
}

static void chat_frame(const pad_state *pad)
{
    if (ch_typing) {
        int k = kbd_frame(&ch_kbd, pad);
        if (k == KBD_DONE && ch_line[0]) {
            if (ch_tab == 1)
                mp_chat(ch_line);
            else
                mp_lobby_chat(ch_line);
            ch_line[0] = 0;
        }
        if (k != KBD_TYPING)
            ch_typing = false;
        return;
    }
    if (pad->nav_x && app.mps.in_room)
        ch_tab = !ch_tab;
    if (!app.mps.in_room)
        ch_tab = 0;
    ch_scroll = clampf(ch_scroll - pad->nav_y, 0, 39);
    if (pad->pressed & (PAD_CROSS | PAD_SQUARE) && app.mp_on && app.mps.conn == MP_ONLINE) {
        ch_kbd.text = ch_line;
        ch_kbd.cap = sizeof(ch_line);
        ch_typing = true;
        return;
    }
    if (pad->pressed & PAD_CIRCLE)
        app.scr = ov_return;
}

static void render_chat(void)
{
    char buf[200];
    /* whatever was behind stays visible above the chat, like lazer's overlay */
    if (ov_return == SCREEN_MENU)
        app.simple_ui ? render_menu_classic() : render_menu();
    else
        draw_background(0.6f);

    const mp_state *s = &app.mps;
    const float top = GFX_H * 0.42f, tab_h = 60;
    draw_rect(0, top, GFX_W, GFX_H - top, ui_hex(0x1C1A22, 0.98f));
    draw_rect(0, top, GFX_W, tab_h, ui_hex(0x2A2733, 1));

    /* channel tabs */
    const char *tabs[2] = {"#lobby", s->in_room ? s->room_name : NULL};
    float x = 40;
    for (int i = 0; i < 2; i++) {
        if (!tabs[i])
            continue;
        bool on = i == ch_tab;
        if (i == 1)
            snprintf(buf, sizeof(buf), "#%.40s", tabs[i]);
        else
            snprintf(buf, sizeof(buf), "%s", tabs[i]);
        float w = draw_text_width(22, buf) + 50;
        ui_round_rect(x, top + 10, w, tab_h - 10, 8, on ? ui_hex(0x1C1A22, 1) : ui_hex(0x35313F, 1));
        draw_text(x + w / 2, top + 22, 22, gfx_rgba(1, 1, 1, on ? 1 : 0.6f), ALIGN_CENTRE, buf);
        x += w + 10;
    }
    if (s->online > 0) {
        snprintf(buf, sizeof(buf), "%d online", s->online);
        draw_text(GFX_W - 40, top + 20, 20, gfx_rgba(1, 1, 1, 0.55f), ALIGN_RIGHT, buf);
    }

    /* messages, newest at the bottom, above the input line */
    const float in_h = 64, bottom = GFX_H - in_h - 20, line_h = 36;
    const mp_line *lines = ch_tab ? s->chat : s->lobby;
    int n = ch_tab ? s->nchat : s->nlobby;
    const char *empty = NULL;
    if (!app.mp_on || s->conn == MP_FAILED || s->conn == MP_OFF)
        empty = app.mp_kind == MP_CADENCE ? "not connected to the room server" : "lobby chat is on the ProjectCadence room server; switch to it in multiplayer";
    else if (s->conn != MP_ONLINE)
        empty = "connecting...";
    else if (!n)
        empty = "nobody's said anything yet";
    if (empty) {
        draw_text(GFX_W / 2, (top + tab_h + bottom) / 2 - 14, 24, gfx_rgba(1, 1, 1, 0.5f), ALIGN_CENTRE, empty);
    } else {
        int last = n - 1 - (int)ch_scroll;
        float y = bottom - line_h;
        for (int i = last; i >= 0 && y > top + tab_h + 10; i--, y -= line_h) {
            const mp_line *l = &lines[i];
            if (!l->from[0]) {
                draw_text(60, y, 20, gfx_rgba(1, 1, 1, 0.45f), ALIGN_LEFT, l->text);
                continue;
            }
            /* a colour per name, the way lazer tints usernames */
            uint32_t h = 2166136261u;
            for (const char *c = l->from; *c; c++)
                h = (h ^ (uint8_t)*c) * 16777619u;
            static const uint32_t name_cols[] = {0xFF66AA, 0x66CCFF, 0xFFCC22, 0x88DD44, 0xBB88FF, 0xFF8866};
            float nw = draw_text(220, y, 21, ui_hex(name_cols[h % 6], 1), ALIGN_RIGHT, l->from);
            (void)nw;
            snprintf(buf, sizeof(buf), "%s", l->text);
            ui_fit(buf, 21, GFX_W - 300);
            draw_text(240, y, 21, gfx_rgba(1, 1, 1, 0.9f), ALIGN_LEFT, buf);
        }
    }

    /* the input line */
    ui_round_rect(40, GFX_H - in_h - 10, GFX_W - 80, in_h - 14, 10, ui_hex(0x2A2733, 1));
    draw_text(64, GFX_H - in_h + 4, 22, gfx_rgba(1, 1, 1, ch_line[0] ? 1 : 0.4f), ALIGN_LEFT,
              ch_line[0] ? ch_line : "CROSS to type a message     LEFT / RIGHT channel     UP / DOWN scroll     CIRCLE close");
    if (ch_typing)
        kbd_render(&ch_kbd);
}
