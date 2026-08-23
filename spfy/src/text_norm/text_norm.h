/* Text normalization - Phase 3 of the in-house FE. */

#ifndef SPFY_TEXT_NORM_H
#define SPFY_TEXT_NORM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SPFY_TOKEN_WORD            = 0,
    SPFY_TOKEN_PHRASE_BREAK    = 1,
    SPFY_TOKEN_SENTENCE_BREAK  = 2,
    SPFY_TOKEN_CUSTOM_PAUSE    = 3,
} spfy_token_type_t;

/* Token capacity caps. */
#define SPFY_TOKEN_TEXT_MAX      64
#define SPFY_TOKEN_PHONEMES_MAX  96

typedef struct {
    spfy_token_type_t type;
    char              text[SPFY_TOKEN_TEXT_MAX];
    /* SSML extensions. */
    char              phonemes[SPFY_TOKEN_PHONEMES_MAX];
    uint16_t          pause_ms;
    int8_t            pitch_st;
    int8_t            rate_pct;
} spfy_token_t;

/* Tokenize + normalize `input`. */
int spfy_text_normalize(const char *input,
                        spfy_token_t *out, size_t cap, size_t *out_n);

#ifdef __cplusplus
}
#endif

#endif
