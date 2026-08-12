
#include "reselect.h"

#include "../voice/unit_table.h"
#include "../../include/spfy/spfy.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MIN_PERIOD 20
#define MAX_PERIOD 200
#define MIN_MARKS  3

void spfy_reselect_defaults(spfy_reselect_params_t *p)
{
    if (!p) return;
    /* A substitution must beat the engine's pick by a clear margin, because
     * the engine's pick was chosen with join cost in mind and ours is not. */
    /* 0.8, not 1.5: the contour asks for at most ~3 st, so a 1.5 st margin
     * plus a join penalty made every substitution impossible (measured: 0
     * swaps). */
    p->min_gain_st  = 0.8f;
    p->same_rec_st  = 0.5f;
    p->cross_rec_st = 2.0f;
    /* 1, not 2: requiring BOTH context slots to match left 0 candidates on
     * a test phrase. */
    p->ctx_strict   = 1;
}

float spfy_reselect_unit_f0(const spfy_pmarks_t *m, uint32_t uid, uint32_t rate)
{
    const int16_t *per = NULL;
    int c = spfy_pmarks_get(m, uid, &per);
    if (c < MIN_MARKS) return 0.0f;
    int16_t tmp[256];
    int n = 0;
    for (int i = 0; i < c && n < (int)(sizeof tmp / sizeof *tmp); ++i)
        if (per[i] >= MIN_PERIOD && per[i] <= MAX_PERIOD) tmp[n++] = per[i];
    if (n < MIN_MARKS) return 0.0f;
    for (int i = 1; i < n; ++i) {
        int16_t k = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j] > k) { tmp[j + 1] = tmp[j]; --j; }
        tmp[j + 1] = k;
    }
    float med = (float)tmp[n / 2];
    return med > 0.0f ? (float)rate / med : 0.0f;
}

int spfy_reselect_build(spfy_reselect_t *r, const void *units_v,
                        const spfy_pmarks_t *marks)
{
    const spfy_unit_table_t *ut = (const spfy_unit_table_t *)units_v;
    if (!r || !ut || !marks || !marks->index) return SPFY_E_INVAL;
    memset(r, 0, sizeof *r);

    uint32_t n = ut->n_units;
    if (marks->n_units < n) n = marks->n_units;
    if (n == 0) return SPFY_E_FORMAT;

    r->n_units  = n;
    r->f0       = (float *)calloc(n, sizeof *r->f0);
    r->cls      = (uint16_t *)calloc(n, sizeof *r->cls);
    r->file_idx = (uint16_t *)calloc(n, sizeof *r->file_idx);
    r->ctx      = (uint16_t *)calloc(n, sizeof *r->ctx);
    if (!r->f0 || !r->cls || !r->file_idx || !r->ctx) {
        spfy_reselect_free(r); return SPFY_E_NOMEM;
    }

    r->n_cls = 512u;
    r->b_off = (uint32_t *)calloc((size_t)r->n_cls + 1u, sizeof *r->b_off);
    if (!r->b_off) { spfy_reselect_free(r); return SPFY_E_NOMEM; }

    uint32_t usable = 0;
    for (uint32_t uid = 0; uid < n; ++uid) {
        spfy_unit_record_t rec;
        if (spfy_unit_record_get(ut, uid, &rec) != SPFY_OK) continue;
        float f = spfy_reselect_unit_f0(marks, uid, marks->rate);
        if (f <= 0.0f) continue;
        uint16_t c = (uint16_t)(((uint16_t)rec.phone_center << 1)
                                | (rec.is_first_half ? 1u : 0u));
        r->f0[uid]       = f;
        r->cls[uid]      = c;
        r->file_idx[uid] = rec.file_idx;
        r->ctx[uid] = (uint16_t)(((uint16_t)rec.phone_ctx[0] << 8)
                                 | (uint16_t)rec.phone_ctx[1]);
        r->b_off[c + 1u] += 1u;
        ++usable;
    }
    if (usable == 0) { spfy_reselect_free(r); return SPFY_E_FORMAT; }

    for (uint32_t c = 0; c < r->n_cls; ++c) r->b_off[c + 1u] += r->b_off[c];
    r->bucket = (uint32_t *)malloc((size_t)usable * sizeof *r->bucket);
    if (!r->bucket) { spfy_reselect_free(r); return SPFY_E_NOMEM; }

    uint32_t *fill = (uint32_t *)calloc(r->n_cls, sizeof *fill);
    if (!fill) { spfy_reselect_free(r); return SPFY_E_NOMEM; }
    for (uint32_t uid = 0; uid < n; ++uid) {
        if (r->f0[uid] <= 0.0f) continue;
        uint16_t c = r->cls[uid];
        r->bucket[r->b_off[c] + fill[c]++] = uid;
    }
    free(fill);

    for (uint32_t c = 0; c < r->n_cls; ++c) {
        uint32_t lo = r->b_off[c], hi = r->b_off[c + 1u];
        for (uint32_t i = lo + 1u; i < hi; ++i) {
            uint32_t k = r->bucket[i];
            float kf = r->f0[k];
            uint32_t j = i;
            while (j > lo && r->f0[r->bucket[j - 1u]] > kf) {
                r->bucket[j] = r->bucket[j - 1u];
                --j;
            }
            r->bucket[j] = k;
        }
    }
    return SPFY_OK;
}

