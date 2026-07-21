/* Phase B4 step 1: derive engine `slice.ctx[5]` per halfphone-leaf
 * slot from the FE-emitted segment chain. See build_graph.h for
 * the encoding overview.
 *
 * The Tom voice's phoneme -> label_idx mapping was extracted
 * empirically by joining captured fe_tree segment names with
 * captured prsl_slot.ctx[2] values across the corpus. Labels 13 and
 * 30 were added 2026-05-13 after the audit corpus expanded from
 * 32 -> 225 phrases surfaced `el` and `oy` (project_pool_uid0_
 * 2026_05_13 / 5 of the 6 pool_uid0 cases). Label 45 (`zh`) added
 * 2026-05-13 evening after master-capture v2 surfaced 4 ctx_center
 * mismatches in mp_031 "zhat" + nat_036 "usual". Labels 42 and 46
 * still unmapped -- no audit phrase exercises them; if a future FE
 * emit fails the lookup, extend by capturing that phrase's fe_tree +
 * prsl_slot ctx[2] and joining.
 */

#include "build_graph.h"

#include "../../include/spfy/spfy.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void spfy_slice_ctx_table_free(spfy_slice_ctx_table_t *t)
{
    if (!t) return;
    free(t->ctx);
    free(t->has);
    memset(t, 0, sizeof *t);
}

/* Tom phoneme -> label idx. Empirically derived from the captured
 * 32-entry oracle corpus. The 47-entry table includes 5 placeholder
 * gaps (UINT32_MAX) for phonemes the corpus didn't exercise. */
typedef struct { const char *name; uint32_t label; } spfy_phone_t;
static const spfy_phone_t TOM_PHONES[] = {
    { "aa",  0 }, { "ae",  1 }, { "ah",  2 }, { "ao",  3 },
    { "aw",  4 }, { "ax",  5 }, { "ay",  6 }, { "b",   7 },
    { "ch",  8 }, { "d",   9 }, { "dh", 10 }, { "dx", 11 },
    { "eh", 12 }, { "el", 13 },     /* el (syllabic L) -- added 2026-05-13 */
    { "en", 14 }, { "er", 15 }, { "ey", 16 }, { "f",  17 },
    { "g",  18 }, { "hh", 19 }, { "ih", 20 }, { "ix", 21 },
    { "iy", 22 }, { "jh", 23 }, { "k",  24 }, { "l",  25 },
    { "m",  26 }, { "n",  27 }, { "ng", 28 }, { "ow", 29 },
    { "oy", 30 },                   /* oy diphthong -- added 2026-05-13 */
    { "p",  31 }, { "pau",32 }, { "r",  33 }, { "s",  34 },
    { "sh", 35 }, { "t",  36 }, { "th", 37 }, { "uh", 38 },
    { "uw", 39 }, { "v",  40 }, { "w",  41 },
    /* 42 unmapped */
    { "y",  43 }, { "z",  44 },
    { "zh", 45 },                   /* zh -- added 2026-05-13 evening
                                       (mp_031 "zhat", nat_036 "usual") */
    /* 46 unmapped */
};

uint32_t spfy_tom_phone_to_label(const char *name)
{
    if (!name) return UINT32_MAX;
    for (size_t i = 0; i < sizeof TOM_PHONES / sizeof TOM_PHONES[0]; ++i) {
        if (strcmp(TOM_PHONES[i].name, name) == 0) return TOM_PHONES[i].label;
    }
    return UINT32_MAX;
}

/* Tom's pau sits at feat index 32, so his silence hp_classes are 64/65.
 * These are the fallback ONLY for the NULL-phone_names (Tom) path --
 * every other voice puts pau elsewhere (felix 35 -> 70/71, javier 24 ->
 * 48/49), so the real sentinel is computed per-voice below. */
#define TOM_HP_PAU_L 64u
#define TOM_HP_PAU_R 65u

