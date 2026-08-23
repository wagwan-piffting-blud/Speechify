/* Grapheme-to-phoneme (G2P) - multi-stage lookup that replaces the
 * SpeechWorks FE DLL's word-pronunciation step for the in-house FE. */

#ifndef SPFY_G2P_H
#define SPFY_G2P_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Which stage produced the result. */
typedef enum {
    SPFY_G2P_HIT_DICT   = 0,
    SPFY_G2P_HIT_SUFFIX = 1,
    SPFY_G2P_HIT_LTS    = 2,
} spfy_g2p_origin_t;

/* Look up `word`. */
int spfy_g2p_word_lookup_ex(const char *word, char *out, size_t out_n,
                             spfy_g2p_origin_t *origin);

/* Legacy form - same as _ex but doesn't return the origin and preserves the
 * original Phase 1 contract of returning -1 on OOV (dict miss) so callers
 * that wanted to detect "not in CMU dict" specifically still can. */
int spfy_g2p_word_lookup(const char *word, char *out, size_t out_n);

size_t spfy_g2p_dict_size(void);

#ifdef __cplusplus
}
#endif

#endif
