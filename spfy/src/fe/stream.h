#ifndef SPFY_FE_STREAM_H
#define SPFY_FE_STREAM_H

#include <stddef.h>
#include <stdint.h>

/* Multi-stream token framework -- the central data structure.
 *
 * Direct lift from the Delta architecture (Hertz/Karplus 1985):
 * an utterance is a collection of named streams (%text, %morph,
 * %syllable, %word, %phrase, %phoneme, ...). Each stream is a
 * sequence of tokens; tokens across streams are tied by SYNC MARKS
 * so that a single morpheme in %morph spans multiple letters in
 * %text and multiple phonemes in %phoneme.
 *
 * In our C port we represent the structure as parallel arrays of
 * tokens with explicit sync-link fields: each token in any stream
 * carries a "syl_id" (which syllable it belongs to), a "word_id",
 * a "phrase_id", etc. Cross-stream queries become array-index
 * lookups instead of pattern matching.
 *
 * This is the working data structure that pipeline stages mutate.
 * The final FE output (spfy_fe_utterance_t in fe.h) is computed by
 * serialising the %phoneme stream + per-token metadata into the
 * per-slot scoring schema the C scoring stack expects.
 */

#define SPFY_FE_MAX_STREAMS  16
#define SPFY_FE_TOKEN_FIELDS 8    /* per-token user fields */

typedef enum {
    SPFY_STREAM_TEXT = 0,        /* input characters / letters */
    SPFY_STREAM_TOKEN,           /* lexical tokens (words, numbers) */
    SPFY_STREAM_MORPH,           /* morphemes (root/prefix/suffix/clitic) */
    SPFY_STREAM_SYL,             /* syllables */
    SPFY_STREAM_WORD,            /* word boundaries */
    SPFY_STREAM_PHRASE,          /* phrase / intonation-phrase boundaries */
    SPFY_STREAM_PHONEME,         /* output phonemes */
    SPFY_STREAM_HALFPHONE,       /* halfphone slots (USel input) */
    SPFY_STREAM__MAX
} spfy_stream_kind_t;

typedef struct {
    /* Symbol vocabulary ID (from fe_symbol_table). For %text this is a
     * letter ID; for %phoneme it's a phoneme ID; for %morph it's
     * "root"/"prefix"/"suffix"/etc. */
    uint16_t name;

    /* Cross-stream sync indices: which syl/word/phrase contains this
     * token. UINT16_MAX = unset. */
    uint16_t syl_id;
    uint16_t word_id;
    uint16_t phrase_id;

    /* Per-token attribute fields. Generic u16 slots whose meaning is
     * stream-specific (for %syl: stress level; for %phoneme: phonetic
     * features; for %word: POS tag; etc.). */
    uint16_t fields[SPFY_FE_TOKEN_FIELDS];
} spfy_fe_token_t;

typedef struct {
    spfy_stream_kind_t kind;
    spfy_fe_token_t   *tokens;
    uint32_t           n_tokens;
    uint32_t           cap;
} spfy_fe_stream_t;

/* The full multi-stream utterance representation. */
typedef struct {
    spfy_fe_stream_t streams[SPFY_STREAM__MAX];
} spfy_fe_delta_t;

void  spfy_fe_delta_init (spfy_fe_delta_t *d);
void  spfy_fe_delta_free (spfy_fe_delta_t *d);

/* Append a token to a stream. Returns the inserted token's index, or
 * UINT32_MAX on alloc failure. */
uint32_t spfy_fe_stream_push(spfy_fe_delta_t   *d,
                              spfy_stream_kind_t kind,
                              spfy_fe_token_t    tok);

/* Resolve a stream's tokens for read access. NULL if empty. */
const spfy_fe_token_t *spfy_fe_stream_tokens(const spfy_fe_delta_t *d,
                                              spfy_stream_kind_t    kind,
                                              uint32_t             *out_n);

#endif
