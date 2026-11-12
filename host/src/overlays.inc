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
static int ch_tab;      /* index into the tab list */
static float ch_scroll;
static bool ch_joining;      /* the keyboard is up for a channel or name */
static char ch_pending[32];  /* a DM with nobody's line in it yet */

static bool ch_bancho(void);
static void chat_with(const char *name, screen back);
static void spectate_open(int user_id, const char *name, screen back);

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

/* bancho players when we're on a bancho server, unless it came from the
 * osu.ppy.sh rankings */
static bool pf_bancho;

static const mp_user *pf_bancho_user(void)
{
    for (int i = 0; i < app.mps.nusers; i++)
        if (!strcasecmp(app.mps.users[i].name, pf_name))
            return &app.mps.users[i];
    return NULL;
}

static void profile_open(const char *who, int mode, screen back)
{
    snprintf(pf_name, sizeof(pf_name), "%s", who);
    pf_mode = mode;
    pf_return = back;
    pf_bancho = ch_bancho() && back != SCREEN_RANKINGS;
    if (pf_bancho) {
        if (pf_name[0])
            mp_user_lookup(pf_name);
    } else {
        web_user_get(pf_name, pf_mode);
    }
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
    int flick = pad->pressed & PAD_L1 ? -1 : pad->pressed & PAD_R1 ? 1 : 0;
    if (flick) {
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
        if (k == KBD_DONE && pf_name[0]) {
            if (pf_bancho)
                mp_user_lookup(pf_name);
            else
                web_user_get(pf_name, pf_mode);
        }
        if (k != KBD_TYPING)
            pf_typing = false;
        return;
    }
    int flick = pad->pressed & PAD_L1 ? -1 : pad->pressed & PAD_R1 ? 1 : 0;
    if (flick && pf_name[0]) {
        pf_mode = (pf_mode + flick + 4) % 4;
        web_user_get(pf_name, pf_mode);
    }
    if (pad->pressed & PAD_SQUARE) {
        pf_kbd.text = pf_name;
        pf_kbd.cap = sizeof(pf_name);
        pf_typing = true;
        return;
    }
    const mp_user *bu = pf_bancho ? pf_bancho_user() : NULL;
    if (bu) {
        static double asked;
        if (wall_ms() - asked > 4000) { /* keeps "playing ..." current */
            mp_user_stats(bu->id);
            asked = wall_ms();
        }
        if (pad->pressed & PAD_TRIANGLE)
            mp_friend(bu->id, !bu->friend_);
        if (pad->pressed & PAD_R2) {
            chat_with(bu->name, SCREEN_PROFILE);
            return;
        }
        if (pad->pressed & PAD_CROSS && bu->online) {
            spectate_open(bu->id, bu->name, SCREEN_PROFILE);
            return;
        }
    }
    if (pad->pressed & PAD_CIRCLE)
        app.scr = pf_return;
}

static void pf_stat(float x, float y, const char *label, const char *value, float size);

static const char *const bancho_actions[] = {"idle", "away", "playing", "editing", "modding", "in multiplayer",
                                             "watching", "", "testing", "submitting", "paused", "in the lobby",
                                             "multiplaying", "browsing osu!direct"};

