/* Per-unit TD-PSOLA pitch warp. */

#include "psola_unit.h"
#include "env.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* M_PI is a POSIX/GNU extension; the project builds with a strict -std
 * where math.h does not expose it. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MIN_PERIOD  20
#define MAX_PERIOD  200

/* Fractional-delay interpolator: Kaiser-windowed sinc, 48 taps. */
#define FD_TAPS 48
#define FD_HALF 24
#define FD_KAISER_BETA 4.0

/* Grain placement / blending mode, read fresh on every call. */
static int smooth_level(void)
{
    const char *s = spfy_env("SPFY_PSOLA_SMOOTH");
    if (!s || !*s) return 0;
    return (int)strtol(s, NULL, 10);
}

/* Modified Bessel I0, series form. */
static double kaiser_i0(double x)
{
    double s = 1.0, t = 1.0;
    for (int k = 1; k < 40; ++k) {
        double q = x / (2.0 * (double)k);
        t *= q * q;
        s += t;
        if (t < 1e-14 * s) break;
    }
    return s;
}

/* Taps for a delay of `frac` samples: y(m) = sum_o x(m+o) * sinc(o + frac). */
static void fd_coeffs(double frac, double *h)
{
    const double i0b = kaiser_i0(FD_KAISER_BETA);
    double sum = 0.0;
    for (int i = 0; i < FD_TAPS; ++i) {
        double o = (double)(i - FD_HALF) + frac;
        double s = (fabs(o) < 1e-12) ? 1.0 : sin(M_PI * o) / (M_PI * o);
        double r = o / ((double)FD_HALF + 0.5);
        double w;
        if (r < -1.0) r = -1.0;
        if (r >  1.0) r =  1.0;
        w = kaiser_i0(FD_KAISER_BETA * sqrt(1.0 - r * r)) / i0b;
        h[i] = s * w;
        sum += h[i];
    }
    if (sum > 1e-12 || sum < -1e-12)
        for (int i = 0; i < FD_TAPS; ++i) h[i] /= sum;
}

/* Tap sets for frac quantised to 1/256 of a sample, built once. 257 entries so
 * both endpoints are exact.
 *
 * Quantising costs nothing measurable: the residual is +-1/512 sample against
 * an instrument floor of 0.026 and the 0.29 samples of jitter this removes,
 * and the probe reports the same 0.0257 either way.
 *
 * ⚠ It is NOT where the time goes. This was added believing per-grain design
 * explained the +14.7% synthesis cost; re-measuring after it landed still
 * showed +14.1%, so the coefficients were roughly 0.4 ms of 18 and the
 * convolution is the whole story. Kept because it is free, not because it
 * fixed anything.
 *
 * frac == 0 never reaches here -- the caller takes a plain-copy branch -- so
 * the bit-exact identity does not depend on this table at all. */
#define FD_PHASES 256
static double g_fd_tab[FD_PHASES + 1][FD_TAPS];
static int g_fd_ready = 0;

static const double *fd_taps_for(double frac)
{
    if (!g_fd_ready) {
        for (int p = 0; p <= FD_PHASES; ++p)
            fd_coeffs(-0.5 + (double)p / (double)FD_PHASES, g_fd_tab[p]);
        g_fd_ready = 1;
    }
    int p = (int)((frac + 0.5) * (double)FD_PHASES + 0.5);
    if (p < 0) p = 0;
    if (p > FD_PHASES) p = FD_PHASES;
    return g_fd_tab[p];
}

/* Read the source at (m - frac). */
static double fd_read(const int16_t *b, int32_t n, int32_t m, const double *h)
{
    double s = 0.0;
    /* Interior fast path. */
    if (m >= FD_HALF && m - FD_HALF + FD_TAPS <= n) {
        const int16_t *p = b + m - FD_HALF;
        for (int i = 0; i < FD_TAPS; ++i) s += h[i] * (double)p[i];
        return s;
    }
    for (int i = 0; i < FD_TAPS; ++i) {
        int32_t idx = m + (int32_t)i - FD_HALF;
        if (idx < 0) idx = 0;
        if (idx >= n) idx = n - 1;
        s += h[i] * (double)b[idx];
    }
    return s;
}

