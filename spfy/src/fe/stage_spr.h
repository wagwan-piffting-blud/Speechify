#ifndef SPFY_FE_STAGE_SPR_H
#define SPFY_FE_STAGE_SPR_H

#include "fe.h"

/* Stage 6: SPR formatter -- per-slot ctx[5]/sp[5] for USel input. */

int spfy_fe_spr_run(const spfy_fe_t       *fe,
                    spfy_fe_delta_t       *delta,
                    spfy_fe_utterance_t   *utt);

#endif
