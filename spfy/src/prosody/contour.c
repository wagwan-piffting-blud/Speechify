
#include "contour.h"
#include "env.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Measured from Tom, not guessed -- f0_range_compare.py over 333k voiced
 * frames of the VDB: accent height (p95 - median) = +5.06 st, final
 * lowering (tail - median) = -2.57 st, median F0 110.7 Hz. */
void spfy_contour_defaults(spfy_contour_params_t *p)
{
    if (!p) return;
    /* 0 = RELATIVE: the contour rides on each unit's natural F0, so zeroed
     * parameters are exactly identity. */
    p->base_hz   = 0.0f;
    /* NOT the measured +5.06 st. */
    p->accent_st = 3.0f;
    /* 0.70 — restored 2026-08-05 BY EAR, over a measurement that preferred
     * 1.0. */
    p->downstep  = 0.70f;
    /* OFF (-1). */
    p->nuclear   = -1.0f;
    /* 0.30 -> late accents settle at 0.9 st with accent_st 3.0 instead of
     * decaying to zero. */
    p->downstep_floor = 0.30f;
    p->decl_st   = -2.0f;
    /* 0 = off, i.e. */
    p->decl_rate_st_s = 0.0f;
    p->decl_max_st    = 6.0f;
    /* 1.0 = the straight ramp this has always been; see the header for why
     * a straight ramp cannot separate the onset from the phrase mean. */
    p->decl_shape = 1.0f;
    p->zero_mean_acc = 1.0f;
    p->fall_st   = 2.57f;
    /* 90, not 120: a 120 ms bump is wider than a short accented syllable
     * and bleeds onto the next one. */
    p->width_ms  = 90.0f;
    p->align_ms  = -25.0f;
    /* Inter-accent shape: OFF by default. */
    p->valley_st  = 0.0f;
    p->group_ms   = 250.0f;
    p->group_damp = 1.0f;
    p->group_head = 0;
    p->absolute   = 0.0f;
    p->word_ramp_ms = 40.0f;
    p->max_st    = 4.0f;
    p->level_st  = 0.0f;
    p->zero_mean = 1.0f;
}

static void envf(const char *name, float *dst, float lo, float hi)
{
    const char *s = spfy_env(name);
    if (!s || !*s) return;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s) return;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    *dst = (float)v;
}

int spfy_contour_env(spfy_contour_params_t *p)
{
    if (!p) return 0;
    spfy_contour_defaults(p);
    const char *on = spfy_env("SPFY_PROSODY_STAGE");
    if (!on || !*on || *on == '0') return 0;
    envf("SPFY_PROSODY_BASE_HZ",   &p->base_hz,    0.0f, 400.0f);
    envf("SPFY_PROSODY_ACCENT_ST", &p->accent_st,  0.0f,  12.0f);
    envf("SPFY_PROSODY_DOWNSTEP",  &p->downstep,   0.0f,   1.5f);
    envf("SPFY_PROSODY_NUCLEAR",   &p->nuclear,   -1.0f,   1.5f);
    envf("SPFY_PROSODY_DOWNSTEP_FLOOR", &p->downstep_floor, 0.0f, 1.0f);
    envf("SPFY_PROSODY_DECL_ST",   &p->decl_st,  -12.0f,  12.0f);
    envf("SPFY_PROSODY_DECL_RATE_ST_S", &p->decl_rate_st_s, -6.0f, 6.0f);
    envf("SPFY_PROSODY_DECL_MAX_ST",    &p->decl_max_st,     0.0f, 24.0f);
    envf("SPFY_PROSODY_FALL_ST",   &p->fall_st,    0.0f,  12.0f);
    envf("SPFY_PROSODY_WIDTH_MS",  &p->width_ms,  20.0f, 600.0f);
    envf("SPFY_PROSODY_WORD_RAMP_MS", &p->word_ramp_ms, 0.0f, 300.0f);
    envf("SPFY_PROSODY_ALIGN_MS",  &p->align_ms, -300.0f, 300.0f);
    envf("SPFY_PROSODY_MAX_ST",    &p->max_st,     0.0f,  12.0f);
    envf("SPFY_PROSODY_DECL_SHAPE", &p->decl_shape, 0.2f,  8.0f);
    envf("SPFY_PROSODY_LEVEL_ST",  &p->level_st,  -12.0f,  12.0f);
    /* base_hz > 0 has always meant "impose an absolute contour", so keep
     * that exactly when ABSOLUTE is not named explicitly. */
    if (p->base_hz > 0.0f && !spfy_env("SPFY_PROSODY_ABSOLUTE"))
        p->absolute = 1.0f;
    envf("SPFY_PROSODY_ABSOLUTE",   &p->absolute,   0.0f,   1.0f);
    envf("SPFY_PROSODY_VALLEY_ST",  &p->valley_st,  0.0f,  12.0f);
    envf("SPFY_PROSODY_GROUP_MS",   &p->group_ms,  50.0f, 1500.0f);
    envf("SPFY_PROSODY_GROUP_DAMP", &p->group_damp, 0.0f,   1.0f);
    {
        const char *h = spfy_env("SPFY_PROSODY_GROUP_HEAD");
        if (h && *h) p->group_head = atoi(h) ? 1 : 0;
    }
    {
        const char *z = spfy_env("SPFY_PROSODY_ZEROMEAN");
        if (z && *z) {
            double zv = strtod(z, NULL);
            if (zv < 0.0) zv = 0.0;
            if (zv > 1.0) zv = 1.0;
            p->zero_mean = (float)zv;
            p->zero_mean_acc = (float)zv;
        }
    }
    {
        const char *z = spfy_env("SPFY_PROSODY_ZEROMEAN_ACC");
        if (z && *z) {
            double zv = strtod(z, NULL);
            if (zv < 0.0) zv = 0.0;
            if (zv > 1.0) zv = 1.0;
            p->zero_mean_acc = (float)zv;
        }
    }
    return 1;
}