/* Phone symbol -> engine phone id, against the voice's own inventory.
 * Index in feat["name"] order IS the id. Falls back to the Tom table
 * when the caller supplied no inventory. UINT32_MAX if unknown. */
static uint32_t ctx_phone_to_label(char *const *phone_names,
                                   uint32_t     n_phones,
                                   const char  *name)
{
    if (!phone_names) return spfy_tom_phone_to_label(name);
    if (!name) return UINT32_MAX;
    for (uint32_t i = 0; i < n_phones; ++i) {
        if (phone_names[i] && strcmp(phone_names[i], name) == 0) return i;
    }
    return UINT32_MAX;
}

int spfy_derive_slice_ctx(const spfy_slot_tree_t *tree,
                          const char         **fe_segments_in_order,
                          uint32_t              n_segments,
                          char *const          *phone_names,
                          uint32_t              n_phones,
                          spfy_slice_ctx_table_t *out)
{
    if (!tree || !fe_segments_in_order || !out) return SPFY_E_INVAL;
    if (n_segments != tree->n_halfphone / 2u) return SPFY_E_FORMAT;

    /* Silence sentinel for out-of-range / unknown neighbours, in THIS
     * voice's numbering. */
    uint32_t pau = ctx_phone_to_label(phone_names, n_phones, "pau");
    uint32_t hp_pau_l = (pau == UINT32_MAX) ? TOM_HP_PAU_L : pau * 2u;
    uint32_t hp_pau_r = hp_pau_l + 1u;

    memset(out, 0, sizeof *out);
    out->n_slots = tree->n_slots;
    out->ctx = (uint32_t (*)[5])
        calloc(tree->n_slots, sizeof *out->ctx);
    out->has = (uint8_t *)calloc(tree->n_slots, sizeof *out->has);
    if (!out->ctx || !out->has) {
        spfy_slice_ctx_table_free(out);
        return SPFY_E_NOMEM;
    }

    /* Pre-compute hp_class per phoneme position k for each side.
     * Out-of-range = silence sentinel. */
    uint32_t n_positions = n_segments;

    /* Pass 1: for each halfphone-leaf slot, identify which (segment,
     * side) it represents. The slot's `fe_shared` is the segment's
     * shared id; combined with `halfphone_side` we know exactly which
     * (k, side) it is. We also need k -- enumerate halfphones in
     * post-order: the FIRST halfphone-leaf in post-order IS the
     * leftmost segment's left half, etc.
     *
     * Since BuildGraph allocates HPs in (word, syl, seg, L, seg, R)
     * order with syl-then-segments ordering, the post-order traversal
     * visits halfphones in left-to-right utterance order:
     *   slot[0] = seg 0 L, slot[1] = seg 0 R,
     *   slot[lowest_hp_in_word2] = seg 1 L (in order they appear in
     *   syl_segs), etc.
     * So we can just walk halfphone-kind slots in post-order; each
     * pair (i, i+1) corresponds to segment i's L and R halfphones. */

    uint32_t k = 0;
    for (uint32_t s = 0; s < tree->n_slots && k < n_positions * 2; ++s) {
        if (tree->slots[s].kind != SPFY_SK_HALFPHONE) continue;
        uint32_t pos = k / 2u;
        uint32_t side = k & 1u;
        if (pos >= n_positions) break;
        uint32_t label = ctx_phone_to_label(phone_names, n_phones,
                                            fe_segments_in_order[pos]);
        if (label == UINT32_MAX) {
            /* Unknown phoneme -- mark this slot as not having ctx. */
            ++k;
            continue;
        }
        /* ctx[i] = phoneme at (pos + (i-2)), same side, sentinel OOR. */
        uint32_t (*ctx5)[5] = &out->ctx[s];
        for (int i = 0; i < 5; ++i) {
            int32_t off = (int32_t)pos + (i - 2);
            if (off < 0 || off >= (int32_t)n_positions) {
                (*ctx5)[i] = side ? hp_pau_r : hp_pau_l;
            } else {
                uint32_t l2 = ctx_phone_to_label(phone_names, n_phones,
                                                 fe_segments_in_order[off]);
                if (l2 == UINT32_MAX) {
                    /* Neighbor unknown -- best-effort: silence sentinel. */
                    (*ctx5)[i] = side ? hp_pau_r : hp_pau_l;
                } else {
                    (*ctx5)[i] = l2 * 2u + side;
                }
            }
        }
        out->has[s] = 1;
        ++k;
    }
    return SPFY_OK;
}

