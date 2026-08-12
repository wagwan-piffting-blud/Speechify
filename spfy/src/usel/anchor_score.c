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
 */

#include "anchor_score.h"
#include "env.h"
#include "../common/log.h"
#include "../../include/spfy/spfy.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* feat-order phone -> ccos labl index, i.e. */
static uint32_t phone_feat_to_labl(const spfy_anchor_voice_t *av,
                                   uint32_t phone)
{
    if (!av || !av->feat_to_labl || phone >= av->n_feat_phones) return phone;
    uint8_t lab = av->feat_to_labl[phone];
    return (lab == 0xFFu) ? phone : lab;
}

static int32_t s_ctx_remap(const spfy_anchor_voice_t *av,
                           int32_t c, uint32_t n_labels)
{
    if (c < 0) return -1;
    uint32_t label = ((uint32_t)c) >> 1;
    if (label >= n_labels) return -1;
    return (int32_t)phone_feat_to_labl(av, label);
}

/* hp_class_remap: hp_class (interleaved phone*2+side) -> de-interleaved
 * (side*n_labels + labl(phone)) used as ccos forest index. */
static int32_t hp_class_remap(const spfy_anchor_voice_t *av,
                              int32_t c, uint32_t n_labels)
{
    if (c < 0) return -1;
    uint32_t side  = ((uint32_t)c) & 1u;
    uint32_t label = ((uint32_t)c) >> 1;
    if (label >= n_labels) return -1;
    return (int32_t)(side * n_labels + phone_feat_to_labl(av, label));
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
     * phone_ctx (from voice DB unit_table) for q3/q4 on interior HPs, NOT
     * slot's predicted ctx[1]/ctx[3]. */
    const uint8_t               *cb_phone_ctx;
    int                          is_first_hp;
    int                          is_last_hp;
    const spfy_anchor_voice_t   *av;
    /* q8/q9 come from sp[3]/sp[4] one phone OUTSIDE the anchor -- see the
     * comment at the assignment site. */
    int32_t                      q8_val;
    int32_t                      q9_val;
    /* q3/q4 are the phone ids of the units TWO positions away in the voice
     * DB (= one phone, since units are half-phones), read directly rather
     * than taken from the current unit's stored phone_ctx. */
    uint32_t                     cur_u;
    uint32_t                     ss_u;
    uint32_t                     se_u;
    const uint8_t               *phone_all;
    int32_t                      n_all;
    int32_t                      first_hp_g;
    int32_t                      last_hp_g;
} spfy_anchor_feat_user_t;

static int32_t anchor_cart_feat(uint32_t q_type, void *user)
{
    const spfy_anchor_feat_user_t *u = (const spfy_anchor_feat_user_t *)user;
    const spfy_anchor_hp_feat_t   *h = u->hf;
    switch (q_type) {
        case 1: return (int32_t)h->sp[1];
        case 2: return (int32_t)h->sp[0];
        /* FUN_08e89530 reads byte +0x14 of unit[i-2] for q3 and unit[i+2] for
         * q4 -- the unit two positions back/forward in the voice DB, i.e. one
         * phone. At the anchor's own first/last unit it falls back to the
         * slot's predicted context instead. Measured on paulina es_014: the
         * old phone_ctx[1] route agreed with the engine on only 121 of 586
         * anchor walks. SPFY_NO_UNIT_PHONE_CTX keeps the old behaviour. */
        case 3:
            if (u->cb_phone_ctx && u->cur_u != u->ss_u && u->cur_u >= 2) {
                spfy_unit_record_t r;
                if (u->av->units
                    && spfy_unit_record_get(u->av->units, u->cur_u - 2, &r)
                       == SPFY_OK)
                    return (int32_t)r.phone_center;
            }
            /* anchor's own FIRST unit: the engine takes the external branch
             * and reads the utterance's phone label two half-phones before
             * the anchor. */
            if (u->phone_all && u->first_hp_g >= 2
                && u->first_hp_g - 2 < u->n_all)
                return (int32_t)u->phone_all[u->first_hp_g - 2];
            return (int32_t)phone_feat_to_labl(u->av,
                                               (uint32_t)h->ctx[1] >> 1);
        case 4:
            if (u->cb_phone_ctx && u->cur_u != u->se_u) {
                spfy_unit_record_t r;
                if (u->av->units
                    && u->cur_u + 2 < u->av->units->n_units
                    && spfy_unit_record_get(u->av->units, u->cur_u + 2, &r)
                       == SPFY_OK)
                    return (int32_t)r.phone_center;
            }
            /* anchor's own LAST unit: same external branch, two half-phones
             * AFTER the anchor. */
            if (u->phone_all && u->last_hp_g + 2 >= 0
                && u->last_hp_g + 2 < u->n_all)
                return (int32_t)u->phone_all[u->last_hp_g + 2];
            return (int32_t)phone_feat_to_labl(u->av,
                                               (uint32_t)h->ctx[3] >> 1);
        case 5: return (int32_t)h->q5;
        /* q7 clamped to 0: engine's durt walker (FUN_08e87d90) executes
         * XOR EBX,EBX before each dispatcher call; q_type=7 reads EBX.
         * Anchor-time durt walks go through the same walker, so q7=0
         * applies here too. (Was sp[2], which produced wrong leaves at
         * q_type=7 nodes for nat_036 slots 7/10/12/16/18/20.) */
        case 7: return 0;
        /* q8 = sp[3] (wordInPhrase) read one phone OUTSIDE the anchor. */
        case 8: {
            static int q8_override = -2;
            if (q8_override == -2) {
                const char *e = spfy_env("SPFY_ANCHOR_Q8");
                q8_override = (e && *e) ? atoi(e) : -2;
            }
            if (q8_override != -2) return (int32_t)q8_override;
            if (u->q8_val >= 0) return u->q8_val;
            return (int32_t)u->anchor_type;
        }
        case 9:
            if (u->q9_val >= 0) return u->q9_val;
            return (int32_t)h->sp[4];
        default: return 0;
    }
}