int spfy_contour_build(spfy_contour_t *c,
                       const spfy_contour_params_t *p,
                       const uint32_t *hp_dur, int n_hp,
                       const uint8_t *hp_accented,
                       const int8_t *hp_acctype,
                       const uint32_t *hp_syl,
                       const uint8_t *hp_nucleus,
                       const int8_t *hp_btone,
                       const int8_t *hp_pitch_st,
                       int sample_rate)
{
    if (!c || !p || !hp_dur || n_hp <= 0 || sample_rate <= 0) return -1;
    memset(c, 0, sizeof *c);
    c->p = *p;
    c->sample_rate = sample_rate;

    c->pos    = (double *)malloc((size_t)n_hp * sizeof *c->pos);
    c->height = (double *)malloc((size_t)n_hp * sizeof *c->height);
    if (!c->pos || !c->height) { spfy_contour_free(c); return -1; }

    /* Walk the halfphone timeline, collapsing each run of accented
     * halfphones (one syllable's worth) into a single centred accent. */
    double t = 0.0;
    int i = 0;
    while (i < n_hp) {
        double start = t;
        int accented = hp_accented && hp_accented[i];
        if (!accented) {
            t += (double)hp_dur[i];
            ++i;
            continue;
        }
        double bias = 0.0;
        int j = i;
        /* Nucleus span within this accent's run, in the same time units. */
        double nuc_a = -1.0, nuc_b = -1.0;
        while (j < n_hp && hp_accented[j]) {
            /* SPLIT AT SYLLABLE BOUNDARIES. */
            if (hp_syl && j > i && hp_syl[j] != hp_syl[j - 1]) break;
            if (hp_acctype) bias += (double)hp_acctype[j];
            if (hp_nucleus && hp_nucleus[j]) {
                if (nuc_a < 0.0) nuc_a = t;
                nuc_b = t + (double)hp_dur[j];
            }
            t += (double)hp_dur[j];
            ++j;
        }
        bias /= (double)(j - i);
        /* CENTRE ON THE NUCLEUS, not on the middle of the syllable. */
        c->pos[c->n_acc]    = (nuc_a >= 0.0) ? 0.5 * (nuc_a + nuc_b)
                                             : 0.5 * (start + t);
        c->height[c->n_acc] = bias;
        ++c->n_acc;
        i = j;
    }
    c->total = t;
    if (c->total <= 0.0) { spfy_contour_free(c); return -1; }

    /* DECLINATION AS A RATE, not as a span. */
    if (c->p.decl_rate_st_s != 0.0f) {
        double secs = c->total / (double)(sample_rate > 0 ? sample_rate : 8000);
        double span = (double)c->p.decl_rate_st_s * secs;
        double cap = fabs((double)c->p.decl_max_st);
        if (cap > 0.0) {
            if (span >  cap) span =  cap;
            if (span < -cap) span = -cap;
        }
        c->p.decl_st = (float)span;
    }

    /* Downstep: each successive accent is a fixed fraction of the last, but
     * floored — see spfy_contour_params_t.downstep_floor. */
    for (int k = 0; k < c->n_acc; ++k) {
        double d = pow((double)p->downstep, k);
        if (d < (double)p->downstep_floor) d = (double)p->downstep_floor;
        /* The nuclear accent is exempt: it takes `nuclear` directly rather
         * than the decayed value. */
        if (k == c->n_acc - 1 && c->n_acc > 1 && (double)p->nuclear >= 0.0)
            d = (double)p->nuclear;
        c->height[k] += (double)p->accent_st * d;
    }

    /* Peak alignment. */
    {
        double shift = (double)p->align_ms * 0.001 * (double)sample_rate;
        for (int k = 0; k < c->n_acc; ++k) {
            c->pos[k] += shift;
            if (c->pos[k] < 0.0) c->pos[k] = 0.0;
            if (c->pos[k] > c->total) c->pos[k] = c->total;
        }
    }

    /* Accent GROUPING: consecutive accents closer than group_ms form one
     * gesture. */
    if (c->n_acc > 1
        && ((double)p->valley_st > 0.0 || (double)p->group_damp < 1.0)) {
        double gap = (double)p->group_ms * 0.001 * (double)sample_rate;
        c->vpos   = (double *)malloc((size_t)c->n_acc * sizeof *c->vpos);
        c->vdepth = (double *)malloc((size_t)c->n_acc * sizeof *c->vdepth);
        c->vwidth = (double *)malloc((size_t)c->n_acc * sizeof *c->vwidth);
        if (!c->vpos || !c->vdepth || !c->vwidth) {
            spfy_contour_free(c);
            return -1;
        }
        int i0 = 0;
        while (i0 < c->n_acc) {
            int i1 = i0;
            while (i1 + 1 < c->n_acc && (c->pos[i1 + 1] - c->pos[i1]) <= gap)
                ++i1;
            if (i1 > i0) {
                int head = p->group_head ? i0 : i1;
                for (int k = i0; k <= i1; ++k) {
                    if (k != head) c->height[k] *= (double)p->group_damp;
                    if (k < i1 && (double)p->valley_st > 0.0) {
                        /* Midway between the pair. */
                        c->vpos[c->n_val]  = 0.5 * (c->pos[k] + c->pos[k + 1]);
                        c->vdepth[c->n_val] = (double)p->valley_st;
                        /* Scale the valley to the spacing it has to span; a
                         * fixed 90 ms accent width is far too narrow to
                         * carry a ~900 ms two-word gesture. */
                        {
                            double half = 0.5 * (c->pos[k + 1] - c->pos[k]);
                            double wmin = (double)p->width_ms * 0.001
                                        * (double)sample_rate;
                            c->vwidth[c->n_val] = half > wmin ? half : wmin;
                        }
                        ++c->n_val;
                    }
                }
            }
            i0 = i1 + 1;
        }
    }

    c->last_pos = c->n_acc ? c->pos[c->n_acc - 1] : 0.0;

    if (hp_btone) {
        for (int k = 0; k < n_hp; ++k)
            if (hp_btone[k]) { c->have_fall = 1; break; }
    }

    /* Zero-mean the contour so the stage REDISTRIBUTES pitch rather than
     * adding it. */
    c->mean_st = 0.0;
    c->mean_ramp = 0.0;
    if (c->p.zero_mean > 0.0f || c->p.zero_mean_acc > 0.0f) {
        const int NS = 256;
        double acc = 0.0, acc_rest = 0.0;
        float saved = c->p.zero_mean;
        float saved_acc = c->p.zero_mean_acc;
        /* level_st is excluded as well as zero_mean. */
        float saved_level = c->p.level_st;
        float saved_decl  = c->p.decl_st;
        c->p.zero_mean = 0.0f;
        c->p.zero_mean_acc = 0.0f;
        c->p.level_st  = 0.0f;
        for (int k = 0; k < NS; ++k)
            acc += (double)spfy_contour_st_at(c, c->total * k / (NS - 1));
        /* SECOND PASS with the ramp removed. */
        c->p.decl_st = 0.0f;
        for (int k = 0; k < NS; ++k)
            acc_rest += (double)spfy_contour_st_at(c, c->total * k / (NS - 1));
        c->p.decl_st = saved_decl;
        c->p.zero_mean = saved;
        c->p.zero_mean_acc = saved_acc;
        c->p.level_st  = saved_level;
        c->mean_ramp = (acc - acc_rest) / NS;
        c->mean_st = acc / NS;
    }

    /* Per-word offsets are built AFTER mean_st on purpose. */
    if (hp_pitch_st) {
        int any = 0;
        for (int k = 0; k < n_hp; ++k)
            if (hp_pitch_st[k]) { any = 1; break; }
        if (any) {
            c->seg_end = (double *)malloc((size_t)n_hp * sizeof *c->seg_end);
            c->seg_off = (float *)malloc((size_t)n_hp * sizeof *c->seg_off);
            if (!c->seg_end || !c->seg_off) { spfy_contour_free(c); return -1; }
            double acc_t = 0.0;
            for (int k = 0; k < n_hp; ++k) {
                acc_t += (double)hp_dur[k];
                c->seg_end[k] = acc_t;
                c->seg_off[k] = (float)hp_pitch_st[k];
            }
            c->n_seg = n_hp;
            c->seg_ramp = (double)p->word_ramp_ms * 0.001 * (double)sample_rate;
        }
    }
    return 0;
}

