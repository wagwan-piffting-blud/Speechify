#ifndef SPFY_USEL_ANCHOR_SCORE_H
#define SPFY_USEL_ANCHOR_SCORE_H

#include <stddef.h>
#include <stdint.h>

#include "../voice/voice.h"
#include "../voice/ccos.h"
#include "../voice/unit_table.h"
#include "../voice/vcf_matrix.h"
#include "../voice/voice_runtime.h"
#include "../voice/chunk_table.h"
#include "../cart/cart.h"

/* Anchor pre_dp scorer (M3.4r Phase B4.4 / M5).
 *
 * Computes the engine-truth anchor cand pre_dp cost matching FUN_08e8ce60
 * + FUN_08e8adc0 + FUN_08e897b0 + FUN_08e89530 + FUN_08e893b0. Used at
 * Word/Syl tree-internal slots to score (ss, se) span cands against
 * target context.
 *
 * Reference: spfy/test/anchor_score/anchor_predp_v7.py (94.41% bit-exact
 * vs engine on the 30-text corpus). The C port aims for the same match
 * rate. Algorithm summary:
 *
 *   1. For each (ss, se) posting:
 *      - boundary check: hpclass_table[ss] == first_ctx[2] AND
 *                        hpclass_table[se] == last_ctx[2]
 *      - if pass: 4-cell ccos cost = sum of 4 ccos cells * w_44
 *   2. Phase 1 dynamic prune: drop cands with cost >= norm + best_so_far
 *   3. 50-bin histogram prune: bin = round((cost - best) * 50 / threshold)
 *      Scan bins, exit at k where (norm - cum_count*norm2) > k*0.1
 *      Final threshold = best + slack_at_exit
 *   4. For surviving cands: full pre_dp = ccos_4cell + FLAG-sum*w_3c*0.01
 *      + SP-span (5-matrix sum per unit) + D-span (POOLED Mahalanobis)
 *      + F0-span (per-unit Mahalanobis, voicing-gated by unit's mem+0x13)
 *
 * The hp_class for boundary check uses the engine's in-memory mem+0x13
 * (Frida-dumped to spfy/data/tom_hpclass.bin), NOT a disk-derived value
 * (units with identical disk fields can have different mem+0x13).
 */

/* Engine's slice ctx[5] encoding -- a 5-tuple of hp_class values (label*2 +
 * side, interleaved) for left2/left1/self/right1/right2. */
typedef struct {
    int32_t  ctx[5];
} spfy_anchor_ctx_t;

typedef struct {
    float  durt_mean, durt_var;
    int    durt_valid;
    float  f0tr_mean, f0tr_var;
    int    f0tr_valid;
} spfy_anchor_cart_t;

/* Per-HP target SP indices (5 matrix row indices, one per matrix). */
typedef struct {
    uint32_t  sp[5];
} spfy_anchor_sp_target_t;

/* Per-HP feature data for anchor-time per-unit durt walks (engine
 * FUN_08e89530 -> FUN_08e87d90 path). The engine walks durt with a
 * boundary-extended context that overrides q_type=8 with the anchor
 * type (2=Syl, 4=Word), producing different leaves than the preselect-
 * time walks for HPs whose durt tree branches on q8. Populating these
 * fields plus slot_input.durt_cart switches the D-cost branch from
 * pre-cached lookup to per-unit walk. NULL durt_cart falls back to
 * cart_per_hp[u_idx] preselect-cached values (the 2026-05-13 fix). */
typedef struct {
    int32_t  ctx[5];
    uint32_t sp[5];       /* 5-tuple SP indices (same q-type mapping as spfy_synth.c::cart_feat) */
    uint32_t q5;
    uint8_t  phone_label;
    uint8_t  durt_valid;
} spfy_anchor_hp_feat_t;

typedef struct {
    int32_t                          first_hp;
    int32_t                          last_hp;
    spfy_anchor_ctx_t                first_ctx;
    spfy_anchor_ctx_t                last_ctx;
    int                              anchor_type;
    const spfy_anchor_cart_t        *cart_per_hp;
    const spfy_anchor_sp_target_t   *sp_per_hp;
    /* Optional syl_idx array per HP (workspace+0x18) for advance-on-dup. */
    const int32_t                   *syl_idx_per_hp;
    /* UTTERANCE-WIDE SP targets, indexed by absolute half-phone -- not by
     * anchor-relative position like sp_per_hp. */
    const spfy_anchor_sp_target_t   *sp_all_hp;
    int32_t                          n_all_hp;
    /* UTTERANCE-WIDE durt forest index (phone label) per absolute
     * half-phone. */
    const uint8_t                   *phone_all_hp;
    /* Optional plumbing for anchor-time per-unit durt walks. */
    const spfy_cart_t               *durt_cart;
    const spfy_anchor_hp_feat_t     *hp_feat;
} spfy_anchor_slot_input_t;

