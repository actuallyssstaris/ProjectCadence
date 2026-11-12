#include "osr.h"
#include "bweb.h"
#include "log.h"
#include "md5.h"
#include "mp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* .NET ticks at the unix epoch; osu! stores dates as ticks */
#define EPOCH_TICKS 621355968000000000LL

typedef struct {
    uint8_t *p;
    size_t n, cap;
} buf;

static void put(buf *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) {
        size_t cap = (b->cap + n) * 2;
        uint8_t *g = realloc(b->p, cap);
        if (!g)
            return;
        b->p = g;
        b->cap = cap;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}

static void put_u8(buf *b, uint8_t v) { put(b, &v, 1); }
static void put_u16(buf *b, uint16_t v) { put(b, &v, 2); }
static void put_i32(buf *b, int32_t v) { put(b, &v, 4); }
static void put_i64(buf *b, int64_t v) { put(b, &v, 8); }

static void put_str(buf *b, const char *s)
{
    size_t n = strlen(s);
    if (!n) {
        put_u8(b, 0);
        return;
    }
    put_u8(b, 0x0b);
    for (size_t v = n;;) {
        uint8_t c = v & 0x7f;
        v >>= 7;
        put_u8(b, v ? c | 0x80 : c);
        if (!v)
            break;
    }
    put(b, s, n);
}

bool osr_export(const osr_info *in, const replay *rp, const char *title, char *out_name, size_t n)
{
    size_t packed_n;
    uint8_t *packed = osr_pack(rp, in->mode, &packed_n);
    if (!packed)
        return false;

    buf b = {0};
    put_u8(&b, (uint8_t)in->mode);
    put_i32(&b, 20260924);
    put_str(&b, in->md5);
    put_str(&b, in->player);
    char check_src[200], check[33];
    snprintf(check_src, sizeof(check_src), "%d%s%d%d%lld", in->max_combo, in->md5, in->score, in->mods, in->when);
    md5_text(check_src, check);
    put_str(&b, check);
    put_u16(&b, (uint16_t)in->n300);
    put_u16(&b, (uint16_t)in->n100);
    put_u16(&b, (uint16_t)in->n50);
    put_u16(&b, (uint16_t)in->geki);
    put_u16(&b, (uint16_t)in->katu);
    put_u16(&b, (uint16_t)in->miss);
    put_i32(&b, in->score);
    put_u16(&b, (uint16_t)in->max_combo);
    put_u8(&b, in->perfect);
    put_i32(&b, (int32_t)mp_mods_to_osu(in->mods));
    put_str(&b, ""); /* life bar */
    put_i64(&b, EPOCH_TICKS + in->when * 10000LL);
    put_i32(&b, (int32_t)packed_n);
    put(&b, packed, packed_n);
    put_i64(&b, 0); /* online score id */
    free(packed);

    /* "Artist - Title [Diff] (2026-11-11 2041).osr", nothing a file system minds */
    mkdir(OSR_EXPORT_DIR, 0777);
    char name[220], path[320];
    time_t secs = (time_t)(in->when / 1000);
    struct tm tm;
    gmtime_r(&secs, &tm);
    char date[32];
    strftime(date, sizeof(date), "%Y-%m-%d %H%M", &tm);
    snprintf(name, sizeof(name), "%.150s (%s).osr", title, date);
    for (char *c = name; *c; c++)
        if (strchr("/\\:*?\"<>|", *c) || (unsigned char)*c < 32)
            *c = '_';
    snprintf(path, sizeof(path), OSR_EXPORT_DIR "/%s", name);
    FILE *f = fopen(path, "wb");
    bool ok = f && b.p && fwrite(b.p, 1, b.n, f) == b.n;
    if (f)
        fclose(f);
    free(b.p);
    if (!ok)
        remove(path);
    LOG("osr: export %s %s", path, ok ? "ok" : "failed");
    if (ok)
        snprintf(out_name, n, "%s", name);
    return ok;
}

/* ---- reading ---- */

typedef struct {
    const uint8_t *p, *end;
    bool bad;
} rd;

static bool need(rd *r, size_t n)
{
    if ((size_t)(r->end - r->p) < n)
        r->bad = true;
    return !r->bad;
}

static uint8_t get_u8(rd *r) { return need(r, 1) ? *r->p++ : 0; }

static uint32_t get_u32(rd *r)
{
    uint32_t v = 0;
    if (need(r, 4)) {
        memcpy(&v, r->p, 4);
        r->p += 4;
    }
    return v;
}

static uint16_t get_u16(rd *r)
{
    uint16_t v = 0;
    if (need(r, 2)) {
        memcpy(&v, r->p, 2);
        r->p += 2;
    }
    return v;
}

static int64_t get_i64(rd *r)
{
    int64_t v = 0;
    if (need(r, 8)) {
        memcpy(&v, r->p, 8);
        r->p += 8;
    }
    return v;
}

static void get_str(rd *r, char *out, size_t cap)
{
    out[0] = 0;
    uint8_t k = get_u8(r);
    if (k != 0x0b)
        return;
    size_t n = 0;
    for (int shift = 0; shift < 35; shift += 7) {
        uint8_t c = get_u8(r);
        n |= (size_t)(c & 0x7f) << shift;
        if (!(c & 0x80))
            break;
    }
    if (!need(r, n))
        return;
    size_t keep = n < cap - 1 ? n : cap - 1;
    memcpy(out, r->p, keep);
    out[keep] = 0;
    r->p += n;
}

bool osr_read(const char *path, osr_info *info, replay *rp, const char *osu_path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    if (size <= 0 || size > 32 * 1024 * 1024) {
        fclose(f);
        return false;
    }
    uint8_t *data = malloc((size_t)size);
    bool ok = data && fread(data, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!ok) {
        free(data);
        return false;
    }

    rd r = {data, data + size, false};
    memset(info, 0, sizeof(*info));
    info->mode = get_u8(&r);
    get_u32(&r); /* version */
    get_str(&r, info->md5, sizeof(info->md5));
    get_str(&r, info->player, sizeof(info->player));
    char skip[64];
    get_str(&r, skip, sizeof(skip));
    info->n300 = get_u16(&r);
    info->n100 = get_u16(&r);
    info->n50 = get_u16(&r);
    info->geki = get_u16(&r);
    info->katu = get_u16(&r);
    info->miss = get_u16(&r);
    info->score = (int)get_u32(&r);
    info->max_combo = get_u16(&r);
    info->perfect = get_u8(&r) != 0;
    info->mods = mp_mods_from_osu(get_u32(&r));
    /* the life bar can be long; skip it without keeping it */
    {
        uint8_t k = get_u8(&r);
        if (k == 0x0b) {
            size_t n = 0;
            for (int shift = 0; shift < 35; shift += 7) {
                uint8_t c = get_u8(&r);
                n |= (size_t)(c & 0x7f) << shift;
                if (!(c & 0x80))
                    break;
            }
            if (need(&r, n))
                r.p += n;
        }
    }
    int64_t ticks = get_i64(&r);
    info->when = ticks > EPOCH_TICKS ? (ticks - EPOCH_TICKS) / 10000 : 0;
    uint32_t len = get_u32(&r);
    ok = !r.bad && info->mode <= 3;
    if (ok && rp) {
        ok = need(&r, len);
        if (ok) {
            replay_reset(rp, osu_path ? osu_path : "", info->mods);
            ok = osr_unpack(r.p, len, info->mode, rp);
            rp->mods = info->mods;
        }
    }
    free(data);
    return ok;
}
