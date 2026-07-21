#ifndef SPFY_FE_FE_H
#define SPFY_FE_FE_H

#include <stddef.h>
#include <stdint.h>

#include "vocab.h"
#include "tables.h"
#include "stream.h"
#include "prosody.h"
#include "phoneset.h"

/* Front-End (FE) module: text -> per-slot scoring inputs.
 *
 * The Speechify engine's FE was a compiled IBM Eloquence Delta program
 * (Hertz/Karplus 1985). We re-implement its BEHAVIOUR -- not its
 * bytecode -- in plain C, using the same data tables and vocabulary
 * the original FE accessed. See:
 *   memory/project_fe_f0_eloquence.md  for full RE history.
 *   spfy/data/fe_tables_a/             553 lexical tables (compounds,
 *                                      proper names, contractions, etc).
 *   spfy/data/fe_tables/               173 phonetic tables (LTS rules,
 *                                      pronunciation transcriptions).
 *   spfy/data/fe_phoneme_blob.bin      18,870-byte Delta bytecode --
 *                                      not parsed in Path B; kept for
 *                                      reference / future Path-A work.
 *   spfy/build/fe_symbol_table.json    469-entry vocabulary.
 *
 * The FE produces, per utterance, the inputs the existing C scoring
 * stack expects: per-slot ctx[5] HP-class neighbours, per-slot SP
 * target indices, per-slot CART durt/f0tr predictions (lazy --
 * actually computed by the voice), and the slot-tree topology.
 *
 * Prosody/pitch hints are FIRST-CLASS inputs: callers pass an optional
 * spfy_prosody_hints_t alongside the text, and the FE threads those
 * hints through every stage so the resulting per-slot output reflects
 * <emphasis>, <prosody pitch=...>, etc. without any post-processing.
 */

typedef struct spfy_fe_s spfy_fe_t;

/* Initialise the FE from on-disk resources.
 *
 *   vocab_json: spfy/build/fe_symbol_table.json
 *   tables_a:   spfy/data/fe_tables_a/    (553 *.bin)
 *   tables_b:   spfy/data/fe_tables/      (173 *.bin)
 *
 * On success *out is owned by the caller; free with spfy_fe_close().
 */
int  spfy_fe_open(const char *vocab_json,
                  const char *tables_a_dir,
                  const char *tables_b_dir,
                  spfy_fe_t **out);

/* Same, but selects which embedded SWIttsFe-<lang>.dll image to host.
 * `lang` is a VCF `tts.voiceCfg.language` tag ("en-US", "fr-CA",
 * "es-MX"); NULL or an unbuilt language falls back to the first embedded
 * image with a warning. Which languages are available is a build-time
 * choice -- see SPFY_FE_LANGS in src/fe_host/CMakeLists.txt. */
int  spfy_fe_open_lang(const char *lang,
                       const char *vocab_json,
                       const char *tables_a_dir,
                       const char *tables_b_dir,
                       spfy_fe_t **out);

/* Load voice-specific phoneset from a VCF file. Optional -- if not
 * called, the FE falls back to the hardcoded SAMPA->phone_id mapping
 * baked into stage_spr.c (low-quality placeholder; phone IDs won't
 * match the engine's). When loaded, stage_spr emits voice-correct
 * ctx[5] values usable by spfy_synth_replay end-to-end. */
int  spfy_fe_set_voice_vcf(spfy_fe_t  *fe,
                            const char *vcf_path);

/* Supply the voice's phone-symbol -> engine-phone-id table, in the VIN's
 * feat["name"] order. `names` must outlive the FE (spfy_voice_t owns it
 * via its spfy_phone_order_t). Without this the FE uses the compiled-in
 * en-US ARPAbet table, which is correct for en-US but leaves fr-CA and
 * es-MX phones unmapped. Returns 0 on success. */
int  spfy_fe_set_phone_names(spfy_fe_t   *fe,
                             char *const *names,
                             uint32_t     n);

/* Enable the FE's ESPR output mode using this voice's config. The hosted
 * backend builds and feeds the exact control header the real engine sends
 * (\!SWIcv<version> \!SWIcg<gender> \!SWIcn<name> \!SWIcl<phoneset>
 * \!SWIespr1 \!SWIwd0), which makes the FE emit the engine's fully-reduced
 * phones (barred-i `ix`, flapped `dx`) directly into consprout. When this
 * succeeds the backend ALSO disables the built-in R1/R3/flap heuristic
 * (fe_parse_set_refine(0)), since the FE now does that work exactly.
 *
 * Values come straight from the VCF (tts.voiceCfg.{name,gender,phoneset,
 * version}). The phoneset is what carries the reduction: "swi_plus_ix" for
 * en-US, "swi" for the es-MX/fr-CA voices. NULL fields fall back to the
 * en-US defaults. Returns 0 if ESPR was enabled, nonzero if the backend
 * does not support it (in-house / emulator stubs) -- in which case the
 * heuristic stays on. */
int  spfy_fe_set_espr_config(spfy_fe_t  *fe,
                             const char *name,
                             const char *gender,
                             const char *phoneset,
                             const char *version);

void spfy_fe_close(spfy_fe_t *fe);

