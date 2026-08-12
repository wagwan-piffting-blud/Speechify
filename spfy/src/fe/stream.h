#ifndef SPFY_FE_STREAM_H
#define SPFY_FE_STREAM_H

#include <stddef.h>
#include <stdint.h>

/* Multi-stream token framework -- the central data structure. */

#define SPFY_FE_MAX_STREAMS  16
#define SPFY_FE_TOKEN_FIELDS 8

typedef enum {
    SPFY_STREAM_TEXT = 0,
    SPFY_STREAM_TOKEN,
    SPFY_STREAM_MORPH,
    SPFY_STREAM_SYL,
    SPFY_STREAM_WORD,
    SPFY_STREAM_PHRASE,
    SPFY_STREAM_PHONEME,
    SPFY_STREAM_HALFPHONE,
    SPFY_STREAM__MAX
} spfy_stream_kind_t;

typedef struct {
    /* Symbol vocabulary ID (from fe_symbol_table). */
    uint16_t name;

    /* Cross-stream sync indices: which syl/word/phrase contains this token. */
    uint16_t syl_id;
    uint16_t word_id;
    uint16_t phrase_id;

    /* Per-token attribute fields. */
    uint16_t fields[SPFY_FE_TOKEN_FIELDS];
} spfy_fe_token_t;

typedef struct {
    spfy_stream_kind_t kind;
    spfy_fe_token_t   *tokens;
    uint32_t           n_tokens;
    uint32_t           cap;
} spfy_fe_stream_t;

typedef struct {
    spfy_fe_stream_t streams[SPFY_STREAM__MAX];
} spfy_fe_delta_t;

void  spfy_fe_delta_init (spfy_fe_delta_t *d);
void  spfy_fe_delta_free (spfy_fe_delta_t *d);

/* Append a token to a stream. */
uint32_t spfy_fe_stream_push(spfy_fe_delta_t   *d,
                              spfy_stream_kind_t kind,
                              spfy_fe_token_t    tok);

const spfy_fe_token_t *spfy_fe_stream_tokens(const spfy_fe_delta_t *d,
                                              spfy_stream_kind_t    kind,
                                              uint32_t             *out_n);

#endif
