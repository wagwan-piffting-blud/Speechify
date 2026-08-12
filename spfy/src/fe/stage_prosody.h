#ifndef SPFY_FE_STAGE_PROSODY_H
#define SPFY_FE_STAGE_PROSODY_H

#include "fe.h"

/* Stage 5: Prosody-hint propagation. */

enum {
    SPFY_PROSODY_FIELD_EMPHASIS = 5,
    SPFY_PROSODY_FIELD_PITCH_ST = 6,
    SPFY_PROSODY_FIELD_RATE_PCT = 7,
};

int spfy_fe_prosody_run(const spfy_fe_t *fe,
                        spfy_fe_delta_t  *delta);

#endif
