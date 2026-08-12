#ifndef SPFY_USEL_HASH_H
#define SPFY_USEL_HASH_H

#include <stddef.h>
#include <stdint.h>

#include "../common/le.h"
#include "../voice/voice.h"

/* Join-cost hash: precomputed (uid_left, uid_right) -> f32 join cost. */

/* The three arrays alias the VIN buffer at whatever offset the RIFF layout
 * put the chunk, so they are held as raw bytes and read through the
 * accessors below rather than as `const uint32_t *` / `const float *`. */
typedef struct {
    uint32_t       n_rows;
    uint32_t       n_cells;
    const uint8_t *rows;
    const uint8_t *cells_A;
    const uint8_t *cells_B;
} spfy_hash_t;

static inline uint32_t spfy_hash_row(const spfy_hash_t *h, uint32_t i)
{
    return spfy_le_u32(h->rows + (size_t)i * 4u);
}

static inline uint32_t spfy_hash_cell_a(const spfy_hash_t *h, uint64_t i)
{
    return spfy_le_u32(h->cells_A + (size_t)i * 4u);
}

static inline float spfy_hash_cell_b(const spfy_hash_t *h, uint64_t i)
{
    return spfy_le_f32(h->cells_B + (size_t)i * 4u);
}

/* Load hash sub-chunks from VIN. */
int  spfy_hash_load(const spfy_vin_t *vin, spfy_hash_t *out);
void spfy_hash_free(spfy_hash_t *h);

/* Lookup. */
int  spfy_hash_lookup(const spfy_hash_t *h,
                      uint32_t uid_left, uint32_t uid_right,
                      float *out_cost);

#endif
