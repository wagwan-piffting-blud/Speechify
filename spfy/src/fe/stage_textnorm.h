#ifndef SPFY_FE_STAGE_TEXTNORM_H
#define SPFY_FE_STAGE_TEXTNORM_H

#include "fe.h"

/* Stage 1: Text normalisation. */

enum {
    SPFY_TEXT_FIELD_TYPE      = 0,
    SPFY_TEXT_FIELD_CASE      = 1,
    SPFY_TEXT_FIELD_BYTE_OFF  = 2,
    SPFY_TEXT_FIELD_EMPHASIS  = 3,
    SPFY_TEXT_FIELD_PITCH_ST  = 4,
    SPFY_TEXT_FIELD_RATE_PCT  = 5,
};

int spfy_fe_textnorm_run(const spfy_fe_t            *fe,
                         const char                 *text,
                         const spfy_prosody_hints_t *hints,
                         spfy_fe_delta_t            *delta);

#endif
