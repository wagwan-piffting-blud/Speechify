#ifndef SPFY_VB_TRACK_H
#define SPFY_VB_TRACK_H

#include <stddef.h>
#include <stdint.h>

#include "edge_frames.h"
#include "../voice/voice.h"

/* Whole-unit cepstral tracks, and Festival's unit-to-unit acoustic distance.
 *
 * This is `ccos`'s kernel. Festival builds a per-unit-type N x N distance
 * matrix with ac_unit_distance (clunits/acost.cc) and hands it to `wagon` as
 * the impurity measure; Speechify kept the same quantity but stored it as a
 * TABLE indexed by (hp_class, context slot, target label, candidate label)
 * instead of growing a tree over it.
 *
 * What is established about the ccos axes, and how (none of it is assumed):
 *
 *   (i,j)      the stored triangle runs i = 1..n-1, j = 0..i-1 -- ccos.c:84.
 *              A vowel/consonant control over the chunk's own label list is
 *              FLAT under the other order and gives VV < CC < VC < silence
 *              under this one, on tom and jill alike.
 *   hp_class   label + (half ? n_labels : 0), NOT label*2 + half --
 *              voice_runtime.c:63. With the wrong form a permutation control
 *              scored HIGHER than the correct pairing, i.e. the axis carried
 *              no information at all.
 *   slot       (pp2, pp1, pn1, pn2), confirmed independently by the per-slot
 *              scale table: a LEFT half weights pp1 at 1.0 and a RIGHT half
 *              weights pn1 at 1.0, each most sensitive to the neighbour it
 *              actually touches.
 *
 * With those axes, context COUNTS are refuted (r = 0.0004 against a permuted
 * control of -0.0003) and the duration ratio is real but small (0.081 vs
 * 0.027) -- which is what ac_unit_distance predicts, since its duration term
 * sits beside a spectral term that should dominate. */

typedef struct {
    float   *f;          /* n_frames * n_cep, row-major */
    uint32_t n_frames;
    float    dur_ms;     /* the unit's dur_like; Festival's track end() */
} spfy_vb_track;

typedef struct {
    spfy_vb_track *t;    /* parallel to the requested uid list */
    size_t         n;
    uint32_t       n_cep;
    size_t         n_missing;
} spfy_vb_tracks;

/* Frames at `shift_ms` across each unit's span. Only the requested uids are
 * built: a whole vendor inventory of tracks would be ~90 MB, and the estimate
 * needs at most a bounded sample per context group. */
int  spfy_vb_tracks_build(const spfy_vin_t *vin, const spfy_vdb_t *vdb,
                          uint32_t sample_rate, const spfy_vb_cfg_t *cfg,
                          float shift_ms,
                          const uint32_t *uids, size_t n_uids,
                          spfy_vb_tracks *out);
void spfy_vb_tracks_free(spfy_vb_tracks *t);

/* Festival clunits/acost.cc ac_unit_distance, ported.
 *
 *   swap so `a` is the shorter;  incr = a.end / b.end
 *   for each frame i of b, walk j through a by the warp:
 *       cost = sum_k w[k] * (b[i][k] - a[j][k])^2
 *   score = sum_cost / n_frames_b  +  (b.end / a.end) * dur_pen_w
 *
 * ⚠ The f0 penalty term of the original reads frame TIMESTAMPS, which a
 * fixed-shift track does not carry -- it is identically zero here, and that
 * is why there is no f0 weight argument. Festival's own default for
 * f0_pen_weight is 0.0. */
float spfy_vb_ac_unit_distance(const spfy_vb_track *a, const spfy_vb_track *b,
                               const float *w, uint32_t n_w, float dur_pen_w);

#endif
