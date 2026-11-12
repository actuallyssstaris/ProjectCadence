#ifndef OSU5_RIJNDAEL_H
#define OSU5_RIJNDAEL_H

#include <stddef.h>
#include <stdint.h>

/* Rijndael with 32-byte blocks and a 32-byte key (not AES, which fixes the
 * block at 16), in CBC mode with PKCS#7 padding: what osu! stable encrypts
 * score submissions with. Returns the padded length written to out, which
 * needs n + 32 bytes of room. */
size_t rijndael256_cbc(const uint8_t key[32], const uint8_t iv[32], const uint8_t *in, size_t n, uint8_t *out);

#endif
