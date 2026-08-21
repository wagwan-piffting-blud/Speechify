#include "edge_frames.h"
#include "vb_track.h"

#include "../voice/feat_table.h"
#include "../voice/unit_table.h"
#include "../voice/vdb_lookup.h"
#include "../../include/spfy/spfy.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define BYTES_PER_MS 8u   /* the engine's own arithmetic: 8 kHz, 1 byte/sample */

void spfy_vb_cfg_default(spfy_vb_cfg_t *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->win     = 256u;          /* 32 ms at 8 kHz */
    cfg->n_cep   = 12u;
    cfg->n_mel   = 26u;
    cfg->preemph = 0.97f;
    cfg->anchor  = SPFY_VB_ANCHOR_EDGE;
    cfg->keep_c0 = 0;
    cfg->lifter  = 0;
    cfg->power   = 1;
    cfg->norm    = 1;
}

/* ---------------------------------------------------------------- FFT --- */

/* In-place radix-2 DIT over n points. */
static void fft(double *re, double *im, uint32_t n)
{
    for (uint32_t i = 1u, j = 0u; i < n; ++i) {
        uint32_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double t;
            t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (uint32_t len = 2u; len <= n; len <<= 1) {
        double ang = -2.0 * M_PI / (double)len;
        double wr = cos(ang), wi = sin(ang);
        for (uint32_t i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (uint32_t k = 0; k < len / 2u; ++k) {
                double ur = re[i + k],            ui = im[i + k];
                double vr = re[i + k + len / 2u], vi = im[i + k + len / 2u];
                double tr = vr * cr - vi * ci;
                double ti = vr * ci + vi * cr;
                re[i + k] = ur + tr;  im[i + k] = ui + ti;
                re[i + k + len / 2u] = ur - tr;
                im[i + k + len / 2u] = ui - ti;
                double ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

/* --------------------------------------------------------------- MFCC --- */

typedef struct {
    spfy_vb_cfg_t cfg;
    uint32_t nbin;
    double window[SPFY_VB_MAX_WIN];
    double melfb[SPFY_VB_MAX_MEL][SPFY_VB_MAX_WIN / 2u + 1u];
    double dct[SPFY_VB_MAX_CEP][SPFY_VB_MAX_MEL];
    double lift[SPFY_VB_MAX_CEP];
    double re[SPFY_VB_MAX_WIN], im[SPFY_VB_MAX_WIN];
    double power[SPFY_VB_MAX_WIN / 2u + 1u];
    double logmel[SPFY_VB_MAX_MEL];
} mfcc_ctx;

static double hz_to_mel(double f) { return 2595.0 * log10(1.0 + f / 700.0); }
static double mel_to_hz(double m) { return 700.0 * (pow(10.0, m / 2595.0) - 1.0); }

static void mfcc_init(mfcc_ctx *c, uint32_t sr)
{
    const uint32_t win   = c->cfg.win;
    const uint32_t n_mel = c->cfg.n_mel;
    const uint32_t n_cep = c->cfg.n_cep;

    for (uint32_t i = 0; i < win; ++i)
        c->window[i] = 0.54 - 0.46 * cos(2.0 * M_PI * (double)i / (double)(win - 1u));

    c->nbin = win / 2u + 1u;
    double lo = hz_to_mel(0.0), hi = hz_to_mel((double)sr / 2.0);
    double pts[SPFY_VB_MAX_MEL + 2u];
    for (uint32_t i = 0; i < n_mel + 2u; ++i) {
        double m = lo + (hi - lo) * (double)i / (double)(n_mel + 1u);
        pts[i] = mel_to_hz(m) * (double)win / (double)sr;
    }
    memset(c->melfb, 0, sizeof c->melfb);
    for (uint32_t m = 0; m < n_mel; ++m) {
        double a = pts[m], b = pts[m + 1u], d = pts[m + 2u];
        for (uint32_t k = 0; k < c->nbin; ++k) {
            double x = (double)k;
            if (x >= a && x <= b && b > a)      c->melfb[m][k] = (x - a) / (b - a);
            else if (x > b && x <= d && d > b)  c->melfb[m][k] = (d - x) / (d - b);
        }
    }

    /* Coefficient j of the kept block is DCT bin (j + base): base 0 keeps the
     * energy term, base 1 drops it. */
    const uint32_t base = c->cfg.keep_c0 ? 0u : 1u;
    for (uint32_t j = 0; j < n_cep; ++j) {
        for (uint32_t m = 0; m < n_mel; ++m)
            c->dct[j][m] = cos(M_PI * (double)(j + base) * ((double)m + 0.5)
                               / (double)n_mel);
        c->lift[j] = c->cfg.lifter > 0
                   ? 1.0 + 0.5 * (double)c->cfg.lifter
                           * sin(M_PI * (double)(j + base) / (double)c->cfg.lifter)
                   : 1.0;
    }
}

/* LPC by autocorrelation + Levinson-Durbin, then the standard a->cepstrum
 * recursion. c->re holds the windowed frame on entry. */
static void lpc_cepstrum(mfcc_ctx *c, float *out)
{
    const uint32_t win   = c->cfg.win;
    const uint32_t n_cep = c->cfg.n_cep;
    const uint32_t p     = (uint32_t)c->cfg.lpc;

    double r[SPFY_VB_MAX_CEP + 1], a[SPFY_VB_MAX_CEP + 1], tmp[SPFY_VB_MAX_CEP + 1];
    for (uint32_t k = 0; k <= p; ++k) {
        double s = 0.0;
        for (uint32_t i = k; i < win; ++i) s += c->re[i] * c->re[i - k];
        r[k] = s;
    }
    if (!(r[0] > 0.0)) {
        for (uint32_t j = 0; j < n_cep; ++j) out[j] = 0.0f;
        return;
    }
    r[0] *= 1.0001;                     /* ridge: keeps Levinson stable */

    double err = r[0];
    memset(a, 0, sizeof a);
    for (uint32_t i = 1; i <= p; ++i) {
        double acc = r[i];
        for (uint32_t j = 1; j < i; ++j) acc -= a[j] * r[i - j];
        double k_i = (err != 0.0) ? acc / err : 0.0;
        memcpy(tmp, a, sizeof tmp);
        a[i] = k_i;
        for (uint32_t j = 1; j < i; ++j) a[j] = tmp[j] - k_i * tmp[i - j];
        err *= (1.0 - k_i * k_i);
        if (err <= 0.0) { err = 1e-12; break; }
    }

    /* c[m] = a[m] + sum_{k<m} (k/m) c[k] a[m-k], with a[j>p] = 0. */
    double cep[SPFY_VB_MAX_CEP + 1];
    for (uint32_t m = 1; m <= n_cep; ++m) {
        double v = (m <= p) ? a[m] : 0.0;
        for (uint32_t k = 1; k < m; ++k) {
            uint32_t d = m - k;
            if (d <= p) v += ((double)k / (double)m) * cep[k] * a[d];
        }
        cep[m] = v;
        out[m - 1] = (float)v;
    }
}

/* Pre-emphasise `alen` samples, apply a Hamming of THAT length, and centre the
 * result in the cfg.win FFT buffer with zeros either side. alen == win
 * reproduces the fixed-window path exactly. */
static void fill_frame(mfcc_ctx *c, const int16_t *pcm, uint32_t alen)
{
    const uint32_t win = c->cfg.win;
    const double   pre = (double)c->cfg.preemph;

    for (uint32_t i = 0; i < win; ++i) { c->re[i] = 0.0; c->im[i] = 0.0; }
    if (alen > win) alen = win;
    const uint32_t pad = (win - alen) / 2u;

    double prev = 0.0;
    for (uint32_t i = 0; i < alen; ++i) {
        double s = (double)pcm[i];
        double p = s - pre * prev;
        prev = s;
        double w = (alen > 1u)
                 ? 0.54 - 0.46 * cos(2.0 * M_PI * (double)i / (double)(alen - 1u))
                 : 1.0;
        c->re[pad + i] = p * w;
    }
}

/* pcm holds `alen` samples. Writes cfg.n_cep coefficients. */
static void mfcc_frame_n(mfcc_ctx *c, const int16_t *pcm, uint32_t alen, float *out)
{
    const uint32_t win   = c->cfg.win;
    const uint32_t n_mel = c->cfg.n_mel;
    const uint32_t n_cep = c->cfg.n_cep;

    fill_frame(c, pcm, alen);
    if (c->cfg.lpc > 0) { lpc_cepstrum(c, out); return; }
    fft(c->re, c->im, win);

    for (uint32_t k = 0; k < c->nbin; ++k) {
        double p2 = c->re[k] * c->re[k] + c->im[k] * c->im[k];
        c->power[k] = c->cfg.power ? p2 / (double)win : sqrt(p2);
    }

    for (uint32_t m = 0; m < n_mel; ++m) {
        double e = 0.0;
        for (uint32_t k = 0; k < c->nbin; ++k) e += c->melfb[m][k] * c->power[k];
        c->logmel[m] = log(e > 1e-10 ? e : 1e-10);
    }
    if (c->cfg.logmel) {
        for (uint32_t j = 0; j < n_cep; ++j) out[j] = (float)c->logmel[j];
        return;
    }
    for (uint32_t j = 0; j < n_cep; ++j) {
        double v = 0.0;
        for (uint32_t m = 0; m < n_mel; ++m) v += c->dct[j][m] * c->logmel[m];
        out[j] = (float)(v * c->lift[j]);
    }
}

/* --------------------------------------------------------------- build -- */

/* ------------------------------------------------- pitch-synchronous --- */

#define PS_CTX   512u   /* samples decoded around a boundary for pitch analysis */
#define PS_LMIN   20u   /* 400 Hz at 8 kHz */
#define PS_LMAX  160u   /*  50 Hz at 8 kHz */
#define PS_VOICED 0.30  /* normalised autocorrelation peak to call it voiced */

/* Local pitch period by normalised autocorrelation over `n` samples.
 * Returns the period; *voiced says whether the peak cleared the threshold. */
static uint32_t est_period(const int16_t *x, uint32_t n, int *voiced)
{
    *voiced = 0;
    if (n <= PS_LMIN + 4u) return 80u;

    double e0 = 0.0;
    for (uint32_t i = 0; i < n; ++i) e0 += (double)x[i] * (double)x[i];
    if (!(e0 > 0.0)) return 80u;

    uint32_t lmax = PS_LMAX;
    if (lmax > n / 2u) lmax = n / 2u;

    double best = 0.0;
    uint32_t bl = 80u;
    for (uint32_t l = PS_LMIN; l <= lmax; ++l) {
        double num = 0.0, ea = 0.0, eb = 0.0;
        for (uint32_t i = 0; i + l < n; ++i) {
            double a = (double)x[i], b = (double)x[i + l];
            num += a * b; ea += a * a; eb += b * b;
        }
        double den = sqrt(ea * eb);
        double r = (den > 0.0) ? num / den : 0.0;
        if (r > best) { best = r; bl = l; }
    }
    if (best >= PS_VOICED) { *voiced = 1; return bl; }
    return 80u;                     /* unvoiced: Festival's fixed fallback rate */
}

/* Index of the strongest glottal-closure-like instant within [lo, hi) of `x`,
 * using short-term energy of a 3-point smoothed signal. */
static uint32_t find_epoch(const int16_t *x, uint32_t n, uint32_t lo, uint32_t hi)
{
    if (hi > n) hi = n;
    if (lo >= hi) return (lo < n) ? lo : (n ? n - 1u : 0u);

    double best = -1.0;
    uint32_t bi = lo;
    for (uint32_t i = lo; i < hi; ++i) {
        double acc = 0.0;
        for (int d = -2; d <= 2; ++d) {
            long long j = (long long)i + d;
            if (j < 0 || j >= (long long)n) continue;
            double s = (double)x[j];
            acc += s * s;
        }
        if (acc > best) { best = acc; bi = i; }
    }
    return bi;
}

/* Deterministic white noise at a given SNR, seeded from the window position so
 * a rerun reproduces it exactly and the two edges of a unit get different
 * draws. xorshift32 is plenty for a noise floor. */
static void add_noise(int16_t *pcm, uint32_t n, float snr_db, uint32_t seed)
{
    double e = 0.0;
    for (uint32_t i = 0; i < n; ++i) e += (double)pcm[i] * (double)pcm[i];
    double rms = sqrt(e / (double)n);
    if (!(rms > 0.0)) return;
    double sigma = rms * pow(10.0, -(double)snr_db / 20.0);

    uint32_t s = seed * 2654435761u + 1u;
    for (uint32_t i = 0; i < n; ++i) {
        /* two draws, summed, for a roughly triangular distribution */
        double u = 0.0;
        for (int k = 0; k < 2; ++k) {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            u += ((double)s / 4294967296.0) - 0.5;
        }
        double v = (double)pcm[i] + u * sigma * 3.464;   /* sqrt(12) for unit var */
        pcm[i] = (int16_t)(v > 32767.0 ? 32767.0 : (v < -32768.0 ? -32768.0 : v));
    }
}

/* Clamp a window start so [w, w+win) lies inside [0, sz). */
static uint32_t clamp_win(long long w, uint32_t sz, uint32_t win)
{
    long long hi = (long long)sz - (long long)win;
    if (hi < 0) return 0u;
    if (w < 0) w = 0;
    if (w > hi) w = hi;
    return (uint32_t)w;
}

int spfy_vb_frames_build_ex(const spfy_vin_t *vin, const spfy_vdb_t *vdb,
                            uint32_t sample_rate, const spfy_vb_cfg_t *cfg,
                            spfy_vb_frames_t *out)
{
    if (!vin || !vdb || !out || !cfg) return SPFY_E_INVAL;
    if (cfg->win < 16u || cfg->win > SPFY_VB_MAX_WIN || (cfg->win & (cfg->win - 1u)))
        return SPFY_E_INVAL;
    if (!cfg->n_cep || cfg->n_cep > SPFY_VB_MAX_CEP) return SPFY_E_INVAL;
    if (cfg->n_mel < 4u || cfg->n_mel > SPFY_VB_MAX_MEL) return SPFY_E_INVAL;
    if (cfg->n_cep > cfg->n_mel) return SPFY_E_INVAL;
    if (cfg->logmel && cfg->n_cep != cfg->n_mel) return SPFY_E_INVAL;
    if (cfg->lpc < 0 || cfg->lpc > (int)SPFY_VB_MAX_CEP) return SPFY_E_INVAL;
    if (cfg->lpc > 0 && cfg->logmel) return SPFY_E_INVAL;
    memset(out, 0, sizeof *out);

    spfy_unit_table_t units = {0};
    spfy_feat_table_t feat  = {0};
    spfy_vdb_lookup_t look  = {0};
    /* scratch is per-thread inside the parallel loop below */
    int rc;

    if ((rc = spfy_unit_table_load(vin, &units)) != SPFY_OK) goto fail;
    if ((rc = spfy_feat_table_load(vin, &feat))  != SPFY_OK) goto fail;
    if ((rc = spfy_vdb_lookup_build(vdb, &look)) != SPFY_OK) goto fail;

    /* Scratch is allocated per THREAD in the loop below; these keep the
     * single-threaded fallback and the failure path unchanged. */

    const uint32_t dim = SPFY_JC_DIM_SPEC + cfg->n_cep;
    const uint32_t n   = units.n_units;
    out->frames  = (float *)calloc((size_t)n * 2u * dim, sizeof *out->frames);
    out->weights = (float *)calloc(dim, sizeof *out->weights);
    if (!out->frames || !out->weights) { rc = SPFY_E_NOMEM; goto fail; }
    out->n_units = n;
    out->dim     = dim;

    /* ⭐ PARALLEL OVER UNITS. Each uid writes only its own two frames, and
     * everything read (unit table, feat table, VDB lookup, the encoded audio)
     * is const for the duration -- so the only shared mutable state was the
     * mfcc/pcm scratch, which is now per-thread, and n_missing, which is
     * reduced. Measured 28.3s serial on 457k units; it was the largest single
     * cost in a no-backoff build once the K-best pass was threaded.
     *
     * Bit-exactness is unaffected: no accumulation crosses units. The
     * `--prsl-backoff 0` byte-identity control is the proof and must be
     * re-run after any change here. */
    size_t n_miss_par = 0;
    int par_ok = 1;
#ifdef _OPENMP
#   pragma omp parallel reduction(+:n_miss_par)
#endif
    {
        mfcc_ctx *tctx = (mfcc_ctx *)malloc(sizeof *tctx);
        int16_t *tpcm = (int16_t *)malloc((size_t)cfg->win * sizeof *tpcm);
        int16_t *tctxpcm = (int16_t *)malloc((size_t)PS_CTX * sizeof *tctxpcm);
        int tok = (tctx && tpcm && tctxpcm);
        if (tok) { tctx->cfg = *cfg; mfcc_init(tctx, sample_rate); }
        else {
#ifdef _OPENMP
#           pragma omp critical
#endif
            par_ok = 0;
        }

#ifdef _OPENMP
#       pragma omp for schedule(static)
#endif
    for (long uid_i = 0; uid_i < (long)n; ++uid_i) {
        if (!tok || !par_ok) continue;
        const uint32_t uid = (uint32_t)uid_i;
        mfcc_ctx *ctx = tctx;
        int16_t *pcm = tpcm, *ctxpcm = tctxpcm;
        spfy_unit_record_t r;
        if (spfy_unit_record_get(&units, uid, &r) != SPFY_OK) { ++n_miss_par; continue; }

        float *fs = out->frames + (size_t)uid * 2u * dim;
        float *fe = fs + dim;

        /* Dim 0 is the engine's own edge F0; dim 1 stays 0. */
        if (cfg->zero_f0_dim) {
            fs[SPFY_JC_DIM_F0] = 0.0f;
            fe[SPFY_JC_DIM_F0] = 0.0f;
        } else if (cfg->f0_edge && uid < cfg->n_f0_edge) {
            /* Measured F0, so the ranking is pitch-aware even when the stored
             * bytes are 0. See spfy_vb_cfg_t.f0_edge. */
            fs[SPFY_JC_DIM_F0] = (float)cfg->f0_edge[(size_t)uid * 2u];
            fe[SPFY_JC_DIM_F0] = (float)cfg->f0_edge[(size_t)uid * 2u + 1u];
        } else {
            fs[SPFY_JC_DIM_F0] = (float)r.f0_start;
            fe[SPFY_JC_DIM_F0] = (float)r.f0_end;
        }

        uint32_t off = 0, sz = 0;
        if (r.file_idx >= feat.n_entries) { ++n_miss_par; continue; }
        const spfy_feat_entry_t *ent = &feat.entries[r.file_idx];
        if (spfy_vdb_lookup_by_name(&look, ent->name, ent->name_len,
                                    &off, &sz) != SPFY_OK) { ++n_miss_par; continue; }

        long long start_smp = (long long)r.local_pos * BYTES_PER_MS;
        long long len_smp   = (long long)r.dur_like  * BYTES_PER_MS;
        if (start_smp + len_smp > (long long)sz) { ++n_miss_par; continue; }

        long long s_at = start_smp, e_at = start_smp + len_smp;

        if (cfg->pitch_factor > 0.0f) {
            /* Festival: the frame is `factor * local period` long and centred
             * on a pitch mark. The start frame takes the first mark inside the
             * unit and the end frame the last, per cldb.cc's sub-track. */
            for (int edge = 0; edge < 2; ++edge) {
                long long at = edge ? e_at : s_at;
                uint32_t c0 = clamp_win(at - (long long)PS_CTX / 2, sz, PS_CTX);
                if (spfy_vdb_decode(vdb, off, c0, PS_CTX, ctxpcm) != (size_t)PS_CTX)
                    continue;

                int voiced = 0;
                uint32_t per = est_period(ctxpcm, PS_CTX, &voiced);
                long long rel = at - (long long)c0;
                if (rel < 0) rel = 0;
                if (rel > (long long)PS_CTX) rel = PS_CTX;

                long long mark = rel;
                if (cfg->pitch_align && voiced) {
                    /* first mark at or after the start; last at or before the
                     * end -- both therefore INSIDE the unit, which is what
                     * keeps the two frames distinct. */
                    if (edge) {
                        long long lo = rel - (long long)per;
                        mark = find_epoch(ctxpcm, PS_CTX,
                                          (uint32_t)(lo > 0 ? lo : 0),
                                          (uint32_t)(rel > 0 ? rel : 0));
                    } else {
                        mark = find_epoch(ctxpcm, PS_CTX, (uint32_t)rel,
                                          (uint32_t)(rel + (long long)per));
                    }
                    if (cfg->pitch_align == 2) mark += (long long)per / 2;
                }
                if (cfg->pitch_offset != 0.0f)
                    mark += (long long)((double)per * (double)cfg->pitch_offset
                                        + ((cfg->pitch_offset > 0) ? 0.5 : -0.5));

                uint32_t alen = cfg->pitch_fixlen
                              ? cfg->win
                              : (uint32_t)((double)per * (double)cfg->pitch_factor + 0.5);
                if (alen < 16u) alen = 16u;
                if (alen > cfg->win) alen = cfg->win;

                long long w0 = (long long)c0 + mark - (long long)alen / 2;
                uint32_t ws = clamp_win(w0, sz, alen);
                if (spfy_vdb_decode(vdb, off, ws, alen, pcm) != (size_t)alen)
                    continue;
                if (cfg->noise_snr > 0.0f)
                    add_noise(pcm, alen, cfg->noise_snr, uid * 2u + (uint32_t)edge);

                float *dst = edge ? fe : fs;
                mfcc_frame_n(ctx, pcm, alen, dst + SPFY_JC_DIM_SPEC);
                if (cfg->pitch_f0)
                    dst[SPFY_JC_DIM_F0] = voiced
                                        ? (float)sample_rate / (float)per : 0.0f;
            }
            continue;
        }

        s_at += cfg->shift_abs + cfg->shift_start;
        e_at += cfg->shift_abs + cfg->shift_end;

        long long s0, e0;
        if (cfg->anchor == SPFY_VB_ANCHOR_CENTER) {
            s0 = s_at - (long long)cfg->win / 2;
            e0 = e_at - (long long)cfg->win / 2;
        } else {
            s0 = s_at;                            /* window opens at the edge */
            e0 = e_at - (long long)cfg->win;      /* window closes at the edge */
        }
        uint32_t sw = clamp_win(s0, sz, cfg->win);
        uint32_t ew = clamp_win(e0, sz, cfg->win);

        if (spfy_vdb_decode(vdb, off, sw, cfg->win, pcm) == (size_t)cfg->win) {
            if (cfg->noise_snr > 0.0f) add_noise(pcm, cfg->win, cfg->noise_snr, uid * 2u);
            mfcc_frame_n(ctx, pcm, cfg->win, fs + SPFY_JC_DIM_SPEC);
        }
        if (spfy_vdb_decode(vdb, off, ew, cfg->win, pcm) == (size_t)cfg->win) {
            if (cfg->noise_snr > 0.0f) add_noise(pcm, cfg->win, cfg->noise_snr, uid * 2u + 1u);
            mfcc_frame_n(ctx, pcm, cfg->win, fe + SPFY_JC_DIM_SPEC);
        }
    }
        free(tctx); free(tpcm); free(tctxpcm);
    }   /* omp parallel */
    if (!par_ok) { rc = SPFY_E_NOMEM; goto fail; }
    out->n_missing += n_miss_par;

    /* Normalise the spectral block to zero mean / unit variance.
     *
     * The vendor's weight is 1/(sd_k * (2*dim-4)) and the term is
     * (dX)^2 * w, so a term carries E[(dX)^2]/(sd*(2*dim-4)) ~ 2*sd/(2*dim-4)
     * -- it scales LINEARLY with the feature's own spread. The formula is
     * therefore not scale-invariant, and the absolute cost depends on the
     * representation. Fixing the spectral block at unit variance is the
     * natural gauge: it makes the inverse-sd weight do exactly the job it was
     * written to do. It is also a no-op for per-pair RANK agreement, which is
     * what spfy_vb_jcfit tests. */
    if (cfg->norm) {
        const size_t nf = (size_t)n * 2u;
        for (uint32_t k = SPFY_JC_DIM_SPEC; k < dim; ++k) {
            long double s = 0.0L, ss = 0.0L;
            for (size_t f = 0; f < nf; ++f) {
                long double v = (long double)out->frames[f * dim + k];
                s += v; ss += v * v;
            }
            long double mean = s / (long double)nf;
            long double var  = ss / (long double)nf - mean * mean;
            long double sd   = (var > 0) ? sqrtl(var) : 1.0L;
            for (size_t f = 0; f < nf; ++f) {
                float *p = &out->frames[f * dim + k];
                *p = (float)(((long double)*p - mean) / sd);
            }
        }
    }

    rc = SPFY_OK;
fail:
    spfy_vdb_lookup_free(&look);
    spfy_feat_table_free(&feat);
    if (rc != SPFY_OK) spfy_vb_frames_free(out);
    return rc;
}

/* ---------------------------------------------------------------------
 * Whole-unit tracks and Festival's ac_unit_distance. See vb_track.h for what
 * is established about the ccos axes and what is still open.
 */

int spfy_vb_tracks_build(const spfy_vin_t *vin, const spfy_vdb_t *vdb,
                         uint32_t sample_rate, const spfy_vb_cfg_t *cfg,
                         float shift_ms,
                         const uint32_t *uids, size_t n_uids,
                         spfy_vb_tracks *out)
{
    if (!vin || !vdb || !cfg || !uids || !out) return SPFY_E_INVAL;
    if (shift_ms <= 0.0f) shift_ms = 5.0f;
    memset(out, 0, sizeof *out);

    spfy_unit_table_t units = {0};
    spfy_feat_table_t feat  = {0};
    spfy_vdb_lookup_t look  = {0};
    mfcc_ctx *ctx = NULL;
    int16_t  *pcm = NULL;
    int rc;

    if ((rc = spfy_unit_table_load(vin, &units)) != SPFY_OK) goto fail;
    if ((rc = spfy_feat_table_load(vin, &feat))  != SPFY_OK) goto fail;
    if ((rc = spfy_vdb_lookup_build(vdb, &look)) != SPFY_OK) goto fail;

    ctx = (mfcc_ctx *)malloc(sizeof *ctx);
    pcm = (int16_t *)malloc((size_t)cfg->win * sizeof *pcm);
    if (!ctx || !pcm) { rc = SPFY_E_NOMEM; goto fail; }
    ctx->cfg = *cfg;
    mfcc_init(ctx, sample_rate);

    out->t = (spfy_vb_track *)calloc(n_uids ? n_uids : 1u, sizeof *out->t);
    if (!out->t) { rc = SPFY_E_NOMEM; goto fail; }
    out->n = n_uids;
    out->n_cep = cfg->n_cep;

    const uint32_t bpms = sample_rate / 1000u ? sample_rate / 1000u : 8u;
    const uint32_t step = (uint32_t)(shift_ms * (float)bpms + 0.5f);

    for (size_t k = 0; k < n_uids; ++k) {
        spfy_unit_record_t r;
        if (uids[k] >= units.n_units
            || spfy_unit_record_get(&units, uids[k], &r) != SPFY_OK) {
            ++out->n_missing; continue;
        }
        if (r.file_idx >= feat.n_entries) { ++out->n_missing; continue; }
        const spfy_feat_entry_t *ent = &feat.entries[r.file_idx];
        uint32_t off = 0, sz = 0;
        if (spfy_vdb_lookup_by_name(&look, ent->name, ent->name_len,
                                    &off, &sz) != SPFY_OK) {
            ++out->n_missing; continue;
        }
        long long start = (long long)r.local_pos * bpms;
        long long len   = (long long)r.dur_like  * bpms;
        if (len <= 0 || start + len > (long long)sz) { ++out->n_missing; continue; }

        uint32_t nf = (uint32_t)(len / (long long)(step ? step : 1u));
        if (!nf) nf = 1u;
        float *f = (float *)calloc((size_t)nf * cfg->n_cep, sizeof *f);
        if (!f) { rc = SPFY_E_NOMEM; goto fail; }

        for (uint32_t i = 0; i < nf; ++i) {
            /* Centre each analysis window on its frame position, then clamp
             * into the recording -- a unit shorter than the window would
             * otherwise read from its neighbour. */
            long long at = start + (long long)i * step + (long long)step / 2;
            uint32_t w0 = clamp_win(at - (long long)cfg->win / 2, sz, cfg->win);
            if (spfy_vdb_decode(vdb, off, w0, cfg->win, pcm) == (size_t)cfg->win)
                mfcc_frame_n(ctx, pcm, cfg->win, f + (size_t)i * cfg->n_cep);
        }
        out->t[k].f = f;
        out->t[k].n_frames = nf;
        out->t[k].dur_ms = (float)r.dur_like;
    }
    rc = SPFY_OK;

fail:
    if (rc != SPFY_OK) spfy_vb_tracks_free(out);
    free(ctx); free(pcm);
    spfy_vdb_lookup_free(&look);
    spfy_feat_table_free(&feat);
    return rc;
}

void spfy_vb_tracks_free(spfy_vb_tracks *t)
{
    if (!t) return;
    if (t->t) for (size_t i = 0; i < t->n; ++i) free(t->t[i].f);
    free(t->t);
    memset(t, 0, sizeof *t);
}

float spfy_vb_ac_unit_distance(const spfy_vb_track *a, const spfy_vb_track *b,
                               const float *w, uint32_t n_w, float dur_pen_w)
{
    if (!a || !b || !a->n_frames || !b->n_frames) return 100.0f;
    /* Festival swaps so unit1 is the SHORTER, which is what makes the
     * measure symmetric; without it the warp direction alone would change
     * the answer. */
    if (a->dur_ms > b->dur_ms) { const spfy_vb_track *t = a; a = b; b = t; }
    if (a->dur_ms <= 0.0f) return 100.0f;

    const double incr = (double)a->dur_ms / (double)b->dur_ms;
    double distance = 0.0;
    uint32_t j = 0, i;
    for (i = 0; i < b->n_frames; ++i) {
        /* Festival: while (j < nf1-1 && unit1.t(j) < unit2.t(i)*incr) j++.
         * t() is the frame's own position, so on a fixed shift the shift
         * cancels and the test is just j < i*incr. */
        while (j + 1u < a->n_frames && (double)j < (double)i * incr)
            ++j;
        double cost = 0.0;
        const float *fa = a->f + (size_t)j * n_w;
        const float *fb = b->f + (size_t)i * n_w;
        for (uint32_t k = 0; k < n_w; ++k) {
            if (w[k] == 0.0f) continue;
            double d = (double)fb[k] - (double)fa[k];
            cost += d * d * (double)w[k];
        }
        distance += cost;
    }
    double dur_penalty = (double)b->dur_ms / (double)a->dur_ms;
    return (float)(distance / (double)(i ? i : 1u)
                   + dur_penalty * (double)dur_pen_w);
}

int spfy_vb_frames_build(const spfy_vin_t *vin, const spfy_vdb_t *vdb,
                         uint32_t sample_rate, spfy_vb_frames_t *out)
{
    spfy_vb_cfg_t cfg;
    spfy_vb_cfg_default(&cfg);
    return spfy_vb_frames_build_ex(vin, vdb, sample_rate, &cfg, out);
}

void spfy_vb_frames_free(spfy_vb_frames_t *f)
{
    if (!f) return;
    free(f->frames); free(f->weights);
    memset(f, 0, sizeof *f);
}

void spfy_vb_frames_bind(const spfy_vb_frames_t *f, spfy_jc_t *jc)
{
    memset(jc, 0, sizeof *jc);
    jc->dim     = f->dim;
    jc->n_units = f->n_units;
    jc->frames  = f->frames;
    jc->weights = f->weights;
    jc->f0_gate = 0.0f;
    jc->raw_scale = 1.0f;
}