/* Read a single ccos cell with signed col index (engine reads via MOVSX). */
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

static float compute_4cell_ccos(const spfy_anchor_voice_t *av,
                                 uint32_t ss, uint32_t se,
                                 const spfy_anchor_ctx_t *first_ctx,
                                 const spfy_anchor_ctx_t *last_ctx)
{
    uint32_t n_labels = av->ccos->n_labels;
    int32_t hp_first_remap = hp_class_remap(av, first_ctx->ctx[2], n_labels);
    int32_t hp_last_remap  = hp_class_remap(av, last_ctx->ctx[2],  n_labels);
    if (hp_first_remap < 0 || hp_last_remap < 0) return NAN;

    int32_t sl0 = s_ctx_remap(av, first_ctx->ctx[0], n_labels);
    int32_t sl1 = s_ctx_remap(av, first_ctx->ctx[1], n_labels);
    int32_t sl2 = s_ctx_remap(av, last_ctx->ctx[3],  n_labels);
    int32_t sl3 = s_ctx_remap(av, last_ctx->ctx[4],  n_labels);
    if (sl0 < 0 || sl1 < 0 || sl2 < 0 || sl3 < 0) return NAN;

    /* Cand col bytes: SS phone_ctx[0..1] for slots 0,1, SE phone_ctx[2..3]
     * for slots 2,3. */
    spfy_unit_record_t ss_rec, se_rec;
    if (spfy_unit_record_get(av->units, ss, &ss_rec) != SPFY_OK) return NAN;
    if (spfy_unit_record_get(av->units, se, &se_rec) != SPFY_OK) return NAN;
    int32_t pc_ss0, pc_ss1, pc_se2, pc_se3;
    if (av->ctx4) {
        /* v100005: derived, pre-remapped ccos-labl columns (voice+0xc4). */
        pc_ss0 = (int32_t)(int8_t)av->ctx4[ss * 4u + 0u];
        pc_ss1 = (int32_t)(int8_t)av->ctx4[ss * 4u + 1u];
        pc_se2 = (int32_t)(int8_t)av->ctx4[se * 4u + 2u];
        pc_se3 = (int32_t)(int8_t)av->ctx4[se * 4u + 3u];
    } else {
        pc_ss0 = (int32_t)(int8_t)ss_rec.phone_ctx[0];
        pc_ss1 = (int32_t)(int8_t)ss_rec.phone_ctx[1];
        pc_se2 = (int32_t)(int8_t)se_rec.phone_ctx[2];
        pc_se3 = (int32_t)(int8_t)se_rec.phone_ctx[3];
    }

    float c0 = ccos_cell_signed(av->ccos, (uint32_t)hp_first_remap, 0, sl0, pc_ss0);
    float c1 = ccos_cell_signed(av->ccos, (uint32_t)hp_first_remap, 1, sl1, pc_ss1);
    float c2 = ccos_cell_signed(av->ccos, (uint32_t)hp_last_remap,  2, sl2, pc_se2);
    float c3 = ccos_cell_signed(av->ccos, (uint32_t)hp_last_remap,  3, sl3, pc_se3);
    return (c0 + c1 + c2 + c3) * av->w_ccos;
}

typedef struct {
    float    cost4;
    uint32_t pid;
    uint32_t ss;
    uint32_t se;
} cand_buf_t;

