/*
 * spfy/src/fe_host/fe_parse.h — public interface for the tagged-text
 * FE-output parser.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SPFY_FE_HOST_FE_PARSE_H
#define SPFY_FE_HOST_FE_PARSE_H

#include <stdint.h>
#include <stdio.h>

#include "fe.h"        /* for spfy_fe_slot_t */
#include "phoneset.h"  /* for spfy_phoneset_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     arpabet[8];
    int      duration;      /* pNNN value, default 100 */
    int8_t   syl_stress;    /* 0 = unstressed, 1 = stressed */
    char     accent[24];    /* ToBI string e.g. "H*", "H*;L-L%"; "" if none */
    int      syl_index;     /* 0-based index of the enclosing syllable */
} fe_parsed_phoneme_t;

typedef struct {
    char                 text[64];
    int                  char_start;     /* offset into input text */
    int                  char_len;
    char                 pos[16];        /* POS tag string */
    int                  stress_level;   /* 0/1/2 from word header */
    int                  n_syllables;
    int                  pause_after_ms; /* inter-word pause value, 0 if none */
    int                  phrase_id;      /* 0-based; bumps on each #{...} block */
    /* SSML / Balabolka prosody overrides (extension fields). Parsed
     * from the optional ",p=N,r=M" trailers in the word header. Both
     * default to 0 (= neutral, equivalent to no annotation). */
    int8_t               pitch_st;       /* signed semitones */
    int8_t               rate_pct;       /* signed percent, +N = faster */
    fe_parsed_phoneme_t *phonemes;
    int                  n_phonemes;
    int                  phonemes_cap;   /* allocator bookkeeping */
} fe_parsed_word_t;

/* Max phrases ({...} utterance blocks) tracked per parse. Long inputs —
 * e.g. a NWS bulletin with a 20-item locations list — run past 30
 * utterances; phrases beyond the cap lose their terminator (falling back
 * to '.', the wrong prosody class for list items) and their user pause. */
#define FE_PARSE_MAX_PHRASES 64

typedef struct {
    int               pause_before_ms;
    int               pause_after_ms;
    fe_parsed_word_t *words;
    int               n_words;
    int               words_cap;
    /* Per-phrase terminating punctuation, parsed from the marker char
     * the FE emits inside each `{X` opener (e.g. `#{,` => ',';
     * `{.` => '.'). One char per phrase_id; index by phrase_id.
     * Defaults to '.' if not captured. Used by parsed_to_fe_utt to
     * set the correct phrase_term per-utterance (so multi-utterance
     * inputs like "Hello, world." get local_10=0 for utt 0 ',' and
     * local_10=1 for utt 1 '.'). */
    char              phrase_terms[FE_PARSE_MAX_PHRASES];
    int               n_phrase_terms;
    /* Per-phrase user pause (ms), from `\!pN` embedded tags rendered by
     * build_inline_mixed_tagged as `pau(uN)` openers (the `u` unit marks a
     * USER pause, distinct from the FE's structural `pau(pN)` which is not
     * rendered as silence). phrase_lead_pause_ms[k] is extra silence to
     * inject BEFORE phrase k. Indexed by phrase_id; 0 = none. */
    int               phrase_lead_pause_ms[FE_PARSE_MAX_PHRASES];
} fe_parsed_t;

/* Parse the FE's tagged-text output (see host/PROTOCOL.md). Returns 0
 * on success and fills `out`. On failure returns -1 and leaves `out`
 * in a freed/zeroed state. */
int  fe_parse_tagged_output(const char *tagged, fe_parsed_t *out);

/* Free all allocations owned by `out`. Safe on a zeroed struct. */
void fe_parsed_free(fe_parsed_t *out);

/* Count of phonemes across all words. */
int  fe_parsed_count_phonemes(const fe_parsed_t *out);

/* Flatten the parsed structure into a spfy_fe_slot_t[] of at most
 * `slots_cap`. Fills emphasis_level (from accent); leaves ctx/sp/
 * is_voiced/durt/f0tr at zero. Useful for tests that don't have a
 * voice / phoneset loaded. */
