#ifndef SPFY_FE_STAGE_LTS_H
#define SPFY_FE_STAGE_LTS_H

#include "fe.h"

/* Stage 4: Letter-to-phoneme (LTS) rules.
 *
 * Walks each %word and emits %phoneme tokens carrying SAMPA-style
 * phoneme IDs from the 469-symbol vocabulary (idx 229..321 are the
 * phoneme symbols + features the FE uses).
 *
 * Path B first iteration uses a hand-coded English ruleset. It's
 * intentionally small and rule-based -- enough to cover the high-
 * frequency cases (digraphs th/ch/sh/ph/ng, common vowel patterns,
 * silent letters in selected contexts) and produce phonetically
 * recognisable output. It will be replaced/augmented in a future
 * iteration by a data-driven path that consults registry-B tables
 * (the original Eloquence LTS rules). For the MVP, rules-only is the
 * fastest way to get end-to-end text->phoneme output flowing into
 * USel.
 *
 * Each %phoneme token's `name` field is the phoneme's symbol-vocab ID;
 * its `syl_id` field links back to the parent %syl token (so later
 * stages know which phonemes are stressed); fields[0] = byte_start in
 * input, fields[1] = byte_len of the source letters, fields[2] =
 * is_vowel flag, fields[3] = position in syllable (0=onset, 1=nucleus,
 * 2=coda).
 *
 * Phoneme inventory used (SAMPA-style IDs from vocab):
 *   Consonants: b p d t k g f v T D s z S Z h m n G r l y w
 *   Vowels:     i I e E A a o O u U @ (schwa) Y W
 *
 * The exact ID values come from spfy/build/fe_symbol_table.json --
 * we resolve them by name once at FE-open time, store them in the
 * fe_t for fast access. (See fe.c.)
 */

int spfy_fe_lts_run(const spfy_fe_t *fe,
                    const char       *original_text,
                    spfy_fe_delta_t  *delta);

#endif
