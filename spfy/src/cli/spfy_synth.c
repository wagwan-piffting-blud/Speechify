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
 *   spfy_synth <voice.vin> <voice.vdb> <voice.vcf> "<text>" <out.wav>
 *
 *   spfy_synth <voice.vin> <voice.vdb> <voice.vcf> <hpclass.bin>
 *              <vocab.json> <fe_tables_a> <fe_tables_b>
 *              "<text>" <out.wav>                          (legacy)
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

/* Live-trace event emitters. In the normal spfy_synth build (and the SAPI
 * DLL) SPFY_TRACE is undefined, so every spfy_trace_eventf() below is a
 * no-op macro that compiles away — the synth path stays byte-identical and
 * full-speed. Only the spfy_synth_trace target builds this file with
 * -DSPFY_TRACE=1, turning the emits into NDJSON written to the trace sink. */
#include "../common/le.h"
#include "../common/log.h"

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
/* Diagnostic verbosity                                                */
/* ------------------------------------------------------------------ */

/* Gates the per-synth status chatter emitted by spfy_synth_to_sink (FE
 * slot count, phrase-boundary marker, PRSL pool sizes, PostScoringAdj
 * tally, F0-curve params, final viterbi cost). Resolved lazily on first
 * use: verbose when SPFY_VERBOSE or SPFY_SYNTH_DEBUG is set in the
 * environment, else quiet. The CLI's -v/--verbose and -q/--quiet flags
 * override this by assigning it directly in main() before the first synth
 * call (-1 = unresolved). The one-time "[spfy] FE backend: ..." banner is
 * NOT gated — it prints in both modes.
 *
 * Keying off SPFY_SYNTH_DEBUG keeps the audit drivers
 * (test/oracle/master_compare*.py, which set it) fully verbose with no
 * change on their side, while a bare `spfy_synth` invocation stays quiet. */
static int spfy_synth_verbose = -1;

static int synth_is_verbose(void)
{
    if (spfy_synth_verbose < 0)
        spfy_synth_verbose = (getenv("SPFY_VERBOSE") != NULL
                              || getenv("SPFY_SYNTH_DEBUG") != NULL) ? 1 : 0;
    return spfy_synth_verbose;
}

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
                                 uint32_t sample_rate,
                                 float vol_gain)
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
    /* Per-word volume (\!vp/\!vd embedded tags): scale the decoded unit
     * before the OLA push. gain == 1.0 (the untagged default) is a no-op so
     * normal synth stays byte-identical. */
    if (vol_gain != 1.0f && buf) {
        for (size_t i = 0; i < n; ++i) {
            float v = (float)buf[i] * vol_gain;
            if (v >  32767.0f) v =  32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            buf[i] = (int16_t)lrintf(v);
        }
    }
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

/* feat-order phone -> ccos labl index. Mirrors phone_feat_to_labl in
 * src/usel/anchor_score.c; see voice/phone_order.h for where the table
 * comes from. Was a hardcoded Tom swap (9->10, 10->11, 11->9; 14<->15),
 * which silently mis-indexed the durt tree forest on every other voice. */
static uint32_t phone_to_labl(const spfy_voice_t *v, uint32_t phone)
{
    if (!v || !v->phone_order.feat_to_labl ||
        phone >= v->phone_order.n_phones) return phone;
    uint8_t lab = v->phone_order.feat_to_labl[phone];
    return (lab == SPFY_PHONE_NONE) ? phone : lab;
}

/* Is this slice ctx[2] one of the voice's two `pau` half-phone classes?
 * Was hardcoded to 64/65 -- Tom's pau sits at feat index 32, but felix's
 * is 35 (70/71) and javier's 24 (48/49), so the literal silently
 * mis-classified silence on every non-Tom voice. */
static int ctx_is_silence(const spfy_voice_t *v, uint32_t ctx2)
{
    uint32_t pau = v ? spfy_phone_order_index(&v->phone_order, "pau")
                     : SPFY_PHONE_NONE;
    uint32_t l = (pau == SPFY_PHONE_NONE) ? 64u : pau * 2u;
    return ctx2 == l || ctx2 == l + 1u;
}

typedef struct {
    const spfy_fe_slot_t *slot;
    uint32_t              q5;
    const spfy_voice_t   *voice;    /* for the feat->labl phone permutation */
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
        case 3: v = (int32_t)phone_to_labl(c->voice,
                                           (uint32_t)c->slot->ctx[1] >> 1); break;
        case 4: v = (int32_t)phone_to_labl(c->voice,
                                           (uint32_t)c->slot->ctx[3] >> 1); break;
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

/* Classify a ToBI accent string's phrase-BOUNDARY tone into an F0
 * ramp endpoint in signed semitones (relative to the syllable's
 * carrier). 0 = no boundary tone. Only phrase-level boundary tones
 * (the `X-Y%` forms) trigger a ramp; starred pitch accents (H*, L*)
 * shape prominence and are handled via syl_accent. The two tone
 * letters immediately preceding `%` select the contour:
 *
 *   L-L%  declarative fall    → -6
 *   L-H%  continuation rise    → +4
 *   H-L%  list-final / partial → -3
 *   H-H%  question rise        → +6
 *
 * The magnitude is a nominal contour depth; downstream scaling decides
 * how it maps to an actual F0 bias. Accepts both `L-L%` and `LL%`. */
static int boundary_tone_target_st(const char *accent)
{
    if (!accent || !*accent) return 0;
    const char *pct = strchr(accent, '%');
    if (!pct || pct < accent + 2) return 0;
    char hi = pct[-1];
    char lo = pct[-2];
    if (lo == '-' && pct >= accent + 3) lo = pct[-3];
    if (lo == 'L' && hi == 'L') return -6;
    if (lo == 'L' && hi == 'H') return +4;
    if (lo == 'H' && hi == 'L') return -3;
    if (lo == 'H' && hi == 'H') return +6;
    return 0;
}


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
    out->syl_btone    = (int8_t   *)calloc(total_syls,  sizeof *out->syl_btone);
    out->syl_n_segs   = (uint32_t *)calloc(total_syls,  sizeof *out->syl_n_segs);
    out->syl_segs     = (uint32_t **)calloc(total_syls,  sizeof *out->syl_segs);
    if (!out->word_shareds || !out->word_names || !out->word_n_syls
        || !out->word_syls || !out->syl_stress || !out->syl_accent
        || !out->syl_btone || !out->syl_n_segs || !out->syl_segs) {
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
            /* Phrase-boundary tone: scan the syllable's phonemes for a
             * ToBI boundary marker (it usually rides the last phoneme). */
            {
                int bt = 0;
                for (int pi = first_pi; pi <= last_pi && bt == 0; pi++)
                    bt = boundary_tone_target_st(w->phonemes[pi].accent);
                out->syl_btone[syl_g_idx] = (int8_t)bt;
            }

            /* first_pi and last_pi are assigned together in the scan
             * above, and the first_pi < 0 case already `continue`d, so
             * the true count is [1, n_phonemes]. GCC can't infer that
             * pairing: it sees last_pi - first_pi + 1 as possibly
             * negative, which wraps to a huge uint32 and trips
             * -Walloc-size-larger-than on 32-bit. State the invariant. */
            int n_seg_signed = last_pi - first_pi + 1;
            if (n_seg_signed < 0)                n_seg_signed = 0;
            if (n_seg_signed > w->n_phonemes)    n_seg_signed = w->n_phonemes;
            uint32_t n_seg_in_syl = (uint32_t)n_seg_signed;
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
     * callback uses miss_default for any miss (legacy behaviour).
     *
     * Held as raw bytes and read via spfy_le_f32(): the `data` sub-chunk
     * lands at an arbitrary VIN offset, and a misaligned float load is a
     * SIGBUS on 32-bit ARM. See common/le.h. */
    const uint8_t           *curve;
    int32_t                  curve_max_idx;
    int32_t                  curve_sub_off;
    float                    f0_edge_change_weight;
    float                    missing_join_cost;
} join_ctx_t;

/* NB: the VCF's JOIN_COST_WEIGHT / JOIN_COST_OFFSET are deliberately NOT
 * applied to a hash-HIT cell. Tested 2026-07-20 as `cost = w*cell + off`
 * with each voice's own VCF values: Tom 100% -> 93.2% and Jill 93.8% ->
 * 89.7% path UID. Both voices get worse, so the hash cells are already
 * baked with whatever weighting the voice build applied. This matches the
 * note in spfy_viterbi_replay.c that an earlier attempt to apply
 * JOIN_COST_WEIGHT here was a bug. */

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
            jc->curve = body;
        }
        p = body + sz;
        if (sz & 1) ++p;
    }
}

/* Read candidate i from whichever candidate pool is live for this slot.
 * `prsl_pool` aliases the VIN buffer (possibly unaligned, LE u32); the
 * hp-bucket fallback `bucket_pool` is an owned, aligned uint32_t array.
 * Exactly one of the two is non-NULL. */
