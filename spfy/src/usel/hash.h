#ifndef SPFY_USEL_HASH_H
#define SPFY_USEL_HASH_H

#include <stddef.h>
#include <stdint.h>

#include "../common/le.h"
#include "../voice/voice.h"

/* Join-cost hash: precomputed (uid_left, uid_right) -> f32 join cost.
 *
 * On disk (per reveng/README_TECHNICAL.md `hash` section):
 *   head : { u32 n_rows; u32 n_cells }
 *   rows : u32[n_rows]                row offset table; rows[uid_right] is
 *                                     the base offset into the cells array
 *   cell : u32[n_cells] cells_A       owner uid_right per cell (verifies
 *                                     suffix-shared row offsets)
 *          f32[n_cells] cells_B       precomputed join cost
 *
 * Lookup (CORRECTED 2026-05-05 via the hash_lookup_hook live capture --
 * the README's earlier annotation said the cell key was uid_left, but
 * the hook proved it's actually uid_right):
 *
 *   index = rows[uid_right] + uid_left
 *   if cells_A[index] == uid_right:
 *       return cells_B[index]
 *   else:
 *       miss (engine falls back to ccos gate; out of scope here)
 *
 * Tom: n_rows=692190, n_cells=2416481.
 */

/* The three arrays alias the VIN buffer at whatever offset the RIFF layout
 * put the chunk, so they are held as raw bytes and read through the
 * accessors below rather than as `const uint32_t *` / `const float *`.
 * See common/le.h for why (32-bit ARM faults on the float case). */
typedef struct {
    uint32_t       n_rows;
    uint32_t       n_cells;
    const uint8_t *rows;      /* aliases VIN buffer; not owned; u32[n_rows] */
    const uint8_t *cells_A;   /* aliases VIN buffer; uid_right_owner, u32[] */
    const uint8_t *cells_B;   /* aliases VIN buffer; join cost, f32[] */
} spfy_hash_t;

/* Element accessors. Callers index by element, not byte. */
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

/* Load hash sub-chunks from VIN. Aliases the VIN buffer; do not free vin
 * while using out. */
int  spfy_hash_load(const spfy_vin_t *vin, spfy_hash_t *out);
void spfy_hash_free(spfy_hash_t *h);   /* no-op; here for symmetry */

/* Lookup. Returns SPFY_OK and writes *out_cost on hit; SPFY_E_OOB on miss
 * (caller falls back to ccos / default). SPFY_E_INVAL on bad inputs. */
int  spfy_hash_lookup(const spfy_hash_t *h,
                      uint32_t uid_left, uint32_t uid_right,
                      float *out_cost);

#endif