static void render_bancho_profile(const mp_user *u)
{
    char buf[160], num[48];
    float x = 144 + 60, y = 390, cw = (GFX_W - 2 * 144 - 120) / 4.0f;
    if (u->online && u->action >= 0 && u->action < 14 && bancho_actions[u->action][0]) {
        snprintf(buf, sizeof(buf), "%s%s%.90s", bancho_actions[u->action], u->doing[0] ? ": " : "", u->doing);
        draw_text(x + 190, 340, 20, ui_hex(0x88DD44, 1), ALIGN_LEFT, buf);
    }
    if (u->rank) { group3(u->rank, num, sizeof(num)); snprintf(buf, sizeof(buf), "#%s", num); }
    else snprintf(buf, sizeof(buf), "-");
    pf_stat(x, y, "server ranking", buf, 40);
    group3(u->pp, num, sizeof(num));
    pf_stat(x + 340, y, "pp", num, 40);
    y += 130;
    snprintf(buf, sizeof(buf), "%.2f%%", u->acc * 100);
    pf_stat(x, y, "hit accuracy", buf, 30);
    group3(u->plays, num, sizeof(num));
    pf_stat(x + cw, y, "play count", num, 30);
    group3(u->ranked_score, num, sizeof(num));
    pf_stat(x + 2 * cw, y, "ranked score", num, 30);
    group3(u->total_score, num, sizeof(num));
    pf_stat(x + 3 * cw, y, "total score", num, 30);
    if (u->friend_)
        ui_pill(x + 190, 300, 18, ui_hex(0xFF66AA, 1), gfx_rgba(1, 1, 1, 1), "friend");
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
    const char *nm = !pf_bancho && u.user[0] ? u.user : pf_name;
    char ini[2] = {nm[0] >= 'a' && nm[0] <= 'z' ? (char)(nm[0] - 32) : nm[0], 0};
    draw_text(ax + 75, ay + 40, 64, gfx_rgba(1, 1, 1, 0.9f), ALIGN_CENTRE, ini);
    float nx = ax + 190;
    draw_text(nx, 230, 44, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, !pf_bancho && u.user[0] ? u.user : pf_name);
    float tx = nx;
    if (!pf_bancho && u.country[0])
        tx += ui_pill(tx, 300, 18, ui_hex(0x445566, 1), gfx_rgba(1, 1, 1, 0.9f), u.country) + 12;
    if (!pf_bancho && u.supporter)
        ui_pill(tx, 300, 18, ui_hex(0xFF66AA, 1), gfx_rgba(1, 1, 1, 1), "supporter");

    float y = 390, x = px + 60;
    if (pf_bancho) {
        const mp_user *bu = pf_bancho_user();
        if (bu)
            render_bancho_profile(bu);
        else
            draw_text(GFX_W / 2, 470, 26, gfx_rgba(1, 1, 1, 0.75f), ALIGN_CENTRE,
                      pf_name[0] ? "looking them up... (they have to be online)" : "SQUARE to look someone up");
        ui_footer(bu ? "SQUARE look up   CROSS spectate   TRIANGLE friend   R2 message" : "SQUARE  look someone up", "CIRCLE");
        if (pf_typing)
            kbd_render(&pf_kbd);
        return;
    }
    if (u.state == WEB_LOADING) {
        draw_text(GFX_W / 2, 470, 26, gfx_rgba(1, 1, 1, 0.6f), ALIGN_CENTRE, "loading...");
    } else if (u.state != WEB_READY) {
        draw_text(GFX_W / 2, 470, 26, gfx_rgba(1, 1, 1, 0.75f), ALIGN_CENTRE,
                  !online_enabled() ? "profiles need osu! API keys in /data/cadence/osuapi.txt"
                  : pf_name[0]     ? "no such player, or osu.ppy.sh didn't answer"
                                   : "SQUARE to look someone up");
    } else {
        /* global and country rank */
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

/* ---- chat ----
 * ProjectCadence server: #lobby and the room. bancho: the room, then every
 * channel joined and every DM. */

#define CH_TABS (MP_MAX_CHANNELS + 2)

static bool ch_bancho(void)
{
    return app.mp_on && app.mp_kind == MP_BANCHO && app.mps.conn == MP_ONLINE;
}

/* the tabs as names: "#lobby" / room / channels and DMs */
static int ch_tab_list(const char *names[CH_TABS], char room[64])
{
    const mp_state *s = &app.mps;
    int n = 0;
    if (!ch_bancho())
        names[n++] = "#lobby";
    if (s->in_room) {
        snprintf(room, 64, "#%.40s", s->room_name);
        names[n++] = room;
    }
    if (ch_bancho())
        for (int i = 0; i < s->nchannels && n < CH_TABS; i++)
            names[n++] = s->channels[i].name;
    return n;
}

/* which list a tab reads, and where a message typed on it goes */
static bool ch_is_room(int tab)
{
    return app.mps.in_room && tab == (ch_bancho() ? 0 : 1);
}

static void chat_open(screen back)
{
    ov_return = back;
    ch_tab = 0;
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

/* straight to a DM with someone, from their profile */
static void chat_with(const char *name, screen back)
{
    chat_open(back);
    snprintf(ch_pending, sizeof(ch_pending), "%s", name); /* the tab turns real on the first line */
}

static void chat_frame(const pad_state *pad)
{
    const char *names[CH_TABS];
    char room[64];
    int n = ch_tab_list(names, room);

    if (ch_typing) {
        int k = kbd_frame(&ch_kbd, pad);
        if (k == KBD_DONE && ch_line[0]) {
            if (ch_joining) {
                /* "#name" joins a channel, anything else starts a DM */
                if (ch_line[0] == '#')
                    mp_channel_join(ch_line);
                else
                    snprintf(ch_pending, sizeof(ch_pending), "%s", ch_line);
            } else if (ch_pending[0] && ch_tab >= n) {
                mp_say(ch_pending, ch_line);
            } else if (ch_is_room(ch_tab)) {
                mp_chat(ch_line);
            } else if (ch_bancho() && ch_tab < n) {
                mp_say(names[ch_tab], ch_line);
            } else {
                mp_lobby_chat(ch_line);
            }
            ch_line[0] = 0;
        }
        if (k != KBD_TYPING) {
            ch_typing = false;
            ch_joining = false;
        }
        return;
    }
    /* a DM that's waiting for its first line has a tab of its own, past the rest */
    for (int i = 0; ch_pending[0] && i < n; i++)
        if (!strcasecmp(names[i], ch_pending)) {
            ch_tab = i;
            ch_pending[0] = 0;
        }
    int tabs = n + (ch_pending[0] ? 1 : 0);
    if (pad->nav_x && tabs)
        ch_tab = (ch_tab + pad->nav_x + tabs) % tabs;
    if (ch_tab >= tabs)
        ch_tab = 0;
    if (ch_bancho() && ch_tab < n && !ch_is_room(ch_tab))
        mp_channel_seen(names[ch_tab]);
    ch_scroll = clampf(ch_scroll - pad->nav_y, 0, 119);
    bool online = app.mp_on && app.mps.conn == MP_ONLINE;
    if (pad->pressed & PAD_CROSS && online) {
        ch_kbd.text = ch_line;
        ch_kbd.cap = sizeof(ch_line);
        ch_typing = true;
        return;
    }
    if (pad->pressed & PAD_TRIANGLE && ch_bancho()) {
        ch_kbd.text = ch_line;
        ch_kbd.cap = sizeof(ch_line);
        ch_typing = ch_joining = true;
        return;
    }
    /* L1 closes a channel or DM */
    if (pad->pressed & PAD_L1 && ch_bancho() && ch_tab < n && !ch_is_room(ch_tab)) {
        mp_channel_leave(names[ch_tab]);
        ch_tab = 0;
    }
    /* R2: the profile of whoever spoke last here */
    if (pad->pressed & PAD_R2 && ch_bancho()) {
        const mp_state *s = &app.mps;
        for (int i = s->nmsgs - 1; i >= 0; i--)
            if (ch_tab < n && !strcasecmp(s->msgs[i].to, names[ch_tab]) && strcasecmp(s->msgs[i].from, app.mp_user)) {
                profile_open(s->msgs[i].from, app.mode_pick, SCREEN_CHAT);
                return;
            }
    }
    if (pad->pressed & PAD_CIRCLE)
        app.scr = ov_return;
}

static uint32_t name_colour(const char *name)
{
    uint32_t h = 2166136261u;
    for (const char *c = name; *c; c++)
        h = (h ^ (uint8_t)*c) * 16777619u;
    static const uint32_t cols[] = {0xFF66AA, 0x66CCFF, 0xFFCC22, 0x88DD44, 0xBB88FF, 0xFF8866};
    return ui_hex(cols[h % 6], 1);
}

static void render_chat(void)
{
    char buf[200];
    /* the screen behind stays visible above */
    if (ov_return == SCREEN_MENU)
        app.simple_ui ? render_menu_classic() : render_menu();
    else
        draw_background(0.6f);

    const mp_state *s = &app.mps;
    const char *names[CH_TABS];
    char room[64];
    int n = ch_tab_list(names, room);
    const float top = GFX_H * 0.42f, tab_h = 60;
    draw_rect(0, top, GFX_W, GFX_H - top, ui_hex(0x1C1A22, 0.98f));
    draw_rect(0, top, GFX_W, tab_h, ui_hex(0x2A2733, 1));

    /* tabs; channels with unread lines get a dot */
    float x = 40;
    for (int i = 0; i < n + (ch_pending[0] ? 1 : 0) && x < GFX_W - 300; i++) {
        const char *name = i < n ? names[i] : ch_pending;
        bool on = i == ch_tab;
        snprintf(buf, sizeof(buf), "%.28s", name);
        float w = draw_text_width(22, buf) + 50;
        ui_round_rect(x, top + 10, w, tab_h - 10, 8, on ? ui_hex(0x1C1A22, 1) : ui_hex(0x35313F, 1));
        draw_text(x + w / 2, top + 22, 22, gfx_rgba(1, 1, 1, on ? 1 : 0.6f), ALIGN_CENTRE, buf);
        if (ch_bancho() && i < n && !ch_is_room(i)) {
            for (int k = 0; k < s->nchannels; k++)
                if (!strcasecmp(s->channels[k].name, name) && s->channels[k].unread && !on)
                    ui_disc(x + w - 12, top + 20, 5, ui_hex(0xFF66AA, 1));
        }
        x += w + 10;
    }
    if (s->online > 0) {
        snprintf(buf, sizeof(buf), "%d online", s->online);
        draw_text(GFX_W - 40, top + 20, 20, gfx_rgba(1, 1, 1, 0.55f), ALIGN_RIGHT, buf);
    }

    /* the lines for this tab, newest at the bottom */
    const float in_h = 64, bottom = GFX_H - in_h - 20, line_h = 36;
    const mp_line *pick[120];
    int np = 0;
    if (ch_tab < n && ch_is_room(ch_tab)) {
        for (int i = 0; i < s->nchat; i++)
            pick[np++] = &s->chat[i];
    } else if (!ch_bancho()) {
        for (int i = 0; i < s->nlobby; i++)
            pick[np++] = &s->lobby[i];
    } else {
        const char *want = ch_tab < n ? names[ch_tab] : ch_pending;
        for (int i = 0; i < s->nmsgs && np < 120; i++)
            if (!strcasecmp(s->msgs[i].to, want))
                pick[np++] = &s->msgs[i];
    }
    const char *empty = NULL;
    if (!app.mp_on || s->conn == MP_FAILED || s->conn == MP_OFF)
        empty = "not connected: set a server up in multiplayer";
    else if (s->conn != MP_ONLINE)
        empty = "connecting...";
    else if (!np)
        empty = ch_tab >= n && ch_pending[0] ? "say something to start the conversation" : "nobody's said anything yet";
    if (empty) {
        draw_text(GFX_W / 2, (top + tab_h + bottom) / 2 - 14, 24, gfx_rgba(1, 1, 1, 0.5f), ALIGN_CENTRE, empty);
    } else {
        float y = bottom - line_h;
        for (int i = np - 1 - (int)ch_scroll; i >= 0 && y > top + tab_h + 10; i--, y -= line_h) {
            const mp_line *l = pick[i];
            if (!l->from[0]) {
                draw_text(60, y, 20, gfx_rgba(1, 1, 1, 0.45f), ALIGN_LEFT, l->text);
                continue;
            }
            draw_text(220, y, 21, name_colour(l->from), ALIGN_RIGHT, l->from);
            snprintf(buf, sizeof(buf), "%s", l->text);
            ui_fit(buf, 21, GFX_W - 300);
            draw_text(240, y, 21, gfx_rgba(1, 1, 1, 0.9f), ALIGN_LEFT, buf);
        }
    }

    ui_round_rect(40, GFX_H - in_h - 10, GFX_W - 80, in_h - 14, 10, ui_hex(0x2A2733, 1));
    const char *help = ch_bancho() ? "CROSS type   TRIANGLE join #channel or DM someone   L1 close tab   R2 their profile   CIRCLE close"
                                   : "CROSS to type a message     LEFT / RIGHT channel     UP / DOWN scroll     CIRCLE close";
    draw_text(64, GFX_H - in_h + 4, 22, gfx_rgba(1, 1, 1, ch_line[0] ? 1 : 0.4f), ALIGN_LEFT, ch_line[0] ? ch_line : help);
    if (ch_typing)
        kbd_render(&ch_kbd);
}

/* ---- room settings, for the host: OPTIONS in a room ----
 * teams and free mods everywhere; on bancho also the win condition, and each
 * player with make-host and kick */

static int host_sel = -1; /* row, -1 closed */
static const char *const win_names[4] = {"score", "accuracy", "combo", "score v2"};

static bool host_tools_open(void) { return host_sel >= 0; }

static int host_fixed_rows(void) { return app.mp_kind == MP_BANCHO ? 3 : 2; }

static void host_tools_frame(const pad_state *pad)
{
    const mp_state *s = &app.mps;
    int fixed = host_fixed_rows();
    int rows = fixed + (app.mp_kind == MP_BANCHO ? s->nmembers : 0);
    host_sel = (host_sel + pad->nav_y + rows) % rows;
    bool pick = pad->pressed & PAD_CROSS || pad->nav_x;
    if (host_sel == 0 && pick)
        mp_settings(!s->teams, s->freemod);
    else if (host_sel == 1 && pick)
        mp_settings(s->teams, !s->freemod);
    else if (host_sel == 2 && fixed == 3 && pick)
        mp_win_condition((s->win + (pad->nav_x < 0 ? 3 : 1)) % 4);
    else if (host_sel >= fixed && host_sel - fixed < s->nmembers) {
        const mp_member *m = &s->members[host_sel - fixed];
        if (pad->pressed & PAD_CROSS && !m->me)
            mp_host_to(m->slot);
        if (pad->pressed & PAD_SQUARE && !m->me)
            mp_lock_slot(m->slot); /* locking an occupied slot sends them out */
    }
    if (pad->pressed & (PAD_CIRCLE | PAD_OPTIONS))
        host_sel = -1;
}

static void render_host_tools(void)
{
    const mp_state *s = &app.mps;
    char buf[96];
    int fixed = host_fixed_rows();
    int players = app.mp_kind == MP_BANCHO ? s->nmembers : 0;
    const float w = 700, row = 52, h = 100 + (fixed + players) * row + (players ? 40 : 0);
    const float x = (GFX_W - w) / 2, y = (GFX_H - h) / 2;
    draw_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0, 0, 0, 0.55f));
    ui_round_rect(x, y, w, h, 18, ui_plum(0.16f, 0.98f));
    draw_text(x + 30, y + 24, 30, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, "room settings");
    float ry = y + 80;
    for (int i = 0; i < fixed + players; i++) {
        if (i == fixed) {
            draw_text(x + 30, ry + 8, 20, gfx_rgba(1, 1, 1, 0.55f), ALIGN_LEFT, "players");
            ry += 40;
        }
        bool on = i == host_sel;
        ui_round_rect(x + 20, ry, w - 40, row - 6, 8, on ? ui_plum(0.3f, 1) : ui_plum(0.22f, 1));
        const char *label = buf, *value = "";
        if (i == 0) { label = "teams"; value = s->teams ? "on" : "off"; }
        else if (i == 1) { label = "free mods"; value = s->freemod ? "on" : "off"; }
        else if (i == 2 && fixed == 3) { label = "win by"; value = win_names[s->win & 3]; }
        else {
            const mp_member *m = &s->members[i - fixed];
            snprintf(buf, sizeof(buf), "%s%s%s", m->name, m->host ? "  (host)" : "", m->me ? "  (you)" : "");
            value = on && !m->me ? "CROSS make host   SQUARE kick" : "";
        }
        draw_text(x + 40, ry + 11, 22, gfx_rgba(1, 1, 1, 1), ALIGN_LEFT, label);
        draw_text(x + w - 40, ry + 12, 20, i < fixed ? ui_hex(UI_YELLOW, 1) : gfx_rgba(1, 1, 1, 0.6f), ALIGN_RIGHT, value);
        ry += row;
    }
    draw_text(GFX_W / 2, y + h + 16, 20, gfx_rgba(1, 1, 1, 0.7f), ALIGN_CENTRE, "CROSS change   CIRCLE close");
}

/* ---- spectating someone on bancho ---- */

static int spec_target;
static char spec_name[32];
static bool spec_live;      /* a map is playing from their frames */
static char spec_md5[33];   /* the map we started, so a new one restarts */

static int spec_keys(int k, int mode)
{
    if (mode == 1)
        return (k & 1 ? 1 : 0) | (k & 4 ? 2 : 0) | (k & 2 ? 4 : 0) | (k & 8 ? 8 : 0);
    return (k & 1 ? 1 : 0) | (k & 2 ? 2 : 0);
}

static void spectate_open(int user_id, const char *name, screen back)
{
    spec_target = user_id;
    snprintf(spec_name, sizeof(spec_name), "%s", name);
    spec_live = false;
    spec_md5[0] = 0;
    ov_return = back;
    mp_spectate(user_id);
    mp_user_stats(user_id);
    app.scr = SCREEN_SPECTATE;
}

static const mp_user *spec_user(void)
{
    for (int i = 0; i < app.mps.nusers; i++)
        if (app.mps.users[i].id == spec_target)
            return &app.mps.users[i];
    return NULL;
}

/* the library's copy of what they're playing, -1 if we don't have it */
static int spec_find(const mp_user *u)
{
    for (int i = 0; i < app.songs.count; i++)
        if (u->map_id > 0 && app.songs.items[i].info.beatmap_id == u->map_id)
            return i;
    return -1;
}

/* new spectator frames into the replay, once a frame */
static void spectate_pull(void)
{
    static mp_spec_frame got[512];
    int action = 0;
    int mode = app.ruleset;
    int n = mp_spec_take(got, 512, &action);
    for (int i = 0; i < n; i++)
        replay_push(&app.watch, got[i].t, got[i].x, got[i].y, spec_keys(got[i].keys, mode));
    /* they quit or failed */
    if (action == 7 || action == 4) {
        spec_live = false;
        spec_md5[0] = 0;
    }
}

static void spectate_frame(const pad_state *pad)
{
    static double asked;
    if (wall_ms() - asked > 3000) {
        mp_user_stats(spec_target); /* their status moves on as they play */
        asked = wall_ms();
    }
    const mp_user *u = spec_user();
    if (u && u->action == 2 && u->md5[0] && strcmp(u->md5, spec_md5)) {
        int i = spec_find(u);
        if (i >= 0) {
            snprintf(spec_md5, sizeof(spec_md5), "%s", u->md5);
            app.sel = i;
            replay_reset(&app.watch, app.songs.items[i].file, u->mods);
            app.saved_mods = app.mods;
            app.mods = u->mods;
            app.watching = true;
            spec_live = true;
            if (!start_song(&app.songs.items[i])) {
                app.watching = spec_live = false;
                app.mods = app.saved_mods;
            }
            return;
        }
        if (pad->pressed & PAD_CROSS && u->map_id > 0) {
            mapdl_set want;
            memset(&want, 0, sizeof(want));
            want.beatmap = u->map_id;
            snprintf(want.title, sizeof(want.title), "%.100s", u->doing);
            mapdl_queue(&want);
            toast("downloading their map");
        }
    }
    if (pad->pressed & PAD_CIRCLE) {
        mp_spectate(0);
        spec_target = 0;
        spec_live = false;
        app.scr = ov_return;
    }
}

static void render_spectate(void)
{
    char buf[200];
    draw_background(0.85f);
    const mp_user *u = spec_user();
    ui_header("spectating", spec_name, ui_hex(0x66CCFF, 1));
    const char *line = "waiting for them to play something";
    const char *sub = NULL;
    if (!ch_bancho())
        line = "spectating needs a bancho server: connect in multiplayer";
    else if (u && u->action == 2 && spec_find(u) < 0) {
        line = "they're playing a map you don't have";
        sub = u->map_id > 0 ? "CROSS downloads it, and watching starts once it's in" : "it isn't on the mirror, so it can't be fetched";
    } else if (u && u->action == 2) {
        line = "loading their map...";
    }
    draw_text(GFX_W / 2, GFX_H / 2 - 60, 30, gfx_rgba(1, 1, 1, 0.9f), ALIGN_CENTRE, line);
    if (u && u->doing[0]) {
        snprintf(buf, sizeof(buf), "%.120s", u->doing);
        draw_text(GFX_W / 2, GFX_H / 2, 24, ui_hex(UI_YELLOW, 1), ALIGN_CENTRE, buf);
    }
    if (sub)
        draw_text(GFX_W / 2, GFX_H / 2 + 50, 22, gfx_rgba(1, 1, 1, 0.6f), ALIGN_CENTRE, sub);
    ui_footer(NULL, "CIRCLE");
}

/* back to waiting for their next map */
static void spectate_back(void)
{
    spec_live = false;
    spec_md5[0] = 0;
    app.scr = SCREEN_SPECTATE;
}
