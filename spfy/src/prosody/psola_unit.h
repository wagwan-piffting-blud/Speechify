/* Per-unit, pre-join TD and LP-PSOLA pitch warp driven by supplied pitch marks. LP is the preferred method. */

#ifndef SPFY_PROSODY_PSOLA_UNIT_H
#define SPFY_PROSODY_PSOLA_UNIT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Warp one unit in place. */
int spfy_prosody_warp_unit(int16_t *buf, size_t n,
                           const int16_t *periods, int n_marks,
                           const float *target_hz,
                           float max_semitones, int sample_rate);

/* Limit a pitch ratio to +-max_st semitones. */
double spfy_prosody_limit_ratio(double r, double max_st, int soft);

/* The same limiter with SEPARATE ceilings for upward and downward shifts. */
double spfy_prosody_limit_ratio2(double r, double max_up_st, double max_dn_st,
                                 int soft);

/* Smoothly suppress shifts smaller than dz_st, leaving real accents alone. */
double spfy_prosody_deadzone(double r, double dz_st);

/* LP analysis -> residual -> LP synthesis, with NO grain movement at all. */
int spfy_prosody_lp_roundtrip(int16_t *buf, size_t n,
                              const int16_t *periods, int n_marks);

/* Magnitude response, in dB, of the sub-sample grain placer at delay
 * `frac`. */
void spfy_psola_frac_response(double frac, int n_freq, const double *freq_hz,
                              int sample_rate, double *out_db);

#ifdef __cplusplus
}
#endif

#endif
