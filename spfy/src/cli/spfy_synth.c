/* spfy_synth -- M5 end-to-end text->WAV via FE -> USel -> WSOLA pipeline.
 *
 * Pipeline:
 *   text -> spfy_fe_synth_text() -> per-slot ctx[5] + sp[5] + voicing
 *        -> per-slot PRSL pool query -> per-slot CART durt/f0tr (zeroed
 *           for first cut; engine target cost will be approximate)
 *        -> per-(slot, cand) score via spfy_hp_innerscorer
 *        -> Viterbi DP with hash-based join cost
 *        -> chosen UIDs -> decode + WSOLA Hann-OLA streamer
 *        -> WAV.
 *
 * Usage:
 *   spfy_synth <voice.vin> <voice.vdb> <voice.vcf> <hpclass.bin>
 *              <vocab.json> <fe_tables_a> <fe_tables_b>
 *              "<text>" <out.wav>
 *
 * This is the minimum-viable wiring: any bit-exact gaps (CART, full
 * post-scoring adjustment) are deferred -- the path produces audible
 * speech end-to-end so we can A/B against the engine.
 */

#include <spfy/spfy.h>

#include "../synth/spfy_synth_lib.h"

#include "../voice/voice.h"
#include "../voice/unit_table.h"
#include "../voice/feat_table.h"
#include "../voice/vdb_lookup.h"
#include "../voice/ccos.h"
#include "../voice/voice_runtime.h"
#include "../voice/vcf_matrix.h"
#include "../voice/chunk_table.h"
#include "../usel/anchor_score.h"
#include "../usel/build_graph.h"
#include "../usel/hash.h"
#include "../usel/prsl.h"
#include "../usel/viterbi.h"
#include "../cart/cart.h"
#include "../wsola/ulaw.h"
#include "../wsola/wav.h"
#include "../wsola/wsola.h"
#include "../fe/fe.h"
#include "../fe/phoneset.h"
#include "../fe/prosody.h"
#include "../fe/baked_pos.h"

/* In-house FE — used when SPFY_FE_INTERNAL=1 to bypass the SWIttsFe
 * DLL drive entirely. Lets us A/B audit the new path vs the hosted FE. */
#include "../fe_internal/fe_internal.h"
#  include "../fe_host/fe_parse.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>     /* asset tempdir uniqueness via argv[0] mtime */

#define SILENCE_SENTINEL_UID 169578u
#define MAX_CANDS_PER_SLOT   512

/* ------------------------------------------------------------------ */
/* Audio helpers (mirror spfy_synth_replay.c)                          */
/* ------------------------------------------------------------------ */

static int decode_unit_samples(uint16_t file_idx, uint16_t lp_ms, uint16_t dur_ms,
                                const spfy_feat_table_t *feat,
                                const spfy_vdb_t        *vdb,
                                const spfy_vdb_lookup_t *lookup,
                                uint32_t over_n,
                                int16_t **out, size_t *out_n,
                                size_t *out_nominal_n)
{
    *out = NULL; *out_n = 0;
    if (out_nominal_n) *out_nominal_n = 0;
    if (dur_ms == 0) return SPFY_OK;
    if (file_idx >= feat->n_entries) return SPFY_E_OOB;
    const spfy_feat_entry_t *fe = &feat->entries[file_idx];
    uint32_t rec_off = 0, rec_size = 0;
    int rc = spfy_vdb_lookup_by_name(lookup, fe->name, fe->name_len,
                                     &rec_off, &rec_size);
    if (rc != SPFY_OK) return rc;
    uint32_t off  = lp_ms * 8u;
    uint32_t nominal = dur_ms * 8u;
    uint32_t blen = nominal + over_n;
    if (off >= rec_size) return SPFY_OK;
    /* Don't read past end of recording — clamp the overread; if we hit
     * the recording end before getting the full nominal, that's the
     * caller's problem (audible for non-existent overread but
     * preserves correctness). */
    if (off + blen > rec_size) blen = rec_size - off;
    if (blen == 0) return SPFY_OK;
    int16_t *buf = (int16_t *)malloc(blen * sizeof *buf);
    if (!buf) return SPFY_E_NOMEM;
    spfy_ulaw_decode(vdb->data + rec_off + off, blen, buf);
    *out = buf; *out_n = blen;
    /* Report the nominal sample count for duration-preserving truncation
     * downstream. If we couldn't decode the full nominal (hit recording
     * end), nominal is reduced to what's actually available. */
    if (out_nominal_n)
        *out_nominal_n = (nominal < blen) ? nominal : blen;
    return SPFY_OK;
}

static int append_recording_span(spfy_wsola_streamer_t *ws,
                                 uint32_t file_idx, uint32_t lp, uint32_t dur,
                                 const spfy_feat_table_t *feat,
                                 const spfy_vdb_t        *vdb,
                                 const spfy_vdb_lookup_t *lookup,
                                 int align,
                                 uint8_t f0_tail, uint8_t f0_head,
                                 uint32_t sample_rate)
{
    int16_t *buf = NULL; size_t n = 0, nominal_n = 0;
    /* Over-decode by SPFY_WSOLA_MAX_LAG_DEFAULT (= engine's lag search
     * range = window_size = 80 samples @ 8 kHz) so the lag shift has
     * a look-ahead reservoir. SPFY_WSOLA_NO_OVERREAD=1 disables. */
    static int no_overread = -1;
    if (no_overread < 0)
        no_overread = (getenv("SPFY_WSOLA_NO_OVERREAD") != NULL);
    uint32_t over_n = no_overread ? 0u : SPFY_WSOLA_MAX_LAG_DEFAULT;
    int rc = decode_unit_samples((uint16_t)file_idx, (uint16_t)lp,
                                 (uint16_t)dur, feat, vdb, lookup,
                                 over_n, &buf, &n, &nominal_n);
    if (rc != SPFY_OK) return rc;
    if (getenv("SPFY_TRACE_UNITS")) {
        /* Cumulative sample-count tracker so the user can map output
         * waveform positions back to which unit was being emitted. */
        static uint64_t cum_n = 0;
        fprintf(stderr,
                "[unit] t=%7.1fms  file=%4u  lp=%5u  dur=%4u  n=%4zu  nom=%4zu  align=%d  f0t=%u f0h=%u  cum=%llu\n",
                (double)cum_n * 1000.0 / 8000.0,
                file_idx, lp, dur, n, nominal_n, align, f0_tail, f0_head,
                (unsigned long long)cum_n);
        cum_n += nominal_n;
    }
    rc = spfy_wsola_push_unit_psola(ws, buf, n, nominal_n, align,
                                    f0_tail, f0_head, sample_rate);
    free(buf);
    return rc;
}

/* ------------------------------------------------------------------ */
/* CART feature kernels (durt + f0tr)                                  */
/* ------------------------------------------------------------------ */

/* Tom phone-pair swap: 9->10, 10->11, 11->9; 14<->15. (Mirrors
 * tom_swap_label in src/usel/anchor_score.c.) */
static uint32_t tom_swap(uint32_t label)
{
    if (label == 9)  return 10;
    if (label == 10) return 11;
    if (label == 11) return 9;
    if (label == 14) return 15;
    if (label == 15) return 14;
    return label;
}

typedef struct {
    const spfy_fe_slot_t *slot;
    uint32_t              q5;
    int                   is_f0tr;  /* if 1, q3/q4/q5/q9 are clamped to 0
                                       per engine's f0tr CART convention
                                       (cart_walker_args trace shows these
                                       are always 0 when tree=='f0tr'). */
} cart_feat_ctx_t;

static int32_t cart_feat(uint32_t q_type, void *user)
{
    const cart_feat_ctx_t *c = (const cart_feat_ctx_t *)user;
    if (c->is_f0tr) {
        /* engine's f0tr CART is syllable-level — only q1, q2, q7, q8
         * are populated. Everything else clamps to 0. */
        if (q_type == 3 || q_type == 4 || q_type == 5 || q_type == 9)
            return 0;
    } else {
        /* engine's durt walker (FUN_08e87d90) executes XOR EBX,EBX before
         * each dispatcher call (verified disasm + cart_walker_args hook
         * — ebx field always 0). For q_type=7 the dispatcher reads EBX,
         * so q7 is forced to 0 in durt walks. (q3/q4/q5/q8/q9 come from
         * stack args populated by the walker, and slot-level sp[]/ctx[]
         * are the correct sources for those.) Without this clamp, our
         * durt walks at q_type=7 nodes use sp[2] and diverge from engine
         * — accounted for all 6 durt-mismatch slots in nat_036
         * (slot 7/10/12/16/18/20). */
        if (q_type == 7)
            return 0;
    }
    /* From src/usel/build_graph.h: q_type -> source mapping decoded from
     * the engine's CART walker (FUN_08e87c90):
     *   1: workspace+0x28 = sylType        = sp[1]
     *   2: workspace+0x2c = sylInPhrase    = sp[0]
     *   3: s_ctx_remap[ctx[1]] = LEFT phone label (with Tom swap)
     *   4: s_ctx_remap[ctx[3]] = RIGHT phone label (with Tom swap)
     *   5: halfphones-in-current-syllable (precomputed in c->q5)
     *   7: workspace+0x34 = sylInWord      = sp[2]
     *   8: workspace+0x38 = wordInPhrase   = sp[3]
     *   9: workspace+0x3c = phoneInSyl     = sp[4]
     */
    int32_t v;
    switch (q_type) {
        case 1: v = (int32_t)c->slot->sp[1]; break;
        case 2: v = (int32_t)c->slot->sp[0]; break;
        case 3: v = (int32_t)tom_swap((uint32_t)c->slot->ctx[1] >> 1); break;
        case 4: v = (int32_t)tom_swap((uint32_t)c->slot->ctx[3] >> 1); break;
        case 5: v = (int32_t)c->q5; break;
        case 7: v = (int32_t)c->slot->sp[2]; break;
        case 8: v = (int32_t)c->slot->sp[3]; break;
        case 9: v = (int32_t)c->slot->sp[4]; break;
        default: v = 0; break;
    }
    return v;
}

/* Compute the q5 (halfphones-in-syllable) array. Slots in the same
 * syllable share (sp[2], sp[3]); the count of consecutive same-tuple
 * slots is the slot's q5. Boundary-silence slots (FE pad) get q5=1
 * per the engine's _NULL_-syllable fallback. Unused under SPFY_FE_HOSTED
 * (the slot-tree derive_q5_table replaces it). */
__attribute__((unused)) static
void compute_q5_per_slot(const spfy_fe_utterance_t *utt,
                                 uint32_t *q5_out)
{
    uint32_t i = 0;
    while (i < utt->n_slots) {
        uint32_t j = i;
        uint32_t s2 = utt->slots[i].sp[2];
        uint32_t s3 = utt->slots[i].sp[3];
        while (j + 1 < utt->n_slots
               && utt->slots[j+1].sp[2] == s2
               && utt->slots[j+1].sp[3] == s3) {
            ++j;
        }
        uint32_t run = j - i + 1;
        for (uint32_t k = i; k <= j; ++k) {
            int silence = (utt->slots[k].ctx[2] == 64
                           || utt->slots[k].ctx[2] == 65);
            q5_out[k] = silence ? 1u : run;
        }
        i = j + 1;
    }
}

/* Detect the phrase-terminating punctuation in the input text. The slot
 * derivation reads `phrase_term` to choose between sentence-final and
 * mid-sentence prosody categories. Used by BOTH delta_to_fe_utt (non-
 * hosted) AND parsed_to_fe_utt (hosted); kept outside the ifdef. */
static char detect_phrase_term(const char *text)
{
    char term = '.';
    for (size_t i = strlen(text); i > 0; --i) {
        char c = text[i - 1];
        if (c == '.' || c == '?' || c == '!' || c == ',') { term = c; break; }
        if (!isspace((unsigned char)c)) break;
    }
    return term;
}

/* Forward decl: is_arpa_vowel is defined in the post-ifdef block but
 * used by BOTH delta_to_fe_utt (non-hosted) AND parsed_to_fe_utt
 * (hosted, max-onset re-syllabification). Kept outside the ifdef so
 * the hosted branch can see it. */
static int is_arpa_vowel(const char *a);


/* Hosted FE: build spfy_fe_utt_t directly from the parser's per-word
 * structure for a single phrase_id. The hosted FE has already done
 * POS-aware accenting + correct syllabification, so this converter
 * skips the function-word stop list, baked-POS gating, and max-onset
 * resyllabification that delta_to_fe_utt applies to the hand-written
 * FE's output. */
static int parsed_to_fe_utt(const fe_parsed_t *parsed,
                            const char        *original_text,
                            int                phrase_id,
                            spfy_fe_utt_t     *out)
{
    memset(out, 0, sizeof *out);

    int n_words_phr = 0, n_syls_phr = 0, n_segs_phr = 0;
    for (int i = 0; i < parsed->n_words; i++) {
        if (parsed->words[i].phrase_id != phrase_id) continue;
        n_words_phr++;
        n_syls_phr += parsed->words[i].n_syllables;
        n_segs_phr += parsed->words[i].n_phonemes;
    }
    if (n_words_phr == 0) return SPFY_E_INVAL;

    uint32_t total_words = (uint32_t)n_words_phr + 2u;   /* + 2 _NULL_ pads */
    uint32_t total_syls  = (uint32_t)n_syls_phr  + 2u;
    uint32_t total_segs  = (uint32_t)n_segs_phr  + 2u;

    out->n_words     = total_words;
    out->n_syls      = total_syls;
    out->n_segs      = total_segs;
    /* Per-phrase terminator captured by the parser. Fall back to
     * full-text scan if the parser didn't get it (e.g. older trace
     * material with no per-phrase markers). */
    if (phrase_id >= 0 && phrase_id < parsed->n_phrase_terms
        && parsed->phrase_terms[phrase_id] != 0) {
        out->phrase_term = parsed->phrase_terms[phrase_id];
    } else {
        out->phrase_term = detect_phrase_term(original_text);
    }

    out->word_shareds = (uint32_t *)calloc(total_words, sizeof *out->word_shareds);
    out->word_names   = (char    **)calloc(total_words, sizeof *out->word_names);
    out->word_n_syls  = (uint32_t *)calloc(total_words, sizeof *out->word_n_syls);
    out->word_syls    = (uint32_t **)calloc(total_words, sizeof *out->word_syls);
    out->syl_stress   = (int32_t  *)calloc(total_syls,  sizeof *out->syl_stress);
    out->syl_accent   = (uint32_t *)calloc(total_syls,  sizeof *out->syl_accent);
    out->syl_n_segs   = (uint32_t *)calloc(total_syls,  sizeof *out->syl_n_segs);
    out->syl_segs     = (uint32_t **)calloc(total_syls,  sizeof *out->syl_segs);
    if (!out->word_shareds || !out->word_names || !out->word_n_syls
        || !out->word_syls || !out->syl_stress || !out->syl_accent
        || !out->syl_n_segs || !out->syl_segs) {
        spfy_fe_utt_free(out); return SPFY_E_NOMEM;
    }

    /* Leading silence pad (word 0, syl 0, seg 0; shared ids start at 1). */
    out->word_shareds[0] = 1;
    out->word_names[0]   = strdup("_NULL_");
    out->word_n_syls[0]  = 1;
    out->word_syls[0]    = (uint32_t *)calloc(1, sizeof **out->word_syls);
    if (!out->word_names[0] || !out->word_syls[0]) {
        spfy_fe_utt_free(out); return SPFY_E_NOMEM;
    }
    out->word_syls[0][0] = 1;
    out->syl_stress[0]   = 0;
    out->syl_accent[0]   = 0;
    out->syl_n_segs[0]   = 1;
    out->syl_segs[0]     = (uint32_t *)calloc(1, sizeof **out->syl_segs);
    if (!out->syl_segs[0]) { spfy_fe_utt_free(out); return SPFY_E_NOMEM; }
    out->syl_segs[0][0]  = 1;

    /* Content words. */
    uint32_t next_word_shared = 2, next_syl_shared = 2, next_seg_shared = 2;
    uint32_t syl_g_idx = 1;   /* global syllable index in out->syl_* arrays */
    uint32_t word_out_idx = 1;

    for (int wi = 0; wi < parsed->n_words; wi++) {
        const fe_parsed_word_t *w = &parsed->words[wi];
        if (w->phrase_id != phrase_id) continue;

        out->word_shareds[word_out_idx] = next_word_shared++;
        out->word_names[word_out_idx]   = strdup(w->text);
        out->word_n_syls[word_out_idx]  = (uint32_t)w->n_syllables;
        if (w->n_syllables > 0) {
            out->word_syls[word_out_idx] = (uint32_t *)calloc(
                (size_t)w->n_syllables, sizeof **out->word_syls);
            if (!out->word_names[word_out_idx] || !out->word_syls[word_out_idx]) {
                spfy_fe_utt_free(out); return SPFY_E_NOMEM;
            }
        }

        /* Walk syllables of this word (0..n_syllables-1) and partition
         * its phoneme list by syl_index. The parser's `.X` markers
         * already encode the engine-correct syllable boundaries —
         * unlike the hand-written FE which had to run max-onset to fix
         * its baked-dict's syl_id=0-for-all-phonemes bug. */
        for (int si = 0; si < w->n_syllables; si++) {
            uint32_t this_syl_shared = next_syl_shared++;
            out->word_syls[word_out_idx][si] = this_syl_shared;

            int first_pi = -1, last_pi = -1;
            for (int pi = 0; pi < w->n_phonemes; pi++) {
                if (w->phonemes[pi].syl_index == si) {
                    if (first_pi < 0) first_pi = pi;
                    last_pi = pi;
                }
            }
            if (first_pi < 0) {
                out->syl_stress[syl_g_idx] = 0;
                out->syl_accent[syl_g_idx] = 0;
                out->syl_n_segs[syl_g_idx] = 0;
                syl_g_idx++;
                continue;
            }
            const fe_parsed_phoneme_t *ph0 = &w->phonemes[first_pi];
            out->syl_stress[syl_g_idx] = (int32_t)ph0->syl_stress;
            out->syl_accent[syl_g_idx] =
                (ph0->accent[0] && strchr(ph0->accent, '*')) ? 1u : 0u;

            uint32_t n_seg_in_syl = (uint32_t)(last_pi - first_pi + 1);
            out->syl_n_segs[syl_g_idx] = n_seg_in_syl;
            out->syl_segs[syl_g_idx] = (uint32_t *)calloc(
                n_seg_in_syl, sizeof **out->syl_segs);
            if (!out->syl_segs[syl_g_idx]) {
                spfy_fe_utt_free(out); return SPFY_E_NOMEM;
            }
            for (uint32_t j = 0; j < n_seg_in_syl; j++) {
                out->syl_segs[syl_g_idx][j] = next_seg_shared++;
            }
            syl_g_idx++;
        }
        word_out_idx++;
    }

    /* Trailing silence pad. */
    uint32_t tail_w = total_words - 1u;
    uint32_t tail_s = total_syls  - 1u;
    out->word_shareds[tail_w] = next_word_shared++;
    out->word_names[tail_w]   = strdup("_NULL_");
    out->word_n_syls[tail_w]  = 1;
    out->word_syls[tail_w]    = (uint32_t *)calloc(1, sizeof **out->word_syls);
    if (!out->word_names[tail_w] || !out->word_syls[tail_w]) {
        spfy_fe_utt_free(out); return SPFY_E_NOMEM;
    }
    out->word_syls[tail_w][0] = next_syl_shared++;
    out->syl_stress[tail_s]   = 0;
    out->syl_accent[tail_s]   = 0;
    out->syl_n_segs[tail_s]   = 1;
    out->syl_segs[tail_s]     = (uint32_t *)calloc(1, sizeof **out->syl_segs);
    if (!out->syl_segs[tail_s]) { spfy_fe_utt_free(out); return SPFY_E_NOMEM; }
    out->syl_segs[tail_s][0]  = next_seg_shared++;
    return SPFY_OK;
}

