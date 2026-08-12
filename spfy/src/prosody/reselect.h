/* F0-aware unit RE-SELECTION using mark-derived true F0. */

#ifndef SPFY_PROSODY_RESELECT_H
#define SPFY_PROSODY_RESELECT_H

#include <stddef.h>
#include <stdint.h>

#include "pmarks.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t  n_units;
    float    *f0;
    uint16_t *cls;
    uint16_t *file_idx;
    uint16_t *ctx;       /* immediate phone context, ctx[0]<<8 | ctx[1]. */
    uint32_t *bucket;
    uint32_t *b_off;
    uint32_t  n_cls;
} spfy_reselect_t;

typedef struct {
    float min_gain_st;
    float same_rec_st;
    float cross_rec_st;
    int   ctx_strict;    /* 0 = ignore phone context (swaps can read as the wrong sound), 1 =
 * immediate neighbour must match (default), 2 = both context slots must
 * match (very few candidates survive). */
} spfy_reselect_params_t;

void spfy_reselect_defaults(spfy_reselect_params_t *p);

/* One unit's TRUE F0: sample_rate / median period, over the marks that fall
 * inside the plausible band. */
float spfy_reselect_unit_f0(const spfy_pmarks_t *m, uint32_t uid,
                            uint32_t rate);

/* `units` is a spfy_unit_table_t*; kept void* so this header does not drag
 * the voice layer into every consumer. */
int  spfy_reselect_build(spfy_reselect_t *r, const void *units,
                         const spfy_pmarks_t *marks);

void spfy_reselect_free(spfy_reselect_t *r);

/* Best substitute for `uid` at `target_hz`, or `uid` itself when nothing
 * wins by the margin. */
uint32_t spfy_reselect_find(const spfy_reselect_t *r,
                            const spfy_reselect_params_t *p,
                            uint32_t uid, float target_hz,
                            uint16_t neighbour_file, uint32_t prev_uid);

/* BAND objective: keep the unit's LANDING inside [lo_hz, hi_hz] instead of
 * pulling it toward a point target. */
uint32_t spfy_reselect_find_band(const spfy_reselect_t *r,
                                 const spfy_reselect_params_t *p,
                                 uint32_t uid, double scale,
                                 float lo_hz, float hi_hz,
                                 uint16_t neighbour_file, uint32_t prev_uid);

#ifdef __cplusplus
}
#endif

#endif
