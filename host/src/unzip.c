#include "unzip.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

static uint32_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = n > 0 ? malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *len = buf ? (size_t)n : 0;
    return buf;
}

static void make_parents(char *path)
{
    for (char *p = path + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(path, 0777);
            *p = '/';
        }
    }
}

static int inflate_raw(const uint8_t *src, size_t n, uint8_t *dst, size_t out)
{
    z_stream s;
    memset(&s, 0, sizeof(s));
    if (inflateInit2(&s, -MAX_WBITS) != Z_OK)
        return -1;
    s.next_in = (Bytef *)src;
    s.avail_in = (uInt)n;
    s.next_out = dst;
    s.avail_out = (uInt)out;
    int rc = inflate(&s, Z_FINISH);
    inflateEnd(&s);
    return rc == Z_STREAM_END ? 0 : -1;
}

int unzip_to(const char *zip_path, const char *dir, bool lower)
{
    size_t len;
    uint8_t *z = slurp(zip_path, &len);
    if (!z || len < 22)
        return free(z), -1;

    /* end of central directory, searched from the back past any comment */
    const uint8_t *eocd = NULL;
    for (size_t i = len - 22; i + 65557 >= len && i > 0; i--) {
        if (rd32(z + i) == 0x06054b50) {
            eocd = z + i;
            break;
        }
    }
    if (!eocd)
        return free(z), -1;

    uint32_t count = rd16(eocd + 10), cd = rd32(eocd + 16);

    /* everything inside one folder? then that folder goes */
    char prefix[256] = "";
    size_t plen = 0;
    for (uint32_t k = 0, at = cd; k < count && at + 46 <= len; k++) {
        const uint8_t *h = z + at;
        uint32_t nlen = rd16(h + 28);
        const char *name = (const char *)h + 46;
        const char *slash = memchr(name, '/', nlen);
        if (!slash || (size_t)(slash - name) >= sizeof(prefix) - 1) {
            plen = 0;
            break;
        }
        size_t l = (size_t)(slash - name) + 1;
        if (k == 0) {
            memcpy(prefix, name, l);
            plen = l;
        } else if (l != plen || memcmp(prefix, name, l) != 0) {
            plen = 0;
            break;
        }
        at += 46 + nlen + rd16(h + 30) + rd16(h + 32);
    }

    mkdir(dir, 0777);
    int written = 0;
    for (uint32_t k = 0, at = cd; k < count; k++) {
        if (at + 46 > len || rd32(z + at) != 0x02014b50)
            break;
        const uint8_t *h = z + at;
        uint32_t method = rd16(h + 10), csize = rd32(h + 20), usize = rd32(h + 24);
        uint32_t nlen = rd16(h + 28), local = rd32(h + 42);
        const char *name = (const char *)h + 46;
        at += 46 + nlen + rd16(h + 30) + rd16(h + 32);

        if (nlen <= plen || name[nlen - 1] == '/')
            continue; /* directory entry */
        char rel[512];
        size_t rlen = nlen - plen < sizeof(rel) - 1 ? nlen - plen : sizeof(rel) - 1;
        memcpy(rel, name + plen, rlen);
        rel[rlen] = 0;
        if (rel[0] == '/' || strstr(rel, ".."))
            continue;
        for (char *c = rel; *c; c++)
            *c = (char)(*c == '\\' ? '/' : lower ? tolower((unsigned char)*c) : *c);

        if (local + 30 > len || rd32(z + local) != 0x04034b50)
            continue;
        size_t data = local + 30 + rd16(z + local + 26) + rd16(z + local + 28);
        if (data + csize > len)
            continue;

        uint8_t *out = malloc(usize ? usize : 1);
        if (!out)
            continue;
        int ok = method == 0 ? (memcpy(out, z + data, usize), 0) : method == 8 ? inflate_raw(z + data, csize, out, usize) : -1;
        if (ok == 0) {
            char path[768];
            snprintf(path, sizeof(path), "%s/%s", dir, rel);
            make_parents(path);
            FILE *f = fopen(path, "wb");
            if (f) {
                fwrite(out, 1, usize, f);
                fclose(f);
                written++;
            }
        }
        free(out);
    }
    free(z);
    return written;
}
