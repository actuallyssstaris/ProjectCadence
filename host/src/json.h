#ifndef OSU5_JSON_H
#define OSU5_JSON_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *p;
} js;

void json_ws(js *j);
/* reads a string into out (NULL to skip it); non-ASCII turns into '?' */
bool json_str(js *j, char *out, size_t n);
double json_num(js *j); /* null / true / false read as 0 */
void json_skip(js *j);

/* walks an object, handing each key to fn with the reader on its value; fn
 * consumes the value, or returns false to have it skipped */
typedef bool (*key_fn)(js *j, const char *key, void *ctx);
bool json_object(js *j, key_fn fn, void *ctx);

#endif