/* Per-slot FE output. Mirrors the schema we currently capture from
 * prsl_slot/cart_walks/inner_scorer JSONL traces, so existing C
 * scoring code consumes spfy_fe_slot_t directly. */
typedef struct {
    /* Halfphone-class context. ctx[2] is THIS slot; ctx[0..1] are the
     * two previous halfphone-class neighbours and ctx[3..4] the next
     * two. Encoding is voice-side: phone_id*2 + side, with Tom's
     * known pair-swaps applied (phones 9/10/11, 14/15). */
    int32_t  ctx[5];

    /* Five SP feature row indices used by the per-HP InnerScorer.
     * Order matches the engine: [sylInPhrase, sylType, sylInWord,
     * wordInPhrase, phoneInSyl]. */
    uint32_t sp[5];

    /* Whether this slot is voiced (drives F0-cost gating downstream).
     * Computed from the phoneme's voicing bit. */
    int      is_voiced;

    /* CART target prosody. The voice-side CARTs predict these from
     * (ctx, sp, syl/word features); the FE supplies the FEATURE
     * vector but does not compute the CART output itself. We keep
     * these fields here so the FE's output struct mirrors the
     * trace-replay path 1:1. Filled by spfy_fe_synth_text_with_carts()
     * once the voice CARTs are wired in; left zero for now. */
    float    durt_mean, durt_var;
    int      durt_valid;
    float    f0tr_mean, f0tr_var;
    int      f0tr_valid;

    /* Prosody hints attached to this slot (after parser propagation).
     * 0 = none. Engine's word_prominence > 2 system maps from these. */
    uint8_t  emphasis_level;       /* 0..3, drives \!emph code emission */
    int8_t   pitch_offset_st;      /* semitones; 0 = neutral */
    int8_t   rate_offset_pct;      /* percent; 0 = neutral */
} spfy_fe_slot_t;

typedef struct {
    spfy_fe_slot_t *slots;
    uint32_t        n_slots;
    /* Original prosody-hint bundle the caller passed. Borrowed; not
     * owned by the utterance struct. */
    const spfy_prosody_hints_t *hints;
} spfy_fe_utterance_t;

/* Convert plain text (or SSML, or pre-phonemized SPR) to a per-slot
 * scoring schema. `hints` may be NULL.
 *
 * Returns SPFY_OK + writes ownership to *out_utt. Caller frees with
 * spfy_fe_utterance_free.
 */
int  spfy_fe_synth_text(spfy_fe_t                  *fe,
                        const char                 *text,
                        const spfy_prosody_hints_t *hints,
                        spfy_fe_utterance_t       **out_utt);

/* Bypass the FE DLL entirely: feed a pre-built tagged-output string
 * (same syntax the FE emits naturally — see fe_parse.h / fe_parse.c)
 * directly through the parser + slot-builder. Used for inline phoneme
 * input where we generate the tagged output from SPR/SAPI phone IDs
 * before the FE can mangle it. */
int  spfy_fe_synth_tagged(spfy_fe_t                  *fe,
                          const char                 *tagged,
                          const spfy_prosody_hints_t *hints,
                          spfy_fe_utterance_t       **out_utt);

/* Phonemize plain text and return the FE's RAW tagged-output string in
 * `out` (NUL-terminated, truncated to out_n), stopping before the parse
 * + slot-build that spfy_fe_synth_text does. Lets callers splice the
 * DLL FE's words for normal runs together with spr_inline_to_tagged
 * blocks into one flowing utterance, then feed the result back through
 * spfy_fe_synth_tagged. Returns the string length (>0), or <=0 on
 * failure / empty output. */
int  spfy_fe_text_to_tagged(spfy_fe_t  *fe,
                            const char *text,
                            char       *out,
                            size_t      out_n);

void spfy_fe_utterance_free(spfy_fe_utterance_t *u);

/* ------------------------------------------------------------------ */
/* Internal accessors (used by stages, exposed for testing)            */
/* ------------------------------------------------------------------ */

const spfy_fe_vocab_t  *spfy_fe_vocab   (const spfy_fe_t *fe);
const spfy_fe_tables_t *spfy_fe_tables  (const spfy_fe_t *fe);
const spfy_phoneset_t  *spfy_fe_phoneset(const spfy_fe_t *fe);

/* Run only the text-norm stage. Useful for stage-by-stage testing
 * before the full pipeline is wired up. Caller owns `delta`. */
int  spfy_fe_textnorm_only(const spfy_fe_t            *fe,
                            const char                 *text,
                            const spfy_prosody_hints_t *hints,
                            spfy_fe_delta_t            *delta);

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */

void spfy_fe_print_stats(const spfy_fe_t *fe);

/* ------------------------------------------------------------------ */
/* Hosted-FE escape hatch                                              */
/* ------------------------------------------------------------------ */

/* Opaque pointer to the hosted FE's last-parsed tagged output (type
 * `fe_parsed_t` from spfy/src/fe_host/fe_parse.h). Returns NULL if no
 * synth has been run yet, or if the build is using the hand-written
 * FE (not the hosted one). Drivers that want to bypass the
 * spfy_fe_slot_t marshalling and consume the per-word/per-phoneme
 * structure directly call this. */
const void *spfy_fe_get_parsed(const spfy_fe_t *fe);

#endif
