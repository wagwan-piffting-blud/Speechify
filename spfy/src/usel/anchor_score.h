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

/* Engine's slice ctx[5] encoding -- a 5-tuple of hp_class values
 * (label*2 + side, interleaved) for left2/left1/self/right1/right2. */
typedef struct {
    int32_t  ctx[5];
} spfy_anchor_ctx_t;

/* Per-HP CART leaf prediction (durt + f0tr). */
typedef struct {
    float  durt_mean, durt_var;
    int    durt_valid;
    float  f0tr_mean, f0tr_var;
    int    f0tr_valid;
} spfy_anchor_cart_t;

/* Per-HP target SP indices (5 matrix row indices, one per matrix).
 * Same order as the cost formula: [sp[0], sp[1], sp[2], sp[3], sp[4]]
 * = matrix-0-row, matrix-1-row, ..., matrix-4-row. */
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
    int32_t  ctx[5];      /* 5-tuple HP-class context (left2/L/self/R/right2) */
    uint32_t sp[5];       /* 5-tuple SP indices (same q-type mapping as
                           * spfy_synth.c::cart_feat) */
    uint32_t q5;          /* halfphones-in-current-syllable */
    uint8_t  phone_label; /* tom_swap'd phone label -- the durt tree_idx */
    uint8_t  durt_valid;  /* 0 = skip this HP (out of durt range, e.g. silence-pad) */
} spfy_anchor_hp_feat_t;

/* All inputs needed per anchor slot. */
typedef struct {
    /* span: HPs first_hp..last_hp (engine IS-slot indices). */
    int32_t                          first_hp;
    int32_t                          last_hp;
    /* ctx[5] for first_hp and last_hp (from prsl_slot capture / FE). */
    spfy_anchor_ctx_t                first_ctx;
    spfy_anchor_ctx_t                last_ctx;
    /* anchor_type: 2 = Syl, 4 = Word. Selects norm/norm2 weights. */
    int                              anchor_type;
    /* Per-HP arrays indexed [first_hp..last_hp]. n = last_hp-first_hp+1. */
    const spfy_anchor_cart_t        *cart_per_hp;     /* n entries */
    const spfy_anchor_sp_target_t   *sp_per_hp;       /* n entries */
    /* Optional syl_idx array per HP (workspace+0x18) for advance-on-dup.
     * Same n entries indexed [first_hp..last_hp]. NULL = 1:1 unit:hp. */
    const int32_t                   *syl_idx_per_hp;
    /* Optional plumbing for anchor-time per-unit durt walks. If durt_cart
     * is non-NULL AND hp_feat is non-NULL, the D-cost branch walks durt
     * per unit at scoring time using hp_feat[u_idx] with anchor_type
     * substituted for q_type=8. Otherwise falls back to pre-cached
     * cart_per_hp[u_idx]. */
    const spfy_cart_t               *durt_cart;
    const spfy_anchor_hp_feat_t     *hp_feat;        /* n entries */
} spfy_anchor_slot_input_t;

/* Voice-level static data (loaded once). */
typedef struct {
    const spfy_unit_table_t         *units;
    const spfy_ccos_t               *ccos;
    const spfy_voice_maps_t         *maps;
    const spfy_proscost_matrix_t    *proscost; /* 5 matrices */
    /* Per-uid mem+0x13 hp_class (engine-truth from Frida dump).
     * 169579 bytes for Tom; one byte per unit. */
    const uint8_t                   *hpclass_table;
    uint32_t                         hpclass_n;
    /* Per-hp_class voicing flag (voice+0x5fc). 2*n_labels entries.
     * 0 = voiceless (skip f0tr), nonzero = walk f0tr. */
    const uint32_t                  *voicing;
    uint32_t                         voicing_n;
    /* Cost weights, captured via Frida anchor_score_hook. */
    float    w_sp[5];        /* [0.05, 0.05, 0.05, 0.05, 0.0] */
    float    w_3c;           /* anchor FLAG scaler: 0.25 */
    float    w_flag_scale;   /* DAT_98580: 0.01 */
    float    w_ccos;         /* w_44: 1.0 */
    float    w_dur;          /* w_34: 0.30 */
    float    w_f0;           /* w_24: 0.20 */
    float    w_f0_miss;      /* w_80: 5.0 */
    float    anchor_norm;    /* 0.7 (w_54 syl, w_5c word) */
    float    anchor_norm2;   /* 0.005 (w_58 / w_60) */
    float    dat_971d8;      /* 0.1 (histogram bin width) */
    float    dat_98a24;      /* 50.0 (histogram norm scaler) */
    float    dat_98528;      /* 10000.0 (initial threshold seed) */
    float    dat_8e9857c;    /* 1.0 (emphasis const, used in inv_var calc) */
} spfy_anchor_voice_t;

/* Output cand record. */
typedef struct {
    uint32_t  ss;
    uint32_t  se;
    uint32_t  posting_idx;
    float     pre_dp;        /* full anchor cand cost */
} spfy_anchor_cand_t;

/* Score one anchor slot.
 *
 * Inputs:
 *   av           voice-level static data
 *   in           per-slot input (first_hp/last_hp/ctx/cart/sp_target)
 *   postings     cklx posting indices for the slot key
 *   ckls_grp     ckls group containing span data (ss, se per posting)
 *   n_postings   number of postings
 *
 * Outputs (caller-owned):
 *   out_cands    surviving cands, ranked by pre_dp ascending
 *   out_cap      capacity of out_cands array
 *   out_n        number of surviving cands written
 *
 * Returns SPFY_OK or SPFY_E_*.
 */
int spfy_anchor_score(const spfy_anchor_voice_t          *av,
                       const spfy_anchor_slot_input_t     *in,
                       const uint32_t                     *postings,
                       uint32_t                            n_postings,
                       const spfy_ckls_group_t            *ckls_grp,
                       spfy_anchor_cand_t                 *out_cands,
                       uint32_t                            out_cap,
                       uint32_t                           *out_n);

/* Load the engine-truth hp_class table from disk file. The file is a
 * raw byte array of size n_units. Caller frees with spfy_anchor_hpclass_free. */
int spfy_anchor_hpclass_load(const char *path,
                              uint8_t **out_data, uint32_t *out_n);
void spfy_anchor_hpclass_free(uint8_t *data);

/* Initialize default cost weights from Frida-captured Tom values. */
void spfy_anchor_voice_set_default_weights(spfy_anchor_voice_t *av);

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
