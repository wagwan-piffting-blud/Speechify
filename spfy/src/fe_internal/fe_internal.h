/* In-house front-end — text → tagged-output assembler.
 *
 * This is the bridge between the Phase 3 text normalizer / Phase 2 G2P
 * and the existing fe_host/fe_parse.c parser. Output is the same tagged
 * format the SpeechWorks FE DLL produces, so the rest of the pipeline
 * (parse → slot tree → unit selection → WSOLA) is reused verbatim.
 *
 *   text  ──▶  text_norm  ──▶  spfy_token_t[]
 *                                   │
 *                                   ▼  (this module)
 *                              fe_internal
 *                                   │ per token:
 *                                   │   g2p_lookup_ex(word)
 *                                   │   syllabify(phonemes)
 *                                   │   emit "<word(...) [.S phon(p100) ...]>"
 *                                   ▼
 *                              tagged-text string
 *                                   │
 *                                   ▼
 *                              fe_parse.c → spfy_fe_utt_t
 *
 * Output format (matches spr_inline_to_tagged in spfy_synth.c):
 *
 *   #{. pau(p25) <word_n(off,len) undef,max_stress [
 *                  .stress[,accent] phoneme(p100) ...
 *                  ...syllables...
 *                ]>
 *                ... more words ...
 *                pau(p50) } %%
 *
 * Tagging conventions:
 *   - First syllable of first word in an utterance carries ",H*" (accent)
 *   - Pause durations: 25 for utt start, 50 for end, 30 for phrase break
 *   - off/len are best-effort character positions in the original text
 *
 * For Phase 4 this is good-enough; later iteration can add stress
 * pattern selection, more accent variation, and refined pause timing.
 */

#ifndef SPFY_FE_INTERNAL_H
#define SPFY_FE_INTERNAL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Convert raw input text to tagged-text in `out`. `out_n` should be at
 * least 4 KB for typical sentences; the function NUL-terminates.
 *
 * Returns 0 on success, 1 if the output was truncated, -1 on bad args. */
int spfy_fe_internal_text_to_tagged(const char *text,
                                     char *out, size_t out_n);

#ifdef __cplusplus
}
#endif

#endif /* SPFY_FE_INTERNAL_H */
