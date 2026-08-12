#ifndef SPFY_FE_STAGE_LTS_H
#define SPFY_FE_STAGE_LTS_H

#include "fe.h"

/* Stage 4: Letter-to-phoneme (LTS) rules. */

int spfy_fe_lts_run(const spfy_fe_t *fe,
                    const char       *original_text,
                    spfy_fe_delta_t  *delta);

#endif
