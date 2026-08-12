#ifndef SPFY_USEL_COST_H
#define SPFY_USEL_COST_H

#include <stdint.h>

/* Per-target-per-candidate scoring components. */

/* Compute D cost for a single (target halfphone, candidate unit) pair. */
float spfy_cost_d(uint32_t stored_dur,
                  float durt_pred_mean,
                  float durt_pred_scale,
                  float dur_weight);

/* Compute F0 cost for a single (target halfphone, candidate unit) pair. */
float spfy_cost_f0(uint32_t stored_f0,
                   float f0_pred_mean,
                   float f0_pred_scale,
                   float abs_f0_weight,
                   float missing_f0_cost);

/* SP (position-mismatch) cost. */

typedef struct {
    const float *data;
    uint32_t     n_rows;
    uint32_t     n_cols;
} spfy_sp_matrix_t;

float spfy_cost_sp(const spfy_sp_matrix_t matrices[5],
                   const uint32_t target_feats[5],
                   const uint32_t cand_bytes[5],
                   const float    weights[5]);

/* S (context / ccos) cost. */
float spfy_cost_s(const float   *ccos,
                  uint32_t       n_labels,
                  uint32_t       hp_class,
                  const uint8_t *L,
                  uint32_t       n_phones,
                  const uint8_t  target_ctx[4],
                  const uint8_t  cand_ctx[4],
                  float          ccos_weight);

#endif
