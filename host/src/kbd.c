#include "kbd.h"
#include "draw.h"
#include "gfx.h"
#include "sce.h"

#include "log.h"

#include <stdint.h>
#include <string.h>

/* ---- the system keyboard (ImeDialog); ours is the fallback ---- */

int sceUserServiceGetInitialUser(int32_t *user);
int sceSysmoduleLoadModule(uint16_t id);

typedef struct {
    int32_t user;
    int32_t type;           /* 0 default */
    uint64_t languages;
    int32_t enter_label;    /* 0 default */
    int32_t input_method;
    void *filter;
    uint32_t option;
    uint32_t max_len;
    uint16_t *buffer;       /* UTF-16, at least max_len + 1 */
    float x, y;
    int32_t halign, valign;
    const uint16_t *placeholder;
    const uint16_t *title;
    int8_t reserved[16];
} ime_param;

typedef struct {
    int32_t end; /* 0 ok, 1 cancel, 2 abort */
    int8_t reserved[12];
} ime_result;

enum { IME_NONE, IME_RUNNING, IME_FINISHED };

int sceImeDialogInit(ime_param *, void *);
int sceImeDialogGetStatus(void);
int sceImeDialogGetResult(ime_result *);
int sceImeDialogTerm(void);
#define ime_init sceImeDialogInit
#define ime_status sceImeDialogGetStatus
#define ime_get_result sceImeDialogGetResult
#define ime_term sceImeDialogTerm
static int ime_state; /* 0 untried, 1 ready, -1 not available */
static uint16_t ime_buf[256];

static bool ime_load(void)
{
    if (ime_state)
        return ime_state > 0;
    /* linked against the SDK stub, so the loader brings the module in with us;
     * the sysmodule call is what PS4 titles needed before the first Init */
    int r = sceSysmoduleLoadModule(0x0096);
    if (r < 0)
        LOG("ime: sysmodule load 0x%x, trying anyway", r);
    ime_state = 1;
    return true;
}