/* --------------------------------------------------------------------- */
/* CART feature kernels (q_type 3, 4, 5)                                 */
/* --------------------------------------------------------------------- */
/* Decoded + validated bit-exact (822/822 halfphone slots over 30-text
 * corpus) by `c:/tmp/verify_q345_kernels.py` against the captured
 * `cart_walker_args` trace. See build_graph.h for the q_type ABI. */

uint32_t spfy_cart_feature_q3(uint32_t       ctx1,
                              const uint8_t *s_ctx_remap,
                              uint32_t       n_remap)
{
    if (!s_ctx_remap) return UINT32_MAX;
    if (ctx1 >= n_remap) return UINT32_MAX;
    return (uint32_t)s_ctx_remap[ctx1];
}

uint32_t spfy_cart_feature_q4(uint32_t       ctx3,
                              const uint8_t *s_ctx_remap,
                              uint32_t       n_remap)
{
    if (!s_ctx_remap) return UINT32_MAX;
    if (ctx3 >= n_remap) return UINT32_MAX;
    return (uint32_t)s_ctx_remap[ctx3];
}

int spfy_derive_q5_table(const spfy_slot_tree_t *tree,
                         const char       **word_names,
                         uint32_t            n_words,
                         uint32_t           *q5_per_slot,
                         uint8_t            *has_q5)
{
    if (!tree || !q5_per_slot || !has_q5) return SPFY_E_INVAL;
    if (n_words != tree->n_word) return SPFY_E_FORMAT;

    /* Init per-slot defaults: 0 / has=0. */
    for (uint32_t s = 0; s < tree->n_slots; ++s) {
        q5_per_slot[s] = 0;
        has_q5[s]      = 0;
    }

    /* Walk the slot tree top-down: phrase root -> word -> syllable ->
     * halfphone leaves. For each syllable, count its halfphone children
     * and assign q5 = that count to all of them, EXCEPT when the
     * containing word's name is "_NULL_" (boundary silence) -- then
     * q5 = 1 for all halfphones under that word's syllable.
     *
     * The slot tree's `slots[]` is in post-order index, with parent
     * pointers. We iterate halfphone-leaf slots and walk up parent
     * links to find {syllable, word}.
     */

    /* First, build a word_idx-by-post-order map by enumerating word
     * slots in the tree in their natural order. The fe_utt's
     * word_shareds[] is in FE relation order (head to tail), and
     * spfy_build_graph allocates word slots in that same order under
     * the phrase root. So word slot k corresponds to FE word index k.
     */
    uint32_t *word_slot_idx = (uint32_t *)calloc(tree->n_word,
                                                  sizeof *word_slot_idx);
    if (!word_slot_idx) return SPFY_E_NOMEM;
    uint32_t wcount = 0;
    for (uint32_t s = 0; s < tree->n_slots; ++s) {
        if (tree->slots[s].kind == SPFY_SK_WORD) {
            if (wcount >= tree->n_word) {
                free(word_slot_idx);
                return SPFY_E_FORMAT;
            }
            word_slot_idx[wcount++] = s;
        }
    }
    if (wcount != tree->n_word) {
        free(word_slot_idx);
        return SPFY_E_FORMAT;
    }

    /* Reverse map: slot index -> word_idx (for halfphones, via parent
     * walk up to find the WORD ancestor). */
    for (uint32_t s = 0; s < tree->n_slots; ++s) {
        if (tree->slots[s].kind != SPFY_SK_HALFPHONE) continue;

        /* Walk up: halfphone -> syllable -> word. */
        uint32_t syl_idx = tree->slots[s].parent_idx;
        if (syl_idx == UINT32_MAX || syl_idx >= tree->n_slots) continue;
        if (tree->slots[syl_idx].kind != SPFY_SK_SYLLABLE) continue;
        uint32_t word_idx = tree->slots[syl_idx].parent_idx;
        if (word_idx == UINT32_MAX || word_idx >= tree->n_slots) continue;
        if (tree->slots[word_idx].kind != SPFY_SK_WORD) continue;

        /* Find which FE word this slot corresponds to. */
        uint32_t fe_word_idx = UINT32_MAX;
        for (uint32_t k = 0; k < tree->n_word; ++k) {
            if (word_slot_idx[k] == word_idx) { fe_word_idx = k; break; }
        }

        const char *wname = NULL;
        if (fe_word_idx != UINT32_MAX && word_names &&
            fe_word_idx < n_words) {
            wname = word_names[fe_word_idx];
        }

        if (wname && strcmp(wname, "_NULL_") == 0) {
            q5_per_slot[s] = 1u;       /* engine init default kept */
        } else {
            /* Count halfphones in this syllable. */
            q5_per_slot[s] = tree->slots[syl_idx].n_children;
        }
        has_q5[s] = 1u;
    }

    free(word_slot_idx);
    return SPFY_OK;
}