void spfy_reselect_free(spfy_reselect_t *r)
{
    if (!r) return;
    free(r->f0); free(r->cls); free(r->file_idx); free(r->ctx);
    free(r->bucket); free(r->b_off);
    memset(r, 0, sizeof *r);
}

static double band_deficit(double hz, double lo, double hi)
{
    if (hz <= 0.0) return 0.0;
    if (lo > 0.0 && hz < lo) return 12.0 * log2(lo / hz);
    if (hi > 0.0 && hz > hi) return 12.0 * log2(hz / hi);
    return 0.0;
}

uint32_t spfy_reselect_find_band(const spfy_reselect_t *r,
                                 const spfy_reselect_params_t *p,
                                 uint32_t uid, double scale,
                                 float lo_hz, float hi_hz,
                                 uint16_t neighbour_file, uint32_t prev_uid)
{
    if (!r || !p || uid >= r->n_units || scale <= 0.0) return uid;
    if (lo_hz <= 0.0f && hi_hz <= 0.0f) return uid;
    float f_orig = r->f0[uid];
    if (f_orig <= 0.0f) return uid;

    double d0 = band_deficit((double)f_orig * scale,
                             (double)lo_hz, (double)hi_hz);
    /* Already inside the band: there is nothing to buy, and swapping a unit
     * that is fine is pure join risk. */
    if (d0 <= 0.0) return uid;

    uint16_t c = r->cls[uid];
    uint32_t lo = r->b_off[c], hi = r->b_off[c + 1u];
    if (hi - lo < 2u) return uid;

    uint32_t best_uid = uid;
    double need = d0 - (double)p->min_gain_st;

    for (uint32_t i = lo; i < hi; ++i) {
        uint32_t cand = r->bucket[i];
        if (cand == uid) continue;
        if (p->ctx_strict >= 2) {
            if (r->ctx[cand] != r->ctx[uid]) continue;
        } else if (p->ctx_strict == 1) {
            if ((r->ctx[cand] >> 8) != (r->ctx[uid] >> 8)) continue;
        }
        double d = band_deficit((double)r->f0[cand] * scale,
                                (double)lo_hz, (double)hi_hz);
        if (d >= need) continue;
        double pen;
        if (prev_uid != UINT32_MAX && cand == prev_uid + 1u)
            pen = 0.0;
        else if (neighbour_file != UINT16_MAX
                 && r->file_idx[cand] == neighbour_file)
            pen = (double)p->same_rec_st;
        else
            pen = (double)p->cross_rec_st;
        double sc = d + pen;
        if (sc < need) { need = sc; best_uid = cand; }
    }
    return best_uid;
}

uint32_t spfy_reselect_find(const spfy_reselect_t *r,
                            const spfy_reselect_params_t *p,
                            uint32_t uid, float target_hz,
                            uint16_t neighbour_file, uint32_t prev_uid)
{
    if (!r || !p || uid >= r->n_units || target_hz <= 0.0f) return uid;
    float f_orig = r->f0[uid];
    if (f_orig <= 0.0f) return uid;

    uint16_t c = r->cls[uid];
    uint32_t lo = r->b_off[c], hi = r->b_off[c + 1u];
    if (hi - lo < 2u) return uid;

    /* Cost is distance-to-target in semitones plus a join penalty, so a
     * marginally better pitch never justifies leaving the neighbour's
     * recording. */
    double best = fabs(12.0 * log2((double)f_orig / (double)target_hz));
    uint32_t best_uid = uid;
    double need = best - (double)p->min_gain_st;

    for (uint32_t i = lo; i < hi; ++i) {
        uint32_t cand = r->bucket[i];
        if (cand == uid) continue;
        /* Coarticulation gate. */
        if (p->ctx_strict >= 2) {
            if (r->ctx[cand] != r->ctx[uid]) continue;
        } else if (p->ctx_strict == 1) {
            if ((r->ctx[cand] >> 8) != (r->ctx[uid] >> 8)) continue;
        }
        double d = fabs(12.0 * log2((double)r->f0[cand] / (double)target_hz));
        if (d >= need) continue;
        double pen;
        if (prev_uid != UINT32_MAX && cand == prev_uid + 1u)
            pen = 0.0;
        else if (neighbour_file != UINT16_MAX
                 && r->file_idx[cand] == neighbour_file)
            pen = (double)p->same_rec_st;
        else
            pen = (double)p->cross_rec_st;
        double sc = d + pen;
        if (sc < need) { need = sc; best_uid = cand; }
    }
    (void)best;
    return best_uid;
}
