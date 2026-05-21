/* Text normalization — Phase 3 of the in-house FE.
 *
 * Takes arbitrary input text and emits a token stream that the Phase 4
 * tagged-output assembler can directly walk. Handles:
 *
 *   * Whitespace + word tokenization
 *   * Cardinal numbers up to 999,999,999  ("42" → "forty two")
 *   * 4-digit years (1000-2999)            ("1990" → "nineteen ninety")
 *   * Decimal numbers                      ("3.14" → "three point one four")
 *   * Punctuation → phrase / sentence break tokens
 *   * Lowercasing for downstream G2P lookup
 *
 * Intentionally NOT in this phase (kept for iteration later):
 *
 *   * Abbreviation expansion (Mr./Dr./St.)
 *   * Currency / unit recognition ($5, 5%)
 *   * Date / time parsing
 *   * Acronym vs. word disambiguation
 *
 * The tokenizer keeps numbers as separate words from their successors —
 * "I had 3 apples" → ["i", "had", "three", "apples"]. Phrase / sentence
 * breaks are emitted as their own tokens with empty .text so downstream
 * stages can choose silence durations / prosody.
 */

#ifndef SPFY_TEXT_NORM_H
#define SPFY_TEXT_NORM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SPFY_TOKEN_WORD            = 0,  /* lexical content; .text populated */
    SPFY_TOKEN_PHRASE_BREAK    = 1,  /* ,  ;  :  ( ) -- short pause */
    SPFY_TOKEN_SENTENCE_BREAK  = 2,  /* .  !  ?         -- long pause */
} spfy_token_type_t;

/* Token capacity caps. Words rarely exceed 32 chars; the few that do
 * (long compounds, expanded numbers) fit comfortably here. */
#define SPFY_TOKEN_TEXT_MAX  64

typedef struct {
    spfy_token_type_t type;
    char              text[SPFY_TOKEN_TEXT_MAX];
} spfy_token_t;

/* Tokenize + normalize `input`. Caller provides a fixed-cap output
 * array; we fill up to `cap` tokens and report the actual count via
 * `*out_n`. If the input would overflow, we stop emitting tokens and
 * return 1 (so caller can detect partial output). 0 = clean fit.
 * Returns -1 on bad args.
 *
 * Recommended cap: roughly 2 × wordcount + 10 — punctuation can
 * inflate counts slightly via break tokens.
 */
int spfy_text_normalize(const char *input,
                        spfy_token_t *out, size_t cap, size_t *out_n);

#ifdef __cplusplus
}
#endif

#endif /* SPFY_TEXT_NORM_H */