/* Per-word semitone offset at t, raised-cosine blended across boundaries so
 * a multi-semitone step does not click. */
static double seg_offset_at(const spfy_contour_t *c, double t)
{
    if (!c->seg_off || c->n_seg <= 0) return 0.0;
    int lo = 0, hi = c->n_seg - 1;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (t < c->seg_end[mid]) hi = mid; else lo = mid + 1;
    }
    double cur   = (double)c->seg_off[lo];
    double start = (lo > 0) ? c->seg_end[lo - 1] : 0.0;
    double end   = c->seg_end[lo];
    double r     = c->seg_ramp;
    if (r <= 0.0) return cur;
    if (lo > 0 && (t - start) < r) {
        double prev = (double)c->seg_off[lo - 1];
        if (prev != cur) {
            double u = 0.5 - 0.5 * cos(M_PI * (t - start) / r);
            return prev + (cur - prev) * u;
        }
    } else if (lo + 1 < c->n_seg && (end - t) < r) {
        double nxt = (double)c->seg_off[lo + 1];
        if (nxt != cur) {
            double u = 0.5 - 0.5 * cos(M_PI * (end - t) / r);
            return nxt + (cur - nxt) * u;
        }
    }
    return cur;
}

void spfy_contour_free(spfy_contour_t *c)
{
    if (!c) return;
    free(c->pos);
    free(c->height);
    free(c->vpos);
    free(c->vdepth);
    free(c->vwidth);
    free(c->seg_end);
    free(c->seg_off);
    memset(c, 0, sizeof *c);
}

