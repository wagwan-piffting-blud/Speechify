/* Target F0 contour built from the front end's own ToBI analysis. */

#ifndef SPFY_PROSODY_CONTOUR_H
#define SPFY_PROSODY_CONTOUR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float base_hz;
    float accent_st;
    float downstep;
    float nuclear;      /* height multiplier for the LAST accent of the phrase, applied INSTEAD of
 * downstep^k. */
    float downstep_floor; /* lower bound on downstep^i. */
    float decl_st;

    /* Declination expressed as SEMITONES PER SECOND. */
    float decl_rate_st_s;

    /* Bound on the span decl_rate_st_s may produce, semitones, absolute
     * value. */
    float decl_max_st;
    float fall_st;
    float width_ms;
    float align_ms;     /* shift every accent centre; NEGATIVE = earlier. */
    /* --- inter-accent shape ------------------------------------------ The
     * model above is a SUM OF POSITIVE BUMPS, so it can raise pitch at each
     * accent and nothing else. */
    float valley_st;    /* depth of the low target between grouped accents. */
    float group_ms;
    float group_damp;   /* height multiplier for non-head group members. */
    int   group_head;   /* which member carries the excursion: 0 = the LAST (matches
 * radar|indicated), 1 = the FIRST (matches Lady|Lake). */
    /* How much of the unit's OWN F0 to replace, 0..1. */
    float absolute;
    /* Ramp applied at each per-word offset boundary, ms. */
    float word_ramp_ms;
    float max_st;
    /* Constant semitone offset applied AFTER zero_mean, 0 = off. */
    /* Exponent on the declination ramp: st = decl_st * u^decl_shape.
     * 1.0 = the straight line this has always been.
     *
     * ⚠ THIS IS THE DEGREE OF FREEDOM zero_mean AND level_st DO NOT HAVE.
     * Both of those add a CONSTANT to the whole contour -- proven redundant by
     * measurement: decl -3 with zero_mean 1.0 and decl -3 with zero_mean 0.0
     * plus level_st +1.5 render identically, max |diff| 0.0000 st over 989
     * marks, while the control pair differs by 1.4981 st = decl/2 as predicted.
     *
     * So for a straight ramp the onset is FORCED to sit decl/2 above the
     * phrase mean, and that gap is the onset lift that makes an accented first
     * word overshoot. No setting of zero_mean or level_st can close it,
     * because closing it means moving the onset RELATIVE to the mean.
     *
     * Curvature can: with st = decl*u^k the mean sits decl/(k+1) below the
     * onset, so k = 2 halves the lift and k = 5 cuts it to a third, while the
     * phrase still ends decl semitones down. Physically it back-loads the fall
     * -- the phrase holds its pitch and drops late -- which is a real
     * intonation pattern rather than a fudge.
     *
     * Values below 1 front-load instead, which INCREASES the lift; that is
     * left reachable because a short standalone sentence wanted more onset,
     * not less. */
    float decl_shape;
    float level_st;
    /* FRACTION of the contour's own mean to subtract, 0..1. 1 = the stage
     * REDISTRIBUTES pitch instead of adding it; without it the accents pile on
     * top of the excursions the units already carry and the whole utterance
     * drifts sharp (measured: median 121.6 -> 125.9 Hz).
     *
     * ⚠ WHY THIS IS A FRACTION AND NOT A FLAG. Subtracting the full mean turns
     * a declination request into a LIFT of the phrase onset -- the ramp cannot
     * lower the average, so it raises the start instead. Measured over 4 items
     * at DECL_ST=-6: the applied shift runs +3.25 st at the phrase onset down
     * to -2.64 at its end. Tom's recordings already peak at onset, so the two
     * stack and an accented onset word overshoots.
     *
     * Turning it off removes the lift (+3.25 -> +0.47) but drops the phrase
     * median from 121.2 to 104.6 Hz, against Tom's natural 117.6, because a
     * ramp that only falls averages lower than one centred on zero. level_st
     * cannot separate those: a constant offset moves onset and tail together,
     * so restoring the mean restores the lift. They are one knob.
     *
     * By ear the right value is not the same everywhere -- 0 was preferred on
     * multi-phrase bulletin text and 1 on a short standalone sentence, where
     * the lift supplies a wanted high onset. Hence a dial rather than a flag.
     * "0" and "1" keep their old meaning exactly. */
    float zero_mean;
    /* The same fraction, for everything that is NOT the declination ramp: the
     * accents, the inter-accent valleys and the phrase-final fall.
     *
     * ⚠ WHY THIS IS SEPARATE. mean_st was computed over the WHOLE contour, so
     * one subtraction centred the ramp and the accents together. That dilutes
     * decl_shape badly: with accents at their 3.0 default, the knob's authority
     * over the phrase onset fell from the 1.0 st its own algebra predicts to
     * 0.4 st measured, because the accents were feeding the same mean it was
     * trying to move the ramp against.
     *
     * SPFY_PROSODY_ZEROMEAN sets BOTH, so a single value behaves exactly as
     * before. SPFY_PROSODY_ZEROMEAN_ACC then overrides the accent half. Equal
     * fractions reproduce the old single subtraction identically. */
    float zero_mean_acc;
} spfy_contour_params_t;

void spfy_contour_defaults(spfy_contour_params_t *p);
int  spfy_contour_env(spfy_contour_params_t *p);

typedef struct {
    spfy_contour_params_t p;
    double  total;
    int     n_acc;
    double *pos;
    double *height;
    int     n_val;
    double *vpos;
    double *vdepth;
    double *vwidth;
    /* Per-slot semitone offset from SSML/Balabolka <prosody pitch="Nst">,
     * i.e. */
    int     n_seg;
    double *seg_end;
    float  *seg_off;
    double  seg_ramp;
    int     have_fall;
    double  last_pos;
    double  mean_st;
    double  mean_ramp;   /* the declination ramp's share of mean_st, measured by re-sampling the
 * contour with decl_st zeroed. */
    int     sample_rate;
} spfy_contour_t;

/* Build from a halfphone timeline. */
int  spfy_contour_build(spfy_contour_t *c,
                        const spfy_contour_params_t *p,
                        const uint32_t *hp_dur, int n_hp,
                        const uint8_t *hp_accented,
                        const int8_t *hp_acctype,
                        /* Per-half-phone syllable id; any value that
                         * differs between adjacent syllables will do. */
                        const uint32_t *hp_syl,
                        /* 1 where the half-phone is a syllable NUCLEUS -- a
                         * vowel, or one of the syllabic consonants
                         * (el/en/er). */
                        const uint8_t *hp_nucleus,
                        const int8_t *hp_btone,
                        /* Per-slot semitone offset (SSML <prosody pitch>). */
                        const int8_t *hp_pitch_st,
                        int sample_rate);

void spfy_contour_free(spfy_contour_t *c);

/* Contour value in SEMITONES at an absolute output sample position. */
float spfy_contour_st_at(const spfy_contour_t *c, double t_samples);

/* Target F0 in Hz at an absolute output sample position. */
float spfy_contour_at(const spfy_contour_t *c, double t_samples,
                      float natural_hz);

#ifdef __cplusplus
}
#endif

#endif
