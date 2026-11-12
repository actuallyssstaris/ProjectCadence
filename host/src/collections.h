#ifndef OSU5_COLLECTIONS_H
#define OSU5_COLLECTIONS_H

#include <stdbool.h>

/* Named lists of difficulties, like lazer's collections. Kept in
 * /data/cadence/collections.txt, one "name<TAB>.osu path" per line. */

#define COLL_MAX 32

int coll_count(void);
const char *coll_name(int i);
int coll_size(int i);
bool coll_has(int i, const char *osu_file);
bool coll_toggle(int i, const char *osu_file); /* true if it's in now */
int coll_create(const char *name);              /* its index, -1 if full or taken */
void coll_delete(int i);

#endif