/* --------------------------------------------------------------------- */
/* SP_target populator (workspace+0x28..0x3c)                             */
/* --------------------------------------------------------------------- */
/*
 * Port of `c:/tmp/sp_target_full.py` populator chain. Validated bit-
 * exact 822/822 against captured `inner_scorer.sp_target` on the
 * 30-text corpus. See build_graph.h for the SP_target -> q_type and
 * matrix-row mapping.
 *
 * Algorithm overview:
 *   Build per-slot {syl_idx, word_idx, accent, stress} from the slot
 *   tree + FE utt (word_names, syl_stress, syl_accent). Boundary _NULL_
 *   word slots get init defaults (syl_idx=-1, word_idx=-1, accent=0,
 *   stress=0). Then run the three populator passes:
 *     pass A (FUN_08e8c7d0) -> sp[0] sylInPhrase + sp[1] sylType
 *     pass B (FUN_08e8a670) -> refines sp[0] (vals 6/7/8) +
 *                              produces sp[2] sylInWord + sp[3] wordInPhrase
 *     pass C (FUN_08e8a880) -> sp[4] phoneInSyl
 */

void spfy_sp_target_table_free(spfy_sp_target_table_t *t)
{
    if (!t) return;
    free(t->sp);
    free(t->has);
    memset(t, 0, sizeof *t);
}

/* Per-slot scratch state used by the populator. */
typedef struct {
    int32_t  syl_idx;     /* -1 for boundary _NULL_ slots */
    int32_t  word_idx;    /* -1 for boundary _NULL_ slots */
    uint32_t accent;      /* 0 = none */
    int32_t  stress;      /* 0 default */
} sp_slot_state_t;

