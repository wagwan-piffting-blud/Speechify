/* WSOLA frame-based time-stretch for SSML <prosody rate=...>.
 *
 * Scales duration by 1/factor without affecting pitch. Companion to
 * pitch_shift.{h,c} (which preserves duration). Both used together
 * implement the prosody verb's full state (rate + pitch + volume).
 *
 * Algorithm:
 *   - Split input into Hann-windowed analysis frames (256 samples / 32 ms)
 *   - Synthesis hop = analysis_hop * factor (frames advance through
 *     the input at output speed)
 *   - For each output frame, search +-50 samples around the nominal
 *     input position for best NCC against the previous output frame's
 *     tail; this is the WSOLA "waveform similarity" step that keeps
 *     phase coherent across the time-scale change.
 *   - OLA accumulate, normalize by Hann weight sum.
 */

#ifndef SPFY_DSP_TIME_STRETCH_H
#define SPFY_DSP_TIME_STRETCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Out-of-place WSOLA time-stretch.
 *
 *   in, n_in        input PCM
 *   factor          > 1.0 = speed up (shorter output), < 1.0 = slow down
 *                   (longer output). 1.0 is a pass-through.
 *   sample_rate     PCM rate (frame length scales with it).
 *   out             caller allocates **out via malloc; sets *out_n.
 *
 * Returns 0 on success, negative on error. */
int spfy_time_stretch_block(const int16_t *in, size_t n_in,
                            int16_t **out, size_t *out_n,
                            float factor, int sample_rate);

#ifdef __cplusplus
}
#endif

#endif /* SPFY_DSP_TIME_STRETCH_H */
