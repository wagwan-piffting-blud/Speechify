#ifndef SPFY_FE_STAGE_SPR_H
#define SPFY_FE_STAGE_SPR_H

#include "fe.h"

/* Stage 6: SPR formatter -- per-slot ctx[5]/sp[5] for USel input.
 *
 * Converts the multi-stream %phoneme + %syl + %word + %phrase
 * representation into the per-halfphone-slot scoring schema that
 * `spfy_fe_utterance_t.slots[]` exposes (defined in fe.h).
 *
 * Each phoneme becomes TWO halfphone slots: a "left half" and a "right
 * half". Silences (phoneme name = SPFY_SYM_GAP) become ONE slot with
 * a sentinel HP-class.
 *
 * The slot output schema:
 *   ctx[5]  = HP-class neighbour 5-tuple (left2, left1, this, right1, right2)
 *             where HP-class = phone_id*2 + side, with optional voice-side
 *             pair-swaps applied later by the voice loader.
 *   sp[5]   = 5 SP feature row indices the engine reads at scoring time.
 *             Computed from positional info in the streams.
 *   is_voiced = derived from the phoneme's ID via a small lookup
 *   emphasis_level / pitch_offset_st / rate_offset_pct =
 *               propagated from %phoneme prosody fields (see stage_prosody.c)
 *
 * Phone-ID mapping: a hand-built SAMPA-symbol -> integer phone_id
 * mapping covers ~36 common English phonemes. This is APPROXIMATE
 * (the real Tom voice's phone IDs may differ); it's structured the
 * same way so future tuning swaps in the correct mapping without
 * touching slot-construction code.
 *
 * The slots produced by this stage feed directly into spfy_synth_replay,
 * which then runs Viterbi DP over PRSL candidate pools using these
 * ctx[5]/sp[5] values.
 */

int spfy_fe_spr_run(const spfy_fe_t       *fe,
                    spfy_fe_delta_t       *delta,
                    spfy_fe_utterance_t   *utt);

#endif