typedef struct {
    const spfy_unit_table_t         *units;
    const spfy_ccos_t               *ccos;
    const spfy_voice_maps_t         *maps;
    const spfy_proscost_matrix_t    *proscost;
    /* Per-uid mem+0x13 hp_class (engine-truth from Frida dump). */
    const uint8_t                   *hpclass_table;
    uint32_t                         hpclass_n;
    /* feat-order phone index -> ccos labl index (the engine's voice+0x608
     * permutation, minus the side term). */
    const uint8_t                   *feat_to_labl;
    uint32_t                         n_feat_phones;
    /* Derived per-unit ccos context columns for v100005 voices (Paulina),
     * which ship no on-disk phone_ctx. */
    const uint8_t                   *ctx4;
    /* Per-hp_class voicing flag (voice+0x5fc). */
    const uint32_t                  *voicing;
    uint32_t                         voicing_n;
    float    w_sp[5];
    float    w_3c;
    float    w_flag_scale;   /* DAT_98580: 0.01 */
    float    w_ccos;
    float    w_dur;
    float    w_f0;
    float    w_f0_miss;
    float    anchor_norm;
    float    anchor_norm2;
    float    dat_971d8;
    float    dat_98a24;
    float    dat_98528;
    float    dat_8e9857c;
    /* --- SPFY_POW_TGT_W: per-phone-normalised ENERGY target cost ---------
     * NOT engine behaviour. */
    const float *unit_pow;
    uint32_t     unit_pow_n;
    const float *pow_mean;
    const float *pow_sd;
    uint32_t     pow_rows;
    float        pow_a;
    float        pow_b;
    float        w_pow_t;
} spfy_anchor_voice_t;

typedef struct {
    uint32_t  ss;
    uint32_t  se;
    uint32_t  posting_idx;
    float     pre_dp;
} spfy_anchor_cand_t;

/* Score one anchor slot. */
int spfy_anchor_score(const spfy_anchor_voice_t          *av,
                       const spfy_anchor_slot_input_t     *in,
                       const uint32_t                     *postings,
                       uint32_t                            n_postings,
                       const spfy_ckls_group_t            *ckls_grp,
                       spfy_anchor_cand_t                 *out_cands,
                       uint32_t                            out_cap,
                       uint32_t                           *out_n);

/* Load the engine-truth hp_class table from disk file. */
int spfy_anchor_hpclass_load(const char *path,
                              uint8_t **out_data, uint32_t *out_n);
void spfy_anchor_hpclass_free(uint8_t *data);

/* Build the derived ccos phone-context array for v100005 voices (the
 * voice+0xc4 equivalent). v100005 unit records carry no on-disk phone
 * context; the engine derives each unit's 4-cell context from recording
 * adjacency at load time (FUN_08e91c30) and remaps every neighbour's
 * hp_class through the feat->labl table at ccos time (FUN_08e8adc0). This
 * bakes the remap in so the ccos column code consumes av->ctx4 raw.
 *
 * Needs av->units, av->hpclass_table and av->ccos populated first. On a
 * v100005 voice sets av->ctx4 and *out_owned (caller frees). On any other
 * version it is a no-op: av->ctx4 = NULL, *out_owned = NULL, returns OK.
 * SPFY_NO_V100005_CTX4 in the environment forces the no-op (A/B). */
int spfy_anchor_build_ctx4(spfy_anchor_voice_t *av, uint8_t **out_owned);

void spfy_anchor_voice_set_default_weights(spfy_anchor_voice_t *av);

/* Same, then override from the voice's own VCF. */
void spfy_anchor_voice_set_weights_from_vcf(spfy_anchor_voice_t *av,
                                            const spfy_vcf_t *vcf);

/* Per-HP InnerScorer (FUN_08e88de0 = USelNetworkSlice::all_half_phone_costs).
 *
 * Computes per-cand cost for one HP slot:
 *   cost = SP_sum + FLAG + 4cell_ccos*w_44 + D_cost + F0_cost
 *
 * Where:
 *   SP_sum = sum_{k=0..4} matrix[k][sp_target[k]][unit_mem[+0xa+k]] * w_sp[k]
 *   FLAG   = unit_mem[+0x17] * w_38 * 0.01
 *   4cell  = sum_{slot=0..3} ccos[remap(ctx[2])][slot][s_ctx_remap(ctx[k])]
 *                                              [unit_mem[+0xc0+k]]
 *           where slot 0 row = ctx[0], slot 1 row = ctx[1],
 *                 slot 2 row = ctx[3], slot 3 row = ctx[4]
 *   D_cost = ((unit_mem[+0x12] - durt_mean) * durt_var)^2 * w_dur
 *   F0_cost: if voicing[ctx[2]] == 0 -> 0
 *            else if unit_mem[+0xf] == 0 -> w_f0_miss
 *            else ((unit_mem[+0xf] - f0tr_mean) * f0tr_var)^2 * w_f0
 *
 * Note FLAG weight is w_38 here (per-HP InnerScorer), same value 0.25 as
 * anchor's w_3c. We reuse av->w_3c for both.
 *
 * Returns SPFY_OK + writes total cost into *out_cost.
 * Returns NaN cost on missing data (ccos OOB etc).
 */
int spfy_hp_innerscorer(const spfy_anchor_voice_t       *av,
                         const spfy_anchor_ctx_t          *ctx,
                         const spfy_anchor_sp_target_t    *sp_target,
                         const spfy_anchor_cart_t         *cart,
                         uint32_t                          uid,
                         float                            *out_cost);

#endif