static void run_pass_a(const sp_slot_state_t *st, uint32_t n,
                       int local_10, uint32_t sentence_idx,
                       int flag88,
                       uint32_t *sp0, uint32_t *sp1)
{
    /* Find last_syl: highest syl_idx > 0 from the tail. */
    int32_t last_syl = 0;
    for (int32_t i = (int32_t)n - 1; i >= 0; --i) {
        if (st[i].syl_idx > 0) { last_syl = st[i].syl_idx; break; }
    }

    /* sp0 (sylInPhrase) */
    for (uint32_t i = 0; i < n; ++i) {
        int32_t s = st[i].syl_idx;
        if (s == 0) {
            sp0[i] = (last_syl == 0) ? 4u : 1u;
        } else if (s == last_syl) {
            sp0[i] = local_10 ? 5u : 3u;
        } else {
            sp0[i] = 2u;
        }
    }

    /* sp1 (sylType): default by stress; refined for accent slots. */
    for (uint32_t i = 0; i < n; ++i) {
        sp1[i] = (st[i].stress < 1) ? 1u : 2u;
        if (st[i].accent == 0) continue;

        /* Backward walk over [0..i-1]. */
        int b_var3 = 1;
        for (int32_t j = (int32_t)i - 1; j >= 0; --j) {
            if (st[j].syl_idx != st[i].syl_idx && st[j].accent != 0) {
                b_var3 = 0; break;
            }
        }
        if (b_var3) {
            sp1[i] = (sentence_idx == 0) ? 4u : 5u;
        }

        int do_forward = ((flag88 != 0 && sp0[i] == 4) ||
                          !b_var3 ||
                          (local_10 != 0));
        if (!do_forward) continue;

        int mismatch_fwd = 0;
        for (uint32_t j = i + 1; j < n; ++j) {
            if (st[j].syl_idx != st[i].syl_idx && st[j].accent != 0) {
                mismatch_fwd = 1; break;
            }
        }

        if (mismatch_fwd) {
            if (!b_var3) sp1[i] = 3u;
            /* else keep sp1 (already 4 or 5) */
        } else {
            if (local_10 != 0)            sp1[i] = 7u;
            else if (sp1[i] != 4u)        sp1[i] = 6u;
        }
    }
}

static void run_pass_b(const sp_slot_state_t *st, uint32_t n,
                       int local_10, uint32_t sentence_idx,
                       int voice_d4_flag,
                       uint32_t *sp0, uint32_t *sp2, uint32_t *sp3)
{
    /* Init sp2=1, sp3=4. */
    for (uint32_t i = 0; i < n; ++i) { sp2[i] = 1u; sp3[i] = 4u; }

    uint32_t i = 0;
    while (i < n) {
        int32_t word_idx = st[i].word_idx;
        if (word_idx < 0) { ++i; continue; }

        /* Word group [start..end]. */
        uint32_t start = i;
        while (start > 0 && st[start - 1].word_idx == word_idx) --start;
        uint32_t end = i;
        while (end + 1 < n && st[end + 1].word_idx == word_idx) ++end;

        int32_t first_syl = st[start].syl_idx;
        int32_t last_syl  = st[end].syl_idx;

        /* Refine sp0 (currently 2) -> 6/7/8 by syl position in word. */
        for (uint32_t j = start; j <= end; ++j) {
            if (sp0[j] == 2u) {
                sp0[j] = 7u;
                if (st[j].syl_idx == first_syl) sp0[j] = 6u;
                if (st[j].syl_idx == last_syl)  sp0[j] = 8u;
            }
        }

        /* wordInPhrase */
        uint32_t wp = 2u;
        if (first_syl == 0) {
            if (sentence_idx != 0)      wp = 1u;
            else if (voice_d4_flag)     wp = 1u;
            else                        wp = 5u;
        }
        uint32_t last_sp0 = sp0[end];
        if (last_sp0 == 5u)
            wp = voice_d4_flag ? 3u : 6u;
        if (last_sp0 == 3u)
            wp = 3u;
        if (last_sp0 == 4u) {
            if (local_10 == 0)          wp = 3u;
            else if (voice_d4_flag)     wp = 3u;
            else                        wp = 6u;
        }
        for (uint32_t j = start; j <= end; ++j) sp3[j] = wp;

        /* sylInWord: relative to first accent-bearing syl in word. */
        int32_t accent_anchor = -1;
        for (uint32_t j = start; j <= end; ++j) {
            if (st[j].accent != 0) { accent_anchor = st[j].syl_idx; break; }
        }
        if (accent_anchor >= 0) {
            for (uint32_t j = start; j <= end; ++j) {
                int32_t cs = st[j].syl_idx;
                if (cs < accent_anchor)            sp2[j] = 2u;
                else if (cs == accent_anchor)      sp2[j] = 3u;
                else if (cs == accent_anchor + 1)  sp2[j] = 4u;
                else                               sp2[j] = 5u;
            }
        }

        i = end + 1;
    }
}