/* SPFY_PSOLA_GRAIN_DUMP=<path>: one `g` line per synthesis grain and one
 * `u` line per unit, so "it sounds robotic" can be attributed to a
 * mechanism instead of guessed at. */
static FILE *grain_fp(void)
{
    static FILE *fp = NULL;
    static int opened = 0;
    if (!opened) {
        const char *p = spfy_env("SPFY_PSOLA_GRAIN_DUMP");
        opened = 1;
        if (p && *p) fp = fopen(p, "w");
    }
    return fp;
}

double spfy_prosody_limit_ratio2(double r, double max_up_st, double max_dn_st,
                                 int soft)
{
    /* UP AND DOWN ARE NOT THE SAME OPERATION, which is why they can be
     * limited separately. */
    double up = max_up_st > 0.0 ? max_up_st : max_dn_st;
    double dn = max_dn_st > 0.0 ? max_dn_st : max_up_st;
    double mx = (r > 1.0) ? up : dn;
    if (mx > 12.0) mx = 12.0;
    if (mx < 0.0) mx = 0.0;
    if (r <= 0.0) return r;
    if (mx <= 0.0) return 1.0;
    if (!soft) {
        double lim = pow(2.0, mx / 12.0);
        return r < 1.0 / lim ? 1.0 / lim : (r > lim ? lim : r);
    }
    /* tanh(0) == 0 exactly, so r == 1 maps to r == 1 with no rounding: the
     * zeroed-parameter identity survives bit-for-bit. */
    return pow(2.0, mx * tanh(12.0 * log2(r) / mx) / 12.0);
}

double spfy_prosody_limit_ratio(double r, double max_st, int soft)
{
    return spfy_prosody_limit_ratio2(r, max_st, max_st, soft);
}

double spfy_prosody_deadzone(double r, double dz_st)
{
    /* TD-PSOLA's artifact scales with how far a grain is moved -- raising
     * pitch needs more output pulses than the recording has, and the extra
     * ones are copies. */
    if (dz_st <= 0.0 || r <= 0.0) return r;
    double st = 12.0 * log2(r);
    double g = 1.0 - exp(-(st * st) / (dz_st * dz_st));
    st *= g;
    if (st < 0.01 && st > -0.01) return 1.0;
    return pow(2.0, st / 12.0);
}

#define LP_ORDER 10
#define LP_BW_EXPAND 0.994

/* Levinson-Durbin. */
static int lp_levinson(const double *r, int p, double *a)
{
    double e = r[0];
    if (!(e > 0.0)) return -1;
    for (int i = 0; i < p; ++i) a[i] = 0.0;
    for (int i = 1; i <= p; ++i) {
        double acc = r[i];
        for (int j = 1; j < i; ++j) acc -= a[j - 1] * r[i - j];
        double k = acc / e;
        if (!(k > -0.999 && k < 0.999)) return -1;
        double tmp[LP_ORDER];
        for (int j = 0; j < i - 1; ++j)
            tmp[j] = a[j] - k * a[i - 2 - j];
        for (int j = 0; j < i - 1; ++j) a[j] = tmp[j];
        a[i - 1] = k;
        e *= (1.0 - k * k);
        if (!(e > 0.0)) return -1;
    }
    return 0;
}

/* One all-pole fit per analysis mark, over a two-period Hamming window
 * centred on it. */
