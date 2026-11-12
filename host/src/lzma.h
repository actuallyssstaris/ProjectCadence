#ifndef OSU5_LZMA_H
#define OSU5_LZMA_H

#include <stddef.h>
#include <stdint.h>

/* .lzma ("alone") streams, the format osu! keeps replay frames in.
 * Both return a malloc'd buffer (free it) and its size in *out_n, or NULL. */

/* only ever writes literals: no match finder, so it's small and quick, and
 * still shrinks replay text by about half */
uint8_t *lzma_pack(const uint8_t *in, size_t n, size_t *out_n);

/* a full decoder, for replays osu! itself wrote */
uint8_t *lzma_unpack(const uint8_t *in, size_t n, size_t *out_n);

#endif
