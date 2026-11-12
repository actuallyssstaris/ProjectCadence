/* LZMA as the .lzma format has it (13-byte header, then the range coded
 * stream). Written from the format description in the LZMA SDK's lzma-specification.txt. */

#include "lzma.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define PROB_INIT 1024
#define TOP (1u << 24)

/* ---- writing ---- */

typedef struct {
    uint64_t low;
    uint32_t range;
    uint8_t cache;
    uint64_t cache_n;
    uint8_t *out;
    size_t n, cap;
    bool bad;
} enc;

static void put(enc *e, uint8_t b)
{
    if (e->n == e->cap) {
        size_t cap = e->cap ? e->cap * 2 : 4096;
        uint8_t *p = realloc(e->out, cap);
        if (!p) {
            e->bad = true;
            return;
        }
        e->out = p;
        e->cap = cap;
    }
    e->out[e->n++] = b;
}

static void shift_low(enc *e)
{
    if ((uint32_t)e->low < 0xFF000000u || (e->low >> 32) != 0) {
        uint8_t carry = (uint8_t)(e->low >> 32);
        uint8_t t = e->cache;
        do {
            put(e, (uint8_t)(t + carry));
            t = 0xFF;
        } while (--e->cache_n);
        e->cache = (uint8_t)(e->low >> 24);
    }
    e->cache_n++;
    e->low = (e->low & 0x00FFFFFFu) << 8;
}

static void enc_bit(enc *e, uint16_t *p, int bit)
{
    uint32_t bound = (e->range >> 11) * *p;
    if (!bit) {
        e->range = bound;
        *p += (2048 - *p) >> 5;
    } else {
        e->low += bound;
        e->range -= bound;
        *p -= *p >> 5;
    }
    while (e->range < TOP) {
        e->range <<= 8;
        shift_low(e);
    }
}

uint8_t *lzma_pack(const uint8_t *in, size_t n, size_t *out_n)
{
    /* lc 3, lp 0, pb 2: the usual settings */
    enc e = {0, 0xFFFFFFFFu, 0, 1, NULL, 0, 0, false};
    put(&e, 3 + 0 * 9 + 2 * 45);
    uint32_t dict = 1u << 16;
    for (int i = 0; i < 4; i++)
        put(&e, (uint8_t)(dict >> (8 * i)));
    for (int i = 0; i < 8; i++)
        put(&e, (uint8_t)((uint64_t)n >> (8 * i)));

    uint16_t is_match[4];
    static uint16_t lit[8 * 0x300];
    for (int i = 0; i < 4; i++)
        is_match[i] = PROB_INIT;
    for (int i = 0; i < 8 * 0x300; i++)
        lit[i] = PROB_INIT;

    /* never a match, so the state stays at 0 and every literal is a plain one */
    uint8_t prev = 0;
    for (size_t i = 0; i < n; i++) {
        enc_bit(&e, &is_match[i & 3], 0);
        uint16_t *probs = lit + 0x300 * (prev >> 5);
        unsigned ctx = 1;
        for (int b = 7; b >= 0; b--) {
            int bit = (in[i] >> b) & 1;
            enc_bit(&e, &probs[ctx], bit);
            ctx = (ctx << 1) | (unsigned)bit;
        }
        prev = in[i];
    }
    for (int i = 0; i < 5; i++)
        shift_low(&e);
    if (e.bad) {
        free(e.out);
        return NULL;
    }
    *out_n = e.n;
    return e.out;
}

/* ---- reading ---- */

typedef struct {
    const uint8_t *p, *end;
    uint32_t range, code;
    bool bad;
} dec;

static uint8_t next(dec *d)
{
    if (d->p >= d->end) {
        d->bad = true;
        return 0;
    }
    return *d->p++;
}

static void norm(dec *d)
{
    if (d->range < TOP) {
        d->range <<= 8;
        d->code = (d->code << 8) | next(d);
    }
}

