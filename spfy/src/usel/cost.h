#ifndef SPFY_USEL_COST_H
#define SPFY_USEL_COST_H

#include <stdint.h>

/* Per-target-per-candidate scoring components.
 *
 * Each cost function returns f32 to match what the engine stores and
 * compares. Internally, intermediate accumulators are `long double` to
 * preserve x87 80-bit semantics from MSVC 7.1 (see plan: bit-exact FP
 * strategy). The final cast to float happens once at return.
 *
 * Formulas (per reveng/DLL_ANALYSIS.md "Duration scoring" / "F0 scoring"):
 *
 *   D-cost:
 *     score = | scale * (stored_dur - durt_pred_mean) |^2 * DUR_WEIGHT
 *
 *   F0-cost:
 *     if stored_f0 == 0:    score = MISSING_F0_COST
 *     else:                 score = | scale * (stored_f0 - f0_pred_mean) |^2
 *                                 * ABS_F0_WEIGHT
 *
 * The "scale" is the durt/f0tr leaf's variance field -- which the engine
 * uses directly as 1/stddev (despite the file calling it "variance"; see
 * cart_load.c). Validated empirically: leaf "variance" values for Tom are
 * O(0.1) which is plausible for 1/stddev of duration/F0 distributions.
 *
 * Default cost constants from VCF (Tom):
 *   DUR_WEIGHT      = 0.3
 *   ABS_F0_WEIGHT   = 0.2
 *   MISSING_F0_COST = 1000.0   (stock VCF default; configurable)
 */

/* Compute D cost for a single (target halfphone, candidate unit) pair.
 *   stored_dur    : candidate unit's dur_like field (in 8-byte ms units)
 *   durt_pred_mean: durt CART tree leaf mean for the target
 *   durt_pred_scale: durt CART tree leaf variance/scale for the target
 *   dur_weight    : DUR_WEIGHT from VCF
 */
float spfy_cost_d(uint32_t stored_dur,
                  float durt_pred_mean,
                  float durt_pred_scale,
                  float dur_weight);

/* Compute F0 cost for a single (target halfphone, candidate unit) pair.
 *   stored_f0     : candidate unit's f0_start (v100006) or f0_end (v100007+)
 *   f0_pred_mean  : f0tr CART tree leaf mean
 *   f0_pred_scale : f0tr CART tree leaf variance/scale
 *   abs_f0_weight : ABS_F0_WEIGHT from VCF
 *   missing_f0_cost: MISSING_F0_COST from VCF (when stored_f0 == 0) */
float spfy_cost_f0(uint32_t stored_f0,
                   float f0_pred_mean,
                   float f0_pred_scale,
                   float abs_f0_weight,
                   float missing_f0_cost);

/* SP (position-mismatch) cost.
 *
 *   SP = sum over k=0..4:  weight[k] * matrix[k][target[k]][cand[k]]
 *
 * 5 matrices (loaded from VCF proscost), 5 per-target features, 5 per-
 * candidate column-index bytes. The matrix views are passed as flat row-
 * major f32 arrays with explicit stride (in f32 elements), so callers
 * can store them however they like.
 *
 * Per-table column source for v100006 candidates:
 *   k=0 sylInPhraseCosts    cand[0] = unit.sp_syl_in_phrase  (disk +0x0C)
 *   k=1 sylTypeCosts        cand[1] = unit.sp_syl_type       (disk +0x0D)
 *   k=2 wordInPhraseCosts   cand[2] = unit.sp_word_in_phrase (disk +0x0E)
 *   k=3 sylInWordCosts      cand[3] = unit.sp_syl_in_word    (disk +0x0F)
 *   k=4 phoneInSylCosts     cand[4] = 6 (Tom-hardcoded; other voices
 *                                          read disk +0x14 via the
 *                                          chunky-index path)
 *
 * Per-target features come from the FE-produced halfphone target
 * (M2.5 capture).
 *
 * NOTE on the Tom phoneInSyl special case: the loader hard-codes cand[4]=6
 * for unit_version 0x186a6 (Tom). Pass cand[4]=6 unconditionally on Tom;
 * for v0x186a5/0x186a7 voices read it from the disk byte. */

typedef struct {
    const float *data;     /* row-major, owned by caller */
    uint32_t     n_rows;
    uint32_t     n_cols;
} spfy_sp_matrix_t;

float spfy_cost_sp(const spfy_sp_matrix_t matrices[5],
                   const uint32_t target_feats[5],
                   const uint32_t cand_bytes[5],
                   const float    weights[5]);

/* S (context / ccos) cost.
 *
 *   S = ccos_weight * sum over slot=0..3:
 *           ccos[hp_class * 4 + slot][L[target.ctx[s]]][L[cand.ctx[s]]]
 *
 * where:
 *   ccos        : flat row-major float buffer of all (hp_class, slot) matrices,
 *                 each n_labels x n_labels (matches spfy_ccos_t.tables layout)
 *   n_labels    : matrix dimension (47 for Tom)
 *   hp_class    : target halfphone class index (0..2*n_labels-1)
 *   L           : flat array mapping phone_id -> label_id
 *   target_ctx  : 4 phone_ids of target's pp2/pp1/pn1/pn2 context
 *   cand_ctx    : 4 phone_ids of candidate's pp2/pp1/pn1/pn2 context (from
 *                 unit_record.phone_ctx[0..3])
 *   ccos_weight : VCF CONTEXT_COST_WEIGHT (Tom default 1.0)
 *
 * Out-of-bounds phone_ids (255 = sentinel for "no context") are silently
 * skipped (term contributes 0), matching the engine's handling of recording-
 * boundary contexts. */
float spfy_cost_s(const float   *ccos,
                  uint32_t       n_labels,
                  uint32_t       hp_class,
                  const uint8_t *L,
                  uint32_t       n_phones,
                  const uint8_t  target_ctx[4],
                  const uint8_t  cand_ctx[4],
                  float          ccos_weight);

#endif
