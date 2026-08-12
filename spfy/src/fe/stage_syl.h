#ifndef SPFY_FE_STAGE_SYL_H
#define SPFY_FE_STAGE_SYL_H

#include "fe.h"

/* Stage 3: Syllabification + lexical stress prediction. */

enum {
    SPFY_STRESS_NONE      = 441,
    SPFY_STRESS_DOWN      = 440,
    SPFY_STRESS_PRIMARY   = 442,
    SPFY_STRESS_SECONDARY = 443,
};

int spfy_fe_syl_run(const spfy_fe_t *fe,
                    const char       *original_text,
                    spfy_fe_delta_t  *delta);

#endif
