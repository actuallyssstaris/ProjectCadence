#ifndef OSU5_NET_H
#define OSU5_NET_H

#include <stdbool.h>
#include <stdint.h>

bool net_init(void);

/* GET url into the file at path, following redirects. Adds to *got as bytes come
 * in. Returns the HTTP status, or a negative sce error. Blocks: run it on a worker. */
int net_fetch(const char *url, const char *path, volatile uint64_t *got);

#endif
