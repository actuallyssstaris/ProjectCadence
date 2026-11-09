/* Just enough JSON for the mirror and skin catalog replies: walk objects key by
 * key, read strings (ASCII only, the font has nothing else) and numbers, skip
 * the rest. */
#include "json.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

void json_ws(js *j)
{
    while (*j->p == ' ' || *j->p == '\n' || *j->p == '\r' || *j->p == '\t')
        j->p++;
}

/* reads a string; non-ASCII turns into '?', the font has nothing else */
bool json_str(js *j, char *out, size_t n)
{
    if (*j->p != '"')
        return false;
    j->p++;
    size_t k = 0;
    while (*j->p && *j->p != '"') {
        unsigned char c = (unsigned char)*j->p++;
        if (c == '\\') {
            c = (unsigned char)*j->p++;
            switch (c) {
            case 'n': case 'r': case 't': case 'b': case 'f': c = ' '; break;
            case 'u': {
                unsigned v = 0;
                for (int i = 0; i < 4 && isxdigit((unsigned char)*j->p); i++, j->p++)
                    v = v * 16 + (unsigned)(isdigit((unsigned char)*j->p) ? *j->p - '0' : (tolower((unsigned char)*j->p) - 'a' + 10));
                c = v >= 32 && v < 127 ? (unsigned char)v : '?';
                break;
            }
            case 0: j->p--; c = 0; break;
            default: break; /* \" \\ \/ */
            }
            if (!c)
                break;
        } else if (c >= 0x80) {
            if ((c & 0xC0) != 0xC0)
                continue; /* rest of a multibyte char */
            c = '?';
        }
        if (out && k + 1 < n)
            out[k++] = (char)c;
    }
    if (*j->p == '"')
        j->p++;
    if (out && n)
        out[k] = 0;
    return true;
}

double json_num(js *j)
{
    char *end;
    double v = strtod(j->p, &end);
    if (end == j->p) {
        /* null / true / false */
        while (isalpha((unsigned char)*j->p))
            j->p++;
        return 0;
    }
    j->p = end;
    return v;
}

void json_skip(js *j)
{
    json_ws(j);
    if (*j->p == '"') {
        json_str(j, NULL, 0);
        return;
    }
    if (*j->p != '{' && *j->p != '[') {
        json_num(j);
        return;
    }
    int depth = 0;
    while (*j->p) {
        if (*j->p == '"') {
            json_str(j, NULL, 0);
            continue;
        }
        if (*j->p == '{' || *j->p == '[')
            depth++;
        else if (*j->p == '}' || *j->p == ']') {
            if (--depth == 0) {
                j->p++;
                return;
            }
        }
        j->p++;
    }
}


bool json_object(js *j, key_fn fn, void *ctx)
{
    json_ws(j);
    if (*j->p != '{')
        return false;
    j->p++;
    for (;;) {
        json_ws(j);
        if (*j->p == '}') {
            j->p++;
            return true;
        }
        char key[40];
        if (!json_str(j, key, sizeof(key)))
            return false;
        json_ws(j);
        if (*j->p != ':')
            return false;
        j->p++;
        json_ws(j);
        if (!fn(j, key, ctx))
            json_skip(j);
        json_ws(j);
        if (*j->p == ',')
            j->p++;
        else if (*j->p != '}')
            return false;
    }
}
