/* syl_span.h -- the one place that knows a syllable can straddle a word.
 *
 * THE INVARIANT THAT IS FALSE
 * ---------------------------
 * `spfy_fe_utt_t` models syllables as nested INSIDE a word: word -> syllable
 * -> segment, one parent each. French liaison and elision break that. The FE
 * emits
 *
 *     <l' (?d,9) pro,0 [.0 l(p100) ] > <autobus () noun,1 [ox(p100) ...
 *
 * where `autobus`'s phone list opens with NO `.N` marker because its first
 * phone completes the syllable `l'` started -- "L'autobus" is /lo.to.bys/,
 * and /lo/ spans two words. `spfy_fe_utt_t` cannot represent that, so the
 * parser mints a second syllable record and `syl_cont_prev` records which
 * records are really continuations.
 *
 * THE SLOT TREE DOES NOT HAVE THIS PROBLEM
 * ----------------------------------------
 * `spfy_build_graph` MERGES a continuation into the node it completes, so a
 * merged syllable is ONE node holding all its half-phones, parented to the
 * word where the syllable STARTS. That is what the engine builds: for
 * fr_053 ("L'autobus etait plein ce matin.") the engine reports 62 slots,
 * which is our unmerged 64 minus exactly the two continuation nodes, and all
 * 33 slots of the engine's own chosen path land on the merged post-order --
 * including three multi-unit anchors whose unit spans equal the merged
 * syllables' half-phone spans (`C:\tmp\engine_tree_check.py`).
 *
 * So the merge is a property of the TREE, not something each consumer
 * re-derives. Anything reading the tree already sees the engine's grouping.
 *
 * WHAT IS LEFT HERE
 * -----------------
 * Two things still need the FE-level view, because they index `spfy_fe_utt_t`
 * arrays rather than the tree:
 *
 *   - per-syllable attributes (stress, accent, btone, acctype, volume), read
 *     through a merged node's `fe_shared`. That id is the HEAD syllable's, so
 *     `spfy_syl_effective` is already identity there -- it stays as the guard
 *     that a continuation index can never silently read its own word's record
 *     instead of the record of the syllable it belongs to. One commit did
 *     exactly that and gave a single syllable two different accent values.
 *   - `spfy_derive_sp_targets`, which numbers engine syllables and must not
 *     spend an ordinal on a continuation.
 *
 * ⚠ Everything here is IDENTITY for en-US and es-MX. `syl_cont_prev` can only
 * be set by the fr-CA branch in fe_parse.c's parse_word_body, so a NULL or
 * all-zero flag array leaves the tree and every lookup exactly as they were.
 * That is what protects Tom's byte-exact parity: the guarantee is in the
 * data, not in the test suite.
 */
#ifndef SPFY_SYL_SPAN_H
#define SPFY_SYL_SPAN_H

#include <stdint.h>
#include "build_graph.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Is the liaison merge active? */
int spfy_syl_merge_enabled(void);

/* Is FE syllable record `fe_sidx` a continuation of the previous one? */
int spfy_syl_continues_prev(const spfy_fe_utt_t *utt, uint32_t fe_sidx);

/* The FE syllable record that `fe_sidx` MERGES INTO -- itself when it opens
 * a syllable of its own, else the one it continues. */
uint32_t spfy_syl_effective(const spfy_fe_utt_t *utt, uint32_t fe_sidx);

/* There is deliberately NO half-phone-span helper here. */

#ifdef __cplusplus
}
#endif

#endif