void fe_parsed_flatten_to_slots(const fe_parsed_t *parsed,
                                spfy_fe_slot_t *slots,
                                int slots_cap);

/* Full slot construction: emits `(n_phons + 2) * 2` halfphone slots
 * (with 2 leading + 2 trailing pau pads) filled with ctx[5], sp[5],
 * is_voiced, emphasis_level, pitch/rate offsets. Mirrors stage_spr.c
 * conventions from the hand-written FE, so downstream USel/Viterbi
 * code sees the same shape regardless of FE source.
 *
 *   ctx[2] = phone_id * 2 + side  (side: 0=left half, 1=right half)
 *   ctx[0,1,3,4] = same-side phone_id-encoded neighbours at i±2 / i±4
 *   sp[0] = sylInPhrase (1-based, phrase = utterance for this single-utt path)
 *   sp[1] = sylType   (0=unstressed, 1=primary, 2=secondary — from .X marker)
 *   sp[2] = sylInWord (1-based)
 *   sp[3] = wordInPhrase (1-based, word index in utterance)
 *   sp[4] = phonInSyl (phoneInSylCosts row: 1=WordInitial, 2=SyllInitial,
 *                      3=SyllMedial, 4=SyllFinal, 5=WordFinal)
 *
 * Returns 0 on success, -1 on allocation failure. Caller frees
 * `*slots_out`. */
/* Per-voice phone-symbol -> engine phone-id table.
 *
 * `names[i]` is the phone whose engine id is `i`, in the VIN's
 * feat["name"] order -- the same numbering hp_class is built from. When
 * supplied this replaces the compiled-in en-US table
 * (data/en_us_engine_phone_ids.csv), which only covers ARPAbet and makes
 * fr-CA/es-MX phones fall back to VCF ids. Pass NULL to keep the old
 * behaviour. Deliberately a plain array rather than spfy_phone_order_t so
 * fe_parse does not have to depend on voice/. */
typedef struct {
    char *const *names;
    uint32_t     n;
} fe_phone_names_t;

int  fe_parsed_to_full_slots(const fe_parsed_t       *parsed,
                              const spfy_phoneset_t   *ps,
                              const fe_phone_names_t  *pn,
                              spfy_fe_slot_t         **slots_out,
                              uint32_t                *n_slots_out);

/* Enable/disable the built-in phoneme refinement (R1/R3 vowel reduction +
 * flap rules) applied by fe_parse_tagged_output. Default: enabled.
 *
 * When the FE is driven in ESPR mode it emits the engine's already-reduced
 * phones (ix/dx) directly, so the heuristic must be turned OFF or it
 * double-processes them. fe_host disables it once it has fed the ESPR
 * header. `enabled=0` turns refinement off; nonzero turns it on. */
void fe_parse_set_refine(int enabled);

/* Enable fr-CA liaison stress inheritance: bare leading phones of a word
 * (no `.N`) inherit the previous word's final-syllable stress rather than
 * defaulting to unstressed. fe_host sets this on ONLY for the fr-CA image;
 * en-US/es-MX never emit bare-leading words so it is a no-op there anyway. */
void fe_parse_set_liaison_inherit(int enabled);

/* Dump a human-readable summary to `out` (stderr-style debug). */
void fe_parsed_debug_dump(const fe_parsed_t *p, FILE *out);

/* In-place pre-parse cleanup for the raw drain stream. The live FE
 * delivers tagged output in capped 99-byte chunks; when a flush falls
 * mid-identifier the FE inserts a 2-byte space padding at the seam
 * (observed in the "Hello, world." capture: "inte"+"  "+"rj" instead
 * of "interj"). Rule: runs of >= 2 whitespace between alphanumerics
 * are dropped entirely; other whitespace runs collapse to a single
 * space. */
void fe_clean_stream_inplace(char *s);

#ifdef __cplusplus
}
#endif

#endif /* SPFY_FE_HOST_FE_PARSE_H */