/* BMP only, which covers anything people type in chat */
static void to_utf16(uint16_t *out, size_t n, const char *s)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && o + 1 < n) {
        uint32_t c = *p++;
        if (c >= 0xE0 && p[0] && p[1]) {
            c = ((c & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if (c >= 0xC0 && p[0]) {
            c = ((c & 0x1F) << 6) | (p[0] & 0x3F);
            p++;
        }
        out[o++] = (uint16_t)c;
    }
    out[o] = 0;
}

static void to_utf8(char *out, size_t n, const uint16_t *s)
{
    size_t o = 0;
    for (; *s; s++) {
        uint32_t c = *s;
        int len = c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
        if (o + len >= n)
            break;
        if (len == 1) {
            out[o++] = (char)c;
        } else if (len == 2) {
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[o++] = (char)(0xE0 | (c >> 12));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = 0;
}

static bool sys_open(kbd *k)
{
    if (!ime_load())
        return false;
    int32_t user = 0;
    sceUserServiceGetInitialUser(&user);
    size_t max = k->cap - 1;
    if (max > sizeof(ime_buf) / 2 - 1)
        max = sizeof(ime_buf) / 2 - 1;
    to_utf16(ime_buf, max + 1, k->text);

    ime_param p;
    memset(&p, 0, sizeof(p));
    p.user = user;
    p.languages = 0;
    p.max_len = (uint32_t)max;
    p.buffer = ime_buf;
    p.x = GFX_W / 2.0f;
    p.y = GFX_H / 2.0f;
    p.halign = 1; /* centre */
    p.valign = 1;
    int r = ime_init(&p, NULL);
    if (r < 0) {
        LOG("ime: init failed 0x%x, using ours", r);
        ime_state = -1;
        return false;
    }
    k->sys = 1;
    return true;
}

static int sys_frame(kbd *k)
{
    int st = ime_status();
    if (st == IME_RUNNING)
        return KBD_TYPING;
    ime_result res;
    memset(&res, 0, sizeof(res));
    if (st == IME_FINISHED)
        ime_get_result(&res);
    else
        res.end = 1;
    ime_term();
    k->sys = 0;
    if (res.end != 0)
        return KBD_CANCEL;
    to_utf8(k->text, k->cap, ime_buf);
    return KBD_DONE;
}

/* ---- our own pad keyboard ---- */

static const char *keys[4] = {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm-.!"};
enum { KEY_SPACE, KEY_DEL, KEY_GO }; /* the wide bottom row */

/* kx stays a column of the 10 wide rows; on the bottom row it picks one of the
 * three wide keys, so going up and down keeps roughly the same spot */
static int wide_key(const kbd *k) { return k->kx * 3 / 10; }

static void put(kbd *k, char c)
{
    size_t n = strlen(k->text);
    if (n + 1 < k->cap) {
        k->text[n] = c;
        k->text[n + 1] = 0;
    }
}

static void backspace(kbd *k)
{
    size_t n = strlen(k->text);
    if (n)
        k->text[n - 1] = 0;
}

int kbd_frame(kbd *k, const pad_state *pad)
{
    if (k->sys)
        return sys_frame(k);
    /* first frame of a fresh keyboard: hand it to the system one if we can */
    if (!k->kx && !k->ky && ime_state >= 0 && sys_open(k))
        return KBD_TYPING;

    k->ky = (k->ky + pad->nav_y + 5) % 5;
    if (pad->nav_x) {
        if (k->ky == 4) {
            static const int centre[3] = {1, 5, 8};
            k->kx = centre[(wide_key(k) + pad->nav_x + 3) % 3];
        } else {
            k->kx = (k->kx + pad->nav_x + 10) % 10;
        }
    }

    if (pad->pressed & PAD_CROSS) {
        if (k->ky < 4)
            put(k, keys[k->ky][k->kx]);
        else if (wide_key(k) == KEY_SPACE)
            put(k, ' ');
        else if (wide_key(k) == KEY_DEL)
            backspace(k);
        else
            return KBD_DONE;
    }
    if (pad->pressed & PAD_SQUARE)
        backspace(k);
    if (pad->pressed & PAD_TRIANGLE)
        put(k, ' ');
    if (pad->pressed & (PAD_OPTIONS | PAD_R2))
        return KBD_DONE;
    if (pad->pressed & PAD_CIRCLE)
        return KBD_CANCEL;
    return KBD_TYPING;
}

void kbd_render(const kbd *k)
{
    if (k->sys)
        return; /* the system draws its own over us */
    const float kw = 92, kh = 78, x0 = (GFX_W - kw * 10) / 2, y0 = GFX_H - 5 * kh - 120;
    draw_rect(x0 - 20, y0 - 20, kw * 10 + 40, kh * 5 + 40, gfx_rgba(0, 0, 0, 0.85f));
    char c[2] = {0, 0};
    for (int r = 0; r < 4; r++) {
        for (int i = 0; i < 10; i++) {
            bool on = k->ky == r && k->kx == i;
            float x = x0 + i * kw, y = y0 + r * kh;
            draw_rect(x + 4, y + 4, kw - 8, kh - 8, on ? gfx_rgba(1, 1, 1, 0.95f) : gfx_rgba(1, 1, 1, 0.12f));
            c[0] = keys[r][i];
            draw_text(x + kw / 2, y + kh / 2 - 18, 36, on ? gfx_rgba(0.1f, 0.1f, 0.15f, 1) : gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, c);
        }
    }
    static const char *wide[3] = {"space", "delete", "search"};
    for (int i = 0; i < 3; i++) {
        bool on = k->ky == 4 && wide_key(k) == i;
        float w = kw * 10 / 3, x = x0 + i * w, y = y0 + 4 * kh;
        draw_rect(x + 4, y + 4, w - 8, kh - 8, on ? gfx_rgba(1, 1, 1, 0.95f) : gfx_rgba(1, 1, 1, 0.12f));
        draw_text(x + w / 2, y + kh / 2 - 15, 30, on ? gfx_rgba(0.1f, 0.1f, 0.15f, 1) : gfx_rgba(1, 1, 1, 1), ALIGN_CENTRE, wide[i]);
    }
    draw_text(GFX_W / 2, GFX_H - 90, 24, gfx_rgba(1, 1, 1, 0.75f), ALIGN_CENTRE,
              "CROSS type   SQUARE delete   TRIANGLE space   OPTIONS search   CIRCLE close");
}
