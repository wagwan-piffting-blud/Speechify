/* TD-PSOLA pitch shift for SSML <prosody pitch=...>.
 *
 * The Speechify engine has its own PSOLA branch (FUN_08EE7050) but it's
 * dead code for Tom-family voices and would only modify unit-internal
 * sub-units anyway. SSML pitch is an output-stream feature the engine
 * never had, so we run our own TD-PSOLA on the post-WSOLA PCM stream.
 *
 * Algorithm:
 *   1. Per-frame autocorrelation pitch period estimate (50-400 Hz F0)
 *   2. Frame-to-sample period interpolation
 *   3. Pitch-mark placement at signed-peak extrema, period-spaced
 *   4. Resynthesis: walk output marks at period / pitch_factor, OLA a
 *      2*period Hann window from nearest input mark at each output mark.
 *
 * Duration is preserved exactly: out_n == in_n. Voiced/unvoiced is
 * classified by autocorr-peak ratio; unvoiced grains use uniform pseudo
 * periods so silence/fricatives still OLA-reconstruct cleanly.
 *
 * Output peak amplitude is conservatively limited to the input peak.
 */

#ifndef SPFY_DSP_PITCH_SHIFT_H
#define SPFY_DSP_PITCH_SHIFT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Out-of-place TD-PSOLA pitch shift.
 *
 *   in, n_in        input PCM
 *   out             output buffer, must be allocated to >= n_in samples
 *   semitones       +/- semitones; positive = pitch up
 *   sample_rate     PCM rate (only used to scale period search range;
 *                   8000 is the natively-tuned setpoint)
 *
 * Returns 0 on success.
 */
int spfy_pitch_shift_block(const int16_t *in, size_t n_in,
                           int16_t *out,
                           float semitones, int sample_rate);

/* Convenience: allocate internal output buffer, write back in place. */
int spfy_pitch_shift_inplace(int16_t *samples, size_t n,
                             float semitones, int sample_rate);

#ifdef __cplusplus
}
#endif

#endif /* SPFY_DSP_PITCH_SHIFT_H */
