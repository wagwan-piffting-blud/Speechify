#ifndef SPFY_FE_STAGE_MORPH_H
#define SPFY_FE_STAGE_MORPH_H

#include "fe.h"

/* Stage 2: Morphological analysis.
 *
 * Walks the %word stream and decomposes each word into morphemes
 * (prefix + root + suffix). Output goes to the %morph stream.
 *
 * For Path B's first iteration we use a small hand-curated list of
 * the most common English prefixes (un-, re-, dis-, pre-, in-, im-,
 * non-, anti-, sub-, super-, mis-, over-, under-, out-, fore-) and
 * suffixes (-ing, -ed, -er, -est, -ly, -ness, -ment, -tion, -sion,
 * -able, -ible, -ful, -less, -ish, -ous, -al, -ic, -ity, -ize, -ate,
 * -fy, -y, -s).
 *
 * Recognition rules:
 *   - Suffix match: longest match wins, with a minimum-root-length
 *     guard (root must be >= 3 chars to avoid mis-suffixing words
 *     like "ring" -> "r" + "-ing").
 *   - Prefix match: prefix must be >= 2 chars and root must be
 *     >= 3 chars after stripping it.
 *   - Words too short (<= 3 chars) get a single root morpheme.
 *
 * Each %morph token's `name` field is the symbol-vocabulary ID for
 * the morpheme TYPE (idx 322=pre, 323=root, 324=suf, 326=undef,
 * 327=clitic, 329=ing, 322=pre etc per F2 catalog). The morph's
 * actual text span is encoded in fields[0]=byte_start, fields[1]=
 * byte_len -- caller can read it from the original input.
 *
 * What this stage does NOT do (yet):
 *   - Recursive morpheme decomposition (e.g., "re-organ-ize-ation")
 *     -- it stops after one prefix + one suffix.
 *   - Allomorph normalisation ("running" -> "run" + "ing", with
 *     consonant doubling rule). The root is reported AS-WRITTEN.
 *   - Compound splitting ("blackboard" -> "black" + "board"). Compound
 *     recognition lives in stage_compound.c (separate, uses t229
 *     phrase table + small head-word tables).
 */

/* Symbol-vocabulary IDs we care about (verified against
 * fe_symbol_table.json indices 322..333). */
enum {
    SPFY_MORPH_PRE     = 323,   /* "pre" */
    SPFY_MORPH_ROOT    = 324,   /* "root" */
    SPFY_MORPH_SUF     = 325,   /* "suf" */
    SPFY_MORPH_UNDEF   = 326,   /* "undef" */
    SPFY_MORPH_CLITIC  = 327,   /* "clitic" */
};

int spfy_fe_morph_run(const spfy_fe_t *fe,
                      const char       *original_text,
                      spfy_fe_delta_t  *delta);

#endif
