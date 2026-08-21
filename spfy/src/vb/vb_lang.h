#ifndef SPFY_VB_LANG_H
#define SPFY_VB_LANG_H

#include <stddef.h>
#include <stdint.h>

/* The parts of a VIN that are NOT speaker data, embedded so a build does not
 * have to read them out of a donor voice.
 *
 * ⭐ WHAT COUNTS AS A LANGUAGE TABLE IS MEASURED, NOT ASSERTED. Every section
 * in `spfy_vb_lang_feat` is BYTE-IDENTICAL between jill and tom -- two voices
 * that differ in unit-record version (v100008 / v100006), label count (46/47)
 * and label ordering. `filename` is the one per-voice section in `feat` and is
 * deliberately absent; the builder writes its own from the corpus.
 *
 *     name                            808 B   sha1 84f935ef96
 *     start .. power_z              11-14 B each
 *     lisp_initial_boundary_strength   57 B   sha1 49f0a707a2
 *     lisp_final_boundary_strength     55 B   sha1 a6413e6473
 *     Syllable.stress                  42 B   sha1 0faee96134
 *     lisp_mod_tobi_accent            110 B   sha1 3d22e55dbd
 *     lisp_mod_tobi_endtone           101 B   sha1 1805547c83
 *
 * Regenerate and re-check with
 *   reveng/spfy4/tools/voicebuild/vb_genlang.py --check
 *   reveng/spfy4/tools/voicebuild/vb_genlang.py --out src/vb/vb_lang_enus.c
 * The generator REFUSES to emit a section that differs between the vendors,
 * which is what stops a speaker's data being embedded here by accident. */

const uint8_t      *spfy_vb_lang_feat(size_t *n);

/* The ccos label ordering. ⚠ Per-voice by nature -- jill carries 46 labels and
 * tom 47, and tom inserts `dx` after `ch` where jill appends it last -- so
 * this is a CHOICE rather than a shared asset. Every unit record we have
 * written numbers `phone_center` and `phone_ctx` in this order; changing it
 * renumbers a whole voice, for nothing. */
const char *const  *spfy_vb_lang_labl(size_t *n);

#endif
