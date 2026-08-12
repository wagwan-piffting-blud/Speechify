#ifndef SPFY_FE_PROSODY_H
#define SPFY_FE_PROSODY_H

#include <stddef.h>
#include <stdint.h>

/* First-class prosody hints for the FE.
 *
 * Path-B explicitly designs prosody/pitch/rate as first-class inputs
 * threaded through every FE stage. A caller can pass either:
 *   1. Plain text + NULL hints                  (neutral synthesis)
 *   2. SSML + NULL hints                        (parser extracts hints)
 *   3. Plain text + spfy_prosody_hints_t        (programmatic control)
 *
 * Hints are RANGE-BASED: each hint applies to a half-open character
 * range [byte_start, byte_end) of the input text. The FE's text
 * stage tags those characters; downstream stages propagate the tags
 * to syllables/words and finally to halfphone slots.
 *
 * Why first-class: a downstream patch-on-output approach (Path A)
 * would require us to either modify Delta bytecode or post-process
 * its phoneme output. By contrast, this module makes prosody just
 * another piece of the per-token attribute set. Adding new prosody
 * dimensions = adding a new field to spfy_prosody_hint_t and a new
 * stage that propagates it. ~100 LOC per feature, not 100s.
 *
 * Engine downstream support (already RE'd; see DLL_ANALYSIS.md):
 * the Speechify engine's FUN_08e8a250 reads `word_prominence > 2`
 * and shifts f0tr/durt CART targets. Emitting `\!emph=strong/moderate
 * /reduced` in our FE output activates that machinery directly.
 */

typedef enum {
    SPFY_HINT_EMPHASIS = 0,
    SPFY_HINT_PITCH,
    SPFY_HINT_RATE,
    SPFY_HINT_BREAK,
    SPFY_HINT_PHONEME,
    SPFY_HINT_VOICE,
    SPFY_HINT_LANG,
    SPFY_HINT__MAX
} spfy_hint_kind_t;

typedef enum {
    SPFY_EMPH_NONE     = 0,
    SPFY_EMPH_REDUCED  = 1,
    SPFY_EMPH_MODERATE = 2,
    SPFY_EMPH_STRONG   = 3,
} spfy_emphasis_t;

typedef struct {
    spfy_hint_kind_t kind;

    uint32_t byte_start;
    uint32_t byte_end;

    /* Tag-specific value union. */
    union {
        spfy_emphasis_t emphasis;
        int8_t          pitch_st;
        int16_t         rate_pct;
        uint16_t        break_ms;
        const char     *phoneme_spr;
        const char     *voice_name;
        const char     *lang_tag;
    } v;
} spfy_prosody_hint_t;

typedef struct {
    spfy_prosody_hint_t *hints;
    uint32_t             n_hints;
    uint32_t             cap;
} spfy_prosody_hints_t;

void spfy_prosody_hints_init(spfy_prosody_hints_t *h);
void spfy_prosody_hints_free(spfy_prosody_hints_t *h);
int  spfy_prosody_hints_add (spfy_prosody_hints_t *h,
                              spfy_prosody_hint_t   hint);

int  spfy_prosody_emphasize (spfy_prosody_hints_t *h,
                              uint32_t byte_start, uint32_t byte_end,
                              spfy_emphasis_t level);
int  spfy_prosody_pitch_shift(spfy_prosody_hints_t *h,
                              uint32_t byte_start, uint32_t byte_end,
                              int8_t   semitones);
int  spfy_prosody_rate       (spfy_prosody_hints_t *h,
                              uint32_t byte_start, uint32_t byte_end,
                              int16_t  rate_pct);
int  spfy_prosody_break      (spfy_prosody_hints_t *h,
                              uint32_t byte_pos,
                              uint16_t duration_ms);

/* Parse SSML-flavoured input into (plain_text, hints) pair. */
int  spfy_prosody_parse_ssml(const char            *ssml,
                              char                 **out_plain,
                              spfy_prosody_hints_t  *out_hints);

#endif