static uint32_t pool_cand(const uint8_t *prsl_pool,
                          const uint32_t *bucket_pool, uint32_t i)
{
    return prsl_pool ? spfy_prsl_cand(prsl_pool, i) : bucket_pool[i];
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
            curve_val = jc->f0_edge_change_weight
                      * spfy_le_f32(jc->curve + (size_t)idx * 4u);
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

/* ================================================================== */
/* Embedded \!-tag pre-pass (Speechify User's Guide ch. 2, FE/text tags)*/
/*                                                                      */
/* The real engine resolves user-facing \! embedded tags in            */
/* SWIttsSSML.dll, UPSTREAM of the FE — the FE DLL has no tag parser    */
/* (it spells \!p300 out as "p three hundred"). We mirror the          */
/* text-layer subset here as a portable pure-C text transform that runs */
/* before FE dispatch, so it works on every backend (hosted DLL,        */
/* in-house, WASM, Android):                                            */
/*                                                                      */
/*   \!eos          -> sentence terminator on the preceding word        */
/*   \!ts0/c/a/r    -> character spellout modes (expand spanned chars)   */
/*   \!ny0 / \!ny1  -> 4-digit number as quantity / year (en default)    */
/*                                                                      */
/* \!pN pauses and \![SPR] pass through untouched to the inline builder. */
/* Engine-layer tags (\!bm \!di \!vp \!vd \!rp \!rd) are a later batch;  */
/* any other \!-prefixed token is dropped, per the guide's "unknown \!   */
/* tags are ignored in the speech output" rule.                         */

/* '0'->"zero" .. '9'->"nine", else NULL. */
static const char *etag_digit_name(int c)
{
    static const char *D[10] = {"zero","one","two","three","four",
                                "five","six","seven","eight","nine"};
    return (c >= '0' && c <= '9') ? D[c - '0'] : NULL;
}

/* Spoken letter name for \!tsa / \!tsc. Forms attested in the guide
 * examples (a->ay, i->aye, l->ell, m->emm, n->en, p->pea, r->ar,
 * s->ess, t->tee, z->zee, b->bee, c->cee, e->ee, f->eff); the rest use
 * the conventional spoken spelling the FE pronounces cleanly. */
static const char *etag_letter_name(int c)
{
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (c < 'a' || c > 'z') return NULL;
    static const char *L[26] = {
        "ay","bee","cee","dee","ee","eff","jee","aych","aye","jay",
        "kay","ell","emm","en","oh","pea","cue","ar","ess","tee",
        "you","vee","double you","eks","wy","zee"
    };
    return L[c - 'a'];
}

/* International Radio (NATO) alphabet for \!tsr. */
static const char *etag_radio_name(int c)
{
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (c < 'a' || c > 'z') return NULL;
    static const char *R[26] = {
        "alpha","bravo","charlie","delta","echo","foxtrot","golf",
        "hotel","india","juliet","kilo","lima","mike","november",
        "oscar","papa","quebec","romeo","sierra","tango","uniform",
        "victor","whiskey","xray","yankee","zulu"
    };
    return R[c - 'a'];
}

/* Spoken name of a punctuation/symbol char for all-character spellout
 * (\!tsc). NULL for chars with no special name (caller emits verbatim).
 * Dash/comma attested in the guide ("...dash bee...", "...comma..."). */
static const char *etag_symbol_name(int c)
{
    switch (c) {
        case '-':  return "dash";       case ',':  return "comma";
        case '.':  return "period";     case '/':  return "slash";
        case '\\': return "backslash";  case '@':  return "at";
        case '#':  return "pound";      case '$':  return "dollar";
        case '%':  return "percent";    case '&':  return "and";
        case '*':  return "star";       case '+':  return "plus";
        case '=':  return "equals";     case '!':  return "exclamation point";
        case '?':  return "question mark"; case ':': return "colon";
        case ';':  return "semicolon";  case '(':  return "open paren";
        case ')':  return "close paren";case '[':  return "open bracket";
        case ']':  return "close bracket"; case '{': return "open brace";
        case '}':  return "close brace";case '<':  return "less than";
        case '>':  return "greater than"; case '\'': return "apostrophe";
        case '"':  return "quote";      case '_':  return "underscore";
        case '|':  return "bar";        case '~':  return "tilde";
        case '`':  return "backtick";   case '^':  return "caret";
        default:   return NULL;
    }
}

static const char *ETAG_ONES[20] = {
    "zero","one","two","three","four","five","six","seven","eight","nine",
    "ten","eleven","twelve","thirteen","fourteen","fifteen","sixteen",
    "seventeen","eighteen","nineteen"
};
static const char *ETAG_TENS[10] = {
    "","","twenty","thirty","forty","fifty","sixty","seventy","eighty","ninety"
};

/* Cardinal words for 0..99 -> buf; returns bytes written (excl NUL). */
static int etag_cardinal_2(int n, char *buf, size_t cap)
{
    if (n < 20)        return snprintf(buf, cap, "%s", ETAG_ONES[n]);
    if (n % 10 == 0)   return snprintf(buf, cap, "%s", ETAG_TENS[n / 10]);
    return snprintf(buf, cap, "%s %s", ETAG_TENS[n / 10], ETAG_ONES[n % 10]);
}

/* Cardinal (quantity) words for a 4-digit number 1000..9999, e.g.
 * 1945 -> "one thousand nine hundred forty five". \!ny0 reading. */
static int etag_cardinal_4(int n, char *buf, size_t cap)
{
    int th = n / 1000, hu = (n / 100) % 10, rest = n % 100;
    char *o = buf, *eo = buf + cap;
    o += snprintf(o, (size_t)(eo - o), "%s thousand", ETAG_ONES[th]);
    if (hu)   o += snprintf(o, (size_t)(eo - o), " %s hundred", ETAG_ONES[hu]);
    if (rest) {
        o += snprintf(o, (size_t)(eo - o), " ");
        o += etag_cardinal_2(rest, o, (size_t)(eo - o));
    }
    return (int)(o - buf);
}

/* If `p` begins with "\!"<kw> AND the char after <kw> is a valid tag
 * boundary (not alphanumeric — a tag "cannot be followed immediately by
 * an alphanumeric character"), return p advanced past the keyword; else
 * NULL. */
static const char *etag_after(const char *p, const char *kw)
{
    if (p[0] != '\\' || p[1] != '!') return NULL;
    size_t kn = strlen(kw);
    if (strncmp(p + 2, kw, kn) != 0) return NULL;
    if (isalnum((unsigned char)p[2 + kn])) return NULL;
    return p + 2 + kn;
}

/* True when `text` carries an embedded \! tag this pre-pass resolves
 * (eos / spellout / year) or any \! token other than \![SPR] / \!pN,
 * which the downstream inline builder owns. Plain text and SPR-only /
 * pause-only text return 0 so their existing paths stay byte-identical. */
static int spfy_etags_need_resolve(const char *text)
{
    for (const char *q = text; (q = strstr(q, "\\!")) != NULL; q += 2) {
        int c = (unsigned char)q[2];
        if (c == '[') continue;                              /* \![SPR]  */
        if (c == 'p' && isdigit((unsigned char)q[3])) continue; /* \!pN  */
        return 1;
    }
    return 0;
}

/* Match `\!<vp|vd|rp|rd>(<digits>|r)` (volume/rate control) at p with a
 * valid tag boundary. On match sets *knd ('v'/'r'), *rel ('p' port /
 * 'd' baseline), *reset, *val, and returns p advanced past the tag; else
 * NULL. */
static const char *etag_vr(const char *p, int *knd, int *rel, int *reset, int *val)
{
    if (p[0] != '\\' || p[1] != '!') return NULL;
    int k = (unsigned char)p[2], r = (unsigned char)p[3];
    if ((k != 'v' && k != 'r') || (r != 'p' && r != 'd')) return NULL;
    const char *q = p + 4;
    if (*q == 'r' && !isalnum((unsigned char)q[1])) {
        *knd = k; *rel = r; *reset = 1; *val = 0; return q + 1;
    }
    if (isdigit((unsigned char)*q)) {
        int n = 0; const char *d = q;
        while (isdigit((unsigned char)*d)) { n = n * 10 + (*d - '0'); d++; }
        if (isalpha((unsigned char)*d)) return NULL;   /* tag can't be followed by alpha */
        *knd = k; *rel = r; *reset = 0; *val = n; return d;
    }
    return NULL;
}

/* Resolve eos / spellout / year embedded tags into plain text the FE
 * handles natively, AND emit parallel per-output-char volume/rate maps
 * from the \!vp/\!vd/\!rp/\!rd tags (consumed post-FE via each word's
 * char_start). Returns a malloc'd string (caller frees) or NULL on OOM;
 * *out_vol / *out_rate (each malloc'd, same length as the result, % values
 * where 0 means "default 100") are set on success. \!pN, \![SPR], and
 * <...> markup pass through verbatim; spellout is suspended inside them. */
static char *spfy_etags_resolve(const char *text,
                                uint16_t **out_vol, uint16_t **out_rate)
{
    size_t len = strlen(text);
    size_t cap = len * 16 + 256;          /* worst case: radio / cardinal */
    char *out = (char *)malloc(cap);
    uint16_t *mvol = (uint16_t *)calloc(cap, sizeof *mvol);
    uint16_t *mrate = (uint16_t *)calloc(cap, sizeof *mrate);
    if (!out || !mvol || !mrate) { free(out); free(mvol); free(mrate); return NULL; }
    char *o = out, *eo = out + cap - 64;  /* slack for one token */

    int sp = '0';   /* spellout: '0' default, 'c' all, 'a' alnum, 'r' radio */
    int ny = '1';   /* year mode: '1' year (default), '0' quantity         */
    int last = 0;   /* last non-space char emitted (for \!eos dedup)       */

    /* Volume/rate state. Port-specific and baseline both default to 100%
     * (the CLI has no port; a SAPI port value would seed port_*). `pv`/`pr`
     * are the volume/rate currently in effect; the maps are filled lazily
     * from `filled` up to the current output position whenever they change
     * or at the end. */
    int port_vol = 100, base_vol = 100, port_rate = 100, base_rate = 100;
    int pv = 100, pr = 100;
    size_t filled = 0;
#define ETAG_FLUSH() do { size_t _e = (size_t)(o - out); \
        for (size_t _i = filled; _i < _e; _i++) { mvol[_i] = (uint16_t)pv; \
            mrate[_i] = (uint16_t)pr; } filled = _e; } while (0)

    const char *p = text;
    while (*p && o < eo) {
        const char *a;
        /* ---- \!vp/\!vd/\!rp/\!rd volume & rate control ---- */
        {
            int k, rel, rst, val;
            const char *a2 = etag_vr(p, &k, &rel, &rst, &val);
            if (a2) {
                ETAG_FLUSH();                    /* chars so far keep old pv/pr */
                int basis = (rel == 'p') ? (k == 'v' ? port_vol : port_rate)
                                         : (k == 'v' ? base_vol : base_rate);
                int nv = rst ? basis : (val * basis / 100);
                if (k == 'v') { if (nv < 0) nv = 0; pv = nv; }
                else { if (nv < 33) nv = 33; if (nv > 300) nv = 300; pr = nv; }
                p = a2; continue;
            }
        }
        /* ---- mode tags (consumed, no output) ---- */
        if      ((a = etag_after(p, "ts0"))) { sp = '0'; p = a; continue; }
        else if ((a = etag_after(p, "tsc"))) { sp = 'c'; p = a; continue; }
        else if ((a = etag_after(p, "tsa"))) { sp = 'a'; p = a; continue; }
        else if ((a = etag_after(p, "tsr"))) { sp = 'r'; p = a; continue; }
        else if ((a = etag_after(p, "ny0"))) { ny = '0'; p = a; continue; }
        else if ((a = etag_after(p, "ny1"))) { ny = '1'; p = a; continue; }

        /* ---- \!eos: force a sentence end on the preceding word ---- */
        if ((a = etag_after(p, "eos"))) {
            if (last != '.' && last != '!' && last != '?') {
                if (o > out && o[-1] == ' ') o--;   /* swallow trailing space */
                *o++ = '.'; last = '.';
            }
            p = a; continue;
        }

        /* ---- pass-through tokens (no spellout inside) ---- */
        if (p[0] == '\\' && p[1] == '!' && p[2] == '[') {       /* \![SPR]   */
            const char *close = strchr(p + 3, ']');
            const char *e = close ? close + 1 : p + strlen(p);
            while (p < e && o < eo) *o++ = *p++;
            last = 'x'; continue;
        }
        if (p[0] == '\\' && p[1] == '!' && p[2] == 'p'
            && isdigit((unsigned char)p[3])) {                  /* \!pN      */
            *o++ = *p++; if (o < eo) *o++ = *p++;               /* "\!"      */
            while (o < eo && (*p == 'p' || isdigit((unsigned char)*p))) *o++ = *p++;
            last = 'x'; continue;
        }
        if (p[0] == '<') {                                      /* SSML/pron */
            const char *gt = strchr(p, '>');
            const char *e = gt ? gt + 1 : p + strlen(p);
            while (p < e && o < eo) *o++ = *p++;
            last = 'x'; continue;
        }

        /* ---- other / unknown \! token: dropped (guide: ignored) ---- */
        if (p[0] == '\\' && p[1] == '!') {
            p += 2;
            while (*p && !isspace((unsigned char)*p) && *p != '[') p++;
            if (*p == '[') { const char *c = strchr(p, ']'); p = c ? c + 1 : p + strlen(p); }
            continue;
        }

        /* ---- whitespace: emit verbatim (word separator) ---- */
        if (isspace((unsigned char)*p)) { *o++ = *p++; continue; }

        int c = (unsigned char)*p;

        /* ---- spellout modes ---- */
        if (sp != '0') {
            const char *name = NULL;
            if (sp == 'c') {
                if (isdigit(c))      name = etag_digit_name(c);
                else if (isalpha(c)) name = etag_letter_name(c);
                else                 name = etag_symbol_name(c);
            } else if (sp == 'a') {
                if (isdigit(c))      name = etag_digit_name(c);
                else if (isalpha(c)) name = etag_letter_name(c);
            } else { /* 'r' radio */
                if (isdigit(c))      name = etag_digit_name(c);
                else if (isalpha(c)) name = etag_radio_name(c);
            }
            if (name) {
                size_t nl = strlen(name);
                if (o + nl + 2 >= eo) break;
                if (o > out && o[-1] != ' ') *o++ = ' ';
                memcpy(o, name, nl); o += nl; *o++ = ' ';
                last = (unsigned char)name[nl - 1];
                p++; continue;
            }
            /* alnum-mode punctuation / unmapped symbol: emit verbatim so the
             * FE interprets it normally (may trigger a phrase break). */
            *o++ = (char)c; last = c; p++; continue;
        }

        /* ---- default mode: \!ny0 forces 4-digit quantity reading ---- */
        if (ny == '0' && isdigit(c)) {
            int nd = 0; while (isdigit((unsigned char)p[nd])) nd++;
            int prev_alnum = (o > out && isalnum((unsigned char)o[-1]));
            int nxt = (unsigned char)p[nd];
            if (nd == 4 && !prev_alnum && nxt != '.' && nxt != ',') {
                int val = (p[0]-'0')*1000 + (p[1]-'0')*100
                        + (p[2]-'0')*10   + (p[3]-'0');
                if (val >= 1000) {
                    if (o > out && o[-1] != ' ') *o++ = ' ';
                    int w = etag_cardinal_4(val, o, (size_t)(eo - o));
                    if (w > 0 && o + w < eo) {
                        o += w; last = o[-1]; p += 4; continue;
                    }
                }
            }
        }

        *o++ = (char)c; last = c; p++;
    }
    ETAG_FLUSH();
#undef ETAG_FLUSH
    *o = '\0';
    *out_vol = mvol;
    *out_rate = mrate;
    return out;
}