static int lp_fit(const int16_t *x, int32_t n, int32_t centre, int32_t half,
                  double *a)
{
    double r[LP_ORDER + 1];
    int32_t lo = centre - half, hi = centre + half;
    if (lo < 0) lo = 0;
    if (hi > n) hi = n;
    int32_t len = hi - lo;
    if (len < LP_ORDER * 3) return -1;
    double *w = (double *)malloc((size_t)len * sizeof *w);
    if (!w) return -1;
    for (int32_t i = 0; i < len; ++i) {
        double h = 0.54 - 0.46 * cos(2.0 * M_PI * (double)i / (double)(len - 1));
        w[i] = (double)x[lo + i] * h;
    }
    for (int k = 0; k <= LP_ORDER; ++k) {
        double s = 0.0;
        for (int32_t i = k; i < len; ++i) s += w[i] * w[i - k];
        r[k] = s;
    }
    free(w);
    if (!(r[0] > 0.0)) return -1;
    /* Conditioning: a touch of white noise plus lag windowing. */
    r[0] *= 1.0001;
    for (int k = 1; k <= LP_ORDER; ++k) {
        double f = 2.0 * M_PI * 60.0 * (double)k / 8000.0;
        r[k] *= exp(-0.5 * f * f);
    }
    if (lp_levinson(r, LP_ORDER, a) != 0) return -1;
    double g = 1.0;
    for (int k = 0; k < LP_ORDER; ++k) {
        g *= LP_BW_EXPAND;
        a[k] *= g;
    }
    return 0;
}

/* Read a double array at (m - frac), edge-clamped. */
static double fd_read_d(const double *b, int32_t n, int32_t m, const double *h)
{
    double s = 0.0;
    if (m >= FD_HALF && m - FD_HALF + FD_TAPS <= n) {
        const double *p = b + m - FD_HALF;
        for (int i = 0; i < FD_TAPS; ++i) s += h[i] * p[i];
        return s;
    }
    for (int i = 0; i < FD_TAPS; ++i) {
        int32_t idx = m + (int32_t)i - FD_HALF;
        if (idx < 0) idx = 0;
        if (idx >= n) idx = n - 1;
        s += h[i] * b[idx];
    }
    return s;
}