/* Build the ARPAbet segment-name list directly from the hosted parser
 * output for a single phrase. Order matches what parsed_to_fe_utt's
 * syl_segs encoding implies (leading _NULL_ pad, content phonemes,
 * trailing pad). Caller frees the returned array (but not the strings,
 * which point into the parser's owned memory). */
static int build_segments_from_parsed(const fe_parsed_t *parsed,
                                      int                phrase_id,
                                      const char       ***out, uint32_t *out_n)
{
    int n_phons = 0;
    for (int wi = 0; wi < parsed->n_words; wi++) {
        if (parsed->words[wi].phrase_id == phrase_id)
            n_phons += parsed->words[wi].n_phonemes;
    }
    uint32_t total = (uint32_t)n_phons + 2u;
    const char **arr = (const char **)calloc(total, sizeof *arr);
    if (!arr) return SPFY_E_NOMEM;
    arr[0]            = "pau";
    arr[total - 1u]   = "pau";
    uint32_t k = 1;
    for (int wi = 0; wi < parsed->n_words; wi++) {
        const fe_parsed_word_t *w = &parsed->words[wi];
        if (w->phrase_id != phrase_id) continue;
        for (int pi = 0; pi < w->n_phonemes; pi++) {
            arr[k++] = w->phonemes[pi].arpabet;
        }
    }
    *out   = arr;
    *out_n = total;
    return SPFY_OK;
}


