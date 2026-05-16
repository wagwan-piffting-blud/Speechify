/* Anchor pre_dp scorer -- C port of anchor_predp_v7.py.
 *
 * Reproduces 100.00% bit-exact engine match on the 30-text corpus
 * (179/179 cands, 2026-05-06). Algorithm decoded from FUN_08e8ce60 +
 * FUN_08e8adc0 + sub-functions in SWIttsUSel.dll. The histogram-walk
 * semantic was RE'd from raw FPU disasm at 0x08e8b240..b46e: break when
 * `slack < bin_dist`, threshold = `best + bin_dist_at_break`.
 *
 * Note: this file implements the per-slot scorer. Two ORCHESTRATION
 * choices live in the caller (not here):
 *   - slot_grp must come from the engine anchor_type (Frida-captured),
 *     not from a _SYL_-first cklx lookup; the same (uid, jk) can appear
 *     in both _SYL_ and _WORD_ posting lists.
 *   - When the same (uid, jk) is the first_cand of BOTH a SYL and a
 *     WORD anchor, disambiguate via the matching anchor's
 *     final_n_cands == this Viterbi slot's len(cands).
 * See project_b44_anchor_gap.md.
 */

#include "anchor_score.h"
#include "../common/log.h"
#include "../../include/spfy/spfy.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Tom phone-pair swaps applied by the engine's voice+0x608 hp_class remap.
 * label 9 -> 10, 10 -> 11, 11 -> 9; 14 <-> 15. */
static uint32_t tom_swap_label(uint32_t label)
{
    switch (label) {
    case 9:  return 10;
    case 10: return 11;
    case 11: return 9;
    case 14: return 15;
    case 15: return 14;
    default: return label;
    }
}

/* s_ctx_remap(c, n): hp_class -> label (with Tom swap). Returns -1 on OOB. */
static int32_t s_ctx_remap(int32_t c, uint32_t n_labels)
{
    if (c < 0) return -1;
    uint32_t label = ((uint32_t)c) >> 1;
    if (label >= n_labels) return -1;
    return (int32_t)tom_swap_label(label);
}

/* hp_class_remap: hp_class (interleaved label*2+side) -> de-interleaved
 * (side*n_labels + swap(label)) used as ccos forest index. */
static int32_t hp_class_remap(int32_t c, uint32_t n_labels)
{
    if (c < 0) return -1;
    uint32_t side  = ((uint32_t)c) & 1u;
    uint32_t label = ((uint32_t)c) >> 1;
    if (label >= n_labels) return -1;
    return (int32_t)(side * n_labels + tom_swap_label(label));
}

/* Anchor-time CART feature callback. Mirrors spfy_synth.c::cart_feat
 * for q_types 1, 2, 3, 4, 5, 7, 9 but overrides q_type=8 with the
 * anchor_type (2=Syl, 4=Word) -- engine FUN_08e89530 walks durt with
 * boundary-extended ctx where q8 (wordInPhrase) is substituted by the
 * anchor type, producing different leaves for HPs whose durt branches
 * on q8 (empirically confirmed via cart_walks trace cross-referenced
 * against the durt_walk Frida capture for text_013 phone 22).
 *
 * NB: this is intentionally NOT shared with synth.c::cart_feat -- the
 * synth-side callback has no anchor context. The two functions stay in
 * sync via identical q_type mappings; if synth.c::cart_feat changes
 * its q_type table, this must follow. */
typedef struct {
    const spfy_anchor_hp_feat_t *hf;
    int                          anchor_type;
    /* 2026-05-14 evening: engine's anchor-time durt walks use per-unit
     * phone_ctx (from voice DB unit_table) for q3/q4 on interior HPs,
     * NOT slot's predicted ctx[1]/ctx[3]. Decoded via surgical Frida
     * capture on nat_048 ss=27944 (engine q3 sequence [5, 34, 36, 36,
     * 6, 6] vs slot_init q3 [5, 5, 36, 36, 6, 6] — only HP 1 differs,
     * and engine's HP 1 q3=34 = phone_ctx[1] of u=27945 direct).
     *
     * Rules:
     *   q3 at HP=0    : slot's predicted ctx[1] (= our pre-existing path,
     *                    matches slot_init q3 we already have bit-exact)
     *   q3 at HP=1..N-1: unit's phone_ctx[1] DIRECT (no tom_swap, no >>1)
     *   q4 at HP=N-1  : slot's predicted ctx[3] (slot_init q4)
     *   q4 at HP=0..N-2: unit's phone_ctx[2] DIRECT
     *
     * NULL cb_phone_ctx falls back to slot's predicted ctx for both
     * (preserves caller compatibility — non-anchor-time uses pass NULL).
     */
    const uint8_t               *cb_phone_ctx;  /* len 4 OR NULL */
    int                          is_first_hp;   /* HP at index 0 of anchor */
    int                          is_last_hp;    /* HP at index n_hp-1 */
} spfy_anchor_feat_user_t;

static int32_t anchor_cart_feat(uint32_t q_type, void *user)
{
    const spfy_anchor_feat_user_t *u = (const spfy_anchor_feat_user_t *)user;
    const spfy_anchor_hp_feat_t   *h = u->hf;
    switch (q_type) {
        case 1: return (int32_t)h->sp[1];
        case 2: return (int32_t)h->sp[0];
        case 3:
            if (u->cb_phone_ctx && !u->is_first_hp)
                return (int32_t)u->cb_phone_ctx[1];
            return (int32_t)tom_swap_label((uint32_t)h->ctx[1] >> 1);
        case 4:
            if (u->cb_phone_ctx && !u->is_last_hp)
                return (int32_t)u->cb_phone_ctx[2];
            return (int32_t)tom_swap_label((uint32_t)h->ctx[3] >> 1);
        case 5: return (int32_t)h->q5;
        /* q7 clamped to 0: engine's durt walker (FUN_08e87d90) executes
         * XOR EBX,EBX before each dispatcher call; q_type=7 reads EBX.
         * Anchor-time durt walks go through the same walker, so q7=0
         * applies here too. (Was sp[2], which produced wrong leaves at
         * q_type=7 nodes for nat_036 slots 7/10/12/16/18/20.) */
        case 7: return 0;
        case 8: return (int32_t)u->anchor_type;  /* OVERRIDE */
        case 9: return (int32_t)h->sp[4];
        default: return 0;
    }
}