/* Phase 1 + histogram prune. */
static float histogram_prune(const spfy_anchor_voice_t *av,
                              cand_buf_t *cands, uint32_t n_cands,
                              uint32_t *out_n)
{
    float norm  = av->anchor_norm;
    float norm2 = av->anchor_norm2;

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

    /* Phase 2: 50-bin histogram.
     *
     * 2026-08-10, read off the instructions at 0x08e8b1b6..0x08e8b1f4 rather
     * than the decompile (Ghidra folds the conversion into an argument-less
     * `FUN_08e9504c()` and loses the x87 operand):
     *
     *     fld   [0x08e98a24]      ; 50.0
     *     fdiv  <norm>            ; scale = 50.0 / norm
     *     ...
     *     fld   [ebp + esi*4]     ; cost4[i]
     *     fsub  <best>            ; - best
     *     fmul  <scale>
     *     call  0x08e9504c        ; _ftol -> TRUNCATES toward zero
     *     cmp   eax, 0x32 / jge   ; only the upper bound is checked
     *
     * Two corrections against the old port, both invisible when best ~= 0
     * (which is why this measured 179/179 on tom and still lost candidates
     * on paulina, where best is not small):
     *   - the divisor is `norm`, NOT `norm + best` (the running threshold)
     *   - the conversion truncates; lroundf() rounds to nearest, so every
     *     bucket whose fraction is >= .5 landed one bin too high
     *
     * SPFY_ANCHOR_PRUNE_LEGACY=1 restores the old behaviour for A/B. */
    int bins[50] = {0};
    static int prune_legacy = -1;
    if (prune_legacy < 0)
        prune_legacy = (spfy_env("SPFY_ANCHOR_PRUNE_LEGACY") != NULL);
    float scale_den = prune_legacy ? threshold_running : norm;
    float scale = (scale_den > 0.0f) ? (av->dat_98a24 / scale_den) : 1.0f;
    for (uint32_t i = 0; i < accepted_n; ++i) {
        float diff = cands[i].cost4 - best;
        long bin_idx = prune_legacy ? lroundf(diff * scale)
                                    : (long)(diff * scale);
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
     * (179/179 = 100.00% on the 30-text corpus). */
    int cum = 0;
    float final_bin_dist = 50.0f * av->dat_971d8;
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

    uint32_t survive_n = 0;
    for (uint32_t i = 0; i < accepted_n; ++i) {
        if (cands[i].cost4 <= final_threshold) {
            cands[survive_n++] = cands[i];
        }
    }
    *out_n = survive_n;
    return final_threshold;
}

/* Full anchor cand cost = 4-cell + FLAG-sum + SP-span + D-span + F0-span. */
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
            if (spfy_env("SPFY_ANCHOR_TC_DUMP_ALL")) {
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
        if (spfy_env("SPFY_ANCHOR_TC_DUMP_ALL")) {
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
     * KIND_NAME[3]="wordInPhraseCosts"). */
    static const uint8_t k_to_proscost[5] = { 0, 1, 2, 3, 4 };

    /* target_hp tracking. Mirrors engine FUN_08e89530 / FUN_08e897b0's
     * local_34+local_10 (D) / iVar10+local_24 (SP) walk: start at first_hp,
     * and on a gated iteration advance forward through syl_idx_per_hp[]
     * until the value differs from the stored cur_syl.
     *
     * ⚠ THE GATE IS `is_first_half`, AND IT IS NOT DEGENERATE.
     *
     * Both engine functions guard the advance with
     *     iVar9 != first_unit && *(char *)(unit_rec + 0x15) != 0 && tgt < last
     * where +0x15 is an IN-MEMORY offset. A previous reading mapped that to
     * DISK +0x16 -- a per-voice constant (3 for Tom) -- concluded the gate
     * was always true, and dropped it, advancing on EVERY non-first unit.
     *
     * The loader (FUN_08e857a0) says otherwise. It reads the post-prefix
     * byte block and scatters it: byte[7] -> mem 0x12, byte[8] -> mem 0x14,
     * byte[9] -> mem 0x15. With the 12-byte fixed prefix that makes
     * mem 0x15 == DISK 0x15 == is_first_half. (Cross-checks: byte[7] ->
     * mem 0x12 == disk 0x13 == f0_context, which is the D byte we already
     * use; and mem 0x0e defaults to 6 when phone_in_syl is absent, which is
     * the SP cand_bytes[4] fallback we already implement.)
     *
     * Cost of getting this wrong: advancing every unit runs the target index
     * off the end of a WIDE anchor and pins it at n_hp-1. On edge_042's
     * 14-unit / 16-HP "etcetera" Word anchor the walk went 0,4,8,12 then
     * saturated at 15 for the last ten units, so they were all scored
     * against the final HP's CART target. D came out 1.1817 against the
     * engine's 0.0064 and SP 1.8650 against 0.3097, and the anchor lost by
     * 3.81 to a path the engine never takes.
     *
     * SPFY_ANCHOR_NO_FH_GATE=1 restores the always-advance behaviour. */
    static int no_fh_gate = -1;
    if (no_fh_gate < 0)
        no_fh_gate = (spfy_env("SPFY_ANCHOR_NO_FH_GATE") != NULL);
    int target_idx = 0;
    int32_t cur_syl = -1;
    if (in->syl_idx_per_hp && n_hp > 0) {
        cur_syl = in->syl_idx_per_hp[0];
    }

    /* D-cost target index, the engine's `local_34` in FUN_08e89530.
     *
     * The decompile is explicit: local_34 starts at first_hp and advances ONLY
     * when the unit is not the first AND unit+0x15 (is_first_half) is set AND
     * local_34 < last_hp; the advance then walks forward to the next index
     * whose entry in the per-half-phone array at net+0x18 DIFFERS, capped at
     * last_hp. Every unit's durt walk is then indexed by local_34, and the
     * walk is unconditional -- there is no `d_idx < n_hp` guard.
     *
     * `target_idx` above groups by SYLLABLE, which cannot be that array: in a
     * Syl anchor every half-phone shares one syllable, so the advance runs
     * straight to the end and pins there -- precisely the "advanced on every
     * unit and pinned at n_hp-1" failure recorded below. The array has to
     * change at PHONE boundaries for the advance to land on the next phone's
     * left half, so the group is the segment ordinal (hp >> 1) and the advance
     * is +2, clamped.
     *
     * Kept separate from `target_idx` because F0 uses that one and F0 is
     * already exact against the engine; folding them together would break a
     * term that currently matches.
     *
     * ⚠ OFF BY DEFAULT, and that is a measurement, not caution. The clamp is
     * real -- it takes our anchor walk count to the engine's 666 exactly,
     * from 654 -- but the 12 walks it adds are performed with FEATURES we
     * still compute wrongly, and doing them is worse than skipping them:
     * tom 221 -> 215, javier 85 -> 83, paulina 74 -> 73, felix 100 -> 99.
     * The grouping is not the missing piece either: `none`, `step1` and
     * `segment` all score IDENTICALLY against the engine's walk capture
     * (multiset distance 432), so the target index barely moves the leaf.
     * Whatever is wrong lives in the question features, not the index.
     * SPFY_ANCHOR_D_TGT_DECOMP=1 enables this path for further work. */
    int d_tgt = 0;
    static int d_tgt_legacy = -1;
    if (d_tgt_legacy < 0)
        d_tgt_legacy = (spfy_env("SPFY_ANCHOR_D_TGT_DECOMP") == NULL);

    for (uint32_t u = ss, u_idx = 0; u <= se; ++u, ++u_idx) {
        spfy_unit_record_t u_rec;
        if (spfy_unit_record_get(av->units, u, &u_rec) != SPFY_OK) {
            return NAN;
        }
        /* The engine tracks is_first_half via in-mem mem+0x15. */
        /* Re-read prev unit's is_first_half to mirror Python (which uses
         * u_rec[0x15] of CURRENT unit at the start of each iteration -- but
         * Python checks `u_rec[0x15] != 0` of the just-read unit at iter
         * start, which equals the... */
        if (in->syl_idx_per_hp && u_idx > 0 && target_idx < n_hp - 1
            && (no_fh_gate || u_rec.is_first_half != 0)) {
            while (target_idx < n_hp - 1) {
                ++target_idx;
                int32_t ns = in->syl_idx_per_hp[target_idx];
                if (ns != cur_syl) {
                    cur_syl = ns;
                    break;
                }
            }
        }

        /* The advance gate and the clamp are read straight off the decompile. */
        if (u_idx > 0 && d_tgt < n_hp - 1 && u_rec.is_first_half != 0) {
            static int grp = -1;
            if (grp < 0) {
                const char *e = spfy_env("SPFY_ANCHOR_D_GROUP");
                grp = (e && *e) ? atoi(e) : 2;
            }
            if (grp == 1) {
                ++d_tgt;
            } else if (grp == 2) {
                int seg = d_tgt >> 1;
                while (d_tgt < n_hp - 1 && (d_tgt >> 1) == seg) ++d_tgt;
            } else if (grp == 3 && in->syl_idx_per_hp) {
                int32_t s0 = in->syl_idx_per_hp[d_tgt];
                while (d_tgt < n_hp - 1
                       && in->syl_idx_per_hp[d_tgt + 1] == s0) ++d_tgt;
                if (d_tgt < n_hp - 1) ++d_tgt;
            }
        }

        flag_sum += u_rec.context_cost;

        if (target_idx >= 0 && target_idx < n_hp) {
            const spfy_anchor_sp_target_t *spt = &in->sp_per_hp[target_idx];
            uint8_t cand_bytes[5];
            cand_bytes[0] = u_rec.sp_syl_in_phrase;
            cand_bytes[1] = u_rec.sp_syl_type;
            cand_bytes[2] = u_rec.sp_word_in_phrase;
            cand_bytes[3] = u_rec.sp_syl_in_word;
            /* v100008 (Jill) stores this at disk 0x10; older record
             * versions have no column and the decoder yields 6. */
            cand_bytes[4] = u_rec.sp_phone_in_syl;
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
        /* q8/q9 are sp[3]/sp[4] at `iVar11` in FUN_08e89530, which the compiler
         * assigns ONLY inside the two boundary branches. So it holds
         * first_hp-2 for every unit except the last, where it becomes
         * last_hp+2 -- one phone OUTSIDE the anchor either way, landing on the
         * utterance's silence padding at the edges (measured: first_hp never
         * below 2, last_hp never above n_hp-3, so the reads stay in range).
         *
         * Measured, not inferred: hooking FUN_08e87d90 at entry for the full
         * 7-arg question tuple and FUN_08e89530 at entry for the net arrays
         * gives 666/666 agreement on BOTH q8 and q9 over every anchor walk of
         * paulina es_014, 0 mismatches. The rule can fail and does not --
         * pinning the index instead drops q1/q2 to 634/666, and all 137 anchor
         * candidates do take a different index on their last unit.
         *
         * The old q8 = anchor_type was fitted: the engine puts q8 outside
         * {2,4} on 175 of those 666 walks. */
        int32_t q8_val = -1, q9_val = -1;
        int     tq3 = -1, tq4 = -1;
        if (in->sp_all_hp && in->n_all_hp > 0) {
            int32_t oi = (u == se) ? in->last_hp + 2 : in->first_hp - 2;
            if (oi < 0) oi = 0;
            if (oi >= in->n_all_hp) oi = in->n_all_hp - 1;
            q8_val = (int32_t)in->sp_all_hp[oi].sp[3];
            q9_val = (int32_t)in->sp_all_hp[oi].sp[4];
        }
        /* ⭐ target_idx, per FUN_08e89530 (local_34). This was u_idx while the
         * walk itself was broken (it advanced on every unit and pinned at
         * n_hp-1, so target_idx was garbage and u_idx beat it empirically).
         * With the is_first_half gate restored the walk is correct and the
         * engine's own index wins: on edge_042's Word anchor D drops
         * 1.1817 -> 0.0941 against the engine's 0.0064.
         * SPFY_D_IDX_UIDX=1 restores the old per-unit indexing. */
        int d_idx = d_tgt_legacy
                    ? (spfy_env("SPFY_D_IDX_UIDX") ? (int)u_idx : target_idx)
                    : d_tgt;
        /* ⚠ WORD anchors only, and this split is EMPIRICAL -- I cannot derive
         * it from the decompile. FUN_08e89530 uses local_34 for every anchor
         * type, and for a Syl anchor (all HPs in one syllable) the walk
         * saturates at n_hp-1 on both sides, so the engine should be using
         * target_feat[last] there too. Yet applying target_idx to Syl anchors
         * costs 5 texts (235 -> 229 emitted-unit matches) while restricting it
         * to Word anchors gives 235/235.
         *
         * ⚠ And even for the Word anchor our D is NOT exact: 0.0941 against
         * the engine's 0.0064. It is close enough to flip the decision, not
         * close enough to call the formula understood -- the residual is
         * somewhere in the durt lookup (the FUN_08e87d90 walk with the
         * q_type=8 anchor_type override), not in the pooling, which matches
         * the decompile exactly. SP and F0 both land EXACT after the same
         * index change, so D is the odd one out.
         *
         * Treat this line as a known-incomplete port, not a settled rule.
         * SPFY_D_IDX_ALL_TYPES=1 applies target_idx to every anchor type. */
        /* The per-anchor-type split below was empirical and is not in the
         * decompile: FUN_08e89530 uses local_34 for EVERY anchor type. It only
         * looked necessary because the syllable-grouped index saturated for Syl
         * anchors. With the segment-grouped index it no longer applies. */
        if (d_tgt_legacy
            && !spfy_env("SPFY_D_IDX_ALL_TYPES") && in->anchor_type != 4)
            d_idx = (int)u_idx;
        /* The engine never drops a unit: local_34 is clamped at last_hp, so
         * a candidate whose unit count exceeds its half-phone count keeps
         * walking with the saturated index. */
        if (d_idx >= n_hp && n_hp > 0
            && !spfy_env("SPFY_ANCHOR_NO_D_CLAMP"))
            d_idx = n_hp - 1;
        if (in->durt_cart && in->hp_feat
            && d_idx >= 0 && d_idx < n_hp
            && in->hp_feat[d_idx].durt_valid
            && !spfy_env("SPFY_NO_ANCHOR_DURT_WALK")) {
            /* q4 for NON-LAST HP = phone of unit AFTER the pair containing
             * this HP (walk forward until phone_center differs). */
            uint8_t next_pair_phone = u_rec.phone_ctx[2];
            if ((int)u_idx < n_hp - 1) {
                uint32_t scan = u + 1;
                uint32_t scan_max = u + 8;
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
                .cb_phone_ctx = spfy_env("SPFY_NO_UNIT_PHONE_CTX")
                                  ? NULL : feat_pctx,
                .is_first_hp = (u_idx == 0),
                .is_last_hp  = ((int)u_idx == n_hp - 1),
                .av = av,
                .q8_val = q8_val,
                .q9_val = q9_val,
                .cur_u = u,
                .ss_u = ss,
                .se_u = se,
                .phone_all = in->phone_all_hp,
                .n_all = in->n_all_hp,
                .first_hp_g = in->first_hp,
                .last_hp_g = in->last_hp
            };
            if (spfy_env("SPFY_ANCHOR_D_TRACE")) {
                /* the production callback itself, so the trace cannot drift
                 * from what the walk actually asked for */
                tq3 = (int)anchor_cart_feat(3, &fu);
                tq4 = (int)anchor_cart_feat(4, &fu);
            }
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
            /* SPFY_ANCHOR_COMP_UID narrows this to one candidate; with only
             * SPFY_ANCHOR_D_TRACE set it dumps every walk, in call order,
             * so the sequence can be aligned against the engine's durt_walk
             * capture (which records EDX = the... */
            if (spfy_env("SPFY_ANCHOR_D_TRACE")
                && (!spfy_env("SPFY_ANCHOR_COMP_UID")
                    || (uint32_t)atoi(spfy_env("SPFY_ANCHOR_COMP_UID")) == ss)) {
                /* d_idx and the question tuple are dumped too: the engine's
                 * FUN_08e89530-entry capture records first_unit, which IS ss,
                 * so engine walks key to ours on (ss, u_idx) exactly and every
                 * question can be diffed one at a time. */
                int tq1 = -1, tq2 = -1, tq5 = -1;
                if (d_idx >= 0 && d_idx < n_hp && in->hp_feat) {
                    tq1 = (int)in->hp_feat[d_idx].sp[1];
                    tq2 = (int)in->hp_feat[d_idx].sp[0];
                    tq5 = (int)in->hp_feat[d_idx].q5;
                }
                fprintf(stderr, "    D ss=%u fhp=%d lhp=%d atype=%d "
                                "u_idx=%u u=%u target_idx=%d "
                                "d_idx=%d phone=%u f0mid=%u f0c=%u "
                                "q1=%d q2=%d q3=%d q4=%d q5=%d q8=%d q9=%d "
                                "mean=%.4f var=%.6f inv_var=%.4f delta=%.4f\n",
                        ss, in->first_hp, in->last_hp, in->anchor_type,
                        u_idx, u, target_idx, d_idx,
                        (unsigned)u_rec.phone_center,
                        u_rec.f0_mid, u_rec.f0_context,
                        tq1, tq2, tq3, tq4, tq5, q8_val, q9_val,
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

        /* ⭐ target_idx, same story as D above: the earlier u_idx default was
         * chosen while the walk was broken. On edge_042's Word anchor this
         * takes f0 from 6.5366 to 5.7052 -- EXACT against the engine.
         * SPFY_F0_IDX_UIDX=1 restores the old per-unit indexing. */
        int f0_idx = spfy_env("SPFY_F0_IDX_UIDX") ? (int)u_idx : target_idx;
        if (v_active && f0_idx >= 0 && f0_idx < n_hp
            && in->cart_per_hp[f0_idx].f0tr_valid) {
            const spfy_anchor_cart_t *ct = &in->cart_per_hp[f0_idx];
            uint8_t f0_b = u_rec.f0_start;
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
    if (spfy_env("SPFY_ANCHOR_COMP_UID")
        && (uint32_t)atoi(spfy_env("SPFY_ANCHOR_COMP_UID")) == ss) {
        fprintf(stderr, "  anchor ss=%u se=%u  total=%.4f "
                        "ccos4=%.4f flag=%.4f sp=%.4f d=%.4f f0=%.4f\n",
                ss, se, (double)(ccos_4cell + flag_cost + sp_cost + d_cost + f0_cost),
                (double)ccos_4cell, (double)flag_cost, (double)sp_cost,
                (double)d_cost, (double)f0_cost);
    }
    /* Corpus-wide anchor TC dump for per-anchor delta characterization vs
     * engine's master-capture `anchor_components.final_cands.tc`. */
    if (spfy_env("SPFY_ANCHOR_TC_DUMP_ALL")) {
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

/* See header. Mirrors engine FUN_08e91c30 (per-unit derivation) with the
 * FUN_08e8adc0 v100005 ccos-column remap (voice+0x604) baked in via
 * s_ctx_remap -- which is the SAME table the ccos ROW path already uses
 * bit-exactly, so the column reuse is correct by construction. The neighbour
 * layout is [left2, left1, right1, right2] = uid-4, uid-2, uid+2, uid+4
 * (half-phone stride 2); a low/recording-boundary left edge snaps to unit 0,
 * a high/recording-boundary right edge snaps to unit N-2. */
int spfy_anchor_build_ctx4(spfy_anchor_voice_t *av, uint8_t **out_owned)
{
    if (out_owned) *out_owned = NULL;
    if (!av || !av->units || !av->hpclass_table || !av->ccos)
        return SPFY_E_INVAL;
    av->ctx4 = NULL;
    if (av->units->version != 100005u)   return SPFY_OK;
    if (spfy_env("SPFY_NO_V100005_CTX4"))  return SPFY_OK;

    uint32_t n        = av->units->n_units;
    uint32_t n_labels = av->ccos->n_labels;
    if (n == 0 || n_labels == 0)         return SPFY_OK;

    /* Cache file_idx once so the neighbour walk is O(1) per cell. */
    uint16_t *file_idx = (uint16_t *)malloc((size_t)n * sizeof *file_idx);
    uint8_t  *ctx4     = (uint8_t  *)malloc((size_t)n * 4u);
    if (!file_idx || !ctx4) { free(file_idx); free(ctx4); return SPFY_E_NOMEM; }
    for (uint32_t u = 0; u < n; ++u) {
        spfy_unit_record_t r;
        if (spfy_unit_record_get(av->units, u, &r) != SPFY_OK) {
            free(file_idx); free(ctx4); return SPFY_E_FORMAT;
        }
        file_idx[u] = r.file_idx;
    }

    static const int loff[2] = { 4, 2 };
    static const int roff[2] = { 2, 4 };
    for (uint32_t uid = 0; uid < n; ++uid) {
        uint16_t self = file_idx[uid];
        int32_t  nb[4];
        for (int k = 0; k < 2; ++k) {
            int32_t li = (int32_t)uid - loff[k];
            if (li < 0 || file_idx[li] != self) li = 0;
            nb[k] = li;
        }
        for (int k = 0; k < 2; ++k) {
            int32_t ri = (int32_t)uid + roff[k];
            if (ri >= (int32_t)n || file_idx[ri] != self) ri = (int32_t)n - 2;
            nb[2 + k] = ri;
        }
        for (int k = 0; k < 4; ++k) {
            uint8_t hpc = ((uint32_t)nb[k] < av->hpclass_n)
                          ? av->hpclass_table[nb[k]] : 0u;
            int32_t labl = s_ctx_remap(av, (int32_t)hpc, n_labels);
            ctx4[uid * 4u + (uint32_t)k] = (uint8_t)(labl & 0xff);
        }
    }

    free(file_idx);
    av->ctx4 = ctx4;
    if (out_owned) *out_owned = ctx4;
    return SPFY_OK;
}

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
    int32_t hp_remap = hp_class_remap(av, self_hpc, n_labels);
    if (hp_remap < 0) return SPFY_E_OOB;

    /* Engine k matches C proscost index directly: spfy_proscost_load loads
     * matrices in ENGINE order (KIND_NAME[2]="sylInWordCosts",
     * KIND_NAME[3]="wordInPhraseCosts"). */
    static const uint8_t k_to_proscost[5] = { 0, 1, 2, 3, 4 };
    float sp_cost = 0.0f;
    uint8_t cand_bytes[5];
    cand_bytes[0] = u_rec.sp_syl_in_phrase;
    cand_bytes[1] = u_rec.sp_syl_type;
    cand_bytes[2] = u_rec.sp_word_in_phrase;
    cand_bytes[3] = u_rec.sp_syl_in_word;
    cand_bytes[4] = u_rec.sp_phone_in_syl;
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

    float flag_cost = (float)u_rec.context_cost * av->w_3c * av->w_flag_scale;

    /* 4-cell ccos: rows from ctx[0,1,3,4], cols from voice+0xc0[uid*4+k]
     * (signed bytes; sentinel 0xFF -> -1 wraps to previous row last col). */
    int32_t sl0 = s_ctx_remap(av, ctx->ctx[0], n_labels);
    int32_t sl1 = s_ctx_remap(av, ctx->ctx[1], n_labels);
    int32_t sl2 = s_ctx_remap(av, ctx->ctx[3], n_labels);
    int32_t sl3 = s_ctx_remap(av, ctx->ctx[4], n_labels);
    int32_t pc0, pc1, pc2, pc3;
    if (av->ctx4) {
        pc0 = (int32_t)(int8_t)av->ctx4[uid * 4u + 0u];
        pc1 = (int32_t)(int8_t)av->ctx4[uid * 4u + 1u];
        pc2 = (int32_t)(int8_t)av->ctx4[uid * 4u + 2u];
        pc3 = (int32_t)(int8_t)av->ctx4[uid * 4u + 3u];
    } else {
        pc0 = (int32_t)(int8_t)u_rec.phone_ctx[0];
        pc1 = (int32_t)(int8_t)u_rec.phone_ctx[1];
        pc2 = (int32_t)(int8_t)u_rec.phone_ctx[2];
        pc3 = (int32_t)(int8_t)u_rec.phone_ctx[3];
    }
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

    float d_cost = 0.0f;
    if (cart && cart->durt_valid) {
        float delta = ((float)u_rec.f0_context - cart->durt_mean) * cart->durt_var;
        d_cost = delta * delta * av->w_dur;
    }

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

    /* Energy target cost (SPFY_POW_TGT_W). */
    float pow_cost = 0.0f;
    if (av->w_pow_t > 0.0f && av->unit_pow && av->pow_mean && av->pow_sd
        && uid < av->unit_pow_n) {
        uint32_t row = (uint32_t)u_rec.phone_center * 2u
                     + (u_rec.is_first_half ? 1u : 0u);
        float p = av->unit_pow[uid];
        if (row < av->pow_rows && p > 0.0f) {
            float sd = av->pow_sd[row];
            if (sd > 1e-6f) {
                float z = ((av->pow_a * p + av->pow_b) - av->pow_mean[row])
                          / sd;
                pow_cost = z * z * av->w_pow_t;
            }
        }
    }

    *out_cost = sp_cost + flag_cost + ccos4 + d_cost + f0_cost + pow_cost;

    /* SPFY_HP_COMP_DUMP — emit per-cand component costs for ALL cands (vs
     * SPFY_HP_COMP_UID which gates on a single uid for verbose diagnostic). */
    if (spfy_env("SPFY_HP_COMP_DUMP")) {
        fprintf(stderr,
            "{\"hp_comp\":1,\"uid\":%u,\"total\":%.6f,\"sp\":%.6f,"
            "\"flag\":%.6f,\"ccos\":%.6f,\"d\":%.6f,\"f0\":%.6f}\n",
            uid, (double)*out_cost, (double)sp_cost, (double)flag_cost,
            (double)ccos4, (double)d_cost, (double)f0_cost);
    }

    const char *fuid = spfy_env("SPFY_HP_COMP_UID");
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
    /* Energy target cost OFF. */
    av->unit_pow = NULL;
    av->unit_pow_n = 0u;
    av->pow_mean = NULL;
    av->pow_sd = NULL;
    av->pow_rows = 0u;
    av->pow_a = 1.0f;
    av->pow_b = 0.0f;
    av->w_pow_t = 0.0f;
}

void spfy_anchor_voice_set_weights_from_vcf(spfy_anchor_voice_t *av,
                                            const spfy_vcf_t *vcf)
{
    if (!av) return;
    /* Seed with the engine's built-in defaults, then let the VCF override. */
    spfy_anchor_voice_set_default_weights(av);
    if (!vcf) return;

    /* Slots 2 and 3 are SWAPPED relative to the obvious reading of the VCF
     * key names, matching the matrix order in vcf_matrix.c (KIND_NAME[2] =
     * sylInWordCosts, KIND_NAME[3] = wordInPhraseCosts). */
    av->w_sp[0] = spfy_vcf_f32(vcf, "PHRASE_POS_MISMATCH_COST",    av->w_sp[0]);
    av->w_sp[1] = spfy_vcf_f32(vcf, "STRESS_MISMATCH_COST",        av->w_sp[1]);
    av->w_sp[2] = spfy_vcf_f32_alias(vcf, "SYLL_IN_WORD_MISMATCH_COST",
                                          "SYL_IN_WORD_MISMATCH_COST",
                                          av->w_sp[2]);
    av->w_sp[3] = spfy_vcf_f32(vcf, "WORD_IN_PHRASE_MISMATCH_COST", av->w_sp[3]);
    av->w_sp[4] = spfy_vcf_f32(vcf, "PHONE_IN_SYL_MISMATCH_COST",  av->w_sp[4]);

    av->w_ccos = spfy_vcf_f32(vcf, "CONTEXT_COST_WEIGHT", av->w_ccos);
    av->w_dur  = spfy_vcf_f32(vcf, "DUR_WEIGHT",          av->w_dur);
    av->w_f0   = spfy_vcf_f32(vcf, "ABS_F0_WEIGHT",       av->w_f0);

    /* UNIT_BIAS_WEIGHT and CHUNK_BIAS_WEIGHT are equal in all five shipped
     * voices, so which one feeds w_3c cannot be distinguished from the
     * data. */
    av->w_3c = spfy_vcf_f32(vcf, "UNIT_BIAS_WEIGHT", av->w_3c);

    /* Anchor prune threshold/slope. */
    av->anchor_norm  = spfy_vcf_f32(vcf, "SYL_CAND_PRUNE_THRESH",
                                    av->anchor_norm);
    av->anchor_norm2 = spfy_vcf_f32(vcf, "SYL_CAND_PRUNE_SLOPE",
                                    av->anchor_norm2);
}
