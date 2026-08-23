/* SHA-256 (FIPS 180-4). Self-contained -- the update checker is the only
 * consumer and pulling a crypto library into a 32-bit MinGW static link for
 * one hash would be absurd. */

#ifndef SPFY_UPDATE_SHA256_H
#define SPFY_UPDATE_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t state[8];
    uint64_t bits;
    size_t   buf_n;
    uint8_t  buf[64];
} spfy_sha256_t;

void spfy_sha256_init(spfy_sha256_t *c);
void spfy_sha256_update(spfy_sha256_t *c, const void *data, size_t n);
/* out is 32 raw bytes. */
void spfy_sha256_final(spfy_sha256_t *c, uint8_t out[32]);

/* Lowercase hex, NUL-terminated: out must hold 65 bytes. */
void spfy_sha256_hex(const uint8_t digest[32], char out[65]);

/* Hash a whole file. Returns 0 on success, -1 if it cannot be read.
 * Streams in 1 MiB chunks -- a 96 MB VDB must not be slurped into RAM. */
int  spfy_sha256_file(const char *path, char out_hex[65]);

#ifdef __cplusplus
}
#endif

#endif
