/* Join-cost hash loader + lookup. */

#include "hash.h"

#include "../common/riff.h"
#include "../common/log.h"
#include "../../include/spfy/spfy.h"

#include <string.h>

#define FCC_HEAD SPFY_FOURCC('h','e','a','d')
#define FCC_ROWS SPFY_FOURCC('r','o','w','s')
#define FCC_CELL SPFY_FOURCC('c','e','l','l')

static uint32_t le_u32(const uint8_t *p)
{
    return (uint32_t)p[0]        | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int spfy_hash_load(const spfy_vin_t *vin, spfy_hash_t *out)
{
    if (!vin || !out) return SPFY_E_INVAL;
    if (!vin->hash || vin->hash_n == 0) return SPFY_E_FORMAT;
    memset(out, 0, sizeof *out);

    spfy_riff_iter it;
    spfy_riff_iter_init(&it, vin->hash, vin->hash_n);
    spfy_chunk c;
    int rc;

    const uint8_t *rows_ptr = NULL, *cell_ptr = NULL;
    size_t         rows_n   = 0,    cell_n   = 0;

    while ((rc = spfy_riff_iter_next(&it, &c)) == 1) {
        if (c.fourcc == FCC_HEAD) {
            if (c.size < 8) return SPFY_E_FORMAT;
            out->n_rows  = le_u32(c.data);
            out->n_cells = le_u32(c.data + 4);
        } else if (c.fourcc == FCC_ROWS) {
            rows_ptr = c.data; rows_n = c.size;
        } else if (c.fourcc == FCC_CELL) {
            cell_ptr = c.data; cell_n = c.size;
        }
    }
    if (rc < 0)         return SPFY_E_FORMAT;
    if (out->n_rows == 0 || out->n_cells == 0) return SPFY_E_FORMAT;
    if (!rows_ptr || !cell_ptr)                return SPFY_E_FORMAT;

    /* Validate sizes. */
    if (rows_n != (size_t)out->n_rows * 4u) {
        spfy_log_err("hash: rows size %zu != %u * 4", rows_n, out->n_rows);
        return SPFY_E_FORMAT;
    }
    /* cell sub-chunk holds u32[n_cells] cells_A then f32[n_cells] cells_B. */
    if (cell_n != (size_t)out->n_cells * 8u) {
        spfy_log_err("hash: cell size %zu != %u * 8", cell_n, out->n_cells);
        return SPFY_E_FORMAT;
    }

    out->rows    = (const uint32_t *)rows_ptr;
    out->cells_A = (const uint32_t *)cell_ptr;
    out->cells_B = (const float    *)(cell_ptr + (size_t)out->n_cells * 4u);
    return SPFY_OK;
}

void spfy_hash_free(spfy_hash_t *h)
{
    if (!h) return;
    memset(h, 0, sizeof *h);
}

int spfy_hash_lookup(const spfy_hash_t *h,
                     uint32_t uid_left, uint32_t uid_right,
                     float *out_cost)
{
    if (!h || !out_cost) return SPFY_E_INVAL;
    if (!h->rows || !h->cells_A || !h->cells_B) return SPFY_E_INVAL;
    *out_cost = 0.0f;
    if (uid_right >= h->n_rows) return SPFY_E_OOB;

    /* NB: rows[r] == 0 is NOT necessarily a miss. uid_right=169578 (last
     * valid uid in Tom) has rows[169578]=0 with legitimate entries at
     * cells_A[0..n]. The empty-bucket signal is the cells_A verification
     * key (0xFFFFFFFF) or simply the cell not matching uid_right -- not
     * the rows[] value. The README's "0 means no entry" was a partial
     * truth; only valid for rows[169579..] padding which the engine never
     * queries. */
    uint32_t row_offset = h->rows[uid_right];
    uint64_t idx = (uint64_t)row_offset + uid_left;
    if (idx >= h->n_cells) return SPFY_E_OOB;

    /* Verification key: cells_A[index] must equal uid_right (CORRECTED
     * 2026-05-05 -- was previously documented as uid_left). */
    if (h->cells_A[idx] != uid_right) return SPFY_E_OOB;
    *out_cost = h->cells_B[idx];
    return SPFY_OK;
}
