#ifndef SPFY_FE_STAGE_TEXTNORM_H
#define SPFY_FE_STAGE_TEXTNORM_H

#include "fe.h"

/* Stage 1: Text normalisation.
 *
 * Input:   raw text (caller's UTF-8 / Latin-1 string) + optional hints.
 * Output:  populated %text, %token, %word, %phrase streams in `delta`.
 *
 * What this stage does:
 *   1. Walks the input bytes and emits one %text token per character
 *      with a symbol-vocabulary ID assigned via vocab->byte_to_id.
 *   2. Classifies each character via the letter-type attribute fields
 *      (idx 211-219 in vocab: lower / upper / yes / digit / fraction /
 *      punct / eow_dlmtr / etc).
 *   3. Detects word boundaries: any whitespace or punctuation char is
 *      a word delimiter. Builds the %word stream of (word_id, span)
 *      records and writes word_id into each %text token.
 *   4. Detects phrase boundaries: '.', '!', '?' are phrase-final;
 *      ',', ';', ':' are phrase-medial. Builds %phrase stream with
 *      phrase_id propagated to %text.
 *   5. Emits one %token per logical lexical unit (word, number,
 *      punctuation cluster). The %token's name field holds the FIRST
 *      character's symbol ID so downstream stages can dispatch on it.
 *
 * Prosody hints are applied here: a hint with byte_start..byte_end
 * tags every %text token whose source byte falls in the range. Tags
 * are stored in the token's `fields[]` slots and propagated to the
 * containing word/phrase by later stages.
 *
 * What this stage does NOT do (yet):
 *   - Number expansion ("123" -> "one hundred twenty-three")
 *   - Unit recognition ("5 cm" -> "five centimetres")
 *   - Abbreviation expansion ("Dr." -> "Doctor")
 *   - URL / e-mail / TLD detection
 * Those land in stage_textnorm_v2.c as iterative enrichments.
 */

/* Field indexes used in %text tokens. */
enum {
    SPFY_TEXT_FIELD_TYPE      = 0,  /* letter/digit/punct/space (vocab idx 211-219) */
    SPFY_TEXT_FIELD_CASE      = 1,  /* lower / upper (vocab idx 211-212) */
    SPFY_TEXT_FIELD_BYTE_OFF  = 2,  /* original byte offset in input */
    SPFY_TEXT_FIELD_EMPHASIS  = 3,  /* propagated prosody hint */
    SPFY_TEXT_FIELD_PITCH_ST  = 4,  /* propagated prosody hint */
    SPFY_TEXT_FIELD_RATE_PCT  = 5,  /* propagated prosody hint */
};

/* Run the text-norm stage. */
int spfy_fe_textnorm_run(const spfy_fe_t            *fe,
                         const char                 *text,
                         const spfy_prosody_hints_t *hints,
                         spfy_fe_delta_t            *delta);

#endif
