#ifndef SPFY_FE_STAGE_SYL_H
#define SPFY_FE_STAGE_SYL_H

#include "fe.h"

/* Stage 3: Syllabification + lexical stress prediction.
 *
 * For each %word, finds syllable nuclei (vowel clusters) and assigns
 * boundaries between them. Emits one %syl token per syllable into the
 * delta. Each %syl carries:
 *   name      = stress level: SPFY_STRESS_PRIMARY / _SECONDARY / _NONE
 *   word_id   = parent %word index
 *   phrase_id = parent %phrase index
 *   fields[0] = byte_start in the original input
 *   fields[1] = byte_len
 *   fields[2] = position-in-word (0=first, n-1=last)
 *   fields[3] = total syllables in this word
 *   fields[4] = nucleus character class (vowel category)
 *
 * Syllabification algorithm (Path B first iteration):
 *   1. Walk the word's bytes; group consecutive vowels (a/e/i/o/u/y;
 *      y treated as vowel only in non-initial position) into nuclei.
 *   2. Each nucleus becomes a syllable. Distribute consonants:
 *        - between two nuclei with 1 consonant: goes to second syl
 *          (CV.CV like "rea-son")
 *        - between two nuclei with 2+ consonants: split after first
 *          (CVC.CV like "but-ter")
 *        - word-initial consonants: attach to first syllable
 *        - word-final consonants: attach to last syllable
 *
 * Stress assignment (first iteration):
 *   - Default: primary stress on first syllable.
 *   - Suffix -tion / -sion / -ic / -ical / -ity: stress on syllable
 *     immediately preceding the suffix.
 *   - Suffix -ate (3+ syllables): primary on antepenultimate.
 *   - All other syllables in the word: stress none.
 *
 * Future iterations will:
 *   - Look up explicit stress patterns from rootdict / worddict
 *     (registry-B tables) for irregulars.
 *   - Add secondary stress for compound and long words.
 *   - Handle two-syllable noun-vs-verb stress contrast (PROduce
 *     vs proDUCE).
 */

/* Stress levels stored in %syl token's `name` field. */
enum {
    SPFY_STRESS_NONE      = 441,    /* "none"   in vocab */
    SPFY_STRESS_DOWN      = 440,    /* "down"   secondary equivalent */
    SPFY_STRESS_PRIMARY   = 442,    /* "str"    primary stress */
    SPFY_STRESS_SECONDARY = 443,    /* "acc"    accent */
};

int spfy_fe_syl_run(const spfy_fe_t *fe,
                    const char       *original_text,
                    spfy_fe_delta_t  *delta);

#endif
