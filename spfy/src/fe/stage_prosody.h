#ifndef SPFY_FE_STAGE_PROSODY_H
#define SPFY_FE_STAGE_PROSODY_H

#include "fe.h"

/* Stage 5: Prosody-hint propagation.
 *
 * The prosody hints (emphasis/pitch/rate) are first set on per-byte
 * %text tokens during text-norm. This stage climbs them up the
 * stream hierarchy:
 *
 *   %text (per byte) -> %word -> %syl -> %phoneme
 *
 * Propagation rule (per dimension):
 *   - emphasis: MAX over child tokens (any byte tagged "strong"
 *     promotes the whole word/syllable/phoneme to "strong")
 *   - pitch:    SIGNED MAX-ABS (the largest absolute shift wins;
 *     this keeps a `<prosody pitch="-3st">` tag from being cancelled
 *     by an unrelated `+1` on a later byte)
 *   - rate:     SIGNED MAX-ABS, same reasoning
 *
 * Per-token field layout:
 *   %word/%syl: fields[5]=emphasis, fields[6]=pitch_st, fields[7]=rate_pct
 *   %phoneme:   fields[5]=emphasis, fields[6]=pitch_st, fields[7]=rate_pct
 *
 * After this stage runs, every %phoneme token carries the prosody
 * envelope of its parent syllable -- which is what M6's SPR formatter
 * uses to emit \!emph escape codes for the engine downstream.
 */

enum {
    /* Field indexes shared across %word, %syl, %phoneme for prosody. */
    SPFY_PROSODY_FIELD_EMPHASIS = 5,
    SPFY_PROSODY_FIELD_PITCH_ST = 6,
    SPFY_PROSODY_FIELD_RATE_PCT = 7,
};

int spfy_fe_prosody_run(const spfy_fe_t *fe,
                        spfy_fe_delta_t  *delta);

#endif