static int dec_bit(dec *d, uint16_t *p)
{
    uint32_t bound = (d->range >> 11) * *p;
    int bit;
    if (d->code < bound) {
        d->range = bound;
        *p += (2048 - *p) >> 5;
        bit = 0;
    } else {
        d->code -= bound;
        d->range -= bound;
        *p -= *p >> 5;
        bit = 1;
    }
    norm(d);
    return bit;
}

static unsigned direct(dec *d, int count)
{
    unsigned v = 0;
    while (count--) {
        d->range >>= 1;
        d->code -= d->range;
        unsigned t = 0u - (d->code >> 31);
        d->code += d->range & t;
        v = (v << 1) + (t + 1);
        norm(d);
    }
    return v;
}

static unsigned tree(dec *d, uint16_t *probs, int bits)
{
    unsigned m = 1;
    for (int i = 0; i < bits; i++)
        m = (m << 1) + (unsigned)dec_bit(d, &probs[m]);
    return m - (1u << bits);
}

static unsigned tree_rev(dec *d, uint16_t *probs, int bits)
{
    unsigned m = 1, sym = 0;
    for (int i = 0; i < bits; i++) {
        unsigned bit = (unsigned)dec_bit(d, &probs[m]);
        m = (m << 1) + bit;
        sym |= bit << i;
    }
    return sym;
}

typedef struct {
    uint16_t choice, choice2;
    uint16_t low[16][8], mid[16][8], high[256];
} len_dec;

static void len_init(len_dec *l)
{
    l->choice = l->choice2 = PROB_INIT;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 8; j++)
            l->low[i][j] = l->mid[i][j] = PROB_INIT;
    for (int i = 0; i < 256; i++)
        l->high[i] = PROB_INIT;
}

static unsigned len_read(dec *d, len_dec *l, unsigned pos_state)
{
    if (!dec_bit(d, &l->choice))
        return tree(d, l->low[pos_state], 3);
    if (!dec_bit(d, &l->choice2))
        return 8 + tree(d, l->mid[pos_state], 3);
    return 16 + tree(d, l->high, 8);
}

typedef struct {
    uint16_t is_match[12 << 4], is_rep[12], is_rep0[12], is_rep1[12], is_rep2[12], is_rep0_long[12 << 4];
    uint16_t slot[4][64], special[115], align[16];
    len_dec len, rep_len;
} model;