/* Read a single ccos cell with signed col index (engine reads via MOVSX).
 * For col == -1, reads (row-1, n-1) per matrix-flat semantics. */
static float ccos_cell_signed(const spfy_ccos_t *ccos,
                               uint32_t hp, uint32_t slot,
                               int32_t row, int32_t col_signed)
{
    if (!ccos || !ccos->tables) return 0.0f;
    uint32_t n = ccos->n_labels;
    uint32_t matrix_floats = n * n;
    int64_t off = (int64_t)row * (int64_t)n + (int64_t)col_signed;
    if (off < 0 || off >= (int64_t)matrix_floats) return 0.0f;
    size_t base = (size_t)(hp * 4u + slot) * (size_t)matrix_floats;
    return ccos->tables[base + (size_t)off];
}

/* 4-cell ccos boundary cost. Returns NaN on OOB. */
static float compute_4cell_ccos(const spfy_anchor_voice_t *av,
                                 uint32_t ss, uint32_t se,
                                 const spfy_anchor_ctx_t *first_ctx,
                                 const spfy_anchor_ctx_t *last_ctx)
{
    uint32_t n_labels = av->ccos->n_labels;
    int32_t hp_first_remap = hp_class_remap(first_ctx->ctx[2], n_labels);
    int32_t hp_last_remap  = hp_class_remap(last_ctx->ctx[2],  n_labels);
    if (hp_first_remap < 0 || hp_last_remap < 0) return NAN;

    int32_t sl0 = s_ctx_remap(first_ctx->ctx[0], n_labels);
    int32_t sl1 = s_ctx_remap(first_ctx->ctx[1], n_labels);
    int32_t sl2 = s_ctx_remap(last_ctx->ctx[3],  n_labels);
    int32_t sl3 = s_ctx_remap(last_ctx->ctx[4],  n_labels);
    if (sl0 < 0 || sl1 < 0 || sl2 < 0 || sl3 < 0) return NAN;

    /* Cand col bytes: SS phone_ctx[0..1] for slots 0,1, SE phone_ctx[2..3]
     * for slots 2,3. Read SIGNED via MOVSX (255 sentinel = -1). */
    spfy_unit_record_t ss_rec, se_rec;
    if (spfy_unit_record_get(av->units, ss, &ss_rec) != SPFY_OK) return NAN;
    if (spfy_unit_record_get(av->units, se, &se_rec) != SPFY_OK) return NAN;
    int32_t pc_ss0 = (int32_t)(int8_t)ss_rec.phone_ctx[0];
    int32_t pc_ss1 = (int32_t)(int8_t)ss_rec.phone_ctx[1];
    int32_t pc_se2 = (int32_t)(int8_t)se_rec.phone_ctx[2];
    int32_t pc_se3 = (int32_t)(int8_t)se_rec.phone_ctx[3];

    float c0 = ccos_cell_signed(av->ccos, (uint32_t)hp_first_remap, 0, sl0, pc_ss0);
    float c1 = ccos_cell_signed(av->ccos, (uint32_t)hp_first_remap, 1, sl1, pc_ss1);
    float c2 = ccos_cell_signed(av->ccos, (uint32_t)hp_last_remap,  2, sl2, pc_se2);
    float c3 = ccos_cell_signed(av->ccos, (uint32_t)hp_last_remap,  3, sl3, pc_se3);
    return (c0 + c1 + c2 + c3) * av->w_ccos;
}

/* Internal cand record during scoring. */
typedef struct {
    float    cost4;       /* 4-cell ccos cost (or 1e9 if boundary mismatch) */
    uint32_t pid;
    uint32_t ss;
    uint32_t se;
} cand_buf_t;

/* Phase 1 + histogram prune. Compacts `cands` in place to accepted set;
 * writes accepted_n into *out_n. Returns the final cost threshold. */
static float histogram_prune(const spfy_anchor_voice_t *av,
                              cand_buf_t *cands, uint32_t n_cands,
                              uint32_t *out_n)
{
    float norm  = av->anchor_norm;
    float norm2 = av->anchor_norm2;

    /* Phase 1: dynamic threshold tightening. Drop cands w/ cost >= threshold. */
    float best = 10000.0f;
    float threshold_running = norm + 10000.0f;
    uint32_t accepted_n = 0;
    for (uint32_t i = 0; i < n_cands; ++i) {
        if (cands[i].cost4 < threshold_running) {
            if (cands[i].cost4 < best) {
                best = cands[i].cost4;
                threshold_running = norm + best;
            }
            cands[accepted_n++] = cands[i];
        }
    }

    if (accepted_n == 0) { *out_n = 0; return INFINITY; }

    /* Phase 2: 50-bin histogram. */
    int bins[50] = {0};
    float scale = (threshold_running > 0.0f)
                    ? (av->dat_98a24 / threshold_running) : 1.0f;
    for (uint32_t i = 0; i < accepted_n; ++i) {
        float diff = cands[i].cost4 - best;
        long bin_idx = lroundf(diff * scale);
        if (bin_idx >= 0 && bin_idx < 50) {
            bins[bin_idx]++;
        }
    }

    /* Phase 3: scan bins for early-exit threshold.
     *
     * Engine FUN_08e8adc0 histogram walk (asm 0x08e8b240..b46e): unrolled
     * 50-step loop. Step k uses cum = sum(bins[0..k-1]) and bin_dist = k*0.1.
     * Break when slack DROPS below bin_dist; threshold = best + bin_dist
     * AT_BREAK (NOT best + slack). FCOMPP+JZ at b26d jumps when bin_dist
     * > slack; after the jump, FLD best ; FADD ST0,ST1 produces best+bin_dist.
     *
     * Verified bit-exact against engine via the cost4 capture
     * (179/179 = 100.00% on the 30-text corpus).
     * See memory: project_b44_anchor_gap.md. */
    int cum = 0;
    float final_bin_dist = 50.0f * av->dat_971d8;     /* default if no break */
    for (int k = 1; k <= 50; ++k) {
        cum += bins[k - 1];
        float slack = norm - (float)cum * norm2;
        float bin_dist = (float)k * av->dat_971d8;
        if (slack < bin_dist) {
            final_bin_dist = bin_dist;
            break;
        }
    }

    float final_threshold = best + final_bin_dist;

    /* Phase 4: filter accepted by final_threshold. */
    uint32_t survive_n = 0;
    for (uint32_t i = 0; i < accepted_n; ++i) {
        if (cands[i].cost4 <= final_threshold) {
            cands[survive_n++] = cands[i];
        }
    }
    *out_n = survive_n;
    return final_threshold;
}