float spfy_contour_st_at(const spfy_contour_t *c, double t)
{
    if (!c || c->total <= 0.0) return 0.0f;
    if (t < 0.0) t = 0.0;
    if (t > c->total) t = c->total;

    double u = t / c->total;
    double st = (double)c->p.decl_st
              * (c->p.decl_shape == 1.0f ? u
                                         : pow(u, (double)c->p.decl_shape));

    double w = (double)c->p.width_ms * 0.001 * (double)c->sample_rate;
    if (w < 1.0) w = 1.0;
    for (int k = 0; k < c->n_acc; ++k) {
        double d = (t - c->pos[k]) / w;
        if (d > 6.0 || d < -6.0) continue;
        st += c->height[k] * exp(-(d * d));
    }

    /* Low targets between grouped accents. */
    for (int k = 0; k < c->n_val; ++k) {
        double d = (t - c->vpos[k]) / c->vwidth[k];
        if (d > 6.0 || d < -6.0) continue;
        st -= c->vdepth[k] * exp(-(d * d));
    }

    if (c->have_fall && c->n_acc && c->last_pos < c->total) {
        if (t > c->last_pos) {
            double q = (t - c->last_pos) / (c->total - c->last_pos);
            st -= (double)c->p.fall_st * pow(q, 1.5);
        }
    }
    /* The ramp and everything else are centred SEPARATELY. */
    st -= (double)c->p.zero_mean * c->mean_ramp
        + (double)c->p.zero_mean_acc * (c->mean_st - c->mean_ramp);
    /* AFTER zero_mean, and deliberately outside the mean_st computation --
     * mean_st is evaluated with a temporary that has zero_mean off, so a
     * level offset applied earlier would be averaged straight back out. */
    st += (double)c->p.level_st;
    st += seg_offset_at(c, t);
    return (float)st;
}

float spfy_contour_at(const spfy_contour_t *c, double t, float natural_hz)
{
    if (!c) return 0.0f;
    double st = (double)spfy_contour_st_at(c, t);
    double a  = (double)c->p.absolute;
    double base;

    if (c->p.base_hz <= 0.0f) {
        base = (double)natural_hz;
    } else if (a >= 1.0) {
        base = (double)c->p.base_hz;
    } else if (a <= 0.0) {
        base = (double)natural_hz;
    } else {
        /* Partial imposition: pull the unit's own pitch toward base_hz by
         * a, in the log domain, so the unit's wobble survives scaled by
         * (1-a) instead of passing through whole. */
        if (natural_hz <= 0.0f) return 0.0f;
        base = pow(2.0, (1.0 - a) * log2((double)natural_hz)
                      + a * log2((double)c->p.base_hz));
    }
    if (base <= 0.0) return 0.0f;
    return (float)(base * pow(2.0, st / 12.0));
}