/* Parse `\\![SPR]` and emit an FE tagged-output string. SPR `.0`/`.1`
 * (`.2`) starts a syllable with given stress; subsequent single chars
 * are phonemes. Output matches the FE's own tagged format so
 * fe_parse_tagged_output() can consume it.
 *
 * Output: #{. pau(p25) <SPR (0,N) undef,STRESS [.S,H* arpa(p100) ... ] > pau(p50) } %%
 *
 * When `phrase_final` is set, the LAST syllable carries the `;L-L%`
 * boundary tone (falling intonation) the engine puts on any word that ends
 * a phrase/utterance, so an inline-SPR word at a phrase boundary falls like
 * a natural one instead of staying flat. The first syllable keeps its `H*`
 * pitch accent (unless it is also the last, in which case the boundary tone
 * takes the slot — matching the engine's single-syllable phrase-final form,
 * e.g. `is` -> `.1;L-L%`).
 *
 * Returns positive byte count on success, 0 on parse failure. */
static int spr_inline_to_tagged(const char *text, char *out, size_t out_n,
                                int phrase_final)
{
    const char *p = strchr(text, '[');
    if (!p) return 0;
    p++;
    const char *end_br = strrchr(p, ']');
    if (!end_br) return 0;
    int max_stress = 0;
    const char *last_syl = NULL;
    for (const char *q = p; q < end_br; ++q)
        if (*q == '.' && q + 1 < end_br
            && q[1] >= '0' && q[1] <= '9') {
            if ((q[1] - '0') > max_stress) max_stress = q[1] - '0';
            last_syl = q;                    /* track final syllable marker */
        }
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
            const char *marker = q;          /* this syllable's '.' */
            int stress = q[1] - '0';
            q += 2;
            /* Boundary tone on the phrase-final syllable wins the accent
             * slot; otherwise the first syllable carries the H* accent. */
            const char *accent = (phrase_final && marker == last_syl) ? ";L-L%"
                               : first_syl                            ? ",H*"
                               :                                        "";
            n = snprintf(o, (size_t)(eo - o),
                "%s.%d%s ", first_syl ? "" : " ", stress, accent);
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

/* Inline-markup segment kinds. PLAIN runs go through the DLL FE; the two
 * special kinds carry explicit pronunciation the DLL FE cannot read and
 * would spell out literally. */
enum { SEG_PLAIN = 0, SEG_SPR, SEG_PRON, SEG_PAUSE };

/* `lt` points at a '<'. True if it opens a `<pron ...>` tag — name matched
 * case-insensitively and delimited by whitespace, '/', or '>'. */
static int is_pron_open(const char *lt)
{
    static const char kw[] = "pron";
    if (lt[0] != '<') return 0;
    for (int i = 0; i < 4; i++) {
        int c = (unsigned char)lt[1 + i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != kw[i]) return 0;
    }
    int d = (unsigned char)lt[5];
    return d == '\0' || isspace(d) || d == '/' || d == '>';
}

/* One-past-end of a `<pron ...>` construct starting at `lt`: past `/>` for
 * the self-closing form, past `</pron>` for `<pron ...>annotation</pron>`,
 * or just past the open tag's `>` if no close tag follows. NULL if there is
 * no `>` at all. */
static const char *pron_construct_end(const char *lt)
{
    const char *gt = strchr(lt, '>');
    if (!gt) return NULL;
    if (gt > lt && gt[-1] == '/') return gt + 1;        /* <pron .../>        */
    const char *close = strstr(gt + 1, "</pron>");       /* <pron ...>x</pron> */
    if (close) { const char *cg = strchr(close, '>'); if (cg) return cg + 1; }
    return gt + 1;                                        /* unclosed open tag */
}

/* Find the next inline-markup token (`\![...]` SPR escape, `\!pN` pause,
 * or `<pron ...>` tag) at or after `s`. On success returns its start, sets
 * *kind and *tok_end (one-past). Returns NULL if none remains. Unterminated
 * tokens (no `]` / no `>`) are skipped — treated as plain text. */
static const char *find_inline_token(const char *s, int *kind,
                                     const char **tok_end)
{
    for (const char *q = s; *q; ++q) {
        if (q[0] == '\\' && q[1] == '!' && q[2] == '[') {
            const char *close = strchr(q + 3, ']');
            if (close) { *kind = SEG_SPR; *tok_end = close + 1; return q; }
        } else if (q[0] == '\\' && q[1] == '!' && q[2] == 'p'
                   && isdigit((unsigned char)q[3])) {
            const char *e = q + 3;
            while (isdigit((unsigned char)*e)) ++e;
            *kind = SEG_PAUSE; *tok_end = e; return q;          /* \!pN */
        } else if (q[0] == '<' && is_pron_open(q)) {
            const char *e = pron_construct_end(q);
            if (e) { *kind = SEG_PRON; *tok_end = e; return q; }
        }
    }
    return NULL;
}

/* True when `text` carries inline markup the DLL FE can't read — an
 * `\![...]` SPR escape, a `\!pN` pause, or a `<pron ...>` tag — and so must
 * be routed through build_inline_mixed_tagged (which also covers the lone-
 * token case). Plain text returns 0. */
static int spfy_text_has_inline_markup(const char *text)
{
    int kind; const char *te;
    return find_inline_token(text, &kind, &te) != NULL;
}

/* Remove the single `;L-L%` phrase-final boundary tone an in-house-FE core
 * carries when a `<pron>` word is phonemized in isolation, for use when the
 * word is NOT phrase-final in the merged sentence (keeps mid-sentence pron
 * words from falling under SPFY_PROSODY_REALIZE). */
static void strip_boundary_tone(char *s)
{
    char *b = strstr(s, ";L-L%");
    if (b) memmove(b, b + 5, strlen(b + 5) + 1);
}

/* Locate the inner word/pause "core" of one tagged-output block: the span
 * AFTER the leading pad `pau(...)` and BEFORE the trailing pad `pau(...)`.
 * Every block (DLL-FE or spr_inline_to_tagged) is wrapped as
 *   `#{<T> pau(pNN) <word>...<word> pau(pNN) } %%`
 * so the leading/trailing pads are the FIRST and LAST `pau(` occurrences.
 * Internal pads (a comma break's `pau(p50) } {. pau(p25)`) sit between
 * them and are kept verbatim. Returns 1 and sets start+len on success, or
 * 0 when the block has no word core (empty / punctuation-only utterance). */
static int spr_tagged_core(const char *block, const char **start, size_t *len)
{
    const char *first = strstr(block, "pau(");
    if (!first) return 0;
    const char *after = strchr(first, ')');
    if (!after) return 0;
    after++;                                 /* just past the leading pad's ) */

    const char *last = NULL, *q = block;     /* trailing pad = last "pau(" */
    while ((q = strstr(q, "pau(")) != NULL) { last = q; q += 4; }
    if (!last || last <= after) return 0;    /* no distinct trailing pad */

    while (after < last && (unsigned char)*after <= ' ') after++;
    const char *e = last;
    while (e > after && (unsigned char)e[-1] <= ' ') e--;
    if (e <= after) return 0;
    *start = after;
    *len = (size_t)(e - after);
    return 1;
}

/* Map a character to the tagged-output phrase-terminator marker it implies,
 * or 0 if it is not phrase-breaking punctuation. Comma / semicolon and
 * period / `!` / `?` pass through verbatim (all accepted by the tagged
 * parser); colon maps to ';' since the parser has no ':' marker. Used to
 * decide whether an inline-SPR segment junction needs a phrase break. */
static char spr_break_marker(int c)
{
    switch (c) {
        case ',': case ';':
        case '.': case '!': case '?': return (char)c;
        case ':':                     return ';';
        default:                      return 0;
    }
}

/* Build ONE flowing tagged-output utterance from text that mixes plain
 * words with inline markup — `\![...]` SPR escapes, `\!pN` pause tags,
 * and/or `<pron ...>` tags — none of which the DLL FE can read (it would
 * spell the markup out literally).
 *
 * `\!pN` is not phonemized: it injects a `pau(pN)` at its position — either
 * REPLACING an adjacent punctuation pause (when it sits immediately before
 * the mark) or ADDING one (elsewhere), matching the User's Guide.
 *
 * Rendering each segment as its own utterance (the obvious approach) gives
 * every run its own leading + trailing `pau` pad, so at a markup junction
 * the plain run's trailing pad and the markup run's leading pad stack into
 * a long, unnatural silence — and there is no cross-word WSOLA join.
 * Instead we phonemize each run with the right backend — plain runs via the
 * DLL FE, SPR via spr_inline_to_tagged, `<pron>` via the in-house FE (which
 * owns the Balabolka `<pron sym=...>` parser) — strip each block down to its
 * word core (dropping the OUTER pad pauses), and concatenate the cores under
 * a SINGLE leading/trailing pad. The markup words then sit inline among the
 * plain words in one phrase, flowing as a continuation, and downstream
 * USel/WSOLA join across the boundary like any other word pair.
 *
 * Phrase-breaking punctuation that lands at a segment JUNCTION (e.g. the
 * comma in `\![radio], commercial` — which the FE silently drops from a
 * segment-edge position) is re-materialised as a real phrase break so the
 * comma pause is preserved; only spans WITHOUT such punctuation join as a
 * pause-free continuation. A markup word that ends up phrase-final keeps its
 * `;L-L%` boundary tone (falling intonation); a non-final one has it removed.
 *
 * Returns a malloc'd tagged string (caller frees) or NULL on failure. */
static char *build_inline_mixed_tagged(spfy_fe_t *fe, const char *text)
{
    size_t tlen   = strlen(text);
    /* DLL-FE output expands plain text by a large factor (each word grows
     * to `<word (s,l) pos,k [.k,acc phon(pNNN) ...]>`); size generously and
     * bound-check on append so we never overflow. */
    size_t segcap = tlen * 80 + 65536;
    size_t outcap = segcap + 1024;
    char *seg = (char *)malloc(tlen + 1);
    char *segbuf = (char *)malloc(segcap);
    char *acc = (char *)malloc(outcap);
    if (!seg || !segbuf || !acc) { free(seg); free(segbuf); free(acc); return NULL; }

    size_t acc_len = 0;
    acc_len += (size_t)snprintf(acc, outcap, "#{. pau(p25) ");

    const char *p = text;
    int ok = 0, any_core = 0;
    char pend_break = 0;                      /* break punct trailing prev seg */
    int pend_pause = 0;                       /* \!pN duration to inject (ms)  */
    while (*p) {
        int kind = SEG_PLAIN;
        const char *tok_end = NULL;
        const char *tok = find_inline_token(p, &kind, &tok_end);
        const char *seg_start = p;
        const char *seg_end;
        if (tok == p) {                      /* this segment IS a markup token */
            seg_end = tok_end;
        } else if (tok) {                    /* plain run up to the next token */
            seg_end = tok; kind = SEG_PLAIN;
        } else {                             /* trailing plain run             */
            seg_end = p + strlen(p); kind = SEG_PLAIN;
        }
        int is_markup = (kind != SEG_PLAIN);
        size_t seg_len = (size_t)(seg_end - seg_start);
        p = seg_end;
        if (seg_len == 0) continue;

        /* `\!pN` pause: not a phonemizable run — record the duration and
         * whether it sits immediately before punctuation, then apply it at
         * the next junction (or before the closing pad). Per the guide a
         * pause right before a punctuation mark REPLACES that mark's default
         * pause; elsewhere it adds one. `pend_break` is left untouched so a
         * preceding comma still triggers its phrase break. */
        if (kind == SEG_PAUSE) {
            int n = atoi(seg_start + 3);     /* digits after "\!p"        */
            if (n < 1)     n = 1;
            if (n > 32767) n = 32767;        /* guide valid range 1..32767 */
            pend_pause = n;
            continue;
        }

        /* Skip whitespace-only plain runs (the space between a word and a
         * markup token) — no phonemes, and the FE would emit an empty utt.
         * A pending junction break carries across to the next real seg. */
        if (!is_markup) {
            int ws_only = 1;
            for (size_t i = 0; i < seg_len; i++)
                if (!isspace((unsigned char)seg_start[i])) { ws_only = 0; break; }
            if (ws_only) continue;
        }

        /* Phrase-break punctuation adjacent to THIS junction: the break
         * char trailing the previous seg, or leading this one (first non-ws
         * char). The FE keeps a comma INSIDE a run but drops one at a
         * run edge, so we re-insert it here. */
        char lead = 0;
        {
            const char *s = seg_start;
            while (s < seg_end && isspace((unsigned char)*s)) s++;
            if (s < seg_end) lead = spr_break_marker((unsigned char)*s);
        }

        /* A markup run is phrase-final when the next non-whitespace content
         * is phrase-breaking punctuation or end-of-text — i.e. a break (or
         * the closing pad) will follow it. Its last syllable then keeps the
         * falling boundary tone; otherwise the tone is suppressed/removed.
         * `p` already points past this run. */
        int seg_final = 0;
        if (is_markup) {
            const char *la = p;
            while (*la && isspace((unsigned char)*la)) la++;
            seg_final = (*la == '\0' || spr_break_marker((unsigned char)*la) != 0);
        }

        memcpy(seg, seg_start, seg_len);
        seg[seg_len] = '\0';

        const char *block;
        if (kind == SEG_SPR) {
            if (spr_inline_to_tagged(seg, segbuf, segcap, seg_final) <= 0)
                goto done;                   /* fail */
            block = segbuf;
        } else if (kind == SEG_PRON) {
            /* The in-house FE owns the <pron sym=...> parser. It phonemizes
             * the tag in isolation (so always emits the phrase-final boundary
             * tone); drop that tone when the word is NOT phrase-final here. */
            if (spfy_fe_internal_text_to_tagged(seg, segbuf, segcap) < 0)
                goto done;                   /* fail (truncation rc=1 is ok) */
            if (!seg_final) strip_boundary_tone(segbuf);
            block = segbuf;
        } else {
            if (spfy_fe_text_to_tagged(fe, seg, segbuf, segcap) <= 0) continue;
            block = segbuf;
        }

        const char *core; size_t core_len;
        if (!spr_tagged_core(block, &core, &core_len)) continue;  /* empty utt */

        /* Separator: a real phrase break when a comma/period/... sits at
         * this junction, otherwise a plain space (pause-free continuation).
         * Bound-checked (break separator is ~24 bytes + the final close). */
        if (acc_len + core_len + 64 >= outcap) goto done;        /* fail-safe */
        if (any_core) {
            char br = pend_break ? pend_break : lead;
            if (pend_pause) {
                /* \!pN renders as a phrase break carrying a `pau(uN)` USER
                 * pause that the synth loop turns into N ms of injected
                 * silence (the FE pipeline does not size silence from the
                 * structural `pau(pN)` value). Keep the adjacent punctuation
                 * as the opener term so its prosody is preserved; a bare
                 * pause (no punctuation here) uses a neutral ',' break. */
                char term = br ? br : ',';
                acc_len += (size_t)snprintf(acc + acc_len, outcap - acc_len,
                                            " pau(p50) } {%c pau(u%d) ", term, pend_pause);
                pend_pause = 0;
            } else if (br) {
                acc_len += (size_t)snprintf(acc + acc_len, outcap - acc_len,
                                            " pau(p50) } {%c pau(p100) ", br);
            } else {
                acc[acc_len++] = ' ';
            }
        }
        memcpy(acc + acc_len, core, core_len);
        acc_len += core_len;
        any_core = 1;

        /* Remember break punct trailing this seg (last non-ws char) for the
         * NEXT junction — covers `word, \![...]` as well as `\![...], word`. */
        pend_break = 0;
        {
            const char *e = seg_end;
            while (e > seg_start && isspace((unsigned char)e[-1])) e--;
            if (e > seg_start) pend_break = spr_break_marker((unsigned char)e[-1]);
        }
    }

    if (!any_core) goto done;                /* nothing phonemizable */
    acc_len += (size_t)snprintf(acc + acc_len, outcap - acc_len,
                                " pau(p50) } %%");
    ok = 1;

done:
    free(seg);
    free(segbuf);
    if (!ok) { free(acc); return NULL; }
    return acc;
}

/* Per-call synth: text -> FE -> USel -> WSOLA -> sink. The voice is
 * loaded once by the caller (CLI main() or SAPI Speak()) and reused.
 * The sink is any open spfy_wav_writer_t (file or callback mode); we
 * neither open nor close it. Text carrying inline pronunciation markup the
 * DLL FE can't read — `\![...]` SPR escapes or `<pron ...>` tags — is
 * routed through build_inline_mixed_tagged, which phonemizes each run with
 * the right backend and merges everything into one flowing utterance so the
 * markup reads as a continuation rather than standalone phrases with long
 * junction pauses (and so the DLL FE never sees, and literally speaks, the
 * markup). */
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
    uint16_t *syl_vol = NULL;      /* per-syllable volume % from \!vp/\!vd (0 = 100),
                                    * indexed by fe_shared-1 (reliable, unlike the
                                    * tree-word -> parsed-word count) */
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
    /* Per-hp boundary-tone target (signed ST; 0 = none). Built from the
     * slot tree's syllable fe_shared id → fe_utt.syl_btone, the reliable
     * mapping (shared = global_syl_index + 1). Option A: biases the
     * per-slot F0 target to steer unit selection toward naturally
     * contoured units. SPFY_PROSODY_REALIZE-gated. */
    int8_t   *hp_btone = NULL;
    int rc;
    char *etags_text = NULL;     /* owns the \!-tag pre-pass output (freed at cleanup) */
    uint16_t *etag_vol = NULL;   /* per-char volume % map (0 = default 100) */
    uint16_t *etag_rate = NULL;  /* per-char rate % map   (0 = default 100) */
    size_t etag_maps_n = 0;      /* valid length of etag_vol / etag_rate     */
    spfy_prosody_hints_init(&hints);

    /* Embedded \!-tag pre-pass (Speechify User's Guide ch. 2 FE/text tags):
     * resolve \!eos, \!ts0/c/a/r spellout, and \!ny0/1 number-mode into plain
     * text the FE handles natively. \!pN and \![SPR]/<pron> pass through to
     * the inline builder below. Plain untagged text is untouched, so the
     * byte-exact whole-text FE path (and the audit corpus) is unaffected. */
    if (spfy_etags_need_resolve(text)) {
        etags_text = spfy_etags_resolve(text, &etag_vol, &etag_rate);
        if (etags_text) { text = etags_text; etag_maps_n = strlen(etags_text); }
    }
    if (getenv("SPFY_ETAGS_DUMP"))
        fprintf(stderr, "[etags] resolved: %s\n", text);

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
        const char *tagged_file = getenv("SPFY_TAGGED_FILE");
        if (tagged_file) {
            /* Experiment hook: synth from a tagged-output file verbatim,
             * bypassing the FE text pass entirely. Lets A/B tests hand-edit
             * ToBI accent / boundary-tone marks in otherwise identical
             * tagged text. The positional <text> CLI arg is ignored. */
            FILE *tf = fopen(tagged_file, "rb");
            if (!tf) {
                fprintf(stderr, "SPFY_TAGGED_FILE: cannot open %s\n",
                        tagged_file);
                rc = SPFY_E_INVAL; goto fail;
            }
            fseek(tf, 0, SEEK_END);
            long tsz = ftell(tf);
            fseek(tf, 0, SEEK_SET);
            if (tsz < 0) { fclose(tf); rc = SPFY_E_INVAL; goto fail; }
            char *tagged = (char *)malloc((size_t)tsz + 1u);
            if (!tagged) { fclose(tf); rc = SPFY_E_NOMEM; goto fail; }
            size_t trd = fread(tagged, 1, (size_t)tsz, tf);
            fclose(tf);
            tagged[trd] = '\0';
            fprintf(stderr, "[spfy] FE bypass: tagged text from %s "
                    "(%zu bytes)\n", tagged_file, trd);
            rc = spfy_fe_synth_tagged(v->fe, tagged, &hints, &utt_unused);
            free(tagged);
        } else if (spfy_text_has_inline_markup(text)) {
            /* Inline pronunciation markup — `\![...]` SPR escapes and/or
             * `<pron ...>` tags — that the DLL FE can't read. Phonemize the
             * plain runs with the DLL FE, the SPR runs with
             * spr_inline_to_tagged, and the `<pron>` runs with the in-house
             * FE, then merge into ONE flowing utterance so the markup words
             * read as a continuation rather than standalone phrases with long
             * junction pauses. Also covers the lone-token case (a single
             * `\![...]` or `<pron ...>` with no surrounding words). */
            char *mixed = build_inline_mixed_tagged(v->fe, text);
            if (!mixed) {
                fprintf(stderr, "build_inline_mixed_tagged: bad inline markup\n");
                rc = SPFY_E_INVAL; goto fail;
            }
            if (getenv("SPFY_ETAGS_DUMP"))
                fprintf(stderr, "[etags] tagged: %s\n", mixed);
            rc = spfy_fe_synth_tagged(v->fe, mixed, &hints, &utt_unused);
            free(mixed);
        } else if (getenv("SPFY_FE_INTERNAL")) {
            /* In-house FE path: text → fe_internal → tagged-text →
             * spfy_fe_synth_tagged → slot tree. Skips the DLL entirely.
             * Quality is lower than the hosted FE (no engine-specific
             * lexicon, simple syllabifier, no full prosody model) but
             * portable to ARM and platforms where the DLL can't run.
             *
             * On Windows desktop this env var is the ONLY way to force
             * the in-house FE — the default `spfy_fe_synth_text` call
             * below resolves at link time to the hosted DLL backend
             * (SPFY_FE_HOSTED=ON build) and gives 100% engine UID match
             * since the FE output is bit-identical to the engine's. On
             * Android NDK / WASM builds the same `spfy_fe_synth_text`
             * call resolves to fe_stub.c which uses the in-house FE
             * (no DLL loader on ARM / wasm32) — so on those platforms
             * the env var is redundant. Logged once on first synth so
             * the user can verify which backend is live. */
            static int logged = 0;
            if (!logged) {
                fprintf(stderr,
                    "[spfy] FE backend: IN-HOUSE pure-C "
                    "(SPFY_FE_INTERNAL forced override)\n");
                logged = 1;
            }
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
            static int logged = 0;
            if (!logged) {
#if defined(SPFY_FE_EMU)
                fprintf(stderr,
                    "[spfy] FE backend: EMULATED DLL "
                    "(SWIttsFe-<lang> via host_emu, portable to "
                    "arm64/wasm). The image is picked from the voice's "
                    "VCF language -- see the [fe_host] line above.\n");
#else
                fprintf(stderr,
                    "[spfy] FE backend: IN-HOUSE pure-C "
                    "(no DLL loader on this build)\n");
#endif
                logged = 1;
            }
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

    /* [live-trace] one-shot header: sample rate (for waveform decode) plus
     * corpus/phrase counts so the viz can size its lattice up front. */
    spfy_trace_eventf("meta",
        "{\"sample_rate\":%u,\"n_units\":%u,\"n_phrases\":%u}",
        v->vdb.sample_rate, v->units.n_units, n_phrases);

    /* [live-trace] global spoken-word counter (across ALL phrases), aligned
     * with the emitted `word` events. Tagged onto each `unit` event so the viz
     * colors every stitched slice by its true word — stable the instant a
     * slice lands, not re-derived from partial state. -1 until the first word.
     * Guarded so the shipped (non-trace) build's control flow is identical. */
#ifdef SPFY_TRACE
    int g_wseq = -1;
#endif

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
    /* Pass THIS voice's phone inventory: ctx[] is in engine phone-id
     * (feat["name"]) numbering, which differs per voice. */
    if ((rc = spfy_derive_slice_ctx(&tree, seg_names, n_segs_arr,
                                    v->phone_order.phone_names,
                                    v->phone_order.n_phones, &slice_ctx))
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
    if (synth_is_verbose()) {
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
    }
    if (n_hp == 0) { rc = SPFY_E_FORMAT; goto fail; }

    /* [live-trace] phrase start: emitted once n_hp is known and before any
     * candidate scoring, so the viz gets phrase -> slots -> cands -> pick ->
     * unit in temporal order. `t` is the output sample count at phrase entry
     * (the phrase's audio start on the timeline). */
    spfy_trace_eventf("phrase",
        "{\"idx\":%u,\"n_hp\":%u,\"t\":%u}",
        phrase_idx, n_hp, sink->n_samples_written);

#ifdef SPFY_TRACE
    /* [live-trace] Per-word phone breakdown for the Synthesis Tracer's word
     * grouping ({text, [arpabet phones]}, keyed by phrase). `parsed` is the
     * common FE output (populated for hosted AND in-house backends), so this
     * works regardless of SPFY_FE_HOSTED. The whole block is #ifdef'd out of
     * the shipped build so the buffer construction costs nothing there. */
    for (int wi_ = 0; wi_ < parsed->n_words; ++wi_) {
        const fe_parsed_word_t *w_ = &parsed->words[wi_];
        if ((uint32_t)w_->phrase_id != phrase_idx) continue;
        char wbuf[4096];
        int wo = snprintf(wbuf, sizeof wbuf, "{\"phrase\":%u,\"text\":\"", phrase_idx);
        for (const char *t = w_->text; *t && wo < (int)sizeof wbuf - 8; ++t) {
            if (*t == '"' || *t == '\\') wbuf[wo++] = '\\';
            wbuf[wo++] = *t;
        }
        wo += snprintf(wbuf + wo, sizeof wbuf - (size_t)wo, "\",\"phones\":[");
        for (int pi = 0; pi < w_->n_phonemes && wo < (int)sizeof wbuf - 24; ++pi) {
            wo += snprintf(wbuf + wo, sizeof wbuf - (size_t)wo, "%s\"%s\"",
                           pi ? "," : "", w_->phonemes[pi].arpabet);
        }
        if (wo > (int)sizeof wbuf - 4) wo = (int)sizeof wbuf - 4;
        snprintf(wbuf + wo, sizeof wbuf - (size_t)wo, "]}");
        spfy_trace_event("word", wbuf);
    }
#endif

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
    hp_btone = (int8_t *)calloc(n_hp, sizeof *hp_btone);
    if (!hp_btone) { rc = SPFY_E_NOMEM; free(seg_names); goto fail; }
    for (uint32_t hp = 0; hp < n_hp; ++hp) {
        uint32_t s = hp_to_post[hp];
        /* Walk parent chain HP -> SYL -> WORD. */
        uint32_t syl_post = tree.slots[s].parent_idx;
        uint32_t word_post = (syl_post < tree.n_slots)
                             ? tree.slots[syl_post].parent_idx
                             : UINT32_MAX;
        hp_word_idx[hp] = word_post;
        /* Per-hp boundary tone via the syllable slot's fe_shared id.
         * fe_shared = global_syl_index + 1 (parsed_to_fe_utt assigns
         * shared ids sequentially from 1 for the leading pad). This is
         * the reliable HP→syllable map; the tree-word→parsed-word count
         * is NOT (it merged adjacent words in earlier attempts). */
        if (syl_post < tree.n_slots
            && tree.slots[syl_post].kind == SPFY_SK_SYLLABLE
            && fe_utt.syl_btone) {
            uint32_t shared = tree.slots[syl_post].fe_shared;
            if (shared >= 1 && (shared - 1) < fe_utt.n_syls)
                hp_btone[hp] = fe_utt.syl_btone[shared - 1];
        }
    }

    /* Per-hp SSML / Balabolka prosody overrides. Built once from the
     * parsed FE output so the CART block reads them in O(1) per slot.
     * Tree's WORD slots are emitted in fe_parsed_t order, so we count
     * them up to map tree-post-order word index → parsed->words[]. */
    hp_pitch_st = (int8_t *)calloc(n_hp, sizeof *hp_pitch_st);
    hp_rate_pct = (int8_t *)calloc(n_hp, sizeof *hp_rate_pct);
    syl_vol = (uint16_t *)calloc(fe_utt.n_syls ? fe_utt.n_syls : 1,
                                 sizeof *syl_vol);
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

    /* Per-syllable volume map for the \!vp/\!vd embedded tags, keyed by
     * fe_shared-1 (the reliable HP -> syllable id that hp_btone also uses;
     * the tree-word -> parsed-word count drifts when adjacent words merge).
     * fe_utt content words (index 1..N) are this phrase's parsed words in
     * order; each carries char_start into the FE-fed text the etag_vol map
     * is aligned to. */
    if (syl_vol && etag_vol) {
        const fe_parsed_t *pv = (const fe_parsed_t *)spfy_fe_get_parsed(v->fe);
        if (pv) {
            uint32_t fw = 1;                 /* fe_utt content word index */
            for (int wi = 0; wi < pv->n_words; ++wi) {
                if (pv->words[wi].phrase_id != (int)phrase_idx) continue;
                int cs = pv->words[wi].char_start;
                uint16_t vol = (cs >= 0 && (size_t)cs < etag_maps_n)
                               ? etag_vol[cs] : 0;
                if (vol && fw < fe_utt.n_words && fe_utt.word_syls[fw]) {
                    for (uint32_t j = 0; j < fe_utt.word_n_syls[fw]; ++j) {
                        uint32_t sh = fe_utt.word_syls[fw][j];   /* shared id */
                        if (sh >= 1 && (sh - 1) < fe_utt.n_syls)
                            syl_vol[sh - 1] = vol;
                    }
                }
                ++fw;
            }
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
         * project_prsl_92_fallback): substitute the boundary marker for
         * the side that represents "same phone, other half" — for a
         * LEFT-half slot (side 0), substitute right; for a RIGHT-half
         * slot (side 1), substitute left. Verified across all 20 pool_n=0
         * cases in phn_001/003/023/039 + mp_031/038: every engine-returned
         * UID-set is exactly the contents of the substituted key.
         *
         * The marker is one past the last hp_class, i.e. n_phones*2 — 92
         * for Tom's 46 phones, which is where the old hardcoded 92 came
         * from. It is 62 for the 31-phone es-MX voices, so the literal
         * pointed at a nonexistent class for them.
         * SPFY_NO_PRSL_92_FALLBACK=1 disables. */
        uint32_t hp_bound = v->phone_order.n_phones
                              ? v->phone_order.n_phones * 2u : 92u;
        uint32_t ck = spfy_prsl_context_key(ctx5[1], ctx5[2], ctx5[3]);
        /* Two possible pool provenances, kept apart because they differ in
         * alignment: PRSL hands back a u32[] aliasing the VIN buffer at an
         * arbitrary offset (read via spfy_prsl_cand), while the hp-bucket
         * fallback below is an owned, naturally-aligned array. Exactly one
         * is non-NULL; see pool_cand(). */
        const uint8_t  *pool       = NULL;
        const uint32_t *pool_bucket = NULL;
        uint32_t pool_n = 0;
        spfy_prsl_lookup(&v->prsl, ck, &pool, &pool_n);
        if (pool_n == 0 && !getenv("SPFY_NO_PRSL_92_FALLBACK")) {
            uint32_t side = ctx5[2] & 1u;
            uint32_t l_fb = side ? hp_bound : ctx5[1];
            uint32_t r_fb = side ? ctx5[3] : hp_bound;
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
            int silence_slot_dbg = ctx_is_silence(v, ctx5[2]);
            cart_feat_ctx_t cfc_dbg = { &adapter, q5, v, 0 };
            if (!silence_slot_dbg || !getenv("SPFY_NO_SILENCE_CART")) {
                uint32_t didx = phone_to_labl(v, ctx5[2] >> 1);
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
                        pool ? spfy_prsl_cand(pool, i) : SILENCE_SENTINEL_UID);
            }
            fprintf(stderr, "]}\n");
        }
        if (pool_n == 0) ++n_empty;

        /* Triphone miss: fall back to all v->units with this phone_center+side. */
        uint32_t fb_hp = ctx5[2];
        if (pool_n == 0 && fb_hp < v->hpc_buckets && v->bucket_n[fb_hp] > 0) {
            pool        = NULL;          /* switch provenance; see pool_cand */
            pool_bucket = v->bucket[fb_hp];
            pool_n = v->bucket_n[fb_hp];
            if (pool_n > MAX_CANDS_PER_SLOT) pool_n = MAX_CANDS_PER_SLOT;
        }

        /* [live-trace] slot header: the half-phone target and its final
         * candidate-pool size (after PRSL, 92-fallback, and bucket fallback).
         * `phone` is the hp-class centre byte; the viz labels the slot from
         * the eventual pick's VIN metadata. Per-candidate `cand` events for
         * this slot follow once scoring completes. */
        spfy_trace_eventf("slot",
            "{\"slot\":%u,\"phone\":%u,\"pool_n\":%u}",
            hp, ctx5[2], pool_n);

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
            uint32_t cand_uid = pool_cand(pool, pool_bucket, i);
            cbuf[hp][i] = cand_uid;
            spfy_unit_record_t ur;
            if (spfy_unit_record_get(&v->units, cand_uid, &ur) == SPFY_OK) {
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

        cart_feat_ctx_t cfc = { &adapter, q5, v, 0 };
        spfy_anchor_cart_t cart = {0};
        /* Silence-pad CART traversal: engine emits durt-CART leaf statistics
         * for silence slots (ctx5[2] in {64, 65} = HP_PAU_L/R) the same way
         * it does for non-silence; we used to skip these slots and emit
         * (0, 0), which produced 174 leaf_diff records in 03-FE-AUDIT
         * (engine emits the silence-tree leaf (192.5106, 0.097); we emit
         * None). Plan 03-03/03-04 removes the gate so spfy_cart_traverse
         * runs on silence slots too. SPFY_NO_SILENCE_CART=1 reverts to the
         * old behaviour for diagnostic isolation. */
        int silence_slot = ctx_is_silence(v, ctx5[2]);
        if (!silence_slot || !getenv("SPFY_NO_SILENCE_CART")) {
            uint32_t durt_idx = phone_to_labl(v, ctx5[2] >> 1);
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
            /* Global scale on durt CART target. SPFY_DURT_SCALE=N
             * multiplies the per-slot duration target before USel cost
             * is computed; values > 1.0 bias toward longer units (slower
             * speech), < 1.0 toward shorter. Default 1.0 = exact
             * IEEE identity (audit-invariant). Empirically the curve
             * saturates around 1.08-1.10 — past that, USel hits poor
             * Pareto fronts and viterbi cost spikes without further
             * duration gain. Use 1.05-1.07 as a soft slowdown knob;
             * for stronger slowdown, a post-WSOLA time-stretch is the
             * cleaner architectural lever (see spfy_dsp/time_stretch). */
            if (cart.durt_valid) {
                static float dscale = -1.0f;
                if (dscale < 0.0f) {
                    const char *e = getenv("SPFY_DURT_SCALE");
                    dscale = (e && *e) ? (float)atof(e) : 1.0f;
                    if (dscale < 0.1f) dscale = 0.1f;
                    if (dscale > 5.0f) dscale = 5.0f;
                }
                if (dscale != 1.0f) cart.durt_mean *= dscale;
            }
            /* Option A: boundary-tone F0 target bias. Shift the per-slot
             * F0 target by the syllable's boundary tone so the unit-
             * selection target cost prefers naturally-contoured units
             * (no DSP — the chosen units are real recorded speech).
             * Gated by SPFY_PROSODY_REALIZE; zero btone is identity, so
             * the audit-corpus path stays bit-stable. SPFY_PROSODY_BT_GAIN
             * scales the nominal ToBI depth into applied semitones
             * (default 1.0 = use the depth directly). */
            if (cart.f0tr_valid && hp_btone[hp]
                && getenv("SPFY_PROSODY_REALIZE")) {
                float gain = 1.0f;
                const char *g = getenv("SPFY_PROSODY_BT_GAIN");
                if (g) { float f = (float)atof(g); if (f >= 0.0f) gain = f; }
                cart.f0tr_mean *= powf(2.0f,
                    (float)hp_btone[hp] * gain / 12.0f);
            }
            /* Speechify-4 accent-height (F0-range) compression. spfy realizes
             * pitch accents (H*) too HIGH vs Speechify-4 (per-phrase peak F0 is
             * ~+1 st median, up to +6 st, higher than the v4 NWR refs). Pull the
             * f0tr target toward Tom's modal F0 so the unit-selection cost prefers
             * flatter units (selection-driven; no DSP pitch-shift). r<1 flattens,
             * r=1 widens. Gated: SPFY_SPFY4_F0_RANGE unset -> factor 1.0 -> the
             * compression is skipped entirely, so the 3.0.5 audit path stays
             * bit-identical. SPFY_SPFY4_F0_BASE sets the modal-F0 anchor in Hz
             * (default 120 = Tom). See reveng/spfy4/08_accent_height_findings.md. */
            if (cart.f0tr_valid) {
                static float f0range = -1.0f, f0base = -1.0f;
                if (f0range < 0.0f) {
                    const char *e = getenv("SPFY_SPFY4_F0_RANGE");
                    f0range = (e && *e) ? (float)atof(e) : 1.0f;
                    if (f0range < 0.05f) f0range = 0.05f;
                    if (f0range > 3.0f)  f0range = 3.0f;
                    const char *b = getenv("SPFY_SPFY4_F0_BASE");
                    f0base = (b && *b) ? (float)atof(b) : 120.0f;
                    if (f0base < 50.0f)  f0base = 50.0f;
                    if (f0base > 400.0f) f0base = 400.0f;
                }
                if (f0range != 1.0f) {
                    cart.f0tr_mean = f0base
                        + (cart.f0tr_mean - f0base) * f0range;
                }
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
            /* [live-trace] one event per scored candidate — the raw search
             * space the viz shows filling in and then getting pruned by the
             * Viterbi. Compiled out entirely when SPFY_TRACE is off. */
            spfy_trace_eventf("cand",
                "{\"slot\":%u,\"uid\":%u,\"tc\":%.4f}",
                hp, cbuf[hp][i], (double)tbuf[hp][i]);
        }

        /* Engine FUN_08e88de0 running-min early exit (decompiled
         * 2026-07-20). The engine pre-sets every candidate's cost to the
         * 10000.0f sentinel (`MOV [..+4],0x461c4000`), then accumulates
         * the components in stages -- SP+flag, ccos, D, F0 -- re-testing
         * `cost <= bound` after each and abandoning the candidate the
         * moment it exceeds. Only a candidate that clears all four stages
         * is stored, and only then:
         *
         *     if (cost < running_min) {
         *         running_min = cost;
         *         bound = slack + cost;      // local_50 = fVar4 + fVar14
         *     }
         *
         * with `running_min` and `bound` both starting at 10000.0f, and
         * `slack` read from cfg+0x4c -- which is the SAME field the prune
         * takes as its THRESH, i.e. HALFPHONE_CAND_PRUNE_THRESH. No new
         * constant. `running_min` is then what gets passed to
         * FUN_08e88830 as `best`.
         *
         * Staged bail-out is equivalent to a single post-hoc test here:
         * every component is non-negative, so the partial sums are
         * monotonic and `partial > bound` implies `full > bound`. What
         * matters is that the bound tightens IN POOL ORDER, so this must
         * run as a left-to-right sweep and not as a min-then-filter.
         *
         * SPFY_NO_HP_EARLY_EXIT=1 disables; SPFY_HP_EARLY_EXIT_VAL
         * overrides the slack for A/B work. */
        if (!getenv("SPFY_NO_HP_EARLY_EXIT")) {
            float slack = v->hp_prune_thresh;
            const char *eev = getenv("SPFY_HP_EARLY_EXIT_VAL");
            if (eev && *eev) slack = (float)atof(eev);

            float running_min = 10000.0f;   /* engine local_30 */
            float bound       = 10000.0f;   /* engine local_50 */
            for (uint32_t i = 0; i < pool_n; ++i) {
                float t = tbuf[hp][i];
                if (t <= bound) {
                    if (t < running_min) {
                        running_min = t;
                        bound = slack + t;
                    }
                } else {
                    tbuf[hp][i] = 10000.0f;
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
         * THRESH/SLOPE come from the voice's VCF (Tom 0.8/0.005, Jill
         * 1.0/0.005); MAX=50, BIN_WIDTH=0.025, SCALE=40 are engine
         * globals, not per-voice. SPFY_NO_HP_PRUNE=1 disables. */
        /* NB: a "skip the prune for pools smaller than N" guard was tested
         * 2026-07-20 (engine traces show large kept-deltas at small pool
         * sizes on both voices) and REJECTED: N=3 already costs Tom
         * 8532->8525 while gaining Jill only 1983->1988, and larger N is
         * far worse (N=13: Tom 95.1%). Whatever the engine does at small
         * pools, a flat pool-size cutoff is not it. */
        if (pool_n > 1 && !getenv("SPFY_NO_HP_PRUNE")) {
            const float HP_THRESH    = v->hp_prune_thresh;
            const float HP_SLOPE     = v->hp_prune_slope;
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
             * Precision history (superseded 2026-07-20): a 2026-05-14 fix
             * stored the LHS to a float local before comparing, on the
             * theory that the engine compared at 32-bit precision. The
             * disassembly (08e888c6..08e888ee) shows otherwise -- BOTH
             * sides stay in x87 registers and only the final
             * `bd + best` is rounded, by a single FSTP. The old fix and
             * its SPFY_PRUNE_X87 escape hatch are gone; see below. */
            /* Bin index the scan broke at; 40 == ran to completion. The
             * engine gates the whole filter on this (see below). */
            int break_k = 40;
            /* Both sides of the break test are evaluated at x87 EXTENDED
             * precision in the engine and never rounded to 32-bit floats
             * (FUN_08e88830 @ 08e888c6..08e888ee):
             *
             *   FILD (k+1) ; FMUL BIN_WIDTH        -> bd,  extended
             *   FILD cum   ; FMUL SLOPE ; FSUBR THRESH -> lhs, extended
             *   FCOMPP                              -> break when bd > lhs
             *
             * The rounding only happens later, once, when best is added
             * and the result is stored (FADD ; FSTP dword). Getting this
             * wrong flips the k=38 boundary: at cum=5, THRESH=1.0 the two
             * sides are 0.9750000005588 and 0.9750000145286 -- distinct in
             * extended, but they round to the SAME float, so a rounded
             * comparison sees equality and fails to break.
             *
             * long double is 80-bit on this 32-bit x86 target, matching
             * the FPU registers exactly. */
            long double bd_x = 40.0L * (long double)HP_BIN_WIDTH;
            for (int k = 0; k < 40; ++k) {
                cum += bins[k];
                long double cur_bd = (long double)(k + 1)
                                   * (long double)HP_BIN_WIDTH;
                long double lhs_x  = (long double)HP_THRESH
                                   - (long double)cum * (long double)HP_SLOPE;
                if ((uint32_t)cum > HP_MAX || lhs_x < cur_bd) {
                    bd_x = cur_bd; break_k = k; break;
                }
            }
            bin_dist = (float)bd_x;   /* for the debug dump only */
            /* Engine: FADD best onto the still-extended bd, then ONE
             * FSTP to a 32-bit float. Rounding bd first and then adding
             * would round twice. */
            float thresh = (float)(bd_x + (long double)best);
            /* The engine gates the entire threshold filter on the break
             * bin: `if (iVar7 < 0x27)` in FUN_08e88830. If the scan broke
             * at bin 39 or ran off the end, NO candidate is dropped --
             * only the sort and the HP_MAX cap below still run.
             *
             * This is not a corner case, it is the whole sparse-pool
             * story. The break needs THRESH - cum*SLOPE < (k+1)*0.025,
             * and cum >= 1 always (best sits in bin 0), so:
             *   Tom  THRESH=0.8: lhs <= 0.795 -> breaks by k=31, always
             *                    < 39, so Tom ALWAYS prunes (this guard
             *                    is provably a no-op for him).
             *   Jill THRESH=1.0: lhs <= 0.995 -> needs (k+1)*0.025 >
             *                    0.995, i.e. k=39 -> NO prune whenever
             *                    the pool is sparse enough that cum stays
             *                    low through the scan.
             * Without the guard we cut Jill's sparse slots to best+1.0 and
             * drop units the engine kept (text_004 uid 145844 at
             * best+1.883, which is on the engine's chosen run).
             *
             * This guard is only HALF the engine's candidate reduction --
             * it MUST be paired with the running-min early exit above.
             * Enabled alone it overshoots badly (Jill 93.8% -> 92.6%,
             * survivor mean 13.92 -> 17.33 vs an engine mean of 14.66),
             * because our tighter histogram cut had been standing in for
             * the missing early exit. SPFY_NO_HP_PRUNE_BIN39_GUARD=1
             * reverts to the old always-filter behaviour for A/B. */
            uint32_t kept;
            if (break_k < 39 || getenv("SPFY_NO_HP_PRUNE_BIN39_GUARD")) {
                kept = 0;
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
            } else {
                kept = pool_n;
            }
            /* Sort kept cands by target_cost ascending, ties broken by
             * DESCENDING uid. The engine uses a shell sort whose swap
             * predicate decodes (iVar12 = j, iVar9 = j+gap) to
             *
             *   cost_j >= cost_jg && (cost_j != cost_jg || uid_j <= uid_jg)
             *
             * -- i.e. on equal cost it moves the LARGER uid earlier. The
             * int at cand+0 is the uid (FUN_08e88de0 indexes the unit
             * table with it). Ordering matters downstream because the
             * DP's predecessor scan and the early-exit bound both walk
             * candidates in array order.
             *
             * We use a selection sort (counts are small post-prune); with
             * a TOTAL comparator the result is identical to the engine's
             * shell sort regardless of algorithm, since uids are unique
             * within a pool so no two entries compare equal.
             * SPFY_NO_HP_SORT_UID_TIE=1 reverts to cost-only for A/B. */
            int sort_uid_tie = (getenv("SPFY_NO_HP_SORT_UID_TIE") == NULL);
            for (uint32_t a = 0; a + 1 < kept; ++a) {
                uint32_t mn = a;
                for (uint32_t b = a + 1; b < kept; ++b) {
                    if (tbuf[hp][b] < tbuf[hp][mn]
                        || (sort_uid_tie
                            && tbuf[hp][b] == tbuf[hp][mn]
                            && cbuf[hp][b] > cbuf[hp][mn])) mn = b;
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
                                "\"thresh\":%.9f,\"break_k\":%d,"
                                "\"filtered\":%d,\"kept_uids\":[",
                        hp, pool_n, kept, (double)best, (double)bin_dist,
                        (double)(best + bin_dist), break_k, break_k < 39);
                /* cbuf[0..kept-1] is the compacted survivor set; entries
                 * past `kept` are stale leftovers, so only the survivors
                 * are meaningful here. */
                for (uint32_t i = 0; i < kept; ++i)
                    fprintf(stderr, "%s%u", i ? "," : "", cbuf[hp][i]);
                fprintf(stderr, "]}\n");
            }
            pool_n = kept;
        }

        vslots[hp].cands = cbuf[hp];
        vslots[hp].target_cost = tbuf[hp];
        vslots[hp].n_cands = pool_n;
        total_cands += pool_n;
    }
    if (synth_is_verbose())
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
                cart_feat_ctx_t cfc = {NULL, q5_per_slot[post], v, 0};
                spfy_fe_slot_t adapter = {0};
                for (int i = 0; i < 5; ++i) {
                    adapter.ctx[i] = (int32_t)slice_ctx.ctx[post][i];
                    adapter.sp[i]  = sp_tab.sp[post][i];
                }
                cfc.slot = &adapter;
                /* Plan 03-04: silence-pad CART traversal — see primary
                 * spfy_cart_traverse call site for rationale.
                 * SPFY_NO_SILENCE_CART=1 reverts. */
                int silence = ctx_is_silence(v, slice_ctx.ctx[post][2]);
                if (!silence || !getenv("SPFY_NO_SILENCE_CART")) {
                    uint32_t didx = phone_to_labl(v, slice_ctx.ctx[post][2] >> 1);
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
                cart_feat_ctx_t cfc = {NULL, q5_per_slot[post], v, 0};
                spfy_fe_slot_t adapter = {0};
                for (int i = 0; i < 5; ++i) {
                    adapter.ctx[i] = (int32_t)slice_ctx.ctx[post][i];
                    adapter.sp[i]  = sp_tab.sp[post][i];
                }
                cfc.slot = &adapter;
                /* Plan 03-04: silence-pad CART traversal — see primary
                 * spfy_cart_traverse call site for rationale.
                 * SPFY_NO_SILENCE_CART=1 reverts. */
                int silence = ctx_is_silence(v, slice_ctx.ctx[post][2]);
                if (!silence || !getenv("SPFY_NO_SILENCE_CART")) {
                    uint32_t didx = phone_to_labl(v, slice_ctx.ctx[post][2] >> 1);
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

        if (synth_is_verbose())
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
    /* Tom 0.6; Jill 1.5, Felix 0.9, Javier 0.1, Paulina 0.5. */
    jc.f0_edge_change_weight =
        spfy_vcf_f32(&v->vcf, "F0_EDGE_CHANGE_WEIGHT", 0.6f);
    /* MISSING_JOIN_COST = 1000.0 from FE-init default (FUN_08e90dc0
     * sets param_3[0x21] = 0x447a0000 = 1000.0 before VCF override;
     * no shipped VCF overrides it). Huge by design — makes the DP almost
     * exclusively use same-rec runs and v->hash hits. */
    jc.missing_join_cost     =
        spfy_vcf_f32(&v->vcf, "MISSING_JOIN_COST", 1000.0f);
    if (getenv("SPFY_NO_F0_CURVE")) jc.curve = NULL;
    if (jc.curve) {
        if (synth_is_verbose())
            fprintf(stdout, "F0-curve loaded: %d bins, sub_off=%d, "
                            "F0_EDGE=%.2f, MISSING_JOIN=%.2f\n",
                    jc.curve_max_idx, jc.curve_sub_off,
                    (double)jc.f0_edge_change_weight,
                    (double)jc.missing_join_cost);
        if (getenv("SPFY_DUMP_F0_CURVE")) {
            fprintf(stderr, "{\"f0_curve\":1,\"n\":%d,\"sub_off\":%d,\"vals\":[",
                    jc.curve_max_idx, jc.curve_sub_off);
            for (int i = 0; i < jc.curve_max_idx; ++i)
                fprintf(stderr, "%s%.4f", i ? "," : "",
                        (double)spfy_le_f32(jc.curve + (size_t)i * 4u));
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
        if (synth_is_verbose())
            fprintf(stdout, "viterbi[dag] total cost=%.3f path_len=%u "
                            "(of %u tree slots; %u HP)\n",
                    (double)total, path_len, tree.n_slots, n_hp);

        /* [live-trace] DP done: total path cost + lengths. Individual `pick`
         * events (below) carry the chosen UID per half-phone as the path is
         * expanded. */
        spfy_trace_eventf("viterbi",
            "{\"phrase\":%u,\"total\":%.4f,\"path_len\":%u,\"n_slots\":%u}",
            phrase_idx, (double)total, path_len, tree.n_slots);

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
                /* [live-trace] the Viterbi's winning candidate for this
                 * half-phone — the viz uses this to collapse the candidate
                 * cloud onto the chosen unit. */
                spfy_trace_eventf("pick", "{\"slot\":%u,\"uid\":%u}", hp, uid);
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
#ifdef SPFY_TRACE
    /* [live-trace] previous word_post for the global-word tagger; resets per
     * phrase so the first content word of each phrase ticks g_wseq. */
    uint32_t g_wprev = 0xFFFFFFFFu;
#endif
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

    /* Phrase-start boundary event: fires before this phrase's first unit
     * is pushed, at the current output sample count. */
    if (cb && cb->phrase_cb)
        cb->phrase_cb(cb->ctx, phrase_idx, sink->n_samples_written);

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
        int this_is_silence = ctx_is_silence(v, ctx_arr[this_post][2]);
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
        /* Per-word volume gain for this slot (\!vp/\!vd), via the slot's
         * parent-syllable fe_shared into the syl_vol map. */
        float vol_gain = 1.0f;
        if (syl_vol) {
            uint32_t post = hp_to_post[s];
            if (post < tree.n_slots) {
                uint32_t syl_post = tree.slots[post].parent_idx;
                if (syl_post < tree.n_slots
                    && tree.slots[syl_post].kind == SPFY_SK_SYLLABLE) {
                    uint32_t sh = tree.slots[syl_post].fe_shared;
                    if (sh >= 1 && (sh - 1) < fe_utt.n_syls && syl_vol[sh - 1])
                        vol_gain = (float)syl_vol[sh - 1] / 100.0f;
                }
            }
        }
#ifdef SPFY_TRACE
        /* [live-trace] per-slot global word index for this WSOLA push. Advances
         * g_wseq across EVERY slot of the run (even ones the run-batch skips),
         * so a run straddling a word boundary — e.g. the two identical "tea"
         * halves of "TTS" batched into one span — still records both words. The
         * frontend splits the slice's colour at these boundaries. -1 = silence. */
        char ws_buf[1024];
        {
            const uint32_t (*cxg)[5] = (const uint32_t (*)[5])slice_ctx.ctx;
            int wo = 1; ws_buf[0] = '[';
            for (uint32_t k = s; k < s + run_n && wo < (int)sizeof ws_buf - 16; ++k) {
                uint32_t kp = hp_to_post[k];
                int ksil = ctx_is_silence(v, cxg[kp][2]);
                if (!ksil && hp_word_idx[k] != g_wprev) { ++g_wseq; g_wprev = hp_word_idx[k]; }
                wo += snprintf(ws_buf + wo, sizeof ws_buf - (size_t)wo,
                               "%s%d", k > s ? "," : "", ksil ? -1 : g_wseq);
            }
            ws_buf[wo++] = ']'; ws_buf[wo] = '\0';
        }
#endif
        if (run_n >= 2) {
            spfy_unit_record_t r_last;
            (void)spfy_unit_record_get(&v->units, path_uids[s + run_n - 1],
                                       &r_last);
            uint32_t span = (uint32_t)r_last.local_pos
                          + (uint32_t)r_last.dur_like
                          - (uint32_t)r1.local_pos;
            int align = !prev_have || prev_file_idx != r1.file_idx;
            /* [live-trace] emitted span (a run of consecutive UIDs collapsed
             * into one WSOLA push). `t` is the output sample offset where
             * this unit's audio begins; the viz maps it onto the waveform.
             * Flask enriches uid -> phone/half/rec_name during relay. */
            spfy_trace_eventf("unit",
                "{\"slot\":%u,\"uid\":%u,\"lp\":%u,\"dur\":%u,\"t\":%u,\"align\":%d,\"run\":%u,\"ws\":%s}",
                s, u, (unsigned)r1.local_pos, span,
                sink->n_samples_written, align, run_n, ws_buf);
            rc = append_recording_span(&ws, r1.file_idx, r1.local_pos,
                                       span, &v->feat, &v->vdb, &v->lookup, align,
                                       prev_f0_end, r1.f0_start,
                                       v->vdb.sample_rate, vol_gain);
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
            /* [live-trace] single-UID push (run of 1). See run>=2 note above. */
            spfy_trace_eventf("unit",
                "{\"slot\":%u,\"uid\":%u,\"lp\":%u,\"dur\":%u,\"t\":%u,\"align\":%d,\"run\":1,\"ws\":%s}",
                s, u, (unsigned)r1.local_pos, (unsigned)r1.dur_like,
                sink->n_samples_written, align, ws_buf);
            rc = append_recording_span(&ws, r1.file_idx, r1.local_pos,
                                       r1.dur_like, &v->feat, &v->vdb, &v->lookup, align,
                                       prev_f0_end, r1.f0_start,
                                       v->vdb.sample_rate, vol_gain);
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
    free(syl_vol);
    free(hp_btone); hp_btone = NULL;
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
    if (phrase_idx + 1 < n_phrases) {
        int sil_ms = (inter_phrase_ms_override > 0) ? inter_phrase_ms_override : 0;
        /* User `\!pN` pause before the NEXT phrase, threaded from the FE
         * parser's phrase_lead_pause_ms (set from `pau(uN)` markers that
         * build_inline_mixed_tagged emits for embedded pause tags). The FE
         * pipeline does not size silence from structural `pau(pN)` tokens,
         * so this is where an explicit pause duration becomes real silence. */
        int npid = (int)phrase_idx + 1;
        if (npid < FE_PARSE_MAX_PHRASES
            && parsed->phrase_lead_pause_ms[npid] > sil_ms)
            sil_ms = parsed->phrase_lead_pause_ms[npid];
        if (sil_ms > 0) {
            static const int16_t INTER_PHRASE_SILENCE[16000] = {0};
            size_t cap = sizeof INTER_PHRASE_SILENCE / sizeof *INTER_PHRASE_SILENCE;
            size_t remain = (size_t)sil_ms * (size_t)v->vdb.sample_rate / 1000u;
            int sil_align = (getenv("SPFY_WSOLA_NO_SILENCE_FADE") != NULL) ? 0 : 1;
            while (remain > 0) {                 /* chunked: pauses can exceed 1 s */
                size_t n = remain > cap ? cap : remain;
                (void)spfy_wsola_push_unit(&ws, INTER_PHRASE_SILENCE, n, sil_align);
                remain -= n;
                sil_align = 0;                   /* fade-in only the first chunk */
            }
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
    free(hp_btone);
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
    free(etags_text);
    free(etag_vol);
    free(etag_rate);
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
    FILE     *fp;       /* word events (SPFY_WORD_EVENTS_FILE)     */
    FILE     *pfp;      /* phrase events (SPFY_PHRASE_EVENTS_FILE) */
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

void spfy_cli_phrase_cb(void *ctx, uint32_t phrase_idx,
                        uint32_t sample_offset);
void spfy_cli_phrase_cb(void *ctx, uint32_t phrase_idx,
                        uint32_t sample_offset)
{
    struct spfy_cli_wev_ctx *c = (struct spfy_cli_wev_ctx *)ctx;
    if (!c || !c->pfp) return;
    fprintf(c->pfp, "%u\t%u\n", sample_offset, phrase_idx);
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

/* Read an entire text file into a malloc'd, NUL-terminated buffer, with
 * trailing whitespace stripped (an editor / `echo` newline would otherwise
 * ride into the final phrase). Returns NULL on open/read/OOM failure.
 * Caller frees. Backs the -f/--file CLI option so input text can come from
 * a file instead of a shell-quoted argv — avoids the platform quoting and
 * command-line length limits that bite long inputs. */
static char *read_text_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[got] = '\0';
    while (got > 0 && isspace((unsigned char)buf[got - 1])) buf[--got] = '\0';
    return buf;
}

int main(int argc, char **argv)
{
    /* Two CLI forms:
     *   5-arg (preferred):   <voice.vin> <voice.vdb> <voice.vcf>
     *                        "<text>" <out.wav>
     *     vocab + fe_tables_{a,b} are embedded in the binary (see
     *     spfy/tools/embed_assets.py) and extracted to a tempdir on
     *     first invocation; hp_class is derived from the voice's own
     *     VIN, so this form works for EVERY voice, not just Tom.
     *   9-arg (legacy):      adds explicit hpclass/vocab/fe_tables_{a,b}
     *                        paths between the voice triplet and text —
     *                        kept so audit scripts that pass exact data
     *                        paths still work. An EMPTY hpclass argument
     *                        selects VIN derivation, same as the 5-arg
     *                        form.
     *
     * Either form accepts -f/--file <path> anywhere on the command line. It
     * supplies the input text from a file, in which case the "<text>"
     * positional is omitted (dropping the arg count to 4 / 8 positionals
     * plus the flag). */
    const char *vin_path, *vdb_path, *vcf_path;
    /* hpc_path MUST default to NULL: the short form never assigns it, and
     * spfy_voice_load reads paths->hpclass[0] to decide load-vs-derive. */
    const char *hpc_path = NULL;
    const char *vocab, *tab_a, *tab_b;
    const char *text, *out_wav;
    spfy_asset_paths_t embedded_paths = {0};
    char *file_text = NULL;

    /* Pull the option flags (-f/--file and its =VALUE variants, -q/--quiet,
     * -v/--verbose) out of argv, compacting the remaining positionals down so
     * the argc-based layout below is unchanged apart from the missing
     * "<text>" slot. -q/-v assign the file-scope verbosity directly
     * (last flag wins); leaving spfy_synth_verbose at -1 defers to the
     * SPFY_VERBOSE / SPFY_SYNTH_DEBUG env vars, so an explicit flag always
     * overrides the environment. */
    const char *file_path_arg = NULL;
    /* --trace-stream: emit the live NDJSON event stream to stdout. Recognised
     * and stripped in BOTH builds so the positional layout matches; only the
     * spfy_synth_trace target (SPFY_TRACE=1) actually installs the sink. */
    int trace_stream = 0;
    {
        int w = 1;
        for (int r = 1; r < argc; r++) {
            const char *a = argv[r];
            if (strcmp(a, "-f") == 0 || strcmp(a, "--file") == 0) {
                if (r + 1 >= argc) {
                    fprintf(stderr, "%s: %s requires a file path argument\n",
                            argv[0], a);
                    return 2;
                }
                file_path_arg = argv[++r];
            } else if (strncmp(a, "--file=", 7) == 0) {
                file_path_arg = a + 7;
            } else if (strncmp(a, "-f=", 3) == 0) {
                file_path_arg = a + 3;
            } else if (strcmp(a, "-q") == 0 || strcmp(a, "--quiet") == 0) {
                spfy_synth_verbose = 0;
            } else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
                spfy_synth_verbose = 1;
            } else if (strcmp(a, "--trace-stream") == 0) {
                trace_stream = 1;
            } else {
                argv[w++] = argv[r];
            }
        }
        argc = w;
    }

    int short_form;
    if (argc == (file_path_arg ? 5 : 6)) {
        short_form = 1;
    } else if (argc == (file_path_arg ? 9 : 10)) {
        short_form = 0;
    } else {
        fprintf(stderr,
            "usage: %s <voice.vin> <voice.vdb> <voice.vcf> \"<text>\" <out.wav>\n"
            "   or: %s <voice.vin> <voice.vdb> <voice.vcf> <hpclass.bin>\n"
            "          <vocab.json> <fe_tables_a> <fe_tables_b>\n"
            "          \"<text>\" <out.wav>          (legacy)\n"
            "\n"
            "  -f, --file <path>   read input text from <path> instead of the\n"
            "                      \"<text>\" argument (which is then omitted)\n"
            "  -q, --quiet         suppress per-synth diagnostics, keeping only\n"
            "                      the FE-backend banner (default)\n"
            "  -v, --verbose       print the full FE/synth pipeline diagnostics\n",
            argv[0], argv[0]);
        return 2;
    }

    if (file_path_arg) {
        file_text = read_text_file(file_path_arg);
        if (!file_text) {
            fprintf(stderr, "%s: cannot read input file '%s'\n",
                    argv[0], file_path_arg);
            return 1;
        }
    }

    {
        int i = 1;
        vin_path = argv[i++];
        vdb_path = argv[i++];
        vcf_path = argv[i++];
        if (!short_form) {
            hpc_path = argv[i++];
            vocab    = argv[i++];
            tab_a    = argv[i++];
            tab_b    = argv[i++];
        }
        text     = file_path_arg ? file_text : argv[i++];
        out_wav  = argv[i++];
    }

    if (short_form) {
        const char *tmpdir = resolve_asset_tempdir(argv[0]);
        if (spfy_assets_extract(tmpdir, &embedded_paths) != 0) {
            fprintf(stderr, "failed to extract embedded assets to %s\n", tmpdir);
            free(file_text);
            return 1;
        }
        /* hpc_path deliberately stays NULL: spfy_voice_load then derives
         * the hp_class table from THIS voice's VIN. Baking Tom's table in
         * here made the short form reject every other voice on the
         * unit-count check (jill 185475 units vs tom's 169579). */
        vocab    = embedded_paths.vocab;
        tab_a    = embedded_paths.tables_a;
        tab_b    = embedded_paths.tables_b;
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
            free(file_text);
            return 1;
        }
    }



    /* [live-trace] Route the NDJSON event stream to stdout when
     * --trace-stream was given. No-op in the normal spfy_synth build
     * (SPFY_TRACE undefined); only spfy_synth_trace installs the sink. */
    if (trace_stream) spfy_trace_set_sink(stdout);

    /* Open output sink (file mode for CLI; SAPI builds a callback sink). */
    spfy_wav_writer_t wav = {0};
    if ((rc = spfy_wav_open(&wav, out_wav, voice.vdb.sample_rate)) != SPFY_OK) {
        fprintf(stderr, "error opening %s: %s\n", out_wav, spfy_strerror(rc));
        spfy_voice_free(&voice);
        free(file_text);
        return 1;
    }

    /* Optional word-events sidecar for the 64-bit SAPI shim, plus an
     * optional phrase-events sidecar (analysis: exact per-utterance
     * segmentation of multi-phrase renders). */
    FILE *wev_fp = NULL;
    FILE *pev_fp = NULL;
    unsigned wev_word_idx = 0;
    {
        const char *wev_path = getenv("SPFY_WORD_EVENTS_FILE");
        if (wev_path && *wev_path) wev_fp = fopen(wev_path, "wb");
        const char *pev_path = getenv("SPFY_PHRASE_EVENTS_FILE");
        if (pev_path && *pev_path) pev_fp = fopen(pev_path, "wb");
    }
    struct spfy_cli_wev_ctx wev_ctx = { wev_fp, pev_fp, &wev_word_idx };
    spfy_synth_callbacks_t cb = {0};
    spfy_synth_callbacks_t *cbp = NULL;
    if (wev_fp || pev_fp) {
        if (wev_fp) cb.word_cb   = spfy_cli_word_cb;
        if (pev_fp) cb.phrase_cb = spfy_cli_phrase_cb;
        cb.ctx = &wev_ctx;
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
    if (pev_fp) { fclose(pev_fp); pev_fp = NULL; }
    spfy_wav_close(&wav);
    if (rc == SPFY_OK) {
        /* [live-trace] terminal event — lets the viz finalize and fetch the
         * WAV. No-op without an installed sink. */
        spfy_trace_eventf("done", "{\"samples\":%u,\"n_phrases\":%u}",
                          stats.samples_emitted, stats.n_phrases);
        /* Keep stdout pure NDJSON in stream mode; the human summary would
         * otherwise land mid-stream and break the SSE relay's JSON parse. */
        if (!trace_stream)
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
    free(file_text);
    spfy_voice_free(&voice);
    return rc == SPFY_OK ? 0 : 1;
}
#endif /* SPFY_SYNTH_NO_MAIN */