/* Full anchor cand cost = 4-cell + FLAG-sum + SP-span + D-span + F0-span.
 *
 * 2026-05-14: PROPAGATE the 10000.0 placeholder when hp_class boundary
 * doesn't match expected first_ctx[2]/last_ctx[2]. Engine sets cost4=10000
 * in phase 1 (line ~517) and the placeholder PROPAGATES to final TC for
 * survivors. Our previous version recomputed ccos_4cell from scratch and
 * lost the rejection — 361 of 2093 corpus anchors had eng_tc=10000 but
 * our small TC, letting our DP pick anchors engine rejected.
 * Returns NaN on input error.
 */
static float compute_anchor_full_cost(const spfy_anchor_voice_t *av,
                                       const spfy_anchor_slot_input_t *in,
                                       uint32_t ss, uint32_t se,
                                       float ccos_4cell_unused)
{
    (void)ccos_4cell_unused;
    /* Boundary hp_class check (engine-faithful: cost4=10000 placeholder
     * when ss/se's hp_class doesn't match the expected boundary). */
    {
        uint8_t expect_first_hpc = (uint8_t)in->first_ctx.ctx[2];
        uint8_t expect_last_hpc  = (uint8_t)in->last_ctx.ctx[2];
        uint8_t ss_hpc = (av->hpclass_table && ss < av->hpclass_n)
            ? av->hpclass_table[ss] : 0xff;
        uint8_t se_hpc = (av->hpclass_table && se < av->hpclass_n)
            ? av->hpclass_table[se] : 0xff;
        if (ss_hpc != expect_first_hpc || se_hpc != expect_last_hpc) {
            if (getenv("SPFY_ANCHOR_TC_DUMP_ALL")) {
                fprintf(stderr, "{\"anchor_tc\":1,\"ss\":%u,\"se\":%u,"
                                "\"anchor_type\":%d,\"n_hp\":-1,"
                                "\"total\":10000.0,\"rejected\":\"boundary\"}\n",
                        ss, se, in->anchor_type);
            }
            return 10000.0f;
        }
    }
    float ccos_4cell = compute_4cell_ccos(av, ss, se, &in->first_ctx,
                                           &in->last_ctx);
    if (isnan(ccos_4cell)) {
        if (getenv("SPFY_ANCHOR_TC_DUMP_ALL")) {
            fprintf(stderr, "{\"anchor_tc\":1,\"ss\":%u,\"se\":%u,"
                            "\"anchor_type\":%d,\"n_hp\":-1,"
                            "\"total\":10000.0,\"rejected\":\"ccos_nan\"}\n",
                    ss, se, in->anchor_type);
        }
        return 10000.0f;
    }
    uint32_t flag_sum = 0;
    float sp_cost = 0.0f;
    float d_delta_sum = 0.0f;
    float d_var_sum = 0.0f;
    float f0_cost = 0.0f;

    int32_t first_hp = in->first_hp;
    int32_t last_hp  = in->last_hp;
    int32_t n_hp     = last_hp - first_hp + 1;

    /* SP-byte_off mapping per FUN_08e897b0 disasm:
     *   matrix 0 col = unit_mem+0x0a -> disk +0x0c (sp_syl_in_phrase)
     *   matrix 1 col = unit_mem+0x0b -> disk +0x0d (sp_syl_type)
     *   matrix 2 col = unit_mem+0x0c -> disk +0x0e (sp_word_in_phrase byte;
     *                                                pairs with sylInWord matrix
     *                                                at runtime!)
     *   matrix 3 col = unit_mem+0x0d -> disk +0x0f (sp_syl_in_word byte;
     *                                                pairs with wordInPhrase matrix)
     *   matrix 4 col = unit_mem+0x0e -> hardcoded 6 for Tom (skipped: w[4]=0)
     *
     * NOTE: at runtime in voice memory, mat slot 2 is sylInWordCosts and
     * slot 3 is wordInPhraseCosts (swapped from VCF order). The proscost
     * loader (spfy_proscost_load) loads them in C-enum order:
     *   [0] SYL_IN_PHRASE, [1] SYL_TYPE, [2] WORD_IN_PHRASE, [3] SYL_IN_WORD,
     *   [4] PHONE_IN_SYL.
     * We map engine slot k -> proscost C-enum index: 0,1,3,2,4.
     */
    /* Engine k matches C proscost index directly: spfy_proscost_load loads
     * matrices in ENGINE order (KIND_NAME[2]="sylInWordCosts",
     * KIND_NAME[3]="wordInPhraseCosts"). Identity mapping. */
    static const uint8_t k_to_proscost[5] = { 0, 1, 2, 3, 4 };

    /* target_hp tracking. Mirrors engine FUN_08e89530's local_34/local_10
     * walk: starts at slot first_hp, on every non-first iter advances forward
     * through syl_idx_per_hp[] until the value differs from the previously
     * stored cur_syl (== engine's local_10). Engine's outer gate `byte != 0`
     * is degenerate (byte at disk+0x16 is constant 3 for Tom voice), so the
     * advance fires on EVERY non-first iter, NOT on `is_first_half == 1` as
     * a prior version of this code assumed. */
    int target_idx = 0;
    int32_t cur_syl = -1;
    if (in->syl_idx_per_hp && n_hp > 0) {
        cur_syl = in->syl_idx_per_hp[0];
    }

    for (uint32_t u = ss, u_idx = 0; u <= se; ++u, ++u_idx) {
        spfy_unit_record_t u_rec;
        if (spfy_unit_record_get(av->units, u, &u_rec) != SPFY_OK) {
            return NAN;
        }
        /* The engine tracks is_first_half via in-mem mem+0x15. We use
         * disk byte +0x15 here since the existing unit_record loader
         * exposes it. For advance-on-dup, reading prev-unit's byte. */
        /* Re-read prev unit's is_first_half to mirror Python (which uses
         * u_rec[0x15] of CURRENT unit at the start of each iteration --
         * but Python checks `u_rec[0x15] != 0` of the just-read unit at
         * iter start, which equals the current iter's is_first_half).
         *
         * Reviewing v7 line:
         *   if (syl_idx_array is not None and u_idx > 0 and
         *       u_rec[0x15] != 0 and target_idx < len(hp_seq) - 1):
         * It uses u_rec[0x15] of CURRENT unit (not previous).  Match that.
         */
        if (in->syl_idx_per_hp && u_idx > 0 && target_idx < n_hp - 1) {
            while (target_idx < n_hp - 1) {
                ++target_idx;
                int32_t ns = in->syl_idx_per_hp[target_idx];
                if (ns != cur_syl) {
                    cur_syl = ns;
                    break;
                }
            }
        }

        /* FLAG byte = unit context_cost (mem+0x17 = disk+0x1c). */
        flag_sum += u_rec.context_cost;

        /* SP-span: 5-matrix sum at target_hp. */
        if (target_idx >= 0 && target_idx < n_hp) {
            const spfy_anchor_sp_target_t *spt = &in->sp_per_hp[target_idx];
            uint8_t cand_bytes[5];
            cand_bytes[0] = u_rec.sp_syl_in_phrase;   /* disk 0x0c */
            cand_bytes[1] = u_rec.sp_syl_type;        /* disk 0x0d */
            cand_bytes[2] = u_rec.sp_word_in_phrase;  /* disk 0x0e */
            cand_bytes[3] = u_rec.sp_syl_in_word;     /* disk 0x0f */
            cand_bytes[4] = 6;                        /* Tom hardcoded */
            for (int k = 0; k < 5; ++k) {
                if (av->w_sp[k] == 0.0f) continue;
                uint8_t pc_idx = k_to_proscost[k];
                const spfy_proscost_matrix_t *m = &av->proscost[pc_idx];
                if (!m->data || m->n_rows == 0 || m->n_cols == 0) continue;
                uint32_t row = spt->sp[k];
                uint32_t col = cand_bytes[k];
                if (row >= m->n_rows || col >= m->n_cols) continue;
                sp_cost += m->data[row * m->n_cols + col] * av->w_sp[k];
            }
        }

        /* D-span: POOLED Mahalanobis.
         * D-byte = u_rec mem+0x12 -> disk+0x13 (f0_context).
         *
         * Two indexing modes:
         *   (A) Per-unit anchor-time walk when in->durt_cart && in->hp_feat
         *       are provided. Engine FUN_08e89530 -> FUN_08e87d90 walks
         *       durt per unit with q_type=8 overridden by anchor_type
         *       (= 2 for Syl, 4 for Word). Produces different leaves than
         *       preselect-time walks for HPs whose durt branches on q8.
         *       Verified vs Frida durt_walk capture on text_013.
         *   (B) Fallback: cart_per_hp[u_idx] pre-cached preselect-time
         *       walk. Closes most of the gap; missing the q8-override
         *       leaves a residual ~4% per-anchor D undercount.
         *
         * 2026-05-14: engine FUN_08e89530 decomp shows local_34 (target_idx)
         * is what's used for target_feat[] lookup, but EMPIRICALLY u_idx
         * (per-unit position) is correct: nat_033 ss=32414 (8-unit Syl)
         * D drops 4.86 -> 1.06 (engine 0.41); nat_040 0.57 -> 0.35
         * (engine 0.27); text_002 1.10 -> 0.87 (engine 1.48). +14 UIDs
         * corpus-wide (95.7% -> 95.9%). The decomp said target_idx, but
         * either (a) my decomp of the local_34 advance was wrong for Syl
         * anchors, or (b) engine's target_feat[i] for global HP i contains
         * per-HP feat data (not per-syl-advanced-position data) so when
         * local_34 advances to last_hp, target_feat[last_hp] gives the
         * SAME feat as target_feat[first_hp+u_idx] for u_idx=last. For
         * Syl anchors with all HPs in one syl, advance walks all the way,
         * so engine target_feat[local_34=last] for u_idx=1+ — but apparently
         * that produces SAME means as preselect per-slot, which is what u_idx
         * gets us via cart_per_hp[]. Leaving u_idx as the engine-equivalent
         * indexing scheme until further decomp clarifies. SPFY_D_IDX_TARGET=1
         * reverts to old target_idx behavior for A/B audit. */
        float durt_mean = 0.0f, durt_var = 0.0f;
        int   durt_ok = 0;
        int d_idx = getenv("SPFY_D_IDX_TARGET") ? target_idx : (int)u_idx;
        if (in->durt_cart && in->hp_feat
            && d_idx >= 0 && d_idx < n_hp
            && in->hp_feat[d_idx].durt_valid
            && !getenv("SPFY_NO_ANCHOR_DURT_WALK")) {
            /* q4 for NON-LAST HP = phone of unit AFTER the pair containing
             * this HP (walk forward until phone_center differs). For LAST
             * HP, we use slot's predicted ctx[3] (is_last_hp branch).
             *
             * For ss=27944 nat_048 anchor, HPs 0-3 share next-pair phone
             * with current unit's phone_ctx[2], but HP 4 (first HP of
             * anchor's LAST pair) needs voice-DB next-pair phone which
             * may differ from phone_ctx[2]'s prediction. Engine's q4 for
             * such HPs = actual voice-DB next-pair phone. */
            uint8_t next_pair_phone = u_rec.phone_ctx[2]; /* default fallback */
            if ((int)u_idx < n_hp - 1) {
                uint32_t scan = u + 1;
                uint32_t scan_max = u + 8;  /* phone pairs rarely > 4 units */
                if (av->units && scan < av->units->n_units) {
                    while (scan < scan_max && scan < av->units->n_units) {
                        spfy_unit_record_t sr;
                        if (spfy_unit_record_get(av->units, scan, &sr) != SPFY_OK)
                            break;
                        if (sr.phone_center != u_rec.phone_center) {
                            next_pair_phone = sr.phone_center;
                            break;
                        }
                        ++scan;
                    }
                }
            }
            uint8_t feat_pctx[4];
            feat_pctx[0] = u_rec.phone_ctx[0];
            feat_pctx[1] = u_rec.phone_ctx[1];
            feat_pctx[2] = next_pair_phone;
            feat_pctx[3] = u_rec.phone_ctx[3];
            spfy_anchor_feat_user_t fu = {
                .hf = &in->hp_feat[d_idx],
                .anchor_type = in->anchor_type,
                .cb_phone_ctx = getenv("SPFY_NO_UNIT_PHONE_CTX")
                                  ? NULL : feat_pctx,
                .is_first_hp = (u_idx == 0),
                .is_last_hp  = ((int)u_idx == n_hp - 1)
            };
            float am = 0.0f, av_var = 0.0f;
            /* 2026-05-14: engine FUN_08e89530 passes byte at unit_record
             * mem+0x14 = disk+0x14 = phone_center (current unit's phoneme
             * ID), NOT the target HP's phone_label. Engine's CART forest
             * is indexed by phone_id. Frida durt_walk hook's
             * "is_first_half" field is misnamed — values like 22, 32, 33,
             * 37 are phone IDs. */
            if (spfy_cart_traverse(in->durt_cart,
                                    (uint32_t)u_rec.phone_center,
                                    anchor_cart_feat, &fu,
                                    &am, &av_var) == SPFY_OK) {
                durt_mean = am;
                durt_var  = av_var;
                durt_ok   = 1;
            }
        }
        if (!durt_ok
            && d_idx >= 0 && d_idx < n_hp
            && in->cart_per_hp[d_idx].durt_valid) {
            durt_mean = in->cart_per_hp[d_idx].durt_mean;
            durt_var  = in->cart_per_hp[d_idx].durt_var;
            durt_ok   = 1;
        }
        if (durt_ok) {
            float delta = (float)u_rec.f0_context - durt_mean;
            d_delta_sum += delta;
            float inv_var = (durt_var != 0.0f)
                ? (av->dat_8e9857c / durt_var) : 0.0f;
            d_var_sum += inv_var * inv_var;
            if (getenv("SPFY_ANCHOR_D_TRACE")
                && getenv("SPFY_ANCHOR_COMP_UID")
                && (uint32_t)atoi(getenv("SPFY_ANCHOR_COMP_UID")) == ss) {
                fprintf(stderr, "    D u_idx=%u u=%u target_idx=%d "
                                "f0mid=%u f0c=%u "
                                "mean=%.4f var=%.6f inv_var=%.4f delta=%.4f\n",
                        u_idx, u, target_idx,
                        u_rec.f0_mid, u_rec.f0_context,
                        (double)durt_mean, (double)durt_var,
                        (double)inv_var, (double)delta);
            }
        }

        /* F0-span: per-unit Mahalanobis. Engine gate (Ghidra decomp of
         * FUN_08e893b0 @ 0x08e893b0):
         *
         *   voicing[unit_mem+0x13] != 0
         *
         * Engine's `unit_mem+0x13` is the hp_class byte (the engine
         * stores hp_class = 2*phone_id_alpha + side at this offset in
         * the in-memory unit record, where phone_id_alpha follows the
         * "name" feat chunk's alphabetical positional index). Our
         * hpclass_table[u] is bit-exactly that value (verified against
         * the 'name' feat positions for tom voice — 27944 hpclass=72
         * matches name index 72 = "t1", etc.).
         *
         * The second engine branch (weight+0x8c != 0 && f0_start != 0)
         * is RE'd but `weight+0x8c` is unmapped; empirical tests show
         * tom has it = 0 so the branch never fires. Drop it.
         *
         * Voicing[] is now built engine-faithfully via the 'name' feat
         * chunk's positional indexing (in spfy_synth.c). For tom this
         * happens to give identical values at every used index as the
         * legacy VCF-order build, but the new build is correct for any
         * voice where alphabetical-order phone listing differs from
         * VCF-declared order. */
        uint8_t unit_hpc = 0xff;
        if (av->hpclass_table && u < av->hpclass_n) {
            unit_hpc = av->hpclass_table[u];
        }
        int v_active = 1;
        if (av->voicing && unit_hpc < av->voicing_n) {
            v_active = (av->voicing[unit_hpc] != 0);
        }

        /* F0 indexed by u_idx (per-HP), not target_idx. Engine's
         * anchor_f0 walks (Frida surgical on nat_038 ss=66958) match
         * slot_init f0tr leaves per slot. SPFY_F0_IDX_TARGET=1 reverts. */
        int f0_idx = getenv("SPFY_F0_IDX_TARGET") ? target_idx : (int)u_idx;
        if (v_active && f0_idx >= 0 && f0_idx < n_hp
            && in->cart_per_hp[f0_idx].f0tr_valid) {
            const spfy_anchor_cart_t *ct = &in->cart_per_hp[f0_idx];
            uint8_t f0_b = u_rec.f0_start;   /* disk +0x10 = mem+0x0f */
            if (f0_b == 0) {
                f0_cost += av->w_f0_miss;
            } else {
                float delta = fabsf(((float)f0_b - ct->f0tr_mean) * ct->f0tr_var);
                f0_cost += delta * av->w_f0 * delta;
            }
        }
    }

    float flag_cost = (float)flag_sum * av->w_3c * av->w_flag_scale;
    float d_cost = 0.0f;
    if (d_var_sum > 0.0f) {
        d_cost = (d_delta_sum / d_var_sum) * d_delta_sum * av->w_dur;
    }
    if (getenv("SPFY_ANCHOR_COMP_UID")
        && (uint32_t)atoi(getenv("SPFY_ANCHOR_COMP_UID")) == ss) {
        fprintf(stderr, "  anchor ss=%u se=%u  total=%.4f "
                        "ccos4=%.4f flag=%.4f sp=%.4f d=%.4f f0=%.4f\n",
                ss, se, (double)(ccos_4cell + flag_cost + sp_cost + d_cost + f0_cost),
                (double)ccos_4cell, (double)flag_cost, (double)sp_cost,
                (double)d_cost, (double)f0_cost);
    }
    /* Corpus-wide anchor TC dump for per-anchor delta characterization
     * vs engine's master-capture `anchor_components.final_cands.tc`. */
    if (getenv("SPFY_ANCHOR_TC_DUMP_ALL")) {
        fprintf(stderr, "{\"anchor_tc\":1,\"ss\":%u,\"se\":%u,"
                        "\"anchor_type\":%d,\"n_hp\":%d,"
                        "\"total\":%.6f,\"ccos4\":%.6f,\"flag\":%.6f,"
                        "\"sp\":%.6f,\"d\":%.6f,\"f0\":%.6f}\n",
                ss, se, in->anchor_type, n_hp,
                (double)(ccos_4cell + flag_cost + sp_cost + d_cost + f0_cost),
                (double)ccos_4cell, (double)flag_cost, (double)sp_cost,
                (double)d_cost, (double)f0_cost);
    }
    return ccos_4cell + flag_cost + sp_cost + d_cost + f0_cost;
}

