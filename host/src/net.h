#ifndef OSU5_NET_H
#define OSU5_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool net_init(void);

/* GET url into the file at path, following redirects. Adds to *got as bytes come
 * in. Returns the HTTP status, or a negative sce error. Blocks: run it on a worker. */
int net_fetch(const char *url, const char *path, volatile uint64_t *got);

/* same, into a malloc'd buffer (NUL terminated, free it) */
int net_get(const char *url, char **out, size_t *len);

/* any method; headers are name, value pairs ending in NULL; body may be NULL */
int net_request(const char *method, const char *url, const char *const *headers, const char *body, char **out, size_t *len);
/* binary body; the response's headers land in resp_headers when it's given */
int net_request_raw(const char *method, const char *url, const char *const *headers, const void *body, size_t blen,
                    char **out, size_t *len, char *resp_headers, size_t hcap);

#endif