uint8_t *lzma_unpack(const uint8_t *in, size_t n, size_t *out_n)
{
    if (n < 13 || in[0] >= 9 * 5 * 5)
        return NULL;
    unsigned lc = in[0] % 9, lp = (in[0] / 9) % 5, pb = in[0] / 45;
    uint64_t size = 0;
    for (int i = 0; i < 8; i++)
        size |= (uint64_t)in[5 + i] << (8 * i);
    bool sized = size != UINT64_MAX;
    if (sized && size > (256u << 20))
        return NULL;

    size_t cap = sized ? (size_t)size : n * 4 + 1024;
    uint8_t *out = malloc(cap ? cap : 1);
    model *m = malloc(sizeof(*m));
    uint16_t *lit = malloc(sizeof(uint16_t) * (0x300u << (lc + lp)));
    if (!out || !m || !lit) {
        free(out);
        free(m);
        free(lit);
        return NULL;
    }
    uint16_t *all = (uint16_t *)m;
    for (size_t i = 0; i < offsetof(model, len) / sizeof(uint16_t); i++)
        all[i] = PROB_INIT;
    len_init(&m->len);
    len_init(&m->rep_len);
    for (size_t i = 0; i < (0x300u << (lc + lp)); i++)
        lit[i] = PROB_INIT;

    dec d = {in + 13, in + n, 0xFFFFFFFFu, 0, false};
    if (next(&d) != 0)
        d.bad = true;
    for (int i = 0; i < 4; i++)
        d.code = (d.code << 8) | next(&d);

    size_t pos = 0;
    unsigned state = 0, rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0;
    unsigned pb_mask = (1u << pb) - 1, lp_mask = (1u << lp) - 1;
    while (!d.bad && (!sized || pos < size)) {
        unsigned ps = (unsigned)pos & pb_mask;
        if (!dec_bit(&d, &m->is_match[(state << 4) + ps])) {
            uint8_t prev = pos ? out[pos - 1] : 0;
            uint16_t *probs = lit + 0x300u * ((((unsigned)pos & lp_mask) << lc) + (prev >> (8 - lc)));
            unsigned sym = 1;
            if (state >= 7) {
                /* after a match: the byte at rep0 steers the first bits */
                unsigned match = pos > rep0 ? out[pos - rep0 - 1] : 0;
                do {
                    unsigned mbit = (match >> 7) & 1;
                    match <<= 1;
                    unsigned bit = (unsigned)dec_bit(&d, &probs[((1 + mbit) << 8) + sym]);
                    sym = (sym << 1) | bit;
                    if (mbit != bit)
                        break;
                } while (sym < 0x100);
            }
            while (sym < 0x100)
                sym = (sym << 1) | (unsigned)dec_bit(&d, &probs[sym]);
            if (pos == cap) {
                uint8_t *p = realloc(out, cap *= 2);
                if (!p) {
                    d.bad = true;
                    break;
                }
                out = p;
            }
            out[pos++] = (uint8_t)sym;
            state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
            continue;
        }

        unsigned len;
        if (dec_bit(&d, &m->is_rep[state])) {
            if (!pos) {
                d.bad = true;
                break;
            }
            if (!dec_bit(&d, &m->is_rep0[state])) {
                if (!dec_bit(&d, &m->is_rep0_long[(state << 4) + ps])) {
                    state = state < 7 ? 9 : 11;
                    len = 1;
                    goto copy;
                }
            } else {
                unsigned dist;
                if (!dec_bit(&d, &m->is_rep1[state])) {
                    dist = rep1;
                } else {
                    if (!dec_bit(&d, &m->is_rep2[state])) {
                        dist = rep2;
                    } else {
                        dist = rep3;
                        rep3 = rep2;
                    }
                    rep2 = rep1;
                }
                rep1 = rep0;
                rep0 = dist;
            }
            len = len_read(&d, &m->rep_len, ps);
            state = state < 7 ? 8 : 11;
        } else {
            rep3 = rep2;
            rep2 = rep1;
            rep1 = rep0;
            len = len_read(&d, &m->len, ps);
            state = state < 7 ? 7 : 10;
            unsigned ls = len < 3 ? len : 3;
            unsigned slot = tree(&d, m->slot[ls], 6);
            if (slot < 4) {
                rep0 = slot;
            } else {
                int bits = (int)(slot >> 1) - 1;
                unsigned dist = (2 | (slot & 1)) << bits;
                if (slot < 14) {
                    dist += tree_rev(&d, m->special + dist - slot, bits);
                } else {
                    dist += direct(&d, bits - 4) << 4;
                    dist += tree_rev(&d, m->align, 4);
                }
                rep0 = dist;
            }
            if (rep0 == 0xFFFFFFFFu)
                break; /* end marker */
        }
        len += 2;
    copy:
        if (rep0 >= pos) {
            d.bad = true;
            break;
        }
        for (; len > 0 && (!sized || pos < size); len--) {
            if (pos == cap) {
                uint8_t *p = realloc(out, cap *= 2);
                if (!p) {
                    d.bad = true;
                    break;
                }
                out = p;
            }
            out[pos] = out[pos - rep0 - 1];
            pos++;
        }
    }
    free(m);
    free(lit);
    if (d.bad && (!sized || pos < size)) {
        free(out);
        return NULL;
    }
    *out_n = pos;
    return out;
}