/* qsort comparator: sort by pre_dp ascending. */
static int cmp_anchor_cand(const void *a, const void *b)
{
    const spfy_anchor_cand_t *ca = (const spfy_anchor_cand_t *)a;
    const spfy_anchor_cand_t *cb = (const spfy_anchor_cand_t *)b;
    if (ca->pre_dp < cb->pre_dp) return -1;
    if (ca->pre_dp > cb->pre_dp) return 1;
    return 0;
}

int spfy_anchor_score(const spfy_anchor_voice_t          *av,
                       const spfy_anchor_slot_input_t     *in,
                       const uint32_t                     *postings,
                       uint32_t                            n_postings,
                       const spfy_ckls_group_t            *ckls_grp,
                       spfy_anchor_cand_t                 *out_cands,
                       uint32_t                            out_cap,
                       uint32_t                           *out_n)
{
    if (!av || !in || !postings || !ckls_grp || !out_cands || !out_n)
        return SPFY_E_INVAL;
    *out_n = 0;
    if (n_postings == 0 || out_cap == 0) return SPFY_OK;

    /* Phase 1 + 2 + 3: build cand cost array, prune by histogram. */
    cand_buf_t *buf = (cand_buf_t *)calloc(n_postings, sizeof *buf);
    if (!buf) return SPFY_E_NOMEM;

    uint8_t expect_first_hpc = (uint8_t)in->first_ctx.ctx[2];
    uint8_t expect_last_hpc  = (uint8_t)in->last_ctx.ctx[2];

    for (uint32_t i = 0; i < n_postings; ++i) {
        uint32_t pid = postings[i];
        if (pid >= ckls_grp->n_postings) {
            buf[i].cost4 = 1e9f; buf[i].pid = pid;
            buf[i].ss = 0; buf[i].se = 0;
            continue;
        }
        uint32_t ss = ckls_grp->span_start[pid];
        uint32_t se = ckls_grp->span_end[pid];
        buf[i].pid = pid;
        buf[i].ss = ss;
        buf[i].se = se;

        /* Boundary check via engine-truth hp_class table. */
        uint8_t ss_hpc = (av->hpclass_table && ss < av->hpclass_n)
            ? av->hpclass_table[ss] : 0xff;
        uint8_t se_hpc = (av->hpclass_table && se < av->hpclass_n)
            ? av->hpclass_table[se] : 0xff;
        if (ss_hpc != expect_first_hpc || se_hpc != expect_last_hpc) {
            buf[i].cost4 = 10000.0f;
            continue;
        }

        float c4 = compute_4cell_ccos(av, ss, se, &in->first_ctx, &in->last_ctx);
        if (isnan(c4)) {
            buf[i].cost4 = 10000.0f;
        } else {
            buf[i].cost4 = c4;
        }
    }

    uint32_t survived = 0;
    (void)histogram_prune(av, buf, n_postings, &survived);

    /* Phase 4 + 5: compute full pre_dp for survivors and rank. */
    uint32_t n_out = 0;
    for (uint32_t i = 0; i < survived && n_out < out_cap; ++i) {
        float pre_dp = compute_anchor_full_cost(av, in, buf[i].ss, buf[i].se,
                                                  buf[i].cost4);
        if (isnan(pre_dp)) continue;
        out_cands[n_out].ss          = buf[i].ss;
        out_cands[n_out].se          = buf[i].se;
        out_cands[n_out].posting_idx = buf[i].pid;
        out_cands[n_out].pre_dp      = pre_dp;
        ++n_out;
    }

    qsort(out_cands, n_out, sizeof *out_cands, cmp_anchor_cand);

    *out_n = n_out;
    free(buf);
    return SPFY_OK;
}

