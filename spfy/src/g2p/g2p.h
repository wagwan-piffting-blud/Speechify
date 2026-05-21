/* Grapheme-to-phoneme (G2P) — multi-stage lookup that replaces the
 * SpeechWorks FE DLL's word-pronunciation step for the in-house FE.
 *
 * Lookup chain (try each in order, stop on first hit):
 *
 *   1. CMU Pronouncing Dict     ~126K words, ARPAbet + stress
 *   2. Suffix stripping         find stem in dict, append suffix phonemes
 *                                 ("speechifying" = "speechify" + "IH0 NG")
 *   3. Letter-to-sound rules    pure synthesis from spelling
 *                                 ("zyzzyva" → "Z IH0 Z Z IH0 V AH0")
 *
 * Stage 1 is bit-exact for in-dict words. Stage 2 is right ~90% of the
 * time for English inflections. Stage 3 is best-effort — better than
 * silence, but quality drops for words with irregular pronunciation
 * (mostly loanwords / proper nouns / acronyms).
 *
 * Output is uppercase ARPAbet, space-separated, with stress digits on
 * vowels (0/1/2 = none/primary/secondary). The engine's allophone
 * refinements (ix, ax, dx, el, en) are applied LATER by the existing
 * fe_host/fe_parse.c::apply_phoneme_refinement, after slot-tree
 * construction — not at the word-lookup step. */

#ifndef SPFY_G2P_H
#define SPFY_G2P_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Which stage produced the result. Useful for diagnostic output and
 * for quality reporting (e.g., "85% dict, 12% suffix, 3% LTS"). */
typedef enum {
    SPFY_G2P_HIT_DICT   = 0,   /* direct CMU dict hit */
    SPFY_G2P_HIT_SUFFIX = 1,   /* stem in dict + suffix phonemes */
    SPFY_G2P_HIT_LTS    = 2,   /* letter-to-sound rules */
} spfy_g2p_origin_t;

/* Look up `word`. Always returns 0 with a non-empty phoneme string
 * (LTS is the final fallback and never fails for nonempty input).
 *
 *   word     NUL-terminated ASCII; case-insensitive.
 *   out      caller buffer; receives uppercase ARPAbet, space-sep,
 *            NUL-terminated. 160 bytes is comfortable.
 *   out_n    sizeof(out).
 *   origin   if non-NULL, set to the stage that produced the result.
 *
 * Returns 0 on success, -1 on empty input, -2 on bad args. */
int spfy_g2p_word_lookup_ex(const char *word, char *out, size_t out_n,
                             spfy_g2p_origin_t *origin);

/* Legacy form — same as _ex but doesn't return the origin and
 * preserves the original Phase 1 contract of returning -1 on OOV
 * (dict miss) so callers that wanted to detect "not in CMU dict"
 * specifically still can. */
int spfy_g2p_word_lookup(const char *word, char *out, size_t out_n);

/* Sanity / diagnostic: total CMU dict entries embedded. */
size_t spfy_g2p_dict_size(void);

#ifdef __cplusplus
}
#endif

#endif /* SPFY_G2P_H */
