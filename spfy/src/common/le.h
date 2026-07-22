#ifndef SPFY_COMMON_LE_H
#define SPFY_COMMON_LE_H

#include <stdint.h>

/* Unaligned little-endian scalar reads.
 *
 * Several loaders keep pointers directly into a mapped VIN/VDB buffer and
 * read 2/4-byte fields out of it (prsl candidates, hash rows/cells, CART
 * question value-sets, the f0 `hist` curve). Chunk payloads land wherever
 * the RIFF layout puts them, so those addresses are NOT guaranteed to be
 * 4-byte aligned -- casting to `const uint32_t *` / `const float *` and
 * indexing is undefined behaviour.
 *
 * x86 and AArch64 happen to tolerate it. 32-bit ARM does not: `LDR`
 * survives a misaligned address (SCTLR.A permitting) but a float load
 * lowers to `VLDR`, whose alignment requirement is architectural, so it
 * takes a SIGBUS. Read through these helpers instead -- on the tolerant
 * targets the compiler folds each one back into a single load, so there is
 * no cost to the platforms that were already working.
 *
 * These mirror the per-file `le_u32` statics that predate this header
 * (unit_table.c, cart_load.c, ccos.c, ...); the `spfy_` prefix keeps both
 * spellings able to coexist in one translation unit.
 */

static inline uint16_t spfy_le_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t spfy_le_u32(const uint8_t *p)
{
    return (uint32_t)p[0]         | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline float spfy_le_f32(const uint8_t *p)
{
    union { uint32_t u; float f; } v;
    v.u = spfy_le_u32(p);
    return v.f;
}

#endif