static void run_pass_c(const sp_slot_state_t *st, uint32_t n,
                       uint32_t *sp4)
{
    for (uint32_t i = 0; i < n; ++i) sp4[i] = 3u;     /* init default */

    for (uint32_t i = 0; i < n; ++i) {
        int32_t cw = st[i].word_idx;
        int32_t cs = st[i].syl_idx;
        int32_t pw, psy;
        if (i >= 2) {
            pw  = st[i - 2].word_idx;
            psy = st[i - 2].syl_idx;
        } else {
            pw  = -1;
            psy = -1;
        }

        if (pw < cw) {
            sp4[i] = 1u;
        } else if (psy < cs) {
            sp4[i] = 2u;
        } else {
            int32_t nw, nsy;
            if (i + 2 < n) {
                nw  = st[i + 2].word_idx;
                nsy = st[i + 2].syl_idx;
            } else {
                nw  = cw + 1;
                nsy = cs + 1;
            }
            if (cw < nw || nw < 0)             sp4[i] = 5u;
            else if (cs < nsy)                  sp4[i] = 4u;
            /* else stays at 3 */
        }
    }
}

int spfy_derive_sp_targets(const spfy_slot_tree_t *tree,
                           const spfy_fe_utt_t    *utt,
                           uint32_t                sentence_idx_in_para,
                           int                     voice_d4_flag,
                           spfy_sp_target_table_t *out)
{
    if (!tree || !utt || !out) return SPFY_E_INVAL;

    memset(out, 0, sizeof *out);
    out->n_slots = tree->n_slots;
    out->sp  = (uint32_t (*)[5])calloc(tree->n_slots, sizeof *out->sp);
    out->has = (uint8_t *)calloc(tree->n_slots, sizeof *out->has);
    if (!out->sp || !out->has) {
        spfy_sp_target_table_free(out);
        return SPFY_E_NOMEM;
    }

    /* Pass-state operates on a HALFPHONE-ONLY flat array (matching the
     * Python reference's emission order). The slot tree's post-order
     * interleaves non-halfphone slots between halfphones (e.g.
     * HP-L,HP-R,SYL,HP-L,HP-R,...), which would break pass B's word-
     * group walk and pass C's same-side neighbor lookup if we iterated
     * the full slot range. So we collect halfphone-leaf slots into a
     * flat array, run all 3 passes on it, then write back to out->sp[]
     * indexed by the tree slot. */
    sp_slot_state_t *st = (sp_slot_state_t *)calloc(tree->n_halfphone,
                                                    sizeof *st);
    /* hp_idx_to_tree_slot[hp] = tree slot index of the hp-th halfphone
     * leaf in post-order. */
    uint32_t *hp_to_slot = (uint32_t *)calloc(tree->n_halfphone,
                                              sizeof *hp_to_slot);
    if (!st || !hp_to_slot) {
        free(st); free(hp_to_slot);
        spfy_sp_target_table_free(out);
        return SPFY_E_NOMEM;
    }

    /* Index each WORD slot to its FE word index by post-order. Word
     * slots are emitted in word-order under the phrase root. */
    uint32_t *word_slot_to_fe = (uint32_t *)calloc(tree->n_slots,
                                                   sizeof *word_slot_to_fe);
    if (!word_slot_to_fe) { free(st); spfy_sp_target_table_free(out);
                            return SPFY_E_NOMEM; }
    for (uint32_t s = 0; s < tree->n_slots; ++s) word_slot_to_fe[s] = UINT32_MAX;
    {
        uint32_t fe_widx = 0;
        for (uint32_t s = 0; s < tree->n_slots; ++s) {
            if (tree->slots[s].kind == SPFY_SK_WORD) {
                if (fe_widx >= utt->n_words) {
                    free(word_slot_to_fe); free(st);
                    spfy_sp_target_table_free(out);
                    return SPFY_E_FORMAT;
                }
                word_slot_to_fe[s] = fe_widx++;
            }
        }
    }

    /* Index each SYLLABLE slot to its global FE syllable index. Syl
     * slots emitted in word-order, then within each word in
     * daughter-chain order. */
    uint32_t *syl_slot_to_fe = (uint32_t *)calloc(tree->n_slots,
                                                  sizeof *syl_slot_to_fe);
    if (!syl_slot_to_fe) { free(word_slot_to_fe); free(st);
                           spfy_sp_target_table_free(out);
                           return SPFY_E_NOMEM; }
    for (uint32_t s = 0; s < tree->n_slots; ++s) syl_slot_to_fe[s] = UINT32_MAX;
    {
        uint32_t fe_sidx = 0;
        for (uint32_t s = 0; s < tree->n_slots; ++s) {
            if (tree->slots[s].kind == SPFY_SK_SYLLABLE) {
                if (fe_sidx >= utt->n_syls) {
                    free(syl_slot_to_fe); free(word_slot_to_fe); free(st);
                    spfy_sp_target_table_free(out);
                    return SPFY_E_FORMAT;
                }
                syl_slot_to_fe[s] = fe_sidx++;
            }
        }
    }

    /* Counter mirroring FUN_08e8cbb0: increments syl/word indexes only
     * for non-_NULL_ words (boundary _NULL_ slots get -1). Walk
     * halfphone leaves in slot order, infer state from the syl/word
     * parents. */
    int32_t *fe_syl_to_engine_syl  = (int32_t *)malloc(utt->n_syls * sizeof(int32_t));
    int32_t *fe_word_to_engine_word = (int32_t *)malloc(utt->n_words * sizeof(int32_t));
    if (!fe_syl_to_engine_syl || !fe_word_to_engine_word) {
        free(fe_syl_to_engine_syl); free(fe_word_to_engine_word);
        free(syl_slot_to_fe); free(word_slot_to_fe); free(st);
        spfy_sp_target_table_free(out);
        return SPFY_E_NOMEM;
    }
    for (uint32_t k = 0; k < utt->n_syls;  ++k) fe_syl_to_engine_syl[k]  = -1;
    for (uint32_t k = 0; k < utt->n_words; ++k) fe_word_to_engine_word[k] = -1;
    {
        int32_t syl_counter = 0, word_counter = 0;
        uint32_t global_syl = 0;
        for (uint32_t w = 0; w < utt->n_words; ++w) {
            int is_null = utt->word_names && utt->word_names[w] &&
                          strcmp(utt->word_names[w], "_NULL_") == 0;
            for (uint32_t s = 0; s < utt->word_n_syls[w]; ++s) {
                if (is_null) {
                    /* leave fe_syl_to_engine_syl[global_syl] = -1 */
                } else {
                    fe_syl_to_engine_syl[global_syl] = syl_counter++;
                }
                ++global_syl;
            }
            if (is_null)
                fe_word_to_engine_word[w] = -1;
            else
                fe_word_to_engine_word[w] = word_counter++;
        }
    }

    /* Build per-halfphone state by walking halfphone-leaf slots in
     * post-order (= utterance order). */
    {
        uint32_t hp = 0;
        for (uint32_t s = 0; s < tree->n_slots; ++s) {
            if (tree->slots[s].kind != SPFY_SK_HALFPHONE) continue;
            if (hp >= tree->n_halfphone) break;

            hp_to_slot[hp] = s;
            st[hp].syl_idx = -1; st[hp].word_idx = -1;
            st[hp].accent  = 0;  st[hp].stress   = 0;

            uint32_t syl_slot_idx = tree->slots[s].parent_idx;
            if (syl_slot_idx >= tree->n_slots ||
                tree->slots[syl_slot_idx].kind != SPFY_SK_SYLLABLE) {
                ++hp; continue;
            }
            uint32_t word_slot_idx = tree->slots[syl_slot_idx].parent_idx;
            if (word_slot_idx >= tree->n_slots ||
                tree->slots[word_slot_idx].kind != SPFY_SK_WORD) {
                ++hp; continue;
            }

            uint32_t fe_widx = word_slot_to_fe[word_slot_idx];
            uint32_t fe_sidx = syl_slot_to_fe[syl_slot_idx];
            if (fe_widx == UINT32_MAX || fe_sidx == UINT32_MAX) {
                ++hp; continue;
            }

            st[hp].word_idx = fe_word_to_engine_word[fe_widx];
            st[hp].syl_idx  = fe_syl_to_engine_syl[fe_sidx];
            if (utt->syl_accent && fe_sidx < utt->n_syls) {
                uint32_t a = utt->syl_accent[fe_sidx];
                if (st[hp].word_idx >= 0) st[hp].accent = a;
            }
            if (utt->syl_stress && fe_sidx < utt->n_syls) {
                int32_t v = utt->syl_stress[fe_sidx];
                if (st[hp].word_idx >= 0 && v > 0) st[hp].stress = v;
            }
            ++hp;
        }
    }

    free(fe_syl_to_engine_syl);
    free(fe_word_to_engine_word);
    free(syl_slot_to_fe);
    free(word_slot_to_fe);

    /* Phrase terminator decides local_10. */
    char term = utt->phrase_term;
    int local_10 = (term == '.' || term == '?' || term == '!') ? 1 : 0;
    int flag88 = 0;   /* config+0x88; 0 for Tom */

    uint32_t *sp0 = (uint32_t *)calloc(tree->n_halfphone, sizeof(uint32_t));
    uint32_t *sp1 = (uint32_t *)calloc(tree->n_halfphone, sizeof(uint32_t));
    uint32_t *sp2 = (uint32_t *)calloc(tree->n_halfphone, sizeof(uint32_t));
    uint32_t *sp3 = (uint32_t *)calloc(tree->n_halfphone, sizeof(uint32_t));
    uint32_t *sp4 = (uint32_t *)calloc(tree->n_halfphone, sizeof(uint32_t));
    if (!sp0 || !sp1 || !sp2 || !sp3 || !sp4) {
        free(sp0); free(sp1); free(sp2); free(sp3); free(sp4);
        free(st); free(hp_to_slot);
        spfy_sp_target_table_free(out);
        return SPFY_E_NOMEM;
    }

    if (getenv("SPFY_SP_STATE_DUMP")) {
        for (uint32_t hp = 0; hp < tree->n_halfphone; ++hp) {
            fprintf(stderr,
                "{\"sp_state\":1,\"hp\":%u,\"tree_slot\":%u,"
                "\"syl_idx\":%d,\"word_idx\":%d,"
                "\"accent\":%u,\"stress\":%u}\n",
                hp, hp_to_slot[hp],
                st[hp].syl_idx, st[hp].word_idx,
                (unsigned)st[hp].accent, (unsigned)st[hp].stress);
        }
    }
    run_pass_a(st, tree->n_halfphone, local_10, sentence_idx_in_para,
               flag88, sp0, sp1);
    run_pass_b(st, tree->n_halfphone, local_10, sentence_idx_in_para,
               voice_d4_flag, sp0, sp2, sp3);
    run_pass_c(st, tree->n_halfphone, sp4);

    /* Emit by mapping hp index back to the tree slot index. */
    for (uint32_t hp = 0; hp < tree->n_halfphone; ++hp) {
        uint32_t s = hp_to_slot[hp];
        out->sp[s][0] = sp0[hp];
        out->sp[s][1] = sp1[hp];
        out->sp[s][2] = sp2[hp];
        out->sp[s][3] = sp3[hp];
        out->sp[s][4] = sp4[hp];
        out->has[s]   = 1u;
    }

    free(sp0); free(sp1); free(sp2); free(sp3); free(sp4);
    free(st); free(hp_to_slot);
    return SPFY_OK;
}
