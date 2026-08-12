#ifndef SPFY_FE_STAGE_MORPH_H
#define SPFY_FE_STAGE_MORPH_H

#include "fe.h"

/* Stage 2: Morphological analysis. */

/* Symbol-vocabulary IDs we care about (verified against
 * fe_symbol_table.json indices 322..333). */
enum {
    SPFY_MORPH_PRE     = 323,
    SPFY_MORPH_ROOT    = 324,
    SPFY_MORPH_SUF     = 325,
    SPFY_MORPH_UNDEF   = 326,
    SPFY_MORPH_CLITIC  = 327,
};

int spfy_fe_morph_run(const spfy_fe_t *fe,
                      const char       *original_text,
                      spfy_fe_delta_t  *delta);

#endif
