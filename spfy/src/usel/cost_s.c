/* S (context / ccos) cost component. */

#include "cost.h"

#include <stddef.h>

#define SPFY_CCOS_N_SLOTS 4

float spfy_cost_s(const float   *ccos,
                  uint32_t       n_labels,
                  uint32_t       hp_class,
                  const uint8_t *L,
                  uint32_t       n_phones,
                  const uint8_t  target_ctx[4],
                  const uint8_t  cand_ctx[4],
                  float          ccos_weight)
{
    if (!ccos || !L || n_labels == 0) return 0.0f;

    const size_t per_matrix = (size_t)n_labels * n_labels;

    long double sum = 0.0L;
    for (int s = 0; s < SPFY_CCOS_N_SLOTS; ++s) {
        uint8_t t_phone = target_ctx[s];
        uint8_t c_phone = cand_ctx[s];
        /* TARGET sentinel (>= n_phones, e.g. 255 = no context) skips.
         * The engine reads target through a remap table which doesn't
         * go past 93 in valid use; with halfphone-class targets we never
         * hit 255 here in practice. Keep the guard for safety. */
        if (t_phone >= n_phones) continue;
        uint32_t ti = L[t_phone];
        if (ti >= n_labels) continue;

        /* CAND byte: engine uses MOVSX (signed sign-extend) — confirmed
         * via disasm at 0x08e891a8/0x08e891ad/0x08e891c1/0x08e891ca for
         * voice+0xc0 path, and 0x08e891e4/0x08e891ec/0x08e8920a/0x08e89215
         * for voice+0xc4 path. So 0xff is read as -1, NOT 255. With
         * stride n_labels and ti > 0, signed -1 wraps into row (ti-1)
         * col (n_labels-1) of the same matrix, which is in-bounds. This
         * matters for silence-boundary cands (uid 0 has ctx[0..1]=255). */
        int32_t ci_signed = (int32_t)(int8_t)c_phone;
        const float *mat = ccos +
            (size_t)(hp_class * SPFY_CCOS_N_SLOTS + (uint32_t)s) * per_matrix;
        sum += (long double)mat[(intptr_t)ti * (intptr_t)n_labels + ci_signed];
    }
    return (float)(sum * (long double)ccos_weight);
}