int spfy_anchor_hpclass_load(const char *path, uint8_t **out_data,
                              uint32_t *out_n)
{
    if (!path || !out_data || !out_n) return SPFY_E_INVAL;
    *out_data = NULL;
    *out_n = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) return SPFY_E_IO;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return SPFY_E_IO; }
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return SPFY_E_IO; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return SPFY_E_IO; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(fp); return SPFY_E_NOMEM; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf); fclose(fp); return SPFY_E_IO;
    }
    fclose(fp);
    *out_data = buf;
    *out_n = (uint32_t)sz;
    return SPFY_OK;
}

void spfy_anchor_hpclass_free(uint8_t *data) { free(data); }

int spfy_hp_innerscorer(const spfy_anchor_voice_t       *av,
                         const spfy_anchor_ctx_t          *ctx,
                         const spfy_anchor_sp_target_t    *sp_target,
                         const spfy_anchor_cart_t         *cart,
                         uint32_t                          uid,
                         float                            *out_cost)
{
    if (!av || !ctx || !sp_target || !out_cost) return SPFY_E_INVAL;
    *out_cost = NAN;

    spfy_unit_record_t u_rec;
    if (spfy_unit_record_get(av->units, uid, &u_rec) != SPFY_OK)
        return SPFY_E_OOB;

    uint32_t n_labels = av->ccos->n_labels;
    int32_t self_hpc = ctx->ctx[2];
    int32_t hp_remap = hp_class_remap(self_hpc, n_labels);
    if (hp_remap < 0) return SPFY_E_OOB;

    /* SP-sum: 5-matrix sum at this HP's sp_target. */
    /* Engine k matches C proscost index directly: spfy_proscost_load loads
     * matrices in ENGINE order (KIND_NAME[2]="sylInWordCosts",
     * KIND_NAME[3]="wordInPhraseCosts"). Identity mapping. */
    static const uint8_t k_to_proscost[5] = { 0, 1, 2, 3, 4 };
    float sp_cost = 0.0f;
    uint8_t cand_bytes[5];
    cand_bytes[0] = u_rec.sp_syl_in_phrase;
    cand_bytes[1] = u_rec.sp_syl_type;
    cand_bytes[2] = u_rec.sp_word_in_phrase;
    cand_bytes[3] = u_rec.sp_syl_in_word;
    cand_bytes[4] = 6;
    float sp_per_k[5] = {0};
    for (int k = 0; k < 5; ++k) {
        if (av->w_sp[k] == 0.0f) continue;
        uint8_t pc_idx = k_to_proscost[k];
        const spfy_proscost_matrix_t *m = &av->proscost[pc_idx];
        if (!m->data || m->n_rows == 0 || m->n_cols == 0) continue;
        uint32_t row = sp_target->sp[k];
        uint32_t col = cand_bytes[k];
        if (row >= m->n_rows || col >= m->n_cols) continue;
        sp_per_k[k] = m->data[row * m->n_cols + col] * av->w_sp[k];
        sp_cost += sp_per_k[k];
    }

    /* FLAG. weight at +0x38 in IS context = same value as +0x3c. */
    float flag_cost = (float)u_rec.context_cost * av->w_3c * av->w_flag_scale;

    /* 4-cell ccos: rows from ctx[0,1,3,4], cols from voice+0xc0[uid*4+k]
     * (signed bytes; sentinel 0xFF -> -1 wraps to previous row last col). */
    int32_t sl0 = s_ctx_remap(ctx->ctx[0], n_labels);
    int32_t sl1 = s_ctx_remap(ctx->ctx[1], n_labels);
    int32_t sl2 = s_ctx_remap(ctx->ctx[3], n_labels);
    int32_t sl3 = s_ctx_remap(ctx->ctx[4], n_labels);
    int32_t pc0 = (int32_t)(int8_t)u_rec.phone_ctx[0];
    int32_t pc1 = (int32_t)(int8_t)u_rec.phone_ctx[1];
    int32_t pc2 = (int32_t)(int8_t)u_rec.phone_ctx[2];
    int32_t pc3 = (int32_t)(int8_t)u_rec.phone_ctx[3];
    float ccos4 = 0.0f;
    if (sl0 >= 0)
        ccos4 += ccos_cell_signed(av->ccos, (uint32_t)hp_remap, 0, sl0, pc0);
    if (sl1 >= 0)
        ccos4 += ccos_cell_signed(av->ccos, (uint32_t)hp_remap, 1, sl1, pc1);
    if (sl2 >= 0)
        ccos4 += ccos_cell_signed(av->ccos, (uint32_t)hp_remap, 2, sl2, pc2);
    if (sl3 >= 0)
        ccos4 += ccos_cell_signed(av->ccos, (uint32_t)hp_remap, 3, sl3, pc3);
    ccos4 *= av->w_ccos;

    /* D-cost: per-unit Mahalanobis using THIS HP's durt prediction. */
    float d_cost = 0.0f;
    if (cart && cart->durt_valid) {
        float delta = ((float)u_rec.f0_context - cart->durt_mean) * cart->durt_var;
        d_cost = delta * delta * av->w_dur;
    }

    /* F0-cost: voicing gate via voicing[ctx[2]] (per-HP, NOT per-unit). */
    float f0_cost = 0.0f;
    int v_active = 1;
    if (av->voicing && (uint32_t)self_hpc < av->voicing_n) {
        v_active = (av->voicing[self_hpc] != 0);
    }
    if (v_active && cart && cart->f0tr_valid) {
        if (u_rec.f0_start == 0) {
            f0_cost = av->w_f0_miss;
        } else {
            float delta = ((float)u_rec.f0_start - cart->f0tr_mean) * cart->f0tr_var;
            f0_cost = delta * delta * av->w_f0;
        }
    }

    *out_cost = sp_cost + flag_cost + ccos4 + d_cost + f0_cost;

    /* SPFY_HP_COMP_DUMP — emit per-cand component costs for ALL cands
     * (vs SPFY_HP_COMP_UID which gates on a single uid for verbose
     * diagnostic). One JSON line per call. Compatible with engine's
     * inner_scorer.cand_components from master capture (which records
     * 4 components: D, F0, SP, CCOS — engine doesn't store FLAG; it
     * folds FLAG into total directly). Use:
     *   SPFY_HP_COMP_DUMP=1 spfy_synth ... 2> tc_components.jsonl */
    if (getenv("SPFY_HP_COMP_DUMP")) {
        fprintf(stderr,
            "{\"hp_comp\":1,\"uid\":%u,\"total\":%.6f,\"sp\":%.6f,"
            "\"flag\":%.6f,\"ccos\":%.6f,\"d\":%.6f,\"f0\":%.6f}\n",
            uid, (double)*out_cost, (double)sp_cost, (double)flag_cost,
            (double)ccos4, (double)d_cost, (double)f0_cost);
    }

    const char *fuid = getenv("SPFY_HP_COMP_UID");
    if (fuid && (uint32_t)atoi(fuid) == uid) {
        float c0_v = (sl0 >= 0)
            ? ccos_cell_signed(av->ccos, (uint32_t)hp_remap, 0, sl0, pc0) : 0.0f;
        float c1_v = (sl1 >= 0)
            ? ccos_cell_signed(av->ccos, (uint32_t)hp_remap, 1, sl1, pc1) : 0.0f;
        float c2_v = (sl2 >= 0)
            ? ccos_cell_signed(av->ccos, (uint32_t)hp_remap, 2, sl2, pc2) : 0.0f;
        float c3_v = (sl3 >= 0)
            ? ccos_cell_signed(av->ccos, (uint32_t)hp_remap, 3, sl3, pc3) : 0.0f;
        fprintf(stderr,
            "  IS uid=%u total=%.4f sp=%.4f flag=%.4f ccos4=%.4f"
            " d=%.4f f0=%.4f\n"
            "    sp_target=[%u,%u,%u,%u,%u] cand_bytes=[%u,%u,%u,%u,%u]\n"
            "    hp_remap=%d ctx=[%d,%d,%d,%d,%d]"
            " pc=[%d,%d,%d,%d] sl=[%d,%d,%d,%d]"
            " cells=[%.4f,%.4f,%.4f,%.4f]\n",
            uid, (double)*out_cost, (double)sp_cost, (double)flag_cost,
            (double)ccos4, (double)d_cost, (double)f0_cost,
            sp_target->sp[0], sp_target->sp[1], sp_target->sp[2],
            sp_target->sp[3], sp_target->sp[4],
            cand_bytes[0], cand_bytes[1], cand_bytes[2],
            cand_bytes[3], cand_bytes[4],
            (int)hp_remap, (int)ctx->ctx[0], (int)ctx->ctx[1],
            (int)ctx->ctx[2], (int)ctx->ctx[3], (int)ctx->ctx[4],
            (int)pc0, (int)pc1, (int)pc2, (int)pc3,
            (int)sl0, (int)sl1, (int)sl2, (int)sl3,
            (double)c0_v, (double)c1_v, (double)c2_v, (double)c3_v);
        fprintf(stderr, "    sp_per_k=[%.4f,%.4f,%.4f,%.4f,%.4f]"
            " mat[0][1][1]=%.4f mat[1][2][2]=%.4f"
            " mat[3][2][1]=%.4f mat[2][3][5]=%.4f\n",
            (double)sp_per_k[0], (double)sp_per_k[1], (double)sp_per_k[2],
            (double)sp_per_k[3], (double)sp_per_k[4],
            (av->proscost[0].data && 1 < av->proscost[0].n_rows && 1 < av->proscost[0].n_cols)
                ? (double)av->proscost[0].data[1*av->proscost[0].n_cols+1] : -999.0,
            (av->proscost[1].data && 2 < av->proscost[1].n_rows && 2 < av->proscost[1].n_cols)
                ? (double)av->proscost[1].data[2*av->proscost[1].n_cols+2] : -999.0,
            (av->proscost[3].data && 2 < av->proscost[3].n_rows && 1 < av->proscost[3].n_cols)
                ? (double)av->proscost[3].data[2*av->proscost[3].n_cols+1] : -999.0,
            (av->proscost[2].data && 3 < av->proscost[2].n_rows && 5 < av->proscost[2].n_cols)
                ? (double)av->proscost[2].data[3*av->proscost[2].n_cols+5] : -999.0);
    }
    return SPFY_OK;
}

void spfy_anchor_voice_set_default_weights(spfy_anchor_voice_t *av)
{
    if (!av) return;
    av->w_sp[0] = 0.05f; av->w_sp[1] = 0.05f;
    av->w_sp[2] = 0.05f; av->w_sp[3] = 0.05f;
    av->w_sp[4] = 0.0f;
    av->w_3c = 0.25f;
    av->w_flag_scale = 0.01f;
    av->w_ccos = 1.0f;
    av->w_dur = 0.30f;
    av->w_f0  = 0.20f;
    av->w_f0_miss = 5.0f;
    av->anchor_norm  = 0.7f;
    av->anchor_norm2 = 0.005f;
    av->dat_971d8 = 0.1f;
    av->dat_98a24 = 50.0f;
    av->dat_98528 = 10000.0f;
    av->dat_8e9857c = 1.0f;
}
