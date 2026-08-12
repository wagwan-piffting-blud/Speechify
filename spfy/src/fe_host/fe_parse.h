/* spfy/src/fe_host/fe_parse.h — public interface for the tagged-text
 * FE-output parser. */

#ifndef SPFY_FE_HOST_FE_PARSE_H
#define SPFY_FE_HOST_FE_PARSE_H

#include <stdint.h>
#include <stdio.h>

#include "fe.h"
#include "phoneset.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     arpabet[8];
    int      duration;
    int8_t   syl_stress;
    char     accent[24];
    int      syl_index;
} fe_parsed_phoneme_t;

typedef struct {
    char                 text[64];
    int                  char_start;
    int                  char_len;
    char                 pos[16];
    int                  stress_level;
    int                  n_syllables;
    /* fr-CA liaison/elision: this word's phone list opened WITHOUT a `.N`
     * marker, so its leading phones continue the syllable whose nucleus
     * sits in the PREVIOUS word ("m'appelle" = /ma.pEl/, the `a` belonging
     * to the /ma/... */
    int                  first_syl_implicit;
    int                  pause_after_ms;
    int                  phrase_id;
    /* SSML / Balabolka prosody overrides (extension fields). */
    int8_t               pitch_st;
    int8_t               rate_pct;
    fe_parsed_phoneme_t *phonemes;
    int                  n_phonemes;
    int                  phonemes_cap;
} fe_parsed_word_t;

/* Max phrases ({...} utterance blocks) tracked per parse. */
#define FE_PARSE_MAX_PHRASES 64

/* What `pau(p?d)` ("default duration") resolves to. The engine's own value
 * for the pau target is readable at sub+0x18 in FUN_08ee2960; a live capture
 * across six texts reads 25.0 ms wherever the FE emitted `?d`, and exactly
 * p/2 ms wherever it emitted a concrete p. So `?d` == p50. */
#define FE_PAU_DEFAULT_P 50

typedef struct {
    int               pause_before_ms;
    int               pause_after_ms;
    fe_parsed_word_t *words;
    int               n_words;
    int               words_cap;
    /* Per-phrase terminating punctuation, parsed from the marker char the
     * FE emits inside each `{X` opener (e.g. */
    char              phrase_terms[FE_PARSE_MAX_PHRASES];
    int               n_phrase_terms;
    /* Per-phrase user pause (ms), from `\!pN` embedded tags rendered by
     * build_inline_mixed_tagged as `pau(uN)` openers (the `u` unit marks a
     * USER pause, distinct from the FE's structural `pau(pN)` which is not
     * rendered as... */
    int               phrase_lead_pause_ms[FE_PARSE_MAX_PHRASES];
    /* Structural `pau(pN)` values, per phrase, as the FE emitted them.
     *
     * Every phrase opens and closes with a structural pau, which the slot
     * builder turns into two leading and two trailing halfphone pad slots.
     * The engine sizes those from THIS value (target ms = p/2), not from the
     * chosen unit's dur_like -- see FUN_08ee2960, which selects sub+0x18 for
     * a sub whose name starts "pau" and sub+0x08 (dur) for everything else.
     *
     * Distinct from phrase_lead_pause_ms above, which is USER silence from a
     * `\!pN` tag and is injected as extra audio. These size a pau unit that
     * is played either way. 0 = the phrase had no such tag.
     *
     * ⚠ pause_before_ms / pause_after_ms are NOT per-phrase and cannot
     * substitute: they key off the global word count, so phrase 1's leading
     * pau is recorded as a trailing one. */
    int16_t           phrase_pau_p_before[FE_PARSE_MAX_PHRASES];
    int16_t           phrase_pau_p_after [FE_PARSE_MAX_PHRASES];
} fe_parsed_t;

/* Parse the FE's tagged-text output (see host/PROTOCOL.md). */
int  fe_parse_tagged_output(const char *tagged, fe_parsed_t *out);

void fe_parsed_free(fe_parsed_t *out);

int  fe_parsed_count_phonemes(const fe_parsed_t *out);

/* Flatten the parsed structure into a spfy_fe_slot_t[] of at most
 * `slots_cap`. */
void fe_parsed_flatten_to_slots(const fe_parsed_t *parsed,
                                spfy_fe_slot_t *slots,
                                int slots_cap);

/* Full slot construction: emits `(n_phons + 2) * 2` halfphone slots (with 2
 * leading + 2 trailing pau pads) filled with ctx[5], sp[5], is_voiced,
 * emphasis_level, pitch/rate offsets. */
/* Per-voice phone-symbol -> engine phone-id table. */
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
 * flap rules) applied by fe_parse_tagged_output. */
/* Refinement scope, passed to fe_parse_set_refine(). */
#define FE_REFINE_NONE      0
#define FE_REFINE_ALL       1
#define FE_REFINE_FLAP_ONLY 2

void fe_parse_set_refine(int mode);

/* Current refinement state. */
int fe_parse_get_refine(void);

/* Enable fr-CA liaison stress inheritance: bare leading phones of a word
 * (no `.N`) inherit the previous word's final-syllable stress rather than
 * defaulting to unstressed. */
void fe_parse_set_liaison_inherit(int enabled);

void fe_parsed_debug_dump(const fe_parsed_t *p, FILE *out);

/* In-place pre-parse cleanup for the raw drain stream. */
void fe_clean_stream_inplace(char *s);

#ifdef __cplusplus
}
#endif

#endif
