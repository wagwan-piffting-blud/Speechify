/* SP (position-mismatch) cost component.
 *
 *   SP = sum over k=0..4:  weight[k] * matrix[k][target[k]][cand[k]]
 *
 * Per-target features and per-candidate column bytes are out-of-bounds-
 * checked: any out-of-range index contributes 0 to the term (matching the
 * engine's behaviour where invalid feature values fall through to a
 * default empty cell). This keeps callers from having to validate the
 * 5-tuples before each call. */

#include "cost.h"

#include <stddef.h>

float spfy_cost_sp(const spfy_sp_matrix_t matrices[5],
                   const uint32_t target_feats[5],
                   const uint32_t cand_bytes[5],
                   const float    weights[5])
{
    long double total = 0.0L;
    for (int k = 0; k < 5; ++k) {
        const spfy_sp_matrix_t *m = &matrices[k];
        if (!m->data || m->n_rows == 0 || m->n_cols == 0) continue;

        uint32_t r = target_feats[k];
        uint32_t c = cand_bytes[k];
        if (r >= m->n_rows || c >= m->n_cols) continue;

        long double cell = (long double)m->data[r * m->n_cols + c];
        long double term = (long double)weights[k] * cell;
        total += term;
    }
    return (float)total;
}