static int warp_unit_lp(int16_t *buf, size_t n, const int32_t *amark, int m,
                        const int32_t *space, const double *ratio, int level)
{
    const int32_t N = (int32_t)n;
    double *lpc = (double *)calloc((size_t)m * LP_ORDER, sizeof *lpc);
    double *res = (double *)calloc(n, sizeof *res);
    double *acc = (double *)calloc(n, sizeof *acc);
    double *wsum = (double *)calloc(n, sizeof *wsum);
    if (!lpc || !res || !acc || !wsum) {
        free(lpc); free(res); free(acc); free(wsum);
        return -1;
    }
    for (int j = 0; j < m; ++j) {
        int32_t half = space[j];
        if (half < MIN_PERIOD) half = MIN_PERIOD;
        if (half > MAX_PERIOD) half = MAX_PERIOD;
        if (lp_fit(buf, N, amark[j], half, lpc + (size_t)j * LP_ORDER) != 0) {
            free(lpc); free(res); free(acc); free(wsum);
            return -1;
        }
    }

    /* Nearest analysis mark per sample. */
    int32_t *own = (int32_t *)malloc(n * sizeof *own);
    if (!own) { free(lpc); free(res); free(acc); free(wsum); return -1; }
    {
        int j = 0;
        for (int32_t i = 0; i < N; ++i) {
            while (j + 1 < m && llabs((long long)amark[j + 1] - i) <
                                llabs((long long)amark[j] - i)) ++j;
            own[i] = j;
        }
    }
    for (int32_t i = 0; i < N; ++i) {
        const double *a = lpc + (size_t)own[i] * LP_ORDER;
        double e = (double)buf[i];
        for (int k = 0; k < LP_ORDER; ++k)
            if (i - k - 1 >= 0) e -= a[k] * (double)buf[i - k - 1];
        res[i] = e;
    }

    double t = (double)amark[0];
    int j = 0;
    while (t < (double)N) {
        while (j + 1 < m && amark[j + 1] <= (int32_t)t) ++j;
        int jj = j;
        if (jj + 1 < m &&
            llabs((long long)amark[jj + 1] - (long long)t) <
            llabs((long long)amark[jj] - (long long)t)) ++jj;
        double p_in = (double)space[jj];
        if (p_in < MIN_PERIOD) p_in = MIN_PERIOD;
        if (p_in > MAX_PERIOD) p_in = MAX_PERIOD;
        int L = (int)p_in;
        if (jj == m - 1 && t > (double)amark[m - 1] + 0.5) break;
        int32_t c_in = amark[jj];
        int32_t c_out = (int32_t)(t + 0.5);
        double frac = t - (double)c_out;
        int use_fd = (level >= 1) && (frac > 1e-12 || frac < -1e-12);
        const double *h = use_fd ? fd_taps_for(frac) : NULL;
        for (int k = -L; k <= L; ++k) {
            int32_t si = c_in + k, di = c_out + k;
            if (si < 0 || si >= N || di < 0 || di >= N) continue;
            double w = 0.5 * (1.0 + cos(M_PI * (double)k / (double)L));
            double v = use_fd ? fd_read_d(res, N, si, h) : res[si];
            acc[di] += v * w;
            wsum[di] += w;
        }
        double step = (double)space[jj] / ratio[jj];
        if (step < 1.0) step = 1.0;
        t += step;
    }
    for (int32_t i = 0; i < N; ++i)
        if (wsum[i] > 1e-12) res[i] = acc[i] / wsum[i];

    /* Synthesis filter, same coefficients, same time axis. */
    double *out = acc;
    for (int32_t i = 0; i < N; ++i) {
        const double *a = lpc + (size_t)own[i] * LP_ORDER;
        double y = res[i];
        for (int k = 0; k < LP_ORDER; ++k)
            if (i - k - 1 >= 0) y += a[k] * out[i - k - 1];
        if (!(y > -1e7 && y < 1e7)) {
            free(lpc); free(res); free(acc); free(wsum); free(own);
            return -1;
        }
        out[i] = y;
    }
    /* PEAK GUARD -- attenuate if the unit would clip, never boost.
     *
     * The synthesis filter has no gain control of its own, so a unit can come
     * out past full scale; clamping it flattens the crest, and flattened
     * crests click. Measured on the radar phrase before this: 9 samples at the
     * rail with 3 runs of consecutive clamped samples, against 0 for plain TD.
     *
     * ⚠ RMS MATCHING WAS TRIED HERE FIRST AND MADE IT WORSE -- 41 samples and
     * 11 runs. The LP path comes out slightly QUIETER than its input, so an
     * energy match computes a gain above 1 and boosts a signal that is already
     * against the rails. Matching energy and avoiding clipping are different
     * goals and only one of them is the problem here.
     *
     * A pure peak guard can only ever reduce, so it cannot create the fault it
     * is meant to remove. It engages rarely and by a few percent, so the
     * per-unit level step it introduces is far below the clicks it prevents. */
    double pk = 0.0;
    for (int32_t i = 0; i < N; ++i) {
        double v = out[i] < 0.0 ? -out[i] : out[i];
        if (v > pk) pk = v;
    }
    double g = 1.0;
    if (pk > 32700.0) g = 32700.0 / pk;
    for (int32_t i = 0; i < N; ++i) {
        double v = out[i] * g;
        if (v > 32767.0) v = 32767.0;
        if (v < -32767.0) v = -32767.0;
        buf[i] = (int16_t)(v < 0.0 ? -floor(-v + 0.5) : floor(v + 0.5));
    }
    free(lpc); free(res); free(acc); free(wsum); free(own);
    return 0;
}

