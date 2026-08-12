/* WSOLA frame-based time-stretch for SSML <prosody rate=...>. */

#ifndef SPFY_DSP_TIME_STRETCH_H
#define SPFY_DSP_TIME_STRETCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Out-of-place WSOLA time-stretch. */
int spfy_time_stretch_block(const int16_t *in, size_t n_in,
                            int16_t **out, size_t *out_n,
                            float factor, int sample_rate);

#ifdef __cplusplus
}
#endif

#endif
