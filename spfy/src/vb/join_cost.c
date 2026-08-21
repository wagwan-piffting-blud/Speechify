#include "join_cost.h"

#include "../../include/spfy/spfy.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static const float *frame_start(const spfy_jc_t *jc, uint32_t uid)
{
    return jc->frames + (size_t)uid * 2u * jc->dim;
}

static const float *frame_end(const spfy_jc_t *jc, uint32_t uid)
{
    return jc->frames + ((size_t)uid * 2u + 1u) * jc->dim;
}

int spfy_jc_derive_weights(spfy_jc_t *jc, float k0)
{
    if (!jc || !jc->frames || !jc->weights || jc->dim < 2u || !jc->n_units)
        return SPFY_E_INVAL;

    const uint32_t dim = jc->dim;
    const size_t   nf  = (size_t)jc->n_units * 2u;

    /* Long double accumulation: the vendor sums in x87 and the sums run to
     * millions of terms, so float accumulation drifts visibly. */
    long double *sum = (long double *)calloc(dim, sizeof *sum);
    long double *ssq = (long double *)calloc(dim, sizeof *ssq);
    if (!sum || !ssq) { free(sum); free(ssq); return SPFY_E_NOMEM; }

    size_t n_unvoiced = 0;
    for (size_t f = 0; f < nf; ++f) {
        const float *fr = jc->frames + f * dim;
        if (!(fr[SPFY_JC_DIM_F0] > jc->f0_gate)) ++n_unvoiced;
        for (uint32_t k = 0; k < dim; ++k) {
            long double v = (long double)fr[k];
            sum[k] += v;
            ssq[k] += v * v;
        }
    }

    /* dim 0 -- F0. Its count excludes unvoiced frames, exactly as
     * load_edge_frames does by counting the sentinel while accumulating. */
    {
        long double n  = (long double)(nf - n_unvoiced);
        long double ss = ssq[0] - (sum[0] * sum[0]) / (n > 0 ? n : 1);
        jc->weights[0] = (ss > 0) ? (float)(sqrtl(n / ss) * (long double)k0) : 0.0f;
    }

    /* dim 1 -- computed by the vendor and then explicitly disabled. */
    jc->weights[1] = 0.0f;

    /* dims >= 2 -- inverse sd, then shared out across the spectral block. */
    {
        long double n     = (long double)nf;
        long double scale = (long double)(2 * (int)dim - 4);
        for (uint32_t k = 2; k < dim; ++k) {
            long double ss = ssq[k] - (sum[k] * sum[k]) / n;
            jc->weights[k] = (ss > 0)
                           ? (float)(sqrtl(n / ss) / (scale > 0 ? scale : 1))
                           : 0.0f;
        }
    }

    free(sum);
    free(ssq);
    return SPFY_OK;
}

float spfy_jc_kernel(const spfy_jc_t *jc, const float *x, const float *y)
{
    const float *w = jc->weights;
    long double d = 0.0L;

    /* dim 0: absolute, and only when BOTH sides are voiced. An unvoiced
     * endpoint contributes nothing rather than a large penalty. */
    if (x[SPFY_JC_DIM_F0] > jc->f0_gate && y[SPFY_JC_DIM_F0] > jc->f0_gate)
        d = (long double)fabsf(x[SPFY_JC_DIM_F0] - y[SPFY_JC_DIM_F0]) * w[SPFY_JC_DIM_F0];

    for (uint32_t k = 1; k < jc->dim; ++k) {
        long double diff = (long double)x[k] - (long double)y[k];
        d += diff * diff * (long double)w[k];
    }
    return (float)d;
}

float spfy_jc_raw(const spfy_jc_t *jc, uint32_t uid_left, uint32_t uid_right)
{
    if (uid_left >= jc->n_units || uid_right >= jc->n_units) return 0.0f;

    const float *l_end   = frame_end(jc, uid_left);
    const float *r_start = frame_start(jc, uid_right);

    /* The seam itself, doubled (FADD ST0,ST0 at 08e8d3ea). */
    long double d = 2.0L * (long double)spfy_jc_kernel(jc, l_end, r_start);

    /* Left's end against what naturally precedes the right unit. */
    if (uid_right > 0)
        d += (long double)spfy_jc_kernel(jc, l_end, frame_end(jc, uid_right - 1u));

    /* Right's start against what naturally follows the left unit.
     * CONFIRMED 2026-08-16 by tracking ESP through FUN_08e8d3a0: at 08e8d3ec
     * [ESP+0x2c] resolves to the left uid, so [ESI+EDX*8+8] is rec[L+1].ptr0,
     * and EDI still holds start(R) from call 2 (Ghidra reports it as
     * unaff_EDI, i.e. provably preserved). The three terms are SUMMED with
     * the seam doubled -- there is no FDIV or FMUL in the function and the
     * epilogue only unwinds the stack. */
    if (uid_left + 1u < jc->n_units)
        d += (long double)spfy_jc_kernel(jc, r_start, frame_start(jc, uid_left + 1u));

    return (float)(d * (long double)(jc->raw_scale > 0.0f ? jc->raw_scale : 1.0f));
}

float spfy_jc_cached_value(const spfy_jc_t *jc,
                           uint32_t uid_left, uint32_t uid_right,
                           float join_weight, float join_offset)
{
    /* Natural continuations are stored as a hard zero, bypassing the affine
     * map entirely -- measured as an exact bijection with cost == 0. */
    if (uid_right == uid_left + 1u) return 0.0f;
    return spfy_jc_raw(jc, uid_left, uid_right) * join_weight + join_offset;
}

float spfy_jc_runtime_value(const spfy_jc_t *jc,
                            uint32_t uid_left, uint32_t uid_right,
                            int left_voiced, int right_voiced,
                            const spfy_jc_voicing *v)
{
    int idx = (left_voiced ? 1 : 0) + (right_voiced ? 1 : 0);
    return spfy_jc_raw(jc, uid_left, uid_right) * v->w[idx] + v->o[idx];
}