int spfy_prosody_lp_roundtrip(int16_t *buf, size_t n,
                              const int16_t *periods, int n_marks)
{
    if (!buf || n == 0 || !periods || n_marks < 2) return -1;
    const int32_t N = (int32_t)n;
    int32_t *amark = (int32_t *)malloc((size_t)n_marks * sizeof *amark);
    if (!amark) return -1;
    int m = 0;
    int32_t pos = 0;
    for (int i = 0; i < n_marks; ++i) {
        int32_t p = periods[i] < 1 ? 1 : periods[i];
        pos += p;
        if (pos >= N) break;
        amark[m++] = pos;
    }
    if (m < 2) { free(amark); return -1; }

    double *lpc = (double *)calloc((size_t)m * LP_ORDER, sizeof *lpc);
    double *res = (double *)calloc(n, sizeof *res);
    int32_t *own = (int32_t *)malloc(n * sizeof *own);
    if (!lpc || !res || !own) {
        free(amark); free(lpc); free(res); free(own); return -1;
    }
    for (int j = 0; j < m; ++j) {
        int32_t half = (j + 1 < m) ? amark[j + 1] - amark[j] : MIN_PERIOD;
        if (half < MIN_PERIOD) half = MIN_PERIOD;
        if (half > MAX_PERIOD) half = MAX_PERIOD;
        if (lp_fit(buf, N, amark[j], half, lpc + (size_t)j * LP_ORDER) != 0) {
            free(amark); free(lpc); free(res); free(own); return -1;
        }
    }
    {
        int j = 0;
        for (int32_t i = 0; i < N; ++i) {
            while (j + 1 < m && llabs((long long)amark[j + 1] - i) <
                                llabs((long long)amark[j] - i)) ++j;
            own[i] = j;
        }
    }
    for (int32_t i = 0; i < N; ++i) {
        const double *a = lpc + (size_t)own[i] * LP_ORDER;
        double e = (double)buf[i];
        for (int k = 0; k < LP_ORDER; ++k)
            if (i - k - 1 >= 0) e -= a[k] * (double)buf[i - k - 1];
        res[i] = e;
    }
    double *out = (double *)calloc(n, sizeof *out);
    if (!out) { free(amark); free(lpc); free(res); free(own); return -1; }
    for (int32_t i = 0; i < N; ++i) {
        const double *a = lpc + (size_t)own[i] * LP_ORDER;
        double y = res[i];
        for (int k = 0; k < LP_ORDER; ++k)
            if (i - k - 1 >= 0) y += a[k] * out[i - k - 1];
        out[i] = y;
    }
    for (int32_t i = 0; i < N; ++i) {
        double v = out[i];
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        buf[i] = (int16_t)(v < 0.0 ? -floor(-v + 0.5) : floor(v + 0.5));
    }
    free(amark); free(lpc); free(res); free(own); free(out);
    return 0;
}