/* ARPAbet vowel-class check for Syl PSA re-syllabification. */
static int is_arpa_vowel(const char *a)
{
    if (!a) return 0;
    static const char *vs[] = {"aa","ae","ah","ao","aw","ax","ay","eh","er",
                                "ey","ih","ix","iy","ow","oy","uh","uw"};
    for (size_t i = 0; i < sizeof vs / sizeof vs[0]; ++i) {
        if (strcmp(a, vs[i]) == 0) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Viterbi join cost                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    const spfy_hash_t       *hash;
    const spfy_unit_table_t *units;
    float                    miss_default;
    /* Engine-faithful F0-prob curve (VIN `hist` chunk + voice+0xc8).
     * On hash miss the engine adds:
     *   miss_cost = MISSING_JOIN_COST
     *             + (gate ? F0_EDGE_CHANGE_WEIGHT * curve[idx] : 0)
     * gate = (curr.c6c > 20) && (prev.c80 < 15) && (prev.c7c > 20).
     * idx  = clamp(curr.c6c - sub_off - prev.c7c, 0, max_idx-1).
     *
     * curve points into VIN bytes; sub_off is signed (Tom: -50);
     * max_idx is the curve length (Tom: 100). When curve == NULL the
     * callback uses miss_default for any miss (legacy behaviour). */
    const float             *curve;
    int32_t                  curve_max_idx;
    int32_t                  curve_sub_off;
    float                    f0_edge_change_weight;
    float                    missing_join_cost;
} join_ctx_t;

/* Parse VIN `hist` sub-chunks (head + data) and populate the curve params.
 * On any malformed-chunk error returns a "no curve" state (legacy miss). */
static void load_f0_hist_curve(const spfy_vin_t *vin, join_ctx_t *jc)
{
    jc->curve = NULL;
    jc->curve_max_idx = 0;
    jc->curve_sub_off = 0;
    if (!vin || !vin->hist || vin->hist_n < 16) return;
    /* `head` 8 bytes (max_idx, sub_off) then `data` n*4 bytes. We don't
     * use the riff iterator here -- the chunk is small + well-formed. */
    const uint8_t *p   = vin->hist;
    const uint8_t *end = vin->hist + vin->hist_n;
    while (p + 8 <= end) {
        uint32_t fcc = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                     | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        uint32_t sz  = (uint32_t)p[4] | ((uint32_t)p[5] << 8)
                     | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
        const uint8_t *body = p + 8;
        if (body + sz > end) return;
        if (fcc == 0x64616568u /* 'head' LE */ && sz >= 8) {
            uint32_t mx, off;
            memcpy(&mx,  body,     4);
            memcpy(&off, body + 4, 4);
            jc->curve_max_idx = (int32_t)mx;
            jc->curve_sub_off = (int32_t)off;
        } else if (fcc == 0x61746164u /* 'data' LE */) {
            jc->curve = (const float *)body;
        }
        p = body + sz;
        if (sz & 1) ++p;
    }
}

/* Engine-faithful FUN_08e8b620 join cost. Same-rec adjacent
 * (prev_join_key+1 == curr_uid && unit.flag_b) -> 0. Hash hit -> cell
 * cost. Hash miss + curve gate fires -> MISSING_JOIN_COST + F0_EDGE *
 * curve[clamp(curr_c6c - sub_off - prev_c7c, 0, max-1)]. Hash miss + no
 * gate -> MISSING_JOIN_COST + 0. Falls back to legacy miss_default if
 * the caller didn't supply curve params (curve == NULL). */
static float dag_join_cb(uint32_t prev_uid_join_key, uint32_t curr_uid,
                         uint32_t prev_slot, uint32_t prev_idx,
                         uint32_t curr_slot, uint32_t curr_idx,
                         int32_t  prev_c7c,  int32_t  prev_c80,
                         uint32_t curr_c6c,  void    *user)
{
    (void)prev_slot; (void)prev_idx; (void)curr_slot; (void)curr_idx;
    const join_ctx_t *jc = (const join_ctx_t *)user;
    float cost = 0.0f;
    const char *path = "";
    /* Same-rec adjacent bypass — applies regardless of curve. */
    if (curr_uid == prev_uid_join_key + 1u && curr_uid > 0u) {
        spfy_unit_record_t r;
        if (spfy_unit_record_get(jc->units, curr_uid, &r) == SPFY_OK
            && r.flag_b) {
            cost = 0.0f;
            path = "same_rec";
            goto dump;
        }
    }
    int rc = spfy_hash_lookup(jc->hash, prev_uid_join_key, curr_uid, &cost);
    if (rc == SPFY_OK) {
        path = "hash_hit";
        goto dump;
    }
    /* Hash miss — engine-exact (FUN_08e8b620):
     *   miss = MISSING_JOIN_COST + (gate ? F0_EDGE_CHANGE_WEIGHT * curve[idx]
     *                                    : CCOS_DEFAULT)
     * For Tom: MISSING_JOIN_COST=0 (VCF unset), CCOS_DEFAULT=0
     * (_DAT_08e9852c), F0_EDGE_CHANGE_WEIGHT=0.6.
     *
     * Gate condition: curr.c6c > 20 AND prev.c80 < 15 AND prev.c7c > 20.
     * Curve idx: clamp((curr.c6c - sub_off) - prev.c7c, 0, max_idx-1). */
    if (!jc->curve) { cost = jc->miss_default; path = "no_curve"; goto dump; }
    {
        float curve_val = 0.0f;     /* CCOS_DEFAULT = 0 */
        path = "miss_no_gate";
        if ((int32_t)curr_c6c > 20 && prev_c80 < 15 && prev_c7c > 20) {
            int32_t idx = (int32_t)curr_c6c - jc->curve_sub_off - prev_c7c;
            if (idx < 0) idx = 0;
            else if (idx >= jc->curve_max_idx) idx = jc->curve_max_idx - 1;
            curve_val = jc->f0_edge_change_weight * jc->curve[idx];
            path = "miss_gate";
        }
        cost = jc->missing_join_cost + curve_val;
    }
dump:
    if (getenv("SPFY_JOIN_DUMP")) {
        fprintf(stderr, "{\"join\":1,\"prev_slot\":%u,\"prev_idx\":%u,"
                        "\"curr_slot\":%u,\"curr_idx\":%u,"
                        "\"prev_jk\":%u,\"curr_uid\":%u,"
                        "\"prev_c7c\":%d,\"prev_c80\":%d,\"curr_c6c\":%u,"
                        "\"cost\":%.6f,\"path\":\"%s\"}\n",
                prev_slot, prev_idx, curr_slot, curr_idx,
                prev_uid_join_key, curr_uid,
                prev_c7c, prev_c80, curr_c6c,
                (double)cost, path);
    }
    return cost;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */


/* SPR single-char -> ARPAbet name. Lifted from bin/spfy_dumpwav.c. */
static const char *spr_to_arpabet(char c)
{
    static const struct { char spr; const char *arpa; } TABLE[] = {
        {'a',"aa"}, {'A',"ae"}, {'H',"ah"}, {'c',"ao"}, {'W',"aw"},
        {'x',"ax"}, {'Y',"ay"}, {'b',"b"},  {'C',"ch"}, {'d',"d"},
        {'D',"dh"}, {'F',"dx"}, {'E',"eh"}, {'N',"en"}, {'R',"er"},
        {'e',"ey"}, {'f',"f"},  {'g',"g"},  {'h',"hh"}, {'I',"ih"},
        {'X',"ix"}, {'i',"iy"}, {'J',"jh"}, {'k',"k"},  {'l',"l"},
        {'m',"m"},  {'n',"n"},  {'G',"ng"}, {'o',"ow"}, {'O',"oy"},
        {'p',"p"},  {'r',"r"},  {'s',"s"},  {'S',"sh"}, {'t',"t"},
        {'T',"th"}, {'U',"uh"}, {'u',"uw"}, {'v',"v"},  {'w',"w"},
        {'y',"y"},  {'z',"z"},  {'Z',"zh"},
    };
    for (size_t i = 0; i < sizeof TABLE / sizeof TABLE[0]; ++i)
        if (TABLE[i].spr == c) return TABLE[i].arpa;
    return NULL;
}

/* Parse `\\![SPR]` and emit an FE tagged-output string. SPR `.0`/`.1`
 * (`.2`) starts a syllable with given stress; subsequent single chars
 * are phonemes. Output matches the FE's own tagged format so
 * fe_parse_tagged_output() can consume it.
 *
 * Output: #{. pau(p25) <SPR (0,N) undef,STRESS [.S,H* arpa(p100) ... ] > pau(p50) } %%
 *
 * Returns positive byte count on success, 0 on parse failure. */
static int spr_inline_to_tagged(const char *text, char *out, size_t out_n)
{
    const char *p = strchr(text, '[');
    if (!p) return 0;
    p++;
    const char *end_br = strrchr(p, ']');
    if (!end_br) return 0;
    int max_stress = 0;
    for (const char *q = p; q < end_br; ++q)
        if (*q == '.' && q + 1 < end_br
            && q[1] >= '0' && q[1] <= '9'
            && (q[1] - '0') > max_stress) max_stress = q[1] - '0';
    char *o = out;
    char *eo = out + out_n - 1;
    int n = snprintf(o, (size_t)(eo - o),
        "#{. pau(p25) <SPR (0,%d) undef,%d [",
        (int)(end_br - p), max_stress);
    if (n < 0) return 0;
    o += n;
    int first_syl = 1;
    for (const char *q = p; q < end_br && o < eo; ) {
        if (*q == ' ' || *q == '\t') { ++q; continue; }
        if (*q == '.' && q + 1 < end_br
            && q[1] >= '0' && q[1] <= '9') {
            int stress = q[1] - '0';
            q += 2;
            n = snprintf(o, (size_t)(eo - o),
                "%s.%d%s ",
                first_syl ? "" : " ",
                stress,
                first_syl ? ",H*" : "");
            if (n < 0) break;
            o += n;
            first_syl = 0;
            continue;
        }
        const char *arpa = spr_to_arpabet(*q);
        ++q;
        if (!arpa) continue;
        n = snprintf(o, (size_t)(eo - o), "%s(p100) ", arpa);
        if (n < 0) break;
        o += n;
    }
    n = snprintf(o, (size_t)(eo - o), "] > pau(p50) } %%%%");
    if (n < 0) return 0;
    o += n;
    *o = '\0';
    return (int)(o - out);
}

/* Per-call synth: text -> FE -> USel -> WSOLA -> sink. The voice is
 * loaded once by the caller (CLI main() or SAPI Speak()) and reused.
 * The sink is any open spfy_wav_writer_t (file or callback mode); we
 * neither open nor close it. Inputs starting with `\![SPR]` are routed
 * through the FE bypass (spr_inline_to_tagged + spfy_fe_synth_tagged)
 * so the FE DLL never sees the escape syntax (it would otherwise
 * synthesize the literal characters). */
int spfy_synth_to_sink(spfy_voice_t *v, const char *text,
                       spfy_wav_writer_t *sink,
                       const spfy_synth_callbacks_t *cb,
                       spfy_synth_stats_t *out_stats)
{
    spfy_prosody_hints_t hints = {0};
    spfy_fe_utt_t fe_utt = {0};
    spfy_slot_tree_t tree = {0};
    spfy_slice_ctx_table_t slice_ctx = {0};
    spfy_sp_target_table_t sp_tab = {0};
    spfy_slot_preds_table_t preds_tab = {0};
    uint32_t *q5_per_slot = NULL;
    uint8_t  *q5_has = NULL;
    uint32_t *hp_to_post = NULL;
    uint32_t *post_to_hp = NULL;
    uint32_t *hp_word_idx = NULL;
    /* Per-hp SSML / Balabolka prosody overrides (signed; 0 = neutral).
     * Populated alongside hp_word_idx; read in the per-slot CART block
     * so f0tr_mean / durt_mean adjust per-word from user markup. */
    int8_t   *hp_pitch_st = NULL;
    int8_t   *hp_rate_pct = NULL;
    spfy_viterbi_slot_t *vslots = NULL;
    uint32_t **cbuf = NULL;
    float    **tbuf = NULL;
    uint8_t **cand_c68 = NULL;
    uint8_t **cand_c6c = NULL;
    uint8_t **cand_c70 = NULL;
    uint8_t **cand_c78 = NULL;
    uint8_t **anchor_c68 = NULL;
    uint8_t **anchor_c6c = NULL;
    uint8_t **anchor_c70 = NULL;
    uint8_t **anchor_c78 = NULL;
    uint32_t **anchor_cands  = NULL;
    uint32_t **anchor_jks    = NULL;
    float    **anchor_target = NULL;
    uint32_t  *anchor_n      = NULL;
    spfy_viterbi_dag_slot_t *dag_slots = NULL;
    uint32_t  n_slots = 0;
    int rc;
    spfy_prosody_hints_init(&hints);

    if (getenv("SPFY_VOICING_DUMP")) {
        /* Diagnostic: dump voicing[] table to verify per-hp_class
         * voiced/voiceless mapping. Index = 2*phone_id_alpha + side
         * (alphabetical phone order from VCF "name" v->feat chunk). */
        fprintf(stderr, "{\"voicing\":1,\"n\":%u,\"vals\":[",
                v->av.voicing_n);
        for (uint32_t i = 0; i < v->av.voicing_n; ++i)
            fprintf(stderr, "%s%u", i ? "," : "",
                    v->av.voicing ? v->av.voicing[i] : 0u);
        fprintf(stderr, "]}\n");
    }

    /* Hosted FE: drive the public synth_text API; the parsed result is
     * stashed on `v->fe` and retrieved via spfy_fe_get_parsed. We no
     * longer maintain a spfy_fe_delta_t — parsed_to_fe_utt feeds the
     * downstream slot-tree pipeline directly. */
    {
        spfy_fe_utterance_t *utt_unused = NULL;
        int is_inline_spr_ = 0;
        {
            const char *tp = text;
            while (*tp == ' ' || *tp == '\t') tp++;
            is_inline_spr_ = (tp[0] == '\\' && tp[1] == '!' && tp[2] == '[');
        }
        if (is_inline_spr_) {
            char tagged_buf_[4096];
            if (spr_inline_to_tagged(text, tagged_buf_, sizeof tagged_buf_) <= 0) {
                fprintf(stderr, "spr_inline_to_tagged: bad \\![...] input\n");
                rc = SPFY_E_INVAL; goto fail;
            }
            rc = spfy_fe_synth_tagged(v->fe, tagged_buf_, &hints, &utt_unused);
        } else if (getenv("SPFY_FE_INTERNAL")) {
            /* In-house FE path: text → fe_internal → tagged-text →
             * spfy_fe_synth_tagged → slot tree. Skips the DLL entirely.
             * Quality is lower than the hosted FE (no engine-specific
             * lexicon, simple syllabifier, no full prosody model) but
             * portable to ARM and platforms where the DLL can't run. */
            /* 64 KB tagged buffer — accommodates long passages (the
             * sioux pangram in the audit corpus runs ~30 KB tagged). */
            static char tagged_buf_[65536];
            int frc = spfy_fe_internal_text_to_tagged(
                text, tagged_buf_, sizeof tagged_buf_);
            if (frc < 0) {
                fprintf(stderr, "spfy_fe_internal_text_to_tagged failed\n");
                rc = SPFY_E_INVAL; goto fail;
            }
            rc = spfy_fe_synth_tagged(v->fe, tagged_buf_, &hints, &utt_unused);
        } else {
            rc = spfy_fe_synth_text(v->fe, text, &hints, &utt_unused);
        }
        if (rc != SPFY_OK) {
            fprintf(stderr, "spfy_fe_synth_text failed: %s\n",
                    spfy_strerror(rc));
            goto fail;
        }
        spfy_fe_utterance_free(utt_unused);
    }
    const fe_parsed_t *parsed =
        (const fe_parsed_t *)spfy_fe_get_parsed(v->fe);
    if (!parsed || parsed->n_words == 0) {
        /* Valid input that produces no audio (e.g. lone period, just
         * whitespace, an empty SSML frag). Return success with zero
         * samples emitted — NOT an error. Returning failure causes
         * SAPI consumers (Balabolka, NVDA) to abort the entire audio
         * playback queue, dropping previously-queued audio from
         * earlier frags in the same utterance. Lone-period frags
         * arrive routinely in multi-fragment Balabolka SSML splits. */
        rc = SPFY_OK; goto cleanup;
    }

    /* Multi-utterance synthesis. Engine treats each FE phrase (split at
     * commas/semicolons/colons by stage_textnorm) as a separate USel
     * utterance with its own pad slots, own DAG, own audio chunk. We
     * loop over phrase IDs and run the per-utterance pipeline once per
     * phrase, concatenating audio with inter-phrase silence (~200 ms).
     *
     * SPFY_FIRST_PHRASE_ONLY=1 (audit/debugging) limits the loop to
     * phrase 0 only. SPFY_INTERPHRASE_MS overrides the silence (default
     * 200, max 500). */
    uint32_t max_phrase_id = 0;
    for (int i = 0; i < parsed->n_words; i++) {
        if ((uint32_t)parsed->words[i].phrase_id > max_phrase_id)
            max_phrase_id = (uint32_t)parsed->words[i].phrase_id;
    }
    uint32_t n_phrases = max_phrase_id + 1u;
    int first_phrase_only = (getenv("SPFY_FIRST_PHRASE_ONLY") != NULL);
    if (first_phrase_only) n_phrases = 1u;

    /* Inter-phrase silence — DEFAULT OFF as of 2026-05-19 evening.
     *
     * The FE pad slots at each phrase boundary already render natural
     * silence: the leading + trailing slots of every utt have
     * ctx[2]∈{64,65} (HP_PAU_L/R) and the unit selector pulls real
     * recorded `pau` halfphone units for them. Those units carry the
     * recording's low-level noise floor; concatenating two of them
     * across an utt boundary gives the listener a natural breath
     * pause. Engine does exactly this.
     *
     * The legacy 200 ms (and later per-FE-pause-driven) ZERO-padding
     * push that lived here added clinical digital silence on top —
     * which the WSOLA crossfade then merged with the real speech, but
     * with the silence-floor at TRUE zero rather than the recording
     * noise floor. The resulting silence→speech transitions sounded
     * "edited" / "hard-cut" — diagnosed via envelope-flux analysis:
     * with the zero pad, silence-onset attacks ramped in ≤5 ms (one
     * 5 ms frame); with the engine's natural pad units, attacks ramp
     * in 20-35 ms.
     *
     * To restore the old behaviour (e.g. if FE pad duration is too
     * short for your input), set SPFY_INTERPHRASE_MS=<N> with N in
     * milliseconds. SPFY_INTERPHRASE_MS=0 explicitly suppresses. */
    int inter_phrase_ms_override = -1;
    {
        const char *e = getenv("SPFY_INTERPHRASE_MS");
        if (e) {
            inter_phrase_ms_override = atoi(e);
            if (inter_phrase_ms_override < 0)   inter_phrase_ms_override = 0;
            if (inter_phrase_ms_override > 500) inter_phrase_ms_override = 500;
        }
    }

    /* Sink is caller-owned (CLI opened a file, SAPI a callback). WSOLA
     * streams all phrases into it; the streamer state itself is per-call. */
    spfy_wsola_streamer_t ws;
    spfy_wsola_init(&ws, sink);

    /* Cumulative stats across phrases. */
    size_t total_played = 0, total_skipped = 0, total_paired_same = 0,
           total_paired_cross = 0, total_interword_pauses = 0;

    /* sentence_idx_in_para tracks engine's `*param_1` value in
     * FUN_08e8c7d0 — starts at 0, increments for each non-end-punct
     * phrase ('.', '?', '!' reset to 0; ',', ';', etc increment).
     * Affects the slot's sp3 (wordInPhrase) code for words at
     * syl_idx=0 — phrase-initial words get code 5 only when
     * sentence_idx_in_para==0 AND voice config_d4==0; otherwise code 1.
     * SPFY_NO_SENTENCE_IDX_PARA=1 reverts to the legacy hardcoded 0. */
    uint32_t sentence_idx_in_para = 0;

    for (uint32_t phrase_idx = 0; phrase_idx < n_phrases; ++phrase_idx) {


    /* Hosted FE: skip empty phrase ids that produced no words. */
    {
        int phrase_has_words = 0;
        for (int i = 0; i < parsed->n_words; i++) {
            if ((uint32_t)parsed->words[i].phrase_id == phrase_idx) {
                phrase_has_words = 1; break;
            }
        }
        if (!phrase_has_words) continue;
    }

    if (getenv("SPFY_MULTI_DEBUG")) {
        int n_w = 0, n_p = 0;
        for (int i = 0; i < parsed->n_words; i++) {
            if ((uint32_t)parsed->words[i].phrase_id == phrase_idx) {
                n_w++; n_p += parsed->words[i].n_phonemes;
            }
        }
        fprintf(stderr, "[multi] phrase %u: n_word=%d n_phon=%d (hosted)\n",
                phrase_idx, n_w, n_p);
    }

    /* Build the trace-format spfy_fe_utt_t from the FE output, then
     * drive the validated slot-tree pipeline (BuildGraph + LinkGraph +
     * derive_slice_ctx + derive_sp_targets + derive_q5_table). The
     * slot-tree-based derivation is validated bit-exact (822/822)
     * against captured engine traces. */
    const char **seg_names = NULL;
    uint32_t n_segs_arr = 0;
    if ((rc = parsed_to_fe_utt(parsed, text, (int)phrase_idx, &fe_utt)) != SPFY_OK
        || (rc = build_segments_from_parsed(parsed, (int)phrase_idx,
                                            &seg_names, &n_segs_arr)) != SPFY_OK) {
        goto fail;
    }
    /* delta stays alive across the whole phrase loop; we restore its
     * contents from delta_backup_tokens at the start of each iteration.
     * Final spfy_fe_delta_free happens in the cleanup block. */

    if ((rc = spfy_build_graph(&fe_utt, &tree)) != SPFY_OK) {
        free(seg_names); goto fail;
    }
    if ((rc = spfy_derive_slice_ctx(&tree, seg_names, n_segs_arr, &slice_ctx))
        != SPFY_OK
        || (rc = spfy_derive_sp_targets(&tree, &fe_utt,
                getenv("SPFY_NO_SENTENCE_IDX_PARA") ? 0 : sentence_idx_in_para,
                0, &sp_tab))
           != SPFY_OK) {
        free(seg_names); goto fail;
    }
    /* SPFY_SP_TARGET_DUMP=1: emit our per-HP sp[0..4] for offline diff
     * against engine's inner_scorer.sp_target. */
    if (getenv("SPFY_SP_TARGET_DUMP")) {
        for (uint32_t s = 0; s < tree.n_slots; ++s) {
            if (tree.slots[s].kind != SPFY_SK_HALFPHONE) continue;
            if (!sp_tab.has[s]) continue;
            fprintf(stderr,
                "{\"sp_target\":1,\"slot\":%u,\"sp\":[%u,%u,%u,%u,%u]}\n",
                s, sp_tab.sp[s][0], sp_tab.sp[s][1], sp_tab.sp[s][2],
                sp_tab.sp[s][3], sp_tab.sp[s][4]);
        }
    }
    q5_per_slot = (uint32_t *)calloc(tree.n_slots, sizeof *q5_per_slot);
    q5_has      = (uint8_t  *)calloc(tree.n_slots, sizeof *q5_has);
    if (!q5_per_slot || !q5_has) {
        rc = SPFY_E_NOMEM; free(seg_names); goto fail;
    }
    if ((rc = spfy_derive_q5_table(&tree, (const char **)fe_utt.word_names,
                                    fe_utt.n_words, q5_per_slot, q5_has))
        != SPFY_OK) {
        free(seg_names); goto fail;
    }

    /* Engine-faithful predecessor topology (FUN_08e8c700 LinkGraph). For
     * each slot S, preds = exit_chain(left-sibling of nearest ancestor
     * with a left sibling) = [P, P.last_child, ..., leaf]. This produces
     * the parallel-path DAG where Word/Syl anchor slots are alternative
     * routes that bypass the per-HP path through their span. */
    if ((rc = spfy_link_graph(&tree, &preds_tab)) != SPFY_OK) {
        free(seg_names); goto fail;
    }
    if (getenv("SPFY_DUMP_TREE")) {
        for (uint32_t s = 0; s < tree.n_slots; ++s) {
            const spfy_slot_node_t *n = &tree.slots[s];
            const char *ks = (n->kind == SPFY_SK_HALFPHONE) ? "HP"
                            : (n->kind == SPFY_SK_SYLLABLE) ? "SYL"
                            : (n->kind == SPFY_SK_WORD)     ? "WORD"
                            : "PHR";
            fprintf(stderr, "  tree[%u] kind=%s parent=%u nchildren=%u",
                    s, ks, n->parent_idx, n->n_children);
            if (n->n_children > 0) {
                fprintf(stderr, " children=[");
                for (uint32_t i = 0; i < n->n_children; ++i)
                    fprintf(stderr, "%s%u", i?",":"", n->child_idx[i]);
                fprintf(stderr, "]");
            }
            fprintf(stderr, " preds=[");
            for (uint32_t i = 0; i < preds_tab.per_slot[s].n_preds; ++i)
                fprintf(stderr, "%s%u", i?",":"", preds_tab.per_slot[s].preds[i]);
            fprintf(stderr, "]\n");
        }
    }

    /* Iterate halfphone-leaf slots only. The Viterbi runs over those.
     * Map hp_idx (compact) <-> post_idx (slot tree). */
    uint32_t n_hp = tree.n_halfphone;
    hp_to_post = (uint32_t *)calloc(n_hp, sizeof *hp_to_post);
    if (!hp_to_post) {
        rc = SPFY_E_NOMEM; free(seg_names); goto fail;
    }
    post_to_hp = (uint32_t *)malloc(tree.n_slots * sizeof *post_to_hp);
    if (!post_to_hp) {
        rc = SPFY_E_NOMEM; free(seg_names); goto fail;
    }
    for (uint32_t s = 0; s < tree.n_slots; ++s) post_to_hp[s] = UINT32_MAX;
    {
        uint32_t k = 0;
        for (uint32_t s = 0; s < tree.n_slots; ++s) {
            if (tree.slots[s].kind == SPFY_SK_HALFPHONE) {
                hp_to_post[k] = s;
                post_to_hp[s] = k;
                ++k;
            }
        }
    }
    fprintf(stdout, "FE produced %u halfphone slots for text: %s\n", n_hp, text);
    /* Also emit a stderr marker so multi-phrase audit parsers can detect
     * phrase boundaries reliably even when stdout/stderr buffers are
     * flushed out-of-order through a merged pipe. JSON slot dumps and
     * this marker share the stderr stream — guaranteed interleaved.
     * Flush stdout first so previous stdout chunks (path debug, etc)
     * land in the pipe BEFORE this marker; without the flush, prior
     * stdout output appended to the start of the marker line breaks
     * the parser's regex anchor. */
    fflush(stdout);
    fprintf(stderr, "\nspfy_phrase_boundary: phrase_idx=%u n_hp=%u\n",
            phrase_idx, n_hp);
    fflush(stderr);
    if (n_hp == 0) { rc = SPFY_E_FORMAT; goto fail; }

    /* Per-tree-slot anchor storage (populated by PSA below). */
    anchor_cands  = (uint32_t **)calloc(tree.n_slots, sizeof *anchor_cands);
    anchor_jks    = (uint32_t **)calloc(tree.n_slots, sizeof *anchor_jks);
    anchor_target = (float    **)calloc(tree.n_slots, sizeof *anchor_target);
    anchor_n      = (uint32_t  *)calloc(tree.n_slots, sizeof *anchor_n);
    anchor_c68    = (uint8_t **)calloc(tree.n_slots, sizeof *anchor_c68);
    anchor_c6c    = (uint8_t **)calloc(tree.n_slots, sizeof *anchor_c6c);
    anchor_c70    = (uint8_t **)calloc(tree.n_slots, sizeof *anchor_c70);
    anchor_c78    = (uint8_t **)calloc(tree.n_slots, sizeof *anchor_c78);
    if (!anchor_cands || !anchor_jks || !anchor_target || !anchor_n
        || !anchor_c68 || !anchor_c6c || !anchor_c70 || !anchor_c78) {
        rc = SPFY_E_NOMEM; free(seg_names); goto fail;
    }

    /* Build Viterbi inputs per halfphone slot. */
    vslots = (spfy_viterbi_slot_t *)calloc(n_hp, sizeof *vslots);
    cbuf   = (uint32_t **)calloc(n_hp, sizeof *cbuf);
    tbuf   = (float    **)calloc(n_hp, sizeof *tbuf);
    cand_c68 = (uint8_t **)calloc(n_hp, sizeof *cand_c68);
    cand_c6c = (uint8_t **)calloc(n_hp, sizeof *cand_c6c);
    cand_c70 = (uint8_t **)calloc(n_hp, sizeof *cand_c70);
    cand_c78 = (uint8_t **)calloc(n_hp, sizeof *cand_c78);
    hp_word_idx = (uint32_t *)calloc(n_hp, sizeof *hp_word_idx);
    if (!vslots || !cbuf || !tbuf || !hp_word_idx
        || !cand_c68 || !cand_c6c || !cand_c70 || !cand_c78) {
        rc = SPFY_E_NOMEM; free(seg_names); goto fail;
    }
    for (uint32_t hp = 0; hp < n_hp; ++hp) {
        uint32_t s = hp_to_post[hp];
        /* Walk parent chain HP -> SYL -> WORD. */
        uint32_t syl_post = tree.slots[s].parent_idx;
        uint32_t word_post = (syl_post < tree.n_slots)
                             ? tree.slots[syl_post].parent_idx
                             : UINT32_MAX;
        hp_word_idx[hp] = word_post;
    }

    /* Per-hp SSML / Balabolka prosody overrides. Built once from the
     * parsed FE output so the CART block reads them in O(1) per slot.
     * Tree's WORD slots are emitted in fe_parsed_t order, so we count
     * them up to map tree-post-order word index → parsed->words[]. */
    hp_pitch_st = (int8_t *)calloc(n_hp, sizeof *hp_pitch_st);
    hp_rate_pct = (int8_t *)calloc(n_hp, sizeof *hp_rate_pct);
    if (!hp_pitch_st || !hp_rate_pct) {
        rc = SPFY_E_NOMEM; free(seg_names); goto fail;
    }
    {
        /* Build word_post → parsed_word_idx map (UINT32_MAX = not a word). */
        uint32_t *word_post_to_parsed =
            (uint32_t *)malloc(tree.n_slots * sizeof *word_post_to_parsed);
        if (word_post_to_parsed) {
            for (uint32_t s = 0; s < tree.n_slots; ++s)
                word_post_to_parsed[s] = UINT32_MAX;
            uint32_t wc = 0;
            for (uint32_t s = 0; s < tree.n_slots; ++s) {
                if (tree.slots[s].kind == SPFY_SK_WORD)
                    word_post_to_parsed[s] = wc++;
            }
            const fe_parsed_t *parsed_ro =
                (const fe_parsed_t *)spfy_fe_get_parsed(v->fe);
            if (parsed_ro) {
                for (uint32_t hp = 0; hp < n_hp; ++hp) {
                    uint32_t wpost = hp_word_idx[hp];
                    if (wpost < tree.n_slots) {
                        uint32_t pi = word_post_to_parsed[wpost];
                        if (pi != UINT32_MAX && (int)pi < parsed_ro->n_words) {
                            hp_pitch_st[hp] = parsed_ro->words[pi].pitch_st;
                            hp_rate_pct[hp] = parsed_ro->words[pi].rate_pct;
                        }
                    }
                }
            }
            free(word_post_to_parsed);
        }
    }

    uint32_t total_cands = 0, n_empty = 0;
    int verbose = (getenv("SPFY_SYNTH_DEBUG") != NULL);
    for (uint32_t hp = 0; hp < n_hp; ++hp) {
        uint32_t s = hp_to_post[hp];
        const uint32_t *ctx5 = slice_ctx.ctx[s];
        const uint32_t *sp5  = sp_tab.sp[s];
        uint32_t q5 = q5_per_slot[s];

        /* PRSL pool query: triphone context = ctx[1]*10000+ctx[2]*100+ctx[3].
         *
         * Engine fallback on exact-key miss (empirical 2026-05-14,
         * project_prsl_92_fallback): substitute 92 (the silence/boundary
         * marker) for the side that represents "same phone, other half" —
         * for a LEFT-half slot (side 0), substitute right; for a RIGHT-half
         * slot (side 1), substitute left. Verified across all 20 pool_n=0
         * cases in phn_001/003/023/039 + mp_031/038: every engine-returned
         * UID-set is exactly the contents of the 92-substituted key.
         * SPFY_NO_PRSL_92_FALLBACK=1 disables. */
        uint32_t ck = spfy_prsl_context_key(ctx5[1], ctx5[2], ctx5[3]);
        const uint32_t *pool = NULL;
        uint32_t pool_n = 0;
        spfy_prsl_lookup(&v->prsl, ck, &pool, &pool_n);
        if (pool_n == 0 && !getenv("SPFY_NO_PRSL_92_FALLBACK")) {
            uint32_t side = ctx5[2] & 1u;
            uint32_t l_fb = side ? 92u : ctx5[1];
            uint32_t r_fb = side ? ctx5[3] : 92u;
            uint32_t ck_fb = spfy_prsl_context_key(l_fb, ctx5[2], r_fb);
            spfy_prsl_lookup(&v->prsl, ck_fb, &pool, &pool_n);
        }
        if (pool_n > MAX_CANDS_PER_SLOT) pool_n = MAX_CANDS_PER_SLOT;

        /* Build a tiny adapter slot for the cart_feat callback (it expects
         * spfy_fe_slot_t.ctx + .sp). */
        spfy_fe_slot_t adapter = {0};
        for (int i = 0; i < 5; ++i) {
            adapter.ctx[i] = (int32_t)ctx5[i];
            adapter.sp[i]  = sp5[i];
        }

        if (verbose) {
            float dm_dbg = 0, dv_dbg = 0, fm_dbg = 0, fv_dbg = 0;
            /* Plan 03-04: silence-pad CART traversal for the debug-JSON
             * emit path. The audit harness reads this output;
             * SPFY_NO_SILENCE_CART=1 reverts to old skip-on-silence. */
            int silence_slot_dbg = (ctx5[2] == 64 || ctx5[2] == 65);
            cart_feat_ctx_t cfc_dbg = { &adapter, q5, 0 };
            if (!silence_slot_dbg || !getenv("SPFY_NO_SILENCE_CART")) {
                uint32_t didx = tom_swap(ctx5[2] >> 1);
                if (didx < v->durt_cart.n_trees)
                    spfy_cart_traverse(&v->durt_cart, didx, cart_feat, &cfc_dbg,
                                       &dm_dbg, &dv_dbg);
                if (v->f0tr_cart.n_trees > 0) {
                    cfc_dbg.is_f0tr = 1;
                    spfy_cart_traverse(&v->f0tr_cart, 0, cart_feat, &cfc_dbg,
                                       &fm_dbg, &fv_dbg);
                    cfc_dbg.is_f0tr = 0;
                }
            }
            fprintf(stderr, "{\"hp\":%u,\"post\":%u,\"ctx\":[%u,%u,%u,%u,%u],"
                            "\"sp\":[%u,%u,%u,%u,%u],\"q5\":%u,"
                            "\"durt_mean\":%.4f,\"durt_var\":%.4f,"
                            "\"f0tr_mean\":%.4f,\"f0tr_var\":%.4f,"
                            "\"pool_n\":%u",
                    hp, s,
                    ctx5[0], ctx5[1], ctx5[2], ctx5[3], ctx5[4],
                    sp5[0], sp5[1], sp5[2], sp5[3], sp5[4],
                    q5,
                    (double)dm_dbg, (double)dv_dbg,
                    (double)fm_dbg, (double)fv_dbg,
                    pool_n);
            fprintf(stderr, ",\"cands\":[");
            /* Cap at 16 for the audit output (matches master_compare's
             * legacy expectation). SPFY_FULL_POOL_DUMP=1 emits the
             * entire pool for pool-overlap diagnostics. */
            uint32_t cand_cap = getenv("SPFY_FULL_POOL_DUMP") ? pool_n : 16u;
            uint32_t dump_n = pool_n < cand_cap ? pool_n : cand_cap;
            for (uint32_t i = 0; i < dump_n; ++i) {
                fprintf(stderr, "%s%u", i ? "," : "",
                        pool ? pool[i] : SILENCE_SENTINEL_UID);
            }
            fprintf(stderr, "]}\n");
        }
        if (pool_n == 0) ++n_empty;

        /* Triphone miss: fall back to all v->units with this phone_center+side. */
        uint32_t fb_hp = ctx5[2];
        if (pool_n == 0 && fb_hp < v->hpc_buckets && v->bucket_n[fb_hp] > 0) {
            pool   = v->bucket[fb_hp];
            pool_n = v->bucket_n[fb_hp];
            if (pool_n > MAX_CANDS_PER_SLOT) pool_n = MAX_CANDS_PER_SLOT;
        }
        if (pool_n == 0) {
            cbuf[hp] = (uint32_t *)calloc(1, sizeof **cbuf);
            tbuf[hp] = (float    *)calloc(1, sizeof **tbuf);
            if (!cbuf[hp] || !tbuf[hp]) { rc = SPFY_E_NOMEM; goto fail; }
            cbuf[hp][0] = SILENCE_SENTINEL_UID;
            tbuf[hp][0] = 0.0f;
            vslots[hp].cands = cbuf[hp];
            vslots[hp].target_cost = tbuf[hp];
            vslots[hp].n_cands = 1;
            continue;
        }

        cbuf[hp] = (uint32_t *)calloc(pool_n, sizeof **cbuf);
        tbuf[hp] = (float    *)calloc(pool_n, sizeof **tbuf);
        cand_c68[hp] = (uint8_t *)calloc(pool_n, sizeof **cand_c68);
        cand_c6c[hp] = (uint8_t *)calloc(pool_n, sizeof **cand_c6c);
        cand_c70[hp] = (uint8_t *)calloc(pool_n, sizeof **cand_c70);
        cand_c78[hp] = (uint8_t *)calloc(pool_n, sizeof **cand_c78);
        if (!cbuf[hp] || !tbuf[hp] || !cand_c68[hp] || !cand_c6c[hp]
            || !cand_c70[hp] || !cand_c78[hp]) {
            rc = SPFY_E_NOMEM; goto fail;
        }
        for (uint32_t i = 0; i < pool_n; ++i) {
            cbuf[hp][i] = pool[i];
            spfy_unit_record_t ur;
            if (spfy_unit_record_get(&v->units, pool[i], &ur) == SPFY_OK) {
                /* mem+0x10 = disk+0x11 = f0_end, mem+0x11 = disk+0x12 =
                 * f0_mid, mem+0x0f = disk+0x10 = f0_start. */
                cand_c6c[hp][i] = ur.f0_end;
                cand_c68[hp][i] = ur.f0_mid;
                cand_c70[hp][i] = ur.f0_start;
                cand_c78[hp][i] = 0;   /* engine zeros at FUN_08e8abe0 */
            }
        }

        spfy_anchor_ctx_t ctx_in;
        for (int i = 0; i < 5; ++i) ctx_in.ctx[i] = (int32_t)ctx5[i];
        spfy_anchor_sp_target_t sp_in;
        for (int i = 0; i < 5; ++i) sp_in.sp[i] = sp5[i];

        cart_feat_ctx_t cfc = { &adapter, q5, 0 };
        spfy_anchor_cart_t cart = {0};
        /* Silence-pad CART traversal: engine emits durt-CART leaf statistics
         * for silence slots (ctx5[2] in {64, 65} = HP_PAU_L/R) the same way
         * it does for non-silence; we used to skip these slots and emit
         * (0, 0), which produced 174 leaf_diff records in 03-FE-AUDIT
         * (engine emits the silence-tree leaf (192.5106, 0.097); we emit
         * None). Plan 03-03/03-04 removes the gate so spfy_cart_traverse
         * runs on silence slots too. SPFY_NO_SILENCE_CART=1 reverts to the
         * old behaviour for diagnostic isolation. */
        int silence_slot = (ctx5[2] == 64 || ctx5[2] == 65);
        if (!silence_slot || !getenv("SPFY_NO_SILENCE_CART")) {
            uint32_t durt_idx = tom_swap(ctx5[2] >> 1);
            if (durt_idx < v->durt_cart.n_trees) {
                if (spfy_cart_traverse(&v->durt_cart, durt_idx, cart_feat, &cfc,
                                       &cart.durt_mean, &cart.durt_var) == SPFY_OK)
                    cart.durt_valid = 1;
            }
            if (v->f0tr_cart.n_trees > 0) {
                cfc.is_f0tr = 1;
                if (spfy_cart_traverse(&v->f0tr_cart, 0, cart_feat, &cfc,
                                       &cart.f0tr_mean, &cart.f0tr_var) == SPFY_OK) {
                    cart.f0tr_valid = 1;
                    /* Pitch shift via unit-selection bias — see
                     * spfy_synth_set_pitch_semitones(). f0tr_mean is Hz;
                     * multiplying by 2^(semitones/12) shifts the target
                     * Viterbi matches against. scale == 1.0 is exact
                     * IEEE identity → audit-invariant. */
                    cart.f0tr_mean *= v->pitch_scale;
                }
                cfc.is_f0tr = 0;
            }
            /* SSML / Balabolka per-word prosody overrides. Both default
             * to 0 which collapses the scale factors to exact identity
             * so the audit-corpus path stays bit-stable. */
            if (cart.durt_valid && hp_rate_pct[hp]) {
                /* +N% rate = durations shrink by 100/(100+N). */
                cart.durt_mean *= 100.0f
                    / (100.0f + (float)hp_rate_pct[hp]);
            }
            if (cart.f0tr_valid && hp_pitch_st[hp]) {
                cart.f0tr_mean *= powf(2.0f,
                    (float)hp_pitch_st[hp] / 12.0f);
            }
        }

        /* SPFY_HP_COMP_DUMP marker: emit a per-slot header so the
         * follow-on hp_comp lines can be associated with this hp/slot.
         * The hp_innerscorer's hp_comp emits don't carry the slot
         * context themselves. */
        if (getenv("SPFY_HP_COMP_DUMP")) {
            fprintf(stderr,
                "{\"hp_comp_slot\":1,\"hp\":%u,\"post\":%u,\"pool_n\":%u}\n",
                hp, s, pool_n);
        }
        for (uint32_t i = 0; i < pool_n; ++i) {
            float c = NAN;
            int rcs = spfy_hp_innerscorer(&v->av, &ctx_in, &sp_in, &cart,
                                          cbuf[hp][i], &c);
            tbuf[hp][i] = (rcs != SPFY_OK || isnan(c) || isinf(c)) ? 1e9f : c;
        }

        /* Engine FUN_08e88de0 early-exit emulation: as cands are
         * processed left-to-right in pool order, engine tracks
         * `best_so_far` and caps any cand with TC > best_so_far +
         * fVar4 to 1e4 (0x461c4000). fVar4 is voice config weight at
         * offset +0x4c. The cap puts capped cands into histogram bin
         * 39 so they're filtered out by FUN_08e88830's threshold walk.
         *
         * SPFY_HP_EARLY_EXIT_VAL=<float> overrides fVar4 (default 0).
         * Setting it to 0 disables the cap (current behavior).
         *
         * 2026-05-14: for Tom, fVar4 value is TBD via decomp of voice
         * config loader. Empirically test_002 needs fVar4 < 0.746 to
         * cap uid=50038 at HP=4 (TC=0.933, best=0.187). */
        const char *eev = getenv("SPFY_HP_EARLY_EXIT_VAL");
        if (eev) {
            float fvar4 = (float)atof(eev);
            if (fvar4 > 0.0f) {
                /* Iterate in pool order; track best_so_far; cap. */
                float best_so_far = 1e30f;
                for (uint32_t i = 0; i < pool_n; ++i) {
                    float t = tbuf[hp][i];
                    if (t > best_so_far + fvar4) {
                        tbuf[hp][i] = 10000.0f;
                    } else if (t < best_so_far) {
                        best_so_far = t;
                    }
                }
            }
        }

        /* SPFY_TC_DUMP — per-cand target cost dump (post-inner-scorer,
         * pre-hist-prune). Used to A/B-compare against engine's
         * viterbi_dp cand+0x2c (= pre-DP cost) per (slot, uid). Emits
         * to stderr as one JSON line per HP slot. */
        if (getenv("SPFY_TC_DUMP")) {
            fprintf(stderr, "{\"tc_dump\":1,\"hp\":%u,\"post\":%u,\"pool_n\":%u,\"uids\":[", hp, s, pool_n);
            for (uint32_t i = 0; i < pool_n; ++i)
                fprintf(stderr, "%s%u", i ? "," : "", cbuf[hp][i]);
            fprintf(stderr, "],\"tc\":[");
            for (uint32_t i = 0; i < pool_n; ++i)
                fprintf(stderr, "%s%.6f", i ? "," : "", (double)tbuf[hp][i]);
            fprintf(stderr, "]}\n");
        }

        /* Engine-faithful HP-cand histogram prune + sort (FUN_08e88830).
         * Builds a 40-bin histogram of (cost - best) * 40 (so each bin
         * spans 0.025 cost v->units), walks bins until slack drops below
         * bin_dist or cum exceeds MAX_UNITS, then drops cands with cost
         * above (best + bin_dist). Sorts survivors by cost ascending
         * (engine uses shell-sort; we use qsort) and caps at MAX_UNITS.
         *
         * Tom's params (from VCF): THRESH=0.8, SLOPE=0.005, MAX=50.
         * Constants: bin_width=0.025, scale=40.0 (engine globals
         * _DAT_08e98520, _DAT_08e98524).
         *
         * Skip when pool is the silence-sentinel fallback (already 1
         * cand). */
        /* HP cand histogram_prune (FUN_08e88830). Engine-faithful core;
         * scoring-time running-min gating in FUN_08e88de0 not yet
         * implemented (engine prunes tighter than histogram alone).
         * Constants from Tom VCF + engine globals: THRESH=0.8,
         * SLOPE=0.005, MAX=50, BIN_WIDTH=0.025, SCALE=40.
         * SPFY_NO_HP_PRUNE=1 disables. */
        if (pool_n > 1 && !getenv("SPFY_NO_HP_PRUNE")) {
            const float HP_THRESH    = 0.8f;
            const float HP_SLOPE     = 0.005f;
            const float HP_BIN_WIDTH = 0.025f;
            const float HP_SCALE     = 40.0f;
            const uint32_t HP_MAX    = 50u;

            float best = tbuf[hp][0];
            for (uint32_t i = 1; i < pool_n; ++i)
                if (tbuf[hp][i] < best) best = tbuf[hp][i];

            int bins[40] = {0};
            for (uint32_t i = 0; i < pool_n; ++i) {
                float diff = (tbuf[hp][i] - best) * HP_SCALE;
                /* Engine FUN_08e9504c binning is TRUNCATION (round-toward
                 * -zero), NOT round-to-nearest. The decomp shows FRNDINT
                 * (banker's round) followed by an adjustment subtracting
                 * 1 whenever the input wasn't exactly integer — net effect
                 * is floor for positive values.
                 *
                 * Verified 2026-05-14 via Frida trace of FUN_08e88830 on
                 * text_002 HP=4: engine bin_dist=0.725 (break at k=28),
                 * matching truncation binning. Previously we used lroundf
                 * which rounded 29.86 up to 30, putting 33584 in bin 29
                 * instead of bin 28 — engine breaks at k=29 instead of
                 * k=28 and keeps 17 cands instead of 16.
                 *
                 * SPFY_HP_BIN_LROUND=1 reverts to lroundf for A/B audit. */
                int bidx;
                if (diff >= (float)HP_SCALE) {
                    bidx = 39;
                } else if (getenv("SPFY_HP_BIN_LROUND")) {
                    bidx = (int)lroundf(diff);
                    if (bidx < 0) bidx = 0;
                    if (bidx > 39) bidx = 39;
                } else {
                    /* Truncation (floor for positive). diff is always
                     * >= 0 since tc[i] >= best by construction. */
                    bidx = (int)diff;
                    if (bidx < 0) bidx = 0;
                    if (bidx > 39) bidx = 39;
                }
                bins[bidx]++;
            }
            int cum = 0;
            float bin_dist = 40.0f * HP_BIN_WIDTH;
            /* Engine uses local_c8 starting at 2 (not 1), so bin_dist
             * for iteration k is (k+1)*HP_BIN_WIDTH not k*HP_BIN_WIDTH.
             * After break: threshold = bin_dist + best.
             *
             * Precision note (2026-05-14): the LHS must be stored to a
             * float local before the comparison. On 32-bit x86 with
             * x87-FPU the inline expression keeps 80-bit register
             * precision and produces a strict-less-than where engine
             * (SSE2 / 32-bit precision) produces equality (no break).
             * Without this, our prune is one bin tighter than engine
             * exactly at the boundary, dropping cands like mp_050 HP=3
             * uid 156406 (delta=0.778 from best, threshold=0.8). Set
             * SPFY_PRUNE_X87=1 to restore the old (x87-extended)
             * comparison for A/B audit. */
            for (int k = 0; k < 40; ++k) {
                cum += bins[k];
                float bd = (float)(k + 1) * HP_BIN_WIDTH;
                float lhs = HP_THRESH - (float)cum * HP_SLOPE;
                int break_now;
                if (getenv("SPFY_PRUNE_X87")) {
                    break_now = ((uint32_t)cum > HP_MAX
                        || (HP_THRESH - (float)cum * HP_SLOPE) < bd);
                } else {
                    break_now = ((uint32_t)cum > HP_MAX || lhs < bd);
                }
                if (break_now) { bin_dist = bd; break; }
            }
            float thresh = best + bin_dist;
            uint32_t kept = 0;
            for (uint32_t i = 0; i < pool_n; ++i) {
                if (tbuf[hp][i] <= thresh) {
                    cbuf[hp][kept] = cbuf[hp][i];
                    tbuf[hp][kept] = tbuf[hp][i];
                    cand_c68[hp][kept] = cand_c68[hp][i];
                    cand_c6c[hp][kept] = cand_c6c[hp][i];
                    cand_c70[hp][kept] = cand_c70[hp][i];
                    cand_c78[hp][kept] = cand_c78[hp][i];
                    ++kept;
                }
            }
            /* Sort kept cands by target_cost ascending. Engine does
             * this in FUN_08e88830 (shell-sort); we use selection sort
             * (cand counts are small post-prune). +0.2 pp UID match
             * over no-sort with current bit-exact TC + join cost. */
            for (uint32_t a = 0; a + 1 < kept; ++a) {
                uint32_t mn = a;
                for (uint32_t b = a + 1; b < kept; ++b) {
                    if (tbuf[hp][b] < tbuf[hp][mn]) mn = b;
                }
                if (mn != a) {
                    float    tt = tbuf[hp][a];
                    uint32_t cc = cbuf[hp][a];
                    uint8_t  c68v = cand_c68[hp][a];
                    uint8_t  c6cv = cand_c6c[hp][a];
                    uint8_t  c70v = cand_c70[hp][a];
                    uint8_t  c78v = cand_c78[hp][a];
                    tbuf[hp][a]    = tbuf[hp][mn];
                    cbuf[hp][a]    = cbuf[hp][mn];
                    cand_c68[hp][a]= cand_c68[hp][mn];
                    cand_c6c[hp][a]= cand_c6c[hp][mn];
                    cand_c70[hp][a]= cand_c70[hp][mn];
                    cand_c78[hp][a]= cand_c78[hp][mn];
                    tbuf[hp][mn]   = tt;
                    cbuf[hp][mn]   = cc;
                    cand_c68[hp][mn]= c68v;
                    cand_c6c[hp][mn]= c6cv;
                    cand_c70[hp][mn]= c70v;
                    cand_c78[hp][mn]= c78v;
                }
            }
            if (kept > HP_MAX) kept = HP_MAX;
            if (getenv("SPFY_PRUNE_DEBUG")) {
                fprintf(stderr, "{\"prune\":1,\"hp\":%u,\"pool_n_in\":%u,"
                                "\"kept\":%u,\"best\":%.9f,\"bin_dist\":%.6f,"
                                "\"thresh\":%.9f}\n",
                        hp, pool_n, kept, (double)best, (double)bin_dist,
                        (double)(best + bin_dist));
            }
            pool_n = kept;
        }

        vslots[hp].cands = cbuf[hp];
        vslots[hp].target_cost = tbuf[hp];
        vslots[hp].n_cands = pool_n;
        total_cands += pool_n;
    }
    fprintf(stdout, "PRSL pools built: %u total candidates across %u hp slots "
                    "(%u slots had empty pools)\n",
            total_cands, n_hp, n_empty);

    /* PostScoringAdj (Word + Syl level): for each Word/Syl in the slot tree,
     * call spfy_anchor_score on the cklx postings keyed by the word text,
     * then inject the surviving anchor cands' UIDs into the corresponding
     * halfphone-leaf cand pools. Each anchor cand spans contiguous UIDs
     * [ss..se] across [first_hp..last_hp]; we add UID (ss + (hp-first_hp))
     * to leaf-slot[hp]'s pool with a target cost equal to anchor.pre_dp /
     * span_length. The same-rec join cost of 0 between consecutive injected
     * UIDs lets the linear Viterbi find an engine-style continuous-recording
     * path naturally. */
    /* For Syl-level PSA, re-syllabify phonemes left-to-right using
     * max-onset principle: each vowel is a syl nucleus; consonants split
     * between previous-coda and next-onset (last consonant goes to onset
     * if more than one consonant, all go to onset if just one). The
     * baked dict emits all of a multi-syl word's phonemes under a single
     * syl_id, so we can't trust phons[i].syl_id for syllable structure;
     * re-derive from ARPAbet vowel-class instead. */
    uint32_t *psa_syl_start = NULL;
    uint32_t  psa_n_syls    = 0;
    if (n_segs_arr > 2) {
        psa_syl_start = (uint32_t *)calloc(n_segs_arr,
                                           sizeof *psa_syl_start);
    }
    if (psa_syl_start && !getenv("SPFY_PSA_SYL_FROM_RESYL")) {
        /* 2026-05-14: derive syllable boundaries from the slot tree's
         * SK_SYLLABLE nodes (engine FE output). The previous
         * re-syllabifier used max-onset with single-consonant onsets
         * only, missing engine's multi-consonant onsets like "pre" in
         * "representative" (SYL CKLX key `p_r_ix`) and "glish" in
         * "English" (key `g_l_ih_sh`). Engine's FE syllabifies these
         * correctly, and we already walk the tree to attach anchor
         * cands to SK_SYLLABLE slots downstream — use the same tree
         * here. SPFY_PSA_SYL_FROM_RESYL=1 reverts to the re-syllabifier.
         */
        for (uint32_t s = 0; s < tree.n_slots; ++s) {
            if (tree.slots[s].kind != SPFY_SK_SYLLABLE) continue;
            uint32_t cur = s;
            while (tree.slots[cur].n_children > 0)
                cur = tree.slots[cur].child_idx[0];
            uint32_t hp_idx = post_to_hp[cur];
            if (hp_idx == UINT32_MAX) continue;
            uint32_t phon_idx = hp_idx / 2u;
            if (phon_idx == 0) continue;          /* leading pau */
            if (phon_idx >= n_segs_arr - 1) continue;  /* trailing pau */
            if (psa_n_syls == 0
                || phon_idx > psa_syl_start[psa_n_syls - 1])
                psa_syl_start[psa_n_syls++] = phon_idx;
        }
    } else if (psa_syl_start) {
        /* Legacy max-onset re-syllabifier (SPFY_PSA_SYL_FROM_RESYL=1). */
        int seen_vowel = !getenv("SPFY_NO_SYL_INITIAL_VOWEL")
                         && is_arpa_vowel(seg_names[1]);
        uint32_t last_v = 1;
        psa_syl_start[psa_n_syls++] = 1;
        for (uint32_t i = 2; i < n_segs_arr - 1; ++i) {
            int is_v = is_arpa_vowel(seg_names[i]);
            if (is_v && seen_vowel) {
                uint32_t n_cons = i - last_v - 1;
                uint32_t boundary = (n_cons >= 1) ? (i - 1) : i;
                if (boundary > psa_syl_start[psa_n_syls - 1])
                    psa_syl_start[psa_n_syls++] = boundary;
            }
            if (is_v) { seen_vowel = 1; last_v = i; }
        }
    }

    if (!getenv("SPFY_NO_PSA")) {
        uint32_t psa_words = 0, psa_syls = 0;
        /* First: tree-Word iteration (existing path). Then a second pass
         * for psa-derived syllables. */
        for (uint32_t s = 0; s < tree.n_slots; ++s) {
            spfy_slot_kind_t kind = tree.slots[s].kind;
            if (kind != SPFY_SK_WORD) continue;

            /* Word: cklx key = lowercased word name. */
            char lc_key[128];
            int   anchor_type = 4;
            uint32_t group_idx = SPFY_CHUNK_GROUP_WORD;
            int found = 0;
            uint32_t wi = 0;
            for (uint32_t w = 0; w < fe_utt.n_words; ++w) {
                if (fe_utt.word_shareds[w] == tree.slots[s].fe_shared) {
                    wi = w; found = 1; break;
                }
            }
            if (!found) continue;
            const char *wname = fe_utt.word_names[wi];
            if (!wname || strcmp(wname, "_NULL_") == 0) continue;
            size_t kl = strlen(wname);
            if (kl >= sizeof lc_key) kl = sizeof lc_key - 1;
            for (size_t i = 0; i < kl; ++i)
                lc_key[i] = (char)tolower((unsigned char)wname[i]);
            lc_key[kl] = 0;

            const uint32_t *postings = NULL;
            uint32_t n_postings = 0;
            int hit = spfy_cklx_lookup(&v->chunks.cklx[group_idx],
                                       lc_key, &postings, &n_postings);
            if (getenv("SPFY_PSA_DEBUG"))
                fprintf(stderr, "  PSA[%s] key=%-30s hit=%d n_postings=%u\n",
                        kind == SPFY_SK_WORD ? "WORD" : "SYL ",
                        lc_key, hit, n_postings);
            if (hit <= 0) continue;
            if (n_postings == 0) continue;

            /* Find the slot's HP span via post-order halfphone descendants. */
            int32_t first_hp_post = -1;
            uint32_t first_hp_idx = 0, last_hp_idx = 0;
            for (uint32_t hp = 0; hp < n_hp; ++hp) {
                uint32_t post = hp_to_post[hp];
                /* Walk parents until we hit slot s OR fall off. */
                uint32_t cur = post;
                int matches = 0;
                while (cur != UINT32_MAX && cur < tree.n_slots) {
                    if (cur == s) { matches = 1; break; }
                    cur = tree.slots[cur].parent_idx;
                }
                if (!matches) continue;
                if (first_hp_post < 0) {
                    first_hp_post = (int32_t)post; first_hp_idx = hp;
                }
                last_hp_idx = hp;
            }
            if (first_hp_post < 0) continue;
            uint32_t span_n = last_hp_idx - first_hp_idx + 1;

            /* Build per-HP cart + sp_target arrays for this word. */
            spfy_anchor_cart_t      *cart_per = (spfy_anchor_cart_t *)
                calloc(span_n, sizeof *cart_per);
            spfy_anchor_sp_target_t *sp_per   = (spfy_anchor_sp_target_t *)
                calloc(span_n, sizeof *sp_per);
            int32_t                 *syl_idx_per = (int32_t *)
                calloc(span_n, sizeof *syl_idx_per);
            spfy_anchor_hp_feat_t   *hp_feat_per = (spfy_anchor_hp_feat_t *)
                calloc(span_n, sizeof *hp_feat_per);
            if (!cart_per || !sp_per || !syl_idx_per || !hp_feat_per) {
                free(cart_per); free(sp_per); free(syl_idx_per);
                free(hp_feat_per); continue;
            }
            for (uint32_t k = 0; k < span_n; ++k) {
                uint32_t hp = first_hp_idx + k;
                uint32_t post = hp_to_post[hp];
                cart_feat_ctx_t cfc = {NULL, q5_per_slot[post], 0};
                spfy_fe_slot_t adapter = {0};
                for (int i = 0; i < 5; ++i) {
                    adapter.ctx[i] = (int32_t)slice_ctx.ctx[post][i];
                    adapter.sp[i]  = sp_tab.sp[post][i];
                }
                cfc.slot = &adapter;
                /* Plan 03-04: silence-pad CART traversal — see primary
                 * spfy_cart_traverse call site for rationale.
                 * SPFY_NO_SILENCE_CART=1 reverts. */
                int silence = (slice_ctx.ctx[post][2] == 64
                               || slice_ctx.ctx[post][2] == 65);
                if (!silence || !getenv("SPFY_NO_SILENCE_CART")) {
                    uint32_t didx = tom_swap(slice_ctx.ctx[post][2] >> 1);
                    if (didx < v->durt_cart.n_trees) {
                        if (spfy_cart_traverse(&v->durt_cart, didx, cart_feat, &cfc,
                              &cart_per[k].durt_mean, &cart_per[k].durt_var) == SPFY_OK)
                            cart_per[k].durt_valid = 1;
                        hp_feat_per[k].phone_label = (uint8_t)didx;
                        hp_feat_per[k].durt_valid  = 1;
                    }
                    if (v->f0tr_cart.n_trees > 0) {
                        cfc.is_f0tr = 1;
                        if (spfy_cart_traverse(&v->f0tr_cart, 0, cart_feat, &cfc,
                              &cart_per[k].f0tr_mean, &cart_per[k].f0tr_var) == SPFY_OK) {
                            cart_per[k].f0tr_valid = 1;
                            cart_per[k].f0tr_mean *= v->pitch_scale;
                        }
                        cfc.is_f0tr = 0;
                    }
                }
                for (int i = 0; i < 5; ++i) {
                    sp_per[k].sp[i] = sp_tab.sp[post][i];
                    hp_feat_per[k].ctx[i] = (int32_t)slice_ctx.ctx[post][i];
                    hp_feat_per[k].sp[i]  = sp_tab.sp[post][i];
                }
                hp_feat_per[k].q5 = q5_per_slot[post];
                /* 2026-05-14: real syl_idx per HP — walk parent_idx up the
                 * tree until we hit an SK_SYLLABLE node. The anchor-score
                 * advance walk uses this to step target_idx at syllable
                 * boundaries (engine FUN_08e89530 walks `param_2+0x18`
                 * forward until value changes from prior). Previously this
                 * was always -1, making advance jump to last_hp on first
                 * non-initial iter — wrong for multi-syllable Word spans. */
                {
                    int32_t syl_idx = -1;
                    uint32_t cur = post;
                    while (cur != UINT32_MAX && cur < tree.n_slots) {
                        if (tree.slots[cur].kind == SPFY_SK_SYLLABLE) {
                            syl_idx = (int32_t)cur;
                            break;
                        }
                        cur = tree.slots[cur].parent_idx;
                    }
                    syl_idx_per[k] = syl_idx;
                }
            }

            spfy_anchor_slot_input_t aslot = {0};
            aslot.first_hp = (int32_t)first_hp_idx;
            aslot.last_hp  = (int32_t)last_hp_idx;
            for (int i = 0; i < 5; ++i) {
                aslot.first_ctx.ctx[i] =
                    (int32_t)slice_ctx.ctx[hp_to_post[first_hp_idx]][i];
                aslot.last_ctx.ctx[i] =
                    (int32_t)slice_ctx.ctx[hp_to_post[last_hp_idx]][i];
            }
            aslot.anchor_type = anchor_type;   /* 4=Word, 2=Syl */
            aslot.cart_per_hp = cart_per;
            aslot.sp_per_hp   = sp_per;
            aslot.syl_idx_per_hp = syl_idx_per;
            aslot.durt_cart   = &v->durt_cart;
            aslot.hp_feat     = hp_feat_per;

            spfy_anchor_cand_t out_cands[64];
            uint32_t out_n = 0;
            int rcs = spfy_anchor_score(&v->av, &aslot, postings, n_postings,
                                         &v->chunks.ckls[group_idx],
                                         out_cands, 64, &out_n);
            free(cart_per); free(sp_per); free(syl_idx_per); free(hp_feat_per);
            if (rcs != SPFY_OK || out_n == 0) continue;
            ++psa_words;

            /* Store anchor cands per tree slot for DAG Viterbi. cand uid =
             * ss (head); join_key = se (tail uid). Filter cands whose span
             * doesn't match the slot's HP-span. */
            uint32_t *cands_buf = (uint32_t *)calloc(out_n, sizeof *cands_buf);
            uint32_t *jks_buf   = (uint32_t *)calloc(out_n, sizeof *jks_buf);
            float    *tgt_buf   = (float    *)calloc(out_n, sizeof *tgt_buf);
            if (!cands_buf || !jks_buf || !tgt_buf) {
                free(cands_buf); free(jks_buf); free(tgt_buf); continue;
            }
            uint32_t kept = 0;
            for (uint32_t c = 0; c < out_n; ++c) {
                uint32_t ss = out_cands[c].ss;
                uint32_t se = out_cands[c].se;
                if (se < ss) continue;
                if (se >= v->units.n_units) continue;
                /* 2026-05-14: removed `se-ss+1 != span_n` filter.
                 * Engine accepts "partial" anchors where the UID range
                 * (se-ss+1) is SHORTER than the HP span (n_hp). For
                 * nat_037 the engine's Word anchor at hp=32-39 (span=8)
                 * has uid range 42444..42449 (6 UIDs); our DP needs
                 * this cand to match engine's path choice. Path
                 * expansion (`range(uid, jk+1)`) yields the 6 UIDs and
                 * the resulting path is shorter than n_hp by 2, which
                 * is exactly the DIFF_PL pattern.
                 *
                 * SPFY_ANCHOR_STRICT_SPAN=1 restores the strict filter
                 * for A/B audit. */
                if (!getenv("SPFY_ANCHOR_RELAX_SPAN_OFF")
                    && getenv("SPFY_ANCHOR_STRICT_SPAN")
                    && se - ss + 1 != span_n) continue;
                cands_buf[kept] = ss;
                jks_buf  [kept] = se;
                tgt_buf  [kept] = out_cands[c].pre_dp;
                ++kept;
            }
            anchor_cands [s] = cands_buf;
            anchor_jks   [s] = jks_buf;
            anchor_target[s] = tgt_buf;
            anchor_n     [s] = kept;
            /* Engine reads anchor cand+0x68/+0x6c/+0x70 from the
             * RUN-TAIL unit (= se = jks_buf[c]). Initialize parallel
             * state arrays from se's f0_end/f0_mid/f0_start bytes so
             * the F0-curve gate sees the correct trailing pitch. */
            uint8_t *aC68 = (uint8_t *)calloc(kept, sizeof *aC68);
            uint8_t *aC6c = (uint8_t *)calloc(kept, sizeof *aC6c);
            uint8_t *aC70 = (uint8_t *)calloc(kept, sizeof *aC70);
            uint8_t *aC78 = (uint8_t *)calloc(kept, sizeof *aC78);
            if (aC68 && aC6c && aC70 && aC78) {
                for (uint32_t c = 0; c < kept; ++c) {
                    /* Engine-faithful anchor cand state init from
                     * FUN_08e8ce60 @ 0x08e8ce60 (decomp 2026-05-14):
                     * iterate v->units in [ss..se]; per qualifying unit
                     * (voicing[hp_class] != 0 — Tom has weight_8c =
                     * weight_90 = 0 so only voicing gates), update
                     *   c70 = MAX of unit.f0_start
                     *   c6c = FIRST unit.f0_end where f0_end >= 21
                     *   c68 = LAST unit.f0_mid where f0_mid >= 21
                     * Silence (voicing[hp_class]==0) v->units skip all
                     * three. Engine init is c68=c6c=c70=0; values
                     * only get set if at least one qualifying unit
                     * exists.
                     * SPFY_NO_ANCHOR_HEAD_C6C=1 reverts to legacy
                     * tail-based init. */
                    int legacy = getenv("SPFY_NO_ANCHOR_HEAD_C6C") != NULL;
                    if (legacy) {
                        spfy_unit_record_t ur;
                        if (spfy_unit_record_get(&v->units, jks_buf[c], &ur)
                            == SPFY_OK) {
                            aC6c[c] = ur.f0_end;
                            aC68[c] = ur.f0_mid;
                            aC70[c] = ur.f0_start;
                        }
                        aC78[c] = 0;
                        continue;
                    }
                    uint8_t v68 = 0, v6c = 0, v70 = 0;
                    uint16_t v78 = 0;
                    /* Engine FUN_08e8ce60 anchor cand init. The gate
                     * `voicing[hp_class]==0 AND weight_8c==0 AND
                     * weight_90==0 -> silence path` empirically never
                     * fires for Tom (engine c68=120 for uid=74341 which
                     * has hp_class voiceless), so weight_8c or
                     * weight_90 must be non-zero for the init code path.
                     * Treat all v->units as voiced. SPFY_ANCHOR_VOICING_GATE=1
                     * re-enables the gate. */
                    int do_gate = getenv("SPFY_ANCHOR_VOICING_GATE") != NULL;
                    for (uint32_t u = cands_buf[c]; u <= jks_buf[c]; ++u) {
                        spfy_unit_record_t ur;
                        if (spfy_unit_record_get(&v->units, u, &ur) != SPFY_OK)
                            continue;
                        if (do_gate) {
                            uint8_t uhpc = (v->av.hpclass_table
                                            && u < v->av.hpclass_n)
                                           ? v->av.hpclass_table[u] : 0xff;
                            int voiced = (v->av.voicing != NULL
                                          && uhpc < v->av.voicing_n
                                          && v->av.voicing[uhpc] != 0);
                            if (!voiced) { v78 += ur.dur_like; continue; }
                        }
                        if (v70 < ur.f0_start) v70 = ur.f0_start;
                        if (ur.f0_end >= 21 && v6c < 21)
                            v6c = ur.f0_end;
                        if (ur.f0_mid >= 21) { v68 = ur.f0_mid; v78 = 0; }
                        else                 { v78 += ur.dur_like; }
                    }
                    aC68[c] = v68;
                    aC6c[c] = v6c;
                    aC70[c] = v70;
                    /* c78 is a 16-bit accumulator engine-side; our slot
                     * struct holds it as a uint8_t. Clamp to 0xff. */
                    aC78[c] = (v78 > 255u) ? 255u : (uint8_t)v78;
                    if (getenv("SPFY_ANCHOR_STATE_DUMP")) {
                        fprintf(stderr, "{\"anchor_state\":1,\"slot\":%u,"
                                "\"key\":\"%s\",\"ss\":%u,\"se\":%u,"
                                "\"c68\":%u,\"c6c\":%u,\"c70\":%u,"
                                "\"c78\":%u}\n",
                                s, lc_key, cands_buf[c], jks_buf[c],
                                v68, v6c, v70, aC78[c]);
                    }
                }
                anchor_c68[s] = aC68;
                anchor_c6c[s] = aC6c;
                anchor_c70[s] = aC70;
                anchor_c78[s] = aC78;
            } else {
                free(aC68); free(aC6c); free(aC70); free(aC78);
            }
            if (getenv("SPFY_PSA_DEBUG")) {
                fprintf(stderr, "  WORD anchor slot=%u key=%s n=%u "
                                "first_hp=%u last_hp=%u\n",
                        s, lc_key, kept, first_hp_idx, last_hp_idx);
                for (uint32_t c = 0; c < kept && c < 5; ++c)
                    fprintf(stderr, "    cand[%u] ss=%u se=%u pre_dp=%.4f\n",
                            c, cands_buf[c], jks_buf[c], (double)tgt_buf[c]);
            }
        }   /* end tree-Word loop */

        /* Second pass: psa-derived syllables. Walk psa_syl_start[] groups,
         * build cklx[SYL] key, run anchor_score, inject. */
        for (uint32_t g = 0; g < psa_n_syls; ++g) {
            uint32_t start_p = psa_syl_start[g];
            uint32_t end_p = (g + 1 < psa_n_syls)
                              ? (psa_syl_start[g + 1] - 1)
                              : (n_segs_arr >= 2 ? n_segs_arr - 2 : 0);
            if (end_p < start_p) continue;
            if (end_p >= n_segs_arr) continue;

            /* 2026-05-14 INVESTIGATED: SYL CKLX contains onset+first-vowel
             * keys for many syllables ("d_ao" hits, "d_ao_g" misses).
             * Tried using engine-faithful onset+vowel key build → +hits on
             * cklx but corpus UID regressed 95.7%→94.6%. Engine apparently
             * doesn't materialize anchors for every syllable the cklx has
             * keys for — there's a separate selection layer we haven't
             * found. Reverted; the 787 engine-only Syl anchors deferred. */
            char lc_key[128];
            size_t pos = 0;
            lc_key[0] = 0;
            int abort_key = 0;
            for (uint32_t i = start_p; i <= end_p; ++i) {
                const char *aname = seg_names[i];
                if (!aname || strcmp(aname, "pau") == 0) {
                    abort_key = 1; break;
                }
                size_t al = strlen(aname);
                if (pos + al + 2 >= sizeof lc_key) { abort_key = 1; break; }
                if (pos > 0) lc_key[pos++] = '_';
                memcpy(lc_key + pos, aname, al); pos += al;
                lc_key[pos] = 0;
            }
            if (abort_key || pos == 0) continue;

            const uint32_t *postings = NULL;
            uint32_t n_postings = 0;
            int hit = spfy_cklx_lookup(&v->chunks.cklx[SPFY_CHUNK_GROUP_SYL],
                                       lc_key, &postings, &n_postings);
            if (getenv("SPFY_PSA_DEBUG"))
                fprintf(stderr, "  PSA[SYL ] key=%-30s hit=%d n_postings=%u\n",
                        lc_key, hit, n_postings);
            if (hit <= 0 || n_postings == 0) continue;

            /* HP span: phoneme i (in seg_names) -> HP slots 2*i .. 2*i+1.
             * (FE pad has leading pau at i=0 -> HPs 0,1; first content
             * phoneme at i=1 -> HPs 2,3; etc.) */
            uint32_t first_hp_idx = 2u * start_p;
            uint32_t last_hp_idx  = 2u * end_p + 1u;
            if (last_hp_idx >= n_hp) continue;
            uint32_t span_n = last_hp_idx - first_hp_idx + 1u;

            spfy_anchor_cart_t      *cart_per = (spfy_anchor_cart_t *)
                calloc(span_n, sizeof *cart_per);
            spfy_anchor_sp_target_t *sp_per   = (spfy_anchor_sp_target_t *)
                calloc(span_n, sizeof *sp_per);
            int32_t                 *syl_idx_per = (int32_t *)
                calloc(span_n, sizeof *syl_idx_per);
            spfy_anchor_hp_feat_t   *hp_feat_per = (spfy_anchor_hp_feat_t *)
                calloc(span_n, sizeof *hp_feat_per);
            if (!cart_per || !sp_per || !syl_idx_per || !hp_feat_per) {
                free(cart_per); free(sp_per); free(syl_idx_per);
                free(hp_feat_per); continue;
            }
            for (uint32_t k = 0; k < span_n; ++k) {
                uint32_t hp = first_hp_idx + k;
                uint32_t post = hp_to_post[hp];
                cart_feat_ctx_t cfc = {NULL, q5_per_slot[post], 0};
                spfy_fe_slot_t adapter = {0};
                for (int i = 0; i < 5; ++i) {
                    adapter.ctx[i] = (int32_t)slice_ctx.ctx[post][i];
                    adapter.sp[i]  = sp_tab.sp[post][i];
                }
                cfc.slot = &adapter;
                /* Plan 03-04: silence-pad CART traversal — see primary
                 * spfy_cart_traverse call site for rationale.
                 * SPFY_NO_SILENCE_CART=1 reverts. */
                int silence = (slice_ctx.ctx[post][2] == 64
                               || slice_ctx.ctx[post][2] == 65);
                if (!silence || !getenv("SPFY_NO_SILENCE_CART")) {
                    uint32_t didx = tom_swap(slice_ctx.ctx[post][2] >> 1);
                    if (didx < v->durt_cart.n_trees) {
                        if (spfy_cart_traverse(&v->durt_cart, didx, cart_feat, &cfc,
                              &cart_per[k].durt_mean, &cart_per[k].durt_var) == SPFY_OK)
                            cart_per[k].durt_valid = 1;
                        hp_feat_per[k].phone_label = (uint8_t)didx;
                        hp_feat_per[k].durt_valid  = 1;
                    }
                    if (v->f0tr_cart.n_trees > 0) {
                        cfc.is_f0tr = 1;
                        if (spfy_cart_traverse(&v->f0tr_cart, 0, cart_feat, &cfc,
                              &cart_per[k].f0tr_mean, &cart_per[k].f0tr_var) == SPFY_OK) {
                            cart_per[k].f0tr_valid = 1;
                            cart_per[k].f0tr_mean *= v->pitch_scale;
                        }
                        cfc.is_f0tr = 0;
                    }
                }
                for (int i = 0; i < 5; ++i) {
                    sp_per[k].sp[i] = sp_tab.sp[post][i];
                    hp_feat_per[k].ctx[i] = (int32_t)slice_ctx.ctx[post][i];
                    hp_feat_per[k].sp[i]  = sp_tab.sp[post][i];
                }
                hp_feat_per[k].q5 = q5_per_slot[post];
                /* 2026-05-14: real syl_idx per HP — walk parent_idx up the
                 * tree until we hit an SK_SYLLABLE node. The anchor-score
                 * advance walk uses this to step target_idx at syllable
                 * boundaries (engine FUN_08e89530 walks `param_2+0x18`
                 * forward until value changes from prior). Previously this
                 * was always -1, making advance jump to last_hp on first
                 * non-initial iter — wrong for multi-syllable Word spans. */
                {
                    int32_t syl_idx = -1;
                    uint32_t cur = post;
                    while (cur != UINT32_MAX && cur < tree.n_slots) {
                        if (tree.slots[cur].kind == SPFY_SK_SYLLABLE) {
                            syl_idx = (int32_t)cur;
                            break;
                        }
                        cur = tree.slots[cur].parent_idx;
                    }
                    syl_idx_per[k] = syl_idx;
                }
            }

            spfy_anchor_slot_input_t aslot = {0};
            aslot.first_hp = (int32_t)first_hp_idx;
            aslot.last_hp  = (int32_t)last_hp_idx;
            for (int i = 0; i < 5; ++i) {
                aslot.first_ctx.ctx[i] =
                    (int32_t)slice_ctx.ctx[hp_to_post[first_hp_idx]][i];
                aslot.last_ctx.ctx[i] =
                    (int32_t)slice_ctx.ctx[hp_to_post[last_hp_idx]][i];
            }
            aslot.anchor_type = 2;             /* Syl */
            aslot.cart_per_hp = cart_per;
            aslot.sp_per_hp   = sp_per;
            aslot.syl_idx_per_hp = syl_idx_per;
            aslot.durt_cart   = &v->durt_cart;
            aslot.hp_feat     = hp_feat_per;

            spfy_anchor_cand_t out_cands[64];
            uint32_t out_n = 0;
            int rcs = spfy_anchor_score(&v->av, &aslot, postings, n_postings,
                                         &v->chunks.ckls[SPFY_CHUNK_GROUP_SYL],
                                         out_cands, 64, &out_n);
            free(cart_per); free(sp_per); free(syl_idx_per); free(hp_feat_per);
            if (rcs != SPFY_OK) continue;
            ++psa_syls;

            /* Store Syl anchor cands keyed by the SLOT POST_IDX of the
             * matching Syl in the tree (engine attaches anchor cands to
             * the Syl tree-slot, and LinkGraph routes the DAG through
             * them). We use first_hp_idx to find which Syl slot this is.
             * If we can't find a matching Syl slot in the tree, we still
             * preserve the (legacy) leaf-injection so something happens. */
            int32_t syl_slot_post = -1;
            for (uint32_t s2 = 0; s2 < tree.n_slots; ++s2) {
                if (tree.slots[s2].kind != SPFY_SK_SYLLABLE) continue;
                /* Find leftmost-leaf HP under s2; compare to first_hp_idx. */
                uint32_t cur = s2;
                while (tree.slots[cur].n_children > 0)
                    cur = tree.slots[cur].child_idx[0];
                /* cur is leftmost HP descendant; map to hp index. */
                uint32_t hp_lookup = post_to_hp[cur];
                if (hp_lookup == first_hp_idx) {
                    syl_slot_post = (int32_t)s2; break;
                }
            }

            uint32_t kept_s = 0;
            uint32_t *cands_s = NULL, *jks_s = NULL;
            float    *tgt_s   = NULL;
            if (syl_slot_post >= 0) {
                cands_s = (uint32_t *)calloc(out_n, sizeof *cands_s);
                jks_s   = (uint32_t *)calloc(out_n, sizeof *jks_s);
                tgt_s   = (float    *)calloc(out_n, sizeof *tgt_s);
                if (!cands_s || !jks_s || !tgt_s) {
                    free(cands_s); free(jks_s); free(tgt_s);
                    cands_s = jks_s = NULL; tgt_s = NULL;
                }
            }

            for (uint32_t c = 0; c < out_n; ++c) {
                uint32_t ss = out_cands[c].ss;
                uint32_t se = out_cands[c].se;
                if (se < ss) continue;
                uint32_t span_uid_n = se - ss + 1;
                /* 2026-05-14: removed strict span filter — see WORD-pass
                 * comment for rationale. Engine accepts partial anchors
                 * (uid range < n_hp) which our DP needs to consider for
                 * DIFF_PL phrases. */
                if (getenv("SPFY_ANCHOR_STRICT_SPAN")
                    && span_uid_n != span_n) continue;
                if (cands_s && jks_s && tgt_s) {
                    cands_s[kept_s] = ss;
                    jks_s  [kept_s] = se;
                    tgt_s  [kept_s] = out_cands[c].pre_dp;
                    ++kept_s;
                }
            }
            if (syl_slot_post >= 0 && kept_s > 0
                && cands_s && jks_s && tgt_s) {
                /* Replace any prior storage at this slot (rare). */
                free(anchor_cands [syl_slot_post]);
                free(anchor_jks   [syl_slot_post]);
                free(anchor_target[syl_slot_post]);
                free(anchor_c68   [syl_slot_post]);
                free(anchor_c6c   [syl_slot_post]);
                free(anchor_c70   [syl_slot_post]);
                free(anchor_c78   [syl_slot_post]);
                anchor_cands [syl_slot_post] = cands_s;
                anchor_jks   [syl_slot_post] = jks_s;
                anchor_target[syl_slot_post] = tgt_s;
                anchor_n     [syl_slot_post] = kept_s;
                /* Engine-faithful c68/c6c/c70 aggregation. See WORD
                 * anchor block above for the FUN_08e8ce60 derivation.
                 * SPFY_NO_ANCHOR_HEAD_C6C=1 reverts to legacy tail. */
                uint8_t *aC68 = (uint8_t *)calloc(kept_s, sizeof *aC68);
                uint8_t *aC6c = (uint8_t *)calloc(kept_s, sizeof *aC6c);
                uint8_t *aC70 = (uint8_t *)calloc(kept_s, sizeof *aC70);
                uint8_t *aC78 = (uint8_t *)calloc(kept_s, sizeof *aC78);
                if (aC68 && aC6c && aC70 && aC78) {
                    int legacy = getenv("SPFY_NO_ANCHOR_HEAD_C6C") != NULL;
                    for (uint32_t c = 0; c < kept_s; ++c) {
                        if (legacy) {
                            spfy_unit_record_t ur;
                            if (spfy_unit_record_get(&v->units, jks_s[c], &ur)
                                == SPFY_OK) {
                                aC6c[c] = ur.f0_end;
                                aC68[c] = ur.f0_mid;
                                aC70[c] = ur.f0_start;
                            }
                            aC78[c] = 0;
                            continue;
                        }
                        uint8_t v68 = 0, v6c = 0, v70 = 0;
                        uint16_t v78 = 0;
                        int do_gate = getenv("SPFY_ANCHOR_VOICING_GATE")
                                      != NULL;
                        for (uint32_t u = cands_s[c]; u <= jks_s[c]; ++u) {
                            spfy_unit_record_t ur;
                            if (spfy_unit_record_get(&v->units, u, &ur)
                                != SPFY_OK) continue;
                            if (do_gate) {
                                uint8_t uhpc = (v->av.hpclass_table
                                                && u < v->av.hpclass_n)
                                               ? v->av.hpclass_table[u] : 0xff;
                                int voiced = (v->av.voicing != NULL
                                              && uhpc < v->av.voicing_n
                                              && v->av.voicing[uhpc] != 0);
                                if (!voiced) {
                                    v78 += ur.dur_like; continue;
                                }
                            }
                            if (v70 < ur.f0_start) v70 = ur.f0_start;
                            if (ur.f0_end >= 21 && v6c < 21)
                                v6c = ur.f0_end;
                            if (ur.f0_mid >= 21) {
                                v68 = ur.f0_mid; v78 = 0;
                            } else {
                                v78 += ur.dur_like;
                            }
                        }
                        aC68[c] = v68;
                        aC6c[c] = v6c;
                        aC70[c] = v70;
                        aC78[c] = (v78 > 255u) ? 255u : (uint8_t)v78;
                    }
                    anchor_c68[syl_slot_post] = aC68;
                    anchor_c6c[syl_slot_post] = aC6c;
                    anchor_c70[syl_slot_post] = aC70;
                    anchor_c78[syl_slot_post] = aC78;
                } else {
                    free(aC68); free(aC6c); free(aC70); free(aC78);
                }
            } else {
                free(cands_s); free(jks_s); free(tgt_s);
            }
        }   /* end psa-syl loop */

        fprintf(stdout, "PostScoringAdj: %u words + %u syls processed\n",
                psa_words, psa_syls);
    }
    free(seg_names);   /* no longer needed after PSA */
    free(psa_syl_start);

    /* Engine-faithful DAG Viterbi (FUN_08e8b620 semantics): every tree
     * slot is a node, predecessors come from spfy_link_graph, Word/Syl
     * slots carry anchor cands and HP-leaf slots carry PRSL pool cands.
     * The DP picks the cheapest route, which lets a low-cost Word
     * anchor BYPASS its HP children. */
    join_ctx_t jc;
    jc.hash = &v->hash; jc.units = &v->units;
    /* miss_default is only used when the hist curve fails to load
     * (degenerate fallback). Engine never hits this path. */
    jc.miss_default = 0.0f;
    /* F0-prob curve (VIN hist chunk + voice+0xc8). Tom's curve is
     * V-shaped (100 entries, sub_off=-50, min at idx 49, max ~10.96). */
    load_f0_hist_curve(&v->vin, &jc);
    jc.f0_edge_change_weight = 0.6f;     /* Tom VCF F0_EDGE_CHANGE_WEIGHT */
    /* MISSING_JOIN_COST = 1000.0 from FE-init default (FUN_08e90dc0
     * sets param_3[0x21] = 0x447a0000 = 1000.0 before VCF override;
     * Tom VCF doesn't override). Huge by design — makes the DP almost
     * exclusively use same-rec runs and v->hash hits. */
    jc.missing_join_cost     = 1000.0f;
    if (getenv("SPFY_NO_F0_CURVE")) jc.curve = NULL;
    if (jc.curve) {
        fprintf(stdout, "F0-curve loaded: %d bins, sub_off=%d, "
                        "F0_EDGE=%.2f, MISSING_JOIN=%.2f\n",
                jc.curve_max_idx, jc.curve_sub_off,
                (double)jc.f0_edge_change_weight,
                (double)jc.missing_join_cost);
        if (getenv("SPFY_DUMP_F0_CURVE")) {
            fprintf(stderr, "{\"f0_curve\":1,\"n\":%d,\"sub_off\":%d,\"vals\":[",
                    jc.curve_max_idx, jc.curve_sub_off);
            for (int i = 0; i < jc.curve_max_idx; ++i)
                fprintf(stderr, "%s%.4f", i ? "," : "", (double)jc.curve[i]);
            fprintf(stderr, "]}\n");
        }
    }
    n_slots = n_hp;
    uint32_t *path_uids = (uint32_t *)calloc(n_slots, sizeof *path_uids);
    if (!path_uids) { rc = SPFY_E_NOMEM; goto fail; }
    /* Default-fill with silence sentinel so any HP slot not covered by
     * the path (shouldn't happen in a correct DAG) plays as silence. */
    for (uint32_t i = 0; i < n_slots; ++i) path_uids[i] = SILENCE_SENTINEL_UID;
    float total = 0.0f;

    {
        /* Build dag_slots[tree.n_slots]: HP slots get the PRSL pools
         * (cands == join_keys for leaves), Word/Syl slots get anchor
         * cands (cands = ss array, join_keys = se array), all slots get
         * preds from spfy_link_graph. */
        dag_slots = (spfy_viterbi_dag_slot_t *)
                    calloc(tree.n_slots, sizeof *dag_slots);
        if (!dag_slots) { rc = SPFY_E_NOMEM; free(path_uids); goto fail; }
        for (uint32_t s = 0; s < tree.n_slots; ++s) {
            dag_slots[s].preds   = preds_tab.per_slot[s].preds;
            dag_slots[s].n_preds = preds_tab.per_slot[s].n_preds;
        }
        /* HP-leaf slots: copy from vslots[]. cbuf[hp] is the cand UID
         * array (also serves as join_keys since for leaves uid == join
         * key); tbuf[hp] is target_cost. */
        for (uint32_t hp = 0; hp < n_hp; ++hp) {
            uint32_t s = hp_to_post[hp];
            dag_slots[s].cands       = vslots[hp].cands;
            dag_slots[s].join_keys   = vslots[hp].cands;
            dag_slots[s].target_cost = vslots[hp].target_cost;
            dag_slots[s].n_cands     = vslots[hp].n_cands;
            dag_slots[s].c68         = cand_c68[hp];
            dag_slots[s].c6c         = cand_c6c[hp];
            dag_slots[s].c70         = cand_c70[hp];
            dag_slots[s].c78         = cand_c78[hp];
        }
        /* Word/Syl slots: anchor cands (already populated by PSA above). */
        for (uint32_t s = 0; s < tree.n_slots; ++s) {
            spfy_slot_kind_t k = tree.slots[s].kind;
            if (k != SPFY_SK_WORD && k != SPFY_SK_SYLLABLE) continue;
            if (anchor_n[s] == 0 || !anchor_cands[s]) continue;
            dag_slots[s].cands       = anchor_cands [s];
            dag_slots[s].join_keys   = anchor_jks   [s];
            dag_slots[s].target_cost = anchor_target[s];
            dag_slots[s].n_cands     = anchor_n     [s];
            dag_slots[s].c68         = anchor_c68   [s];
            dag_slots[s].c6c         = anchor_c6c   [s];
            dag_slots[s].c70         = anchor_c70   [s];
            dag_slots[s].c78         = anchor_c78   [s];
        }

        uint32_t *path_slots_buf = (uint32_t *)
                                    calloc(tree.n_slots, sizeof *path_slots_buf);
        uint32_t *path_uids_buf  = (uint32_t *)
                                    calloc(tree.n_slots, sizeof *path_uids_buf);
        if (!path_slots_buf || !path_uids_buf) {
            free(path_slots_buf); free(path_uids_buf);
            rc = SPFY_E_NOMEM; free(path_uids); goto fail;
        }
        uint32_t path_len = 0;
        int rc_v = spfy_viterbi_run_dag(dag_slots, tree.n_slots,
                                         dag_join_cb, &jc,
                                         path_slots_buf, path_uids_buf,
                                         &path_len, &total);
        if (rc_v != SPFY_OK) {
            fprintf(stderr, "viterbi-dag failed: %s\n", spfy_strerror(rc_v));
            free(path_slots_buf); free(path_uids_buf);
            rc = rc_v; free(path_uids); goto fail;
        }
        fprintf(stdout, "viterbi[dag] total cost=%.3f path_len=%u "
                        "(of %u tree slots; %u HP)\n",
                (double)total, path_len, tree.n_slots, n_hp);

        /* Expand multi-UID anchor cands into per-HP UIDs. For an HP
         * slot in the path, write the chosen UID directly. For a
         * Word/Syl slot, find its leftmost HP descendant and the
         * matching anchor cand (by ss); write UIDs ss..se across the
         * span. */
        uint32_t expand_anchors = 0, expand_hps = 0;
        for (uint32_t i = 0; i < path_len; ++i) {
            uint32_t s   = path_slots_buf[i];
            uint32_t uid = path_uids_buf[i];
            if (s >= tree.n_slots) continue;
            spfy_slot_kind_t k = tree.slots[s].kind;
            if (k == SPFY_SK_HALFPHONE) {
                uint32_t hp = post_to_hp[s];
                if (hp < n_slots) path_uids[hp] = uid;
                ++expand_hps;
                continue;
            }
            if (k != SPFY_SK_WORD && k != SPFY_SK_SYLLABLE) continue;

            /* Find this slot's HP span: leftmost-leaf .. rightmost-leaf. */
            uint32_t leftmost = s;
            while (tree.slots[leftmost].n_children > 0)
                leftmost = tree.slots[leftmost].child_idx[0];
            uint32_t rightmost = s;
            while (tree.slots[rightmost].n_children > 0) {
                uint32_t nch = tree.slots[rightmost].n_children;
                rightmost = tree.slots[rightmost].child_idx[nch - 1];
            }
            uint32_t first_hp = post_to_hp[leftmost];
            uint32_t last_hp  = post_to_hp[rightmost];
            if (first_hp >= n_slots || last_hp >= n_slots) continue;

            /* Find the chosen anchor cand: cands[c] == uid (= ss). */
            uint32_t se = uid;
            for (uint32_t c = 0; c < anchor_n[s]; ++c) {
                if (anchor_cands[s][c] == uid) {
                    se = anchor_jks[s][c]; break;
                }
            }
            uint32_t ss = uid;
            uint32_t span_n = (se >= ss) ? (se - ss + 1u) : 1u;
            uint32_t hp_span_n = last_hp - first_hp + 1u;
            uint32_t fill_n = (span_n < hp_span_n) ? span_n : hp_span_n;
            for (uint32_t k2 = 0; k2 < fill_n; ++k2) {
                if (first_hp + k2 < n_slots)
                    path_uids[first_hp + k2] = ss + k2;
            }
            /* For partial anchors (uid range < hp span), mark the
             * "overshoot" HPs (consumed by the anchor but with no
             * corresponding UID) with UINT32_MAX so the path dump and
             * audio decode can skip them. Engine emits exactly
             * span_n UIDs for the anchor's hp_span_n HPs; we mirror
             * that. */
            for (uint32_t k2 = fill_n; k2 < hp_span_n; ++k2) {
                if (first_hp + k2 < n_slots)
                    path_uids[first_hp + k2] = 0xFFFFFFFFu;
            }
            ++expand_anchors;
        }
        if (getenv("SPFY_DUMP_PATH")) {
            fprintf(stdout, "  dag path: %u HP-slot hops, %u anchor hops "
                            "(span expanded into %u HP slots)\n",
                    expand_hps, expand_anchors, n_hp);
            fprintf(stdout, "  raw dag path (slot -> uid):\n");
            for (uint32_t i = 0; i < path_len; ++i) {
                uint32_t s = path_slots_buf[i];
                spfy_slot_kind_t k = (s < tree.n_slots)
                                     ? tree.slots[s].kind : SPFY_SK_PHRASE;
                const char *ks = (k == SPFY_SK_HALFPHONE) ? "HP"
                                : (k == SPFY_SK_SYLLABLE) ? "SYL"
                                : (k == SPFY_SK_WORD)     ? "WORD"
                                : "?";
                fprintf(stdout, "    [%u] slot=%u kind=%s uid=%u "
                                "n_preds=%u n_cands=%u\n",
                        i, s, ks, path_uids_buf[i],
                        dag_slots[s].n_preds, dag_slots[s].n_cands);
            }
            for (uint32_t hp = 0; hp < n_hp; ++hp) {
                /* Skip partial-anchor overshoot positions — engine's
                 * path doesn't emit these either. */
                if (path_uids[hp] == 0xFFFFFFFFu) continue;
                fprintf(stdout, "  hp %2u: uid=%u\n", hp, path_uids[hp]);
            }
        }
        free(path_slots_buf); free(path_uids_buf);
    }

    /* (Was: SPFY_NO_FORCE_END_SILENCE / force-set last UID to silence
     * sentinel. The DP now picks the sentinel naturally at the end of
     * an utterance — verified 93.6%→93.6% with and without the
     * post-DP override, so the defensive force-fix was redundant.) */

    /* Decode chosen v->units to audio via WSOLA streamer. WAV+streamer
     * are set up once outside the per-phrase loop; this section just
     * pushes audio for the current phrase. */
    int      prev_have = 0;
    uint16_t prev_file_idx = 0, prev_local_pos = 0, prev_dur_like = 0;
    /* F0 at the trailing edge of the last emitted span — fed into WSOLA
     * for PSOLA voiced-join detection. 0 = unvoiced/unknown (which
     * forces plain WSOLA on the next push). */
    uint8_t  prev_f0_end = 0;
    /* Inter-word silence: engine inserts visible breathing gaps between
     * words (clearly audible in waveform A/B). Our FE only adds leading +
     * trailing pau pads, no inter-word silence, so the audio rushes.
     * sp[3] = wordInPhrase per the engine schema; when it changes between
     * slots we've crossed a word boundary. Insert a small silence chunk
     * (~40 ms @ 8 kHz = 320 samples) so the output paces like the engine. */
    uint32_t prev_word_idx = 0xFFFFFFFFu;
    static const int16_t SILENCE_BUF[320] = {0};
    /* Inter-word silence injection: default OFF (0ms). Engine does
     * NOT inject inter-word silences — speech runs continuously,
     * pacing is handled by DP-selected pause v->units only. Verified
     * 2026-05-15 by comparing pangram oracle (engine output via
     * spfy_dumpwav.exe) against our output: oracle has ONE leading
     * silence region (28ms), zero inter-word silences. v4 had eight
     * 22-24ms inter-word silences that the user perceived as faintly
     * stuttery. Removing them brings our total length from +219ms vs
     * oracle to +9ms vs oracle.
     *
     * SPFY_INTERWORD_MS=N (N in [0..200]) restores legacy behavior. */
    int env_silence_ms = 0;
    {
        const char *e = getenv("SPFY_INTERWORD_MS");
        if (e) env_silence_ms = atoi(e);
        if (env_silence_ms < 0)   env_silence_ms = 0;
        if (env_silence_ms > 200) env_silence_ms = 200;
    }
    size_t silence_n = (size_t)env_silence_ms * (size_t)v->vdb.sample_rate / 1000u;
    if (silence_n > sizeof SILENCE_BUF / sizeof *SILENCE_BUF)
        silence_n = sizeof SILENCE_BUF / sizeof *SILENCE_BUF;
    size_t played = 0, skipped = 0, paired_same = 0, paired_cross = 0,
           interword_pauses = 0;

    uint32_t s = 0;
    while (s < n_slots) {
        uint32_t u = path_uids[s];
        if (u == 0xFFFFFFFFu) {
            /* Partial-anchor overshoot — audio for these HPs is
             * supplied by the anchor's UID range above; don't emit
             * silence here. */
            ++s; continue;
        }
        if (u == 0 || u == SILENCE_SENTINEL_UID || u >= v->units.n_units) {
            ++skipped; ++s; prev_have = 0; prev_f0_end = 0; continue;
        }
        spfy_unit_record_t r1;
        if (spfy_unit_record_get(&v->units, u, &r1) != SPFY_OK) {
            ++skipped; ++s; prev_have = 0; prev_f0_end = 0; continue;
        }

        /* Inter-word silence injection. We track the slot's parent-word
         * post-index (computed from the slot tree) and insert a small
         * silence chunk when it changes between consecutive non-silence
         * halfphone slots. */
        uint32_t this_post = hp_to_post[s];
        const uint32_t (*ctx_arr)[5] = (const uint32_t (*)[5])slice_ctx.ctx;
        int this_is_silence = (ctx_arr[this_post][2] == 64
                               || ctx_arr[this_post][2] == 65);
        uint32_t this_word_idx = hp_word_idx[s];
        if (silence_n > 0 && prev_have
            && prev_word_idx != 0xFFFFFFFFu
            && this_word_idx != prev_word_idx
            && !this_is_silence) {
            /* Push with align=1 so the OLA blend Hann-fades the previous
             * voiced tail down to zero across the OLA region (10 ms),
             * rather than one-sample-cutting it to zero. The latter
             * produces an audible click at the voicing-to-silence
             * boundary; WAV analysis at the Tom pangram's t≈1335ms and
             * t≈1473ms (between "the"/"lazy" and "lazy"/"dog") showed
             * one-sample drops of ±7676 → 0 that the user identified as
             * stutter. SPFY_WSOLA_NO_SILENCE_FADE=1 reverts. */
            int sil_align = (getenv("SPFY_WSOLA_NO_SILENCE_FADE") != NULL)
                            ? 0 : 1;
            (void)spfy_wsola_push_unit(&ws, SILENCE_BUF, silence_n,
                                       sil_align);
            ++interword_pauses;
            prev_have = 0;     /* break alignment chain after silence */
            prev_f0_end = 0;   /* unvoiced gap — disable PSOLA on next push */
        }
        /* Fire the SAPI/CLI word-event callback at every non-silence
         * slot whose parent word differs from the previous one. The
         * callback receives the current PCM sample offset (post-WSOLA),
         * so word/sentence boundaries land at the audible word start.
         * Used by both the CLI (writes SPFY_WORD_EVENTS_FILE sidecar)
         * and the SAPI shim (emits SPEI_WORD_BOUNDARY for Balabolka /
         * Narrator / etc. word highlighting). */
        if (cb && cb->word_cb && !this_is_silence
            && this_word_idx != prev_word_idx) {
            cb->word_cb(cb->ctx, sink->n_samples_written);
        }
        if (!this_is_silence) prev_word_idx = this_word_idx;
        /* Engine-faithful batching: SWIttsWsolaConcat receives a pre-
         * batched WsolaUnit array where each entry groups a run of
         * CONSECUTIVE UIDs (uid, uid+1, uid+2, ...). Verified empirically
         * via wsola_unit_probe on Tom pangram (text_002): all 31 groups
         * are pure uid+1 runs, group sizes ranging 1..5. The engine then
         * does ONE OLA per group regardless of length, with sub-v->units
         * inside the group emitted verbatim.
         *
         * Old logic collapsed only PAIRS that matched phone_center +
         * file_idx + local_pos contiguity (44 pushes for the pangram's
         * 64 path UIDs). Switching to engine's pure uid+1 rule batches
         * arbitrary-length runs across phone boundaries (31 pushes,
         * matching engine), eliminating the ±100ms local timing drift
         * from mismatched OLA placement. SPFY_NO_RUN_BATCH=1 reverts to
         * pair-only legacy. */
        int no_run = (getenv("SPFY_NO_RUN_BATCH") != NULL);
        uint32_t run_n = 1;
        if (!no_run) {
            while (s + run_n < n_slots) {
                uint32_t v_prev = path_uids[s + run_n - 1];
                uint32_t v_next = path_uids[s + run_n];
                if (v_next == 0 || v_next == SILENCE_SENTINEL_UID
                    || v_next >= v->units.n_units) break;
                if (v_next != v_prev + 1u) break;          /* engine rule */
                spfy_unit_record_t rn, rp;
                if (spfy_unit_record_get(&v->units, v_next, &rn) != SPFY_OK) break;
                if (spfy_unit_record_get(&v->units, v_prev, &rp) != SPFY_OK) break;
                /* Sanity: uid+1 should always imply same file_idx and
                 * contiguous local_pos in a well-formed VDB. Guard so a
                 * malformed record doesn't smear a span across files. */
                if (rn.file_idx != rp.file_idx) break;
                if (rn.local_pos < rp.local_pos
                    || rn.local_pos > rp.local_pos + rp.dur_like + 64u) break;
                ++run_n;
            }
        }
        if (run_n >= 2) {
            spfy_unit_record_t r_last;
            (void)spfy_unit_record_get(&v->units, path_uids[s + run_n - 1],
                                       &r_last);
            uint32_t span = (uint32_t)r_last.local_pos
                          + (uint32_t)r_last.dur_like
                          - (uint32_t)r1.local_pos;
            int align = !prev_have || prev_file_idx != r1.file_idx;
            rc = append_recording_span(&ws, r1.file_idx, r1.local_pos,
                                       span, &v->feat, &v->vdb, &v->lookup, align,
                                       prev_f0_end, r1.f0_start,
                                       v->vdb.sample_rate);
            if (rc != SPFY_OK) { free(path_uids); goto fail; }
            ++paired_same; played += run_n;
            prev_have = 1;
            prev_file_idx = r_last.file_idx;
            prev_local_pos = r_last.local_pos;
            prev_dur_like  = r_last.dur_like;
            prev_f0_end    = r_last.f0_end;
            s += run_n;
        } else {
            int align = !prev_have || prev_file_idx != r1.file_idx
                     || (uint32_t)r1.local_pos
                        != (uint32_t)prev_local_pos + prev_dur_like;
            if (s + 1 < n_slots) {
                spfy_unit_record_t r2;
                if (path_uids[s+1] < v->units.n_units
                    && spfy_unit_record_get(&v->units, path_uids[s+1], &r2) == SPFY_OK
                    && r1.phone_center == r2.phone_center
                    && r1.file_idx != r2.file_idx) {
                    ++paired_cross;
                }
            }
            rc = append_recording_span(&ws, r1.file_idx, r1.local_pos,
                                       r1.dur_like, &v->feat, &v->vdb, &v->lookup, align,
                                       prev_f0_end, r1.f0_start,
                                       v->vdb.sample_rate);
            if (rc != SPFY_OK) { free(path_uids); goto fail; }
            ++played; prev_have = 1;
            prev_file_idx = r1.file_idx;
            prev_local_pos = r1.local_pos;
            prev_dur_like  = r1.dur_like;
            prev_f0_end    = r1.f0_end;
            ++s;
        }
    }
    /* End of one phrase. Accumulate stats. */
    total_played += played;
    total_skipped += skipped;
    total_paired_same += paired_same;
    total_paired_cross += paired_cross;
    total_interword_pauses += interword_pauses;
    free(path_uids); path_uids = NULL;

    /* Free per-phrase state so next iteration starts fresh. */
    if (vslots) {
        for (uint32_t i = 0; i < n_slots; ++i) {
            if (cbuf) free(cbuf[i]);
            if (tbuf) free(tbuf[i]);
            if (cand_c68) free(cand_c68[i]);
            if (cand_c6c) free(cand_c6c[i]);
            if (cand_c70) free(cand_c70[i]);
            if (cand_c78) free(cand_c78[i]);
        }
    }
    free(vslots); vslots = NULL;
    free(cbuf); cbuf = NULL;
    free(tbuf); tbuf = NULL;
    free(cand_c68); cand_c68 = NULL;
    free(cand_c6c); cand_c6c = NULL;
    free(cand_c70); cand_c70 = NULL;
    free(cand_c78); cand_c78 = NULL;
    free(q5_per_slot); q5_per_slot = NULL;
    free(q5_has); q5_has = NULL;
    free(hp_to_post); hp_to_post = NULL;
    free(post_to_hp); post_to_hp = NULL;
    free(hp_word_idx); hp_word_idx = NULL;
    free(hp_pitch_st);
    free(hp_rate_pct);
    free(dag_slots); dag_slots = NULL;
    if (anchor_cands) {
        for (uint32_t i = 0; i < tree.n_slots; ++i) {
            free(anchor_cands[i]); free(anchor_jks[i]); free(anchor_target[i]);
            if (anchor_c68) free(anchor_c68[i]);
            if (anchor_c6c) free(anchor_c6c[i]);
            if (anchor_c70) free(anchor_c70[i]);
            if (anchor_c78) free(anchor_c78[i]);
        }
        free(anchor_cands); anchor_cands = NULL;
        free(anchor_jks); anchor_jks = NULL;
        free(anchor_target); anchor_target = NULL;
        free(anchor_n); anchor_n = NULL;
        free(anchor_c68); anchor_c68 = NULL;
        free(anchor_c6c); anchor_c6c = NULL;
        free(anchor_c70); anchor_c70 = NULL;
        free(anchor_c78); anchor_c78 = NULL;
    }
    spfy_slot_preds_table_free(&preds_tab);
    preds_tab = (spfy_slot_preds_table_t){0};
    spfy_sp_target_table_free(&sp_tab);
    sp_tab = (spfy_sp_target_table_t){0};
    spfy_slice_ctx_table_free(&slice_ctx);
    slice_ctx = (spfy_slice_ctx_table_t){0};
    spfy_slot_tree_free(&tree);
    tree = (spfy_slot_tree_t){0};
    spfy_fe_utt_free(&fe_utt);
    fe_utt = (spfy_fe_utt_t){0};
    n_slots = 0;

    /* Inter-phrase silence — DEFAULT OFF (rely on FE pad slots).
     * Re-enable by setting SPFY_INTERPHRASE_MS=<N>. See big comment
     * above the inter_phrase_ms_override declaration for rationale. */
    if (phrase_idx + 1 < n_phrases && inter_phrase_ms_override > 0) {
        size_t n = (size_t)inter_phrase_ms_override
                 * (size_t)v->vdb.sample_rate / 1000u;
        if (n > 0) {
            static const int16_t INTER_PHRASE_SILENCE[16000] = {0};
            size_t cap = sizeof INTER_PHRASE_SILENCE / sizeof *INTER_PHRASE_SILENCE;
            if (n > cap) n = cap;
            int sil_align = (getenv("SPFY_WSOLA_NO_SILENCE_FADE") != NULL)
                            ? 0 : 1;
            (void)spfy_wsola_push_unit(&ws, INTER_PHRASE_SILENCE, n, sil_align);
        }
    }

    /* Update sentence_idx_in_para for next phrase per engine logic
     * (FUN_08e8c7d0): reset to 0 at end-punct ('.', '?', '!'), else
     * increment. */
    {
        char term = '.';
        if ((int)phrase_idx < parsed->n_phrase_terms
            && parsed->phrase_terms[phrase_idx] != 0) {
            term = parsed->phrase_terms[phrase_idx];
        }
        if (term == '.' || term == '?' || term == '!') {
            sentence_idx_in_para = 0;
        } else {
            sentence_idx_in_para += 1;
        }
    }

    }   /* end for(phrase_idx) */

    /* Finalize: flush WSOLA + close WAV + summary print. */
    rc = spfy_wsola_flush(&ws);

    if (out_stats) {
        out_stats->total_played           = total_played;
        out_stats->total_skipped          = total_skipped;
        out_stats->total_paired_same      = total_paired_same;
        out_stats->total_paired_cross     = total_paired_cross;
        out_stats->total_interword_pauses = total_interword_pauses;
        out_stats->wsola_aligned          = ws.n_aligned;
        out_stats->wsola_pushes           = ws.n_pushes;
        out_stats->n_phrases              = n_phrases;
        out_stats->samples_emitted        = sink->n_samples_written;
    }
    rc = SPFY_OK;
    goto cleanup;

fail:
cleanup:
    if (vslots) {
        for (uint32_t i = 0; i < n_slots; ++i) {
            if (cbuf) free(cbuf[i]);
            if (tbuf) free(tbuf[i]);
            if (cand_c68) free(cand_c68[i]);
            if (cand_c6c) free(cand_c6c[i]);
            if (cand_c70) free(cand_c70[i]);
            if (cand_c78) free(cand_c78[i]);
        }
    }
    free(vslots); free(cbuf); free(tbuf);
    free(cand_c68); free(cand_c6c); free(cand_c70); free(cand_c78);
    /* v->voicing_buf is owned by spfy_voice_t (freed by spfy_voice_free). */
    free(q5_per_slot); free(q5_has);
    free(hp_to_post); free(post_to_hp); free(hp_word_idx);
    free(dag_slots);
    if (anchor_cands) {
        for (uint32_t i = 0; i < tree.n_slots; ++i) {
            free(anchor_cands[i]); free(anchor_jks[i]); free(anchor_target[i]);
            if (anchor_c68) free(anchor_c68[i]);
            if (anchor_c6c) free(anchor_c6c[i]);
            if (anchor_c70) free(anchor_c70[i]);
            if (anchor_c78) free(anchor_c78[i]);
        }
        free(anchor_cands); free(anchor_jks); free(anchor_target); free(anchor_n);
        free(anchor_c68); free(anchor_c6c); free(anchor_c70); free(anchor_c78);
    }
    spfy_slot_preds_table_free(&preds_tab);
    spfy_sp_target_table_free(&sp_tab);
    spfy_slice_ctx_table_free(&slice_ctx);
    spfy_slot_tree_free(&tree);
    spfy_fe_utt_free(&fe_utt);
    spfy_prosody_hints_free(&hints);
    /* Everything previously freed here individually (v->bucket, carts,
     * v->chunks, FE, v->hpc, v->prsl, v->hash, v->pros, v->maps, v->ccos, v->lookup, v->feat,
     * v->vcf, v->vdb, v->vin, v->voicing_buf) is owned by the spfy_voice_t and
     * released by spfy_voice_free. */
    return rc;
}


/* Word-event sidecar writer used when SPFY_WORD_EVENTS_FILE env is set.
 * Format: one line per word, '<sample_offset>\t<word_idx>\n'.
 * Consumed by the 64-bit SAPI shim to emit SPEI_WORD_BOUNDARY events
 * after the subprocess synth completes. */
struct spfy_cli_wev_ctx {
    FILE     *fp;
    unsigned *idx;
};
void spfy_cli_word_cb(void *ctx, uint32_t sample_offset);
void spfy_cli_word_cb(void *ctx, uint32_t sample_offset)
{
    struct spfy_cli_wev_ctx *c = (struct spfy_cli_wev_ctx *)ctx;
    if (!c || !c->fp) return;
    fprintf(c->fp, "%u\t%u\n", sample_offset, *c->idx);
    (*c->idx)++;
}

/* The CLI main() is gated so this same .c file can be compiled into both
 * spfy_synth.exe and spfy_sapi.dll. SAPI defines SPFY_SYNTH_NO_MAIN to
 * exclude main() while keeping the synth core (spfy_synth_to_sink + all
 * its helpers: decode_unit_samples, append_recording_span, dag_join_cb,
 * load_f0_hist_curve) linkable. */
#ifndef SPFY_SYNTH_NO_MAIN
#include "embedded_assets.h"

/* Resolve a tempdir for asset extraction. Caller doesn't free.
 * On Windows, prefers %TEMP%; on POSIX, $TMPDIR then /tmp.
 * Subdir name keys off the binary's mtime so a re-extract happens
 * automatically when the executable (and thus the bundled assets)
 * change — no stale-asset corner case. */
static const char *resolve_asset_tempdir(const char *argv0)
{
    static char dir[1024];
    const char *base = NULL;
#ifdef _WIN32
    base = getenv("TEMP");
    if (!base) base = getenv("TMP");
    if (!base) base = "C:\\Windows\\Temp";
#else
    base = getenv("TMPDIR");
    if (!base) base = "/tmp";
#endif
    /* Best-effort uniqueness key: mtime of argv0. Failing that, just use
     * a fixed name. The extractor overwrites whatever's there anyway. */
    long long key = 0;
    struct stat st;
    if (argv0 && stat(argv0, &st) == 0) key = (long long)st.st_mtime;
#ifdef _WIN32
    snprintf(dir, sizeof dir, "%s\\spfy_assets_%lld", base, key);
#else
    snprintf(dir, sizeof dir, "%s/spfy_assets_%lld", base, key);
#endif
    return dir;
}

int main(int argc, char **argv)
{
    /* Two CLI forms:
     *   5-arg (preferred):   <voice.vin> <voice.vdb> <voice.vcf>
     *                        "<text>" <out.wav>
     *     hpclass + vocab + fe_tables_{a,b} are embedded in the binary
     *     (see spfy/tools/embed_assets.py) and extracted to a tempdir
     *     on first invocation.
     *   9-arg (legacy):      adds explicit hpclass/vocab/fe_tables_{a,b}
     *                        paths between the voice triplet and text —
     *                        kept so audit scripts that pass exact data
     *                        paths still work. */
    const char *vin_path, *vdb_path, *vcf_path;
    const char *hpc_path, *vocab, *tab_a, *tab_b;
    const char *text, *out_wav;
    spfy_asset_paths_t embedded_paths = {0};

    if (argc == 6) {
        vin_path = argv[1];
        vdb_path = argv[2];
        vcf_path = argv[3];
        text     = argv[4];
        out_wav  = argv[5];
        const char *tmpdir = resolve_asset_tempdir(argv[0]);
        if (spfy_assets_extract(tmpdir, &embedded_paths) != 0) {
            fprintf(stderr, "failed to extract embedded assets to %s\n", tmpdir);
            return 1;
        }
        hpc_path = embedded_paths.hpclass;
        vocab    = embedded_paths.vocab;
        tab_a    = embedded_paths.tables_a;
        tab_b    = embedded_paths.tables_b;
    } else if (argc == 10) {
        vin_path = argv[1];
        vdb_path = argv[2];
        vcf_path = argv[3];
        hpc_path = argv[4];
        vocab    = argv[5];
        tab_a    = argv[6];
        tab_b    = argv[7];
        text     = argv[8];
        out_wav  = argv[9];
    } else {
        fprintf(stderr,
            "usage: %s <voice.vin> <voice.vdb> <voice.vcf> \"<text>\" <out.wav>\n"
            "   or: %s <voice.vin> <voice.vdb> <voice.vcf> <hpclass.bin>\n"
            "          <vocab.json> <fe_tables_a> <fe_tables_b>\n"
            "          \"<text>\" <out.wav>          (legacy)\n",
            argv[0], argv[0]);
        return 2;
    }

    /* Voice loaded once via the shared synth library; the rest of main()
     * references voice members through the `voice.` prefix below. */
    spfy_voice_t voice = {0};
    int rc;

    /* All voice tables (voice.vin/voice.vdb/voice.vcf, voice.units/voice.feat/voice.lookup, voice.ccos/voice.maps/voice.pros,
     * voice.hash/voice.prsl, voice.durt_cart/voice.f0tr_cart, voice.chunks, voice.hpc + voice.bucket index, voice.av
     * with voicing table, and the FE host) are loaded here in one shot.
     * The implementation is in spfy/src/synth/spfy_synth_lib.c; this is
     * the same code that previously lived inline in main(). */
    {
        spfy_voice_paths_t paths = {
            .vin         = vin_path,
            .vdb         = vdb_path,
            .vcf         = vcf_path,
            .hpclass     = hpc_path,
            .vocab       = vocab,
            .fe_tables_a = tab_a,
            .fe_tables_b = tab_b,
        };
        if ((rc = spfy_voice_load(&paths, &voice)) != SPFY_OK) {
            fprintf(stderr, "error loading voice: %s\n", spfy_strerror(rc));
            return 1;
        }
    }



    /* Open output sink (file mode for CLI; SAPI builds a callback sink). */
    spfy_wav_writer_t wav = {0};
    if ((rc = spfy_wav_open(&wav, out_wav, voice.vdb.sample_rate)) != SPFY_OK) {
        fprintf(stderr, "error opening %s: %s\n", out_wav, spfy_strerror(rc));
        spfy_voice_free(&voice);
        return 1;
    }

    /* Optional word-events sidecar for the 64-bit SAPI shim. */
    FILE *wev_fp = NULL;
    unsigned wev_word_idx = 0;
    {
        const char *wev_path = getenv("SPFY_WORD_EVENTS_FILE");
        if (wev_path && *wev_path) wev_fp = fopen(wev_path, "wb");
    }
    struct spfy_cli_wev_ctx wev_ctx = { wev_fp, &wev_word_idx };
    spfy_synth_callbacks_t cb = {0};
    spfy_synth_callbacks_t *cbp = NULL;
    if (wev_fp) {
        cb.word_cb = spfy_cli_word_cb;
        cb.ctx     = &wev_ctx;
        cbp = &cb;
    }

    /* SPFY_PITCH_SEMITONES — shift target F0 via unit-selection bias.
     * The synth picks naturally higher/lower-pitched units from the
     * corpus; useful range is roughly +-3 st for Tom-family voices. */
    {
        const char *pe = getenv("SPFY_PITCH_SEMITONES");
        if (pe && *pe) {
            float st = (float)atof(pe);
            spfy_synth_set_pitch_semitones(&voice, st);
        }
    }

    spfy_synth_stats_t stats = {0};
    rc = spfy_synth_to_sink(&voice, text, &wav, cbp, &stats);

    if (wev_fp) { fclose(wev_fp); wev_fp = NULL; }
    spfy_wav_close(&wav);
    if (rc == SPFY_OK) {
        fprintf(stdout, "wrote %s: %u samples (%.2f s)  "
                        "[%zu units, %zu same-rec pairs, %zu cross-rec, "
                        "%zu skipped, %zu interword pauses, "
                        "wsola_aligned=%llu/%llu, %u phrases]\n",
                out_wav, stats.samples_emitted,
                (double)stats.samples_emitted / (double)voice.vdb.sample_rate,
                stats.total_played, stats.total_paired_same, stats.total_paired_cross,
                stats.total_skipped, stats.total_interword_pauses,
                (unsigned long long)stats.wsola_aligned,
                (unsigned long long)stats.wsola_pushes,
                stats.n_phrases);
    } else {
        fprintf(stderr, "error: %s\n", spfy_strerror(rc));
    }
    spfy_voice_free(&voice);
    return rc == SPFY_OK ? 0 : 1;
}
#endif /* SPFY_SYNTH_NO_MAIN */