int spfy_prosody_warp_unit(int16_t *buf, size_t n,
                           const int16_t *periods, int n_marks,
                           const float *target_hz,
                           float max_semitones, int sample_rate)
{
    if (!buf || n == 0 || !periods || n_marks < 2 || sample_rate <= 0)
        return 0;

    /* Absolute analysis marks = cumulative sum of periods, matching the
     * engine's own convention (FUN_08EE23D0 does exactly this sum). */
    int32_t *amark = (int32_t *)malloc((size_t)n_marks * sizeof *amark);
    double  *ratio = (double *)malloc((size_t)n_marks * sizeof *ratio);
    double  *acc   = (double *)calloc(n, sizeof *acc);
    double  *wsum  = (double *)calloc(n, sizeof *wsum);
    if (!amark || !ratio || !acc || !wsum) {
        free(amark); free(ratio); free(acc); free(wsum);
        return -1;
    }

    int m = 0;
    int32_t pos = 0;
    for (int i = 0; i < n_marks; ++i) {
        int32_t p = periods[i];
        if (p < 1) p = 1;
        pos += p;
        if (pos >= (int32_t)n) break;
        amark[m++] = pos;
    }
    if (m < 2) {
        free(amark); free(ratio); free(acc); free(wsum);
        return 0;
    }

    /* Soft limiting, not a hard clamp. */
    static int soft = -1;
    if (soft < 0) {
        const char *s = spfy_env("SPFY_PROSODY_SOFT_ST");
        soft = (s && *s == '0') ? 0 : 1;
    }
    /* Separate ceiling for UPWARD shifts. */
    double max_up = 0.0;
    {
        const char *u = spfy_env("SPFY_PROSODY_MAX_UP_ST");
        if (u && *u) max_up = strtod(u, NULL);
    }
    double dz = 0.0;
    {
        const char *z = spfy_env("SPFY_PROSODY_DEADZONE_ST");
        if (z && *z) dz = strtod(z, NULL);
    }
    for (int i = 0; i < m; ++i) {
        double p = (double)periods[i];
        if (p < 1.0) p = 1.0;
        double natural = (double)sample_rate / p;
        double r = 1.0;
        if (target_hz && target_hz[i] > 0.0f && natural > 0.0)
            r = (double)target_hz[i] / natural;
        r = spfy_prosody_limit_ratio2(r, max_up, (double)max_semitones, soft);
        r = spfy_prosody_deadzone(r, dz);
        /* Snap so an exact-identity request really is exact. */
        if (r > 1.0 - 1e-6 && r < 1.0 + 1e-6) r = 1.0;
        ratio[i] = r;
    }

    /* Local inter-mark spacing. */
    int32_t *space = (int32_t *)malloc((size_t)m * sizeof *space);
    if (!space) {
        free(amark); free(ratio); free(acc); free(wsum);
        return -1;
    }
    for (int i = 0; i < m; ++i) {
        int32_t d = (i + 1 < m) ? (amark[i + 1] - amark[i])
                                : (m >= 2 ? amark[i] - amark[i - 1] : MIN_PERIOD);
        if (d < 1) d = 1;
        space[i] = d;
    }

    const int level = smooth_level();

    /* SPFY_PSOLA_METHOD=lp routes through the LP residual instead of the
     * waveform. */
    {
        const char *meth = spfy_env("SPFY_PSOLA_METHOD");
        if (meth && (meth[0] == 'l' || meth[0] == 'L')) {
            int any = 0;
            for (int i = 0; i < m; ++i)
                if (ratio[i] != 1.0) { any = 1; break; }
            if (any) {
                int lp_rc = warp_unit_lp(buf, n, amark, m, space, ratio,
                                         level);
                /* A SILENT fallback rate is the thing to be afraid of here:
                 * if a third of units quietly used TD instead, the render
                 * is a mixture and "LP fixed it" would be half true. */
                if (spfy_env("SPFY_PSOLA_LP_STATS"))
                    fprintf(stderr, "[lp] unit n=%zu marks=%d -> %s\n",
                            n, m, lp_rc == 0 ? "ok" : "FELL BACK TO TD");
                if (lp_rc == 0) {
                    free(amark); free(ratio); free(acc); free(wsum);
                    free(space);
                    return 0;
                }
            }
        }
    }

    FILE *gf = grain_fp();
    long n_grain = 0, n_dup = 0;
    double res_sum = 0.0, res_max = 0.0;
    int prev_src = -1;

    /* Walk output marks along the same time axis; only their SPACING
     * changes, so duration is untouched. */
    double t = (double)amark[0];
    int j = 0;
    while (t < (double)n) {
        while (j + 1 < m && amark[j + 1] <= (int32_t)t) ++j;
        int jj = j;
        if (jj + 1 < m &&
            llabs((long long)amark[jj + 1] - (long long)t) <
            llabs((long long)amark[jj] - (long long)t))
            ++jj;

        double p_in = (double)space[jj];
        if (p_in < MIN_PERIOD) p_in = MIN_PERIOD;
        if (p_in > MAX_PERIOD) p_in = MAX_PERIOD;
        int L = (int)p_in;

        /* Past the final analysis mark there is nothing left to read: the
         * grain would keep sourcing amark[m-1] while writing further and
         * further along, smearing a duplicate period over the unit tail. */
        if (jj == m - 1 && t > (double)amark[m - 1] + 0.5) break;

        int32_t c_in  = amark[jj];
        int32_t c_out = (int32_t)(t + 0.5);

        /* Sub-sample placement. */
        double frac = t - (double)c_out;
        int use_fd = (level >= 1) && (frac > 1e-12 || frac < -1e-12);
        const double *h = use_fd ? fd_taps_for(frac) : NULL;

        /* Grain blending (level 2). */
        double alpha = 0.0;
        int32_t c_in2 = c_in;
        if (level >= 2 && j + 1 < m) {
            int32_t d = amark[j + 1] - amark[j];
            int32_t dp = space[j] > space[j + 1] ? space[j] - space[j + 1]
                                                 : space[j + 1] - space[j];
            if (d > 0 && dp * 4 <= space[j]) {
                alpha = (t - (double)amark[j]) / (double)d;
                if (alpha < 0.0) alpha = 0.0;
                if (alpha > 1.0) alpha = 1.0;
                int32_t sp = space[j];
                if (sp < MIN_PERIOD) sp = MIN_PERIOD;
                if (sp > MAX_PERIOD) sp = MAX_PERIOD;
                c_in  = amark[j];
                c_in2 = amark[j + 1];
                L     = (int)sp;
            }
        }

        for (int k = -L; k <= L; ++k) {
            int32_t si = c_in + k;
            int32_t di = c_out + k;
            if (si < 0 || si >= (int32_t)n) continue;
            if (di < 0 || di >= (int32_t)n) continue;
            /* Hann of width 2L at hop L is COLA: interior window sums are
             * exactly 1, and dividing by wsum fixes the edges too. */
            double w = 0.5 * (1.0 + cos(M_PI * (double)k / (double)L));
            double v;
            if (use_fd) {
                v = fd_read(buf, (int32_t)n, si, h);
                if (alpha > 0.0) {
                    int32_t s2 = c_in2 + k;
                    if (s2 >= 0 && s2 < (int32_t)n)
                        v += alpha * (fd_read(buf, (int32_t)n, s2, h) - v);
                }
            } else {
                v = (double)buf[si];
                if (alpha > 0.0) {
                    int32_t s2 = c_in2 + k;
                    if (s2 >= 0 && s2 < (int32_t)n)
                        v += alpha * ((double)buf[s2] - v);
                }
            }
            acc[di]  += v * w;
            wsum[di] += w;
        }

        if (gf) {
            double ar = fabs(frac);
            ++n_grain;
            if (c_in == prev_src) ++n_dup;
            prev_src = c_in;
            res_sum += ar;
            if (ar > res_max) res_max = ar;
            fprintf(gf, "g %.6f %d %d %d %d %.6f %.4f %.4f\n",
                    t, (int)c_out, (int)c_in, jj, L,
                    ratio[jj], frac, alpha);
        }

        /* Step by the LOCAL inter-mark distance so ratio 1.0 lands exactly
         * on the next analysis mark. */
        double step = (double)space[jj] / ratio[jj];
        if (step < 1.0) step = 1.0;
        t += step;
    }

    if (gf) {
        long uncov = 0, thin = 0;
        double wmin = 1e30;
        for (size_t i = 0; i < n; ++i) {
            if (wsum[i] <= 1e-12) { ++uncov; continue; }
            if (wsum[i] < 0.5) ++thin;
            if (wsum[i] < wmin) wmin = wsum[i];
        }
        fprintf(gf, "u %d %ld %ld %ld %ld %.6f %.6f %.6f %d\n",
                (int)n, n_grain, n_dup, uncov, thin,
                wmin == 1e30 ? 0.0 : wmin,
                n_grain ? res_sum / (double)n_grain : 0.0,
                res_max, level);
    }

    for (size_t i = 0; i < n; ++i) {
        if (wsum[i] <= 1e-12) continue;
        double v = acc[i] / wsum[i];
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        buf[i] = (int16_t)(v < 0.0 ? -floor(-v + 0.5) : floor(v + 0.5));
    }

    free(amark); free(ratio); free(acc); free(wsum); free(space);
    return 0;
}

void spfy_psola_frac_response(double frac, int n_freq, const double *freq_hz,
                              int sample_rate, double *out_db)
{
    double h[FD_TAPS];
    if (!freq_hz || !out_db || n_freq <= 0 || sample_rate <= 0) return;
    fd_coeffs(frac, h);
    for (int q = 0; q < n_freq; ++q) {
        double w = 2.0 * M_PI * freq_hz[q] / (double)sample_rate;
        double re = 0.0, im = 0.0;
        for (int i = 0; i < FD_TAPS; ++i) {
            double o = (double)(i - FD_HALF);
            re += h[i] * cos(w * o);
            im -= h[i] * sin(w * o);
        }
        out_db[q] = 10.0 * log10(re * re + im * im + 1e-300);
    }
}
