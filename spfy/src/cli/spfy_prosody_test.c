/* Gate test for the prosody stage (see reveng/spfy4/PLAN_PROSODY_STAGE.md). */

#include "../prosody/pmarks.h"
#include "../prosody/psola_unit.h"
#include "../prosody/contour.h"
#include "../common/env.h"
#include "../../include/spfy/spfy.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SR 8000

static int fails = 0;

static void check(int ok, const char *what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
}

/* SPFY_PSOLA_SMOOTH has to be written through the SAME API psola_unit.c
 * reads it back through, and the cache dropped afterwards. */
static void set_env(const char *k, const char *v)
{
#if defined(_WIN32)
    SetEnvironmentVariableA(k, v);
#else
    if (v) setenv(k, v, 1);
    else   unsetenv(k);
#endif
    spfy_env_reset();
}

static void set_smooth(const char *v) { set_env("SPFY_PSOLA_SMOOTH", v); }

static double snr_db(const int16_t *ref, const int16_t *y, size_t n)
{
    double s = 0.0, e = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double a = (double)ref[i], d = a - (double)y[i];
        s += a * a;
        e += d * d;
    }
    if (e <= 0.0) return 1e9;
    return 10.0 * log10(s / e);
}

/* One glottal pulse of the synthetic voice, evaluated at a REAL phase so a
 * reference can be built with pulses between samples. */
static double buzz_pulse(double ph, int period)
{
    double env = exp(-ph / (0.35 * (double)period));
    return (sin(2.0 * M_PI * 700.0 * ph / SR)
            + 0.4 * sin(2.0 * M_PI * 1900.0 * ph / SR)) * env;
}

/* Synthetic voiced signal: impulse-ish glottal pulses at `period`, plus a
 * decaying formant ring so the OLA has real spectral content to preserve. */
static void make_buzz(int16_t *buf, size_t n, int period)
{
    memset(buf, 0, n * sizeof *buf);
    for (size_t t = 0; t < n; ++t) {
        size_t ph = t % (size_t)period;
        buf[t] = (int16_t)(9000.0 * buzz_pulse((double)ph, period));
    }
}

/* Same voice, but pulse onsets at `first + i*period` with BOTH real-valued,
 * so a signal whose pulses sit at a known fraction of a sample can be
 * built. */
static void make_buzz_at(int16_t *buf, size_t n, double period, double first)
{
    memset(buf, 0, n * sizeof *buf);
    for (size_t t = 0; t < n; ++t) {
        double k = floor(((double)t - first) / period);
        double ph = (double)t - (first + k * period);
        if (k < 0.0 || ph < 0.0) continue;
        buf[t] = (int16_t)(9000.0 * buzz_pulse(ph, (int)(period + 0.5)));
    }
}

/* Standard deviation -- the mean is removed on purpose, see the note at the
 * call site: a constant position offset is a fixed delay, not audible
 * jitter. */
static double spread(const double *v, int n)
{
    if (n < 2) return 0.0;
    double mu = 0.0, s = 0.0;
    for (int i = 0; i < n; ++i) mu += v[i];
    mu /= (double)n;
    for (int i = 0; i < n; ++i) s += (v[i] - mu) * (v[i] - mu);
    return sqrt(s / (double)(n - 1));
}

/* Locate the pulse nearest `approx` to sub-sample precision: matched filter
 * against one ideal pulse, then a parabolic fit on the correlation peak. */
static double locate_pulse(const int16_t *y, size_t n, const double *tmpl,
                           int tlen, double approx, int search)
{
    int lo = (int)approx - search, hi = (int)approx + search;
    if (lo < 0) lo = 0;
    if (hi + tlen >= (int)n) hi = (int)n - tlen - 1;
    if (hi <= lo) return approx;
    double best = -1e300;
    int bm = lo;
    double *c = (double *)malloc((size_t)(hi - lo + 1) * sizeof *c);
    if (!c) return approx;
    for (int m = lo; m <= hi; ++m) {
        double s = 0.0;
        for (int k = 0; k < tlen; ++k) s += tmpl[k] * (double)y[m + k];
        c[m - lo] = s;
        if (s > best) { best = s; bm = m; }
    }
    double out = (double)bm;
    if (bm > lo && bm < hi) {
        double a = c[bm - lo - 1], b = c[bm - lo], d = c[bm - lo + 1];
        double den = a - 2.0 * b + d;
        if (fabs(den) > 1e-9) out = (double)bm + 0.5 * (a - d) / den;
    }
    free(c);
    return out;
}

static double dom_f0(const int16_t *x, size_t n, int lo, int hi)
{
    double *r = malloc((size_t)(hi + 1) * sizeof *r);
    if (!r) return 0.0;
    double best = 0.0;
    int best_lag = lo;
    for (int lag = lo; lag <= hi; ++lag) {
        double s = 0.0;
        for (size_t i = 0; i + (size_t)lag < n; ++i)
            s += (double)x[i] * (double)x[i + (size_t)lag];
        r[lag] = s;
        if (s > best) { best = s; best_lag = lag; }
    }
    /* Octave guard. */
    for (int half = best_lag / 2; half >= lo; half /= 2) {
        if (r[half] > 0.80 * best) best_lag = half;
        else break;
    }
    free(r);
    return (double)SR / best_lag;
}

int main(int argc, char **argv)
{
    const size_t N = 4000;
    const int period = 68;
    int16_t *orig = malloc(N * sizeof *orig);
    int16_t *work = malloc(N * sizeof *work);
    int n_marks = (int)(N / (size_t)period) - 1;
    int16_t *periods = malloc((size_t)n_marks * sizeof *periods);
    float   *tgt = malloc((size_t)n_marks * sizeof *tgt);
    if (!orig || !work || !periods || !tgt) return 1;

    make_buzz(orig, N, period);
    for (int i = 0; i < n_marks; ++i) periods[i] = (int16_t)period;

    printf("prosody stage gate tests (n=%zu, period=%d, marks=%d)\n",
           N, period, n_marks);

    float natural = (float)SR / (float)period;
    for (int i = 0; i < n_marks; ++i) tgt[i] = natural;
    memcpy(work, orig, N * sizeof *orig);
    spfy_prosody_warp_unit(work, N, periods, n_marks, tgt, 4.0f, SR);
    check(memcmp(work, orig, N * sizeof *orig) == 0,
          "identity: target == natural is bit-identical");

    memcpy(work, orig, N * sizeof *orig);
    spfy_prosody_warp_unit(work, N, periods, n_marks, NULL, 4.0f, SR);
    check(memcmp(work, orig, N * sizeof *orig) == 0,
          "identity: NULL target is bit-identical");

    /* 2b. */
    {
        int16_t vper[64];
        float   vtgt[64];
        int nv = 0;
        int32_t acc2 = 0;
        while (nv < 64) {
            int pp = 60 + (nv % 7) * 4;
            if (acc2 + pp >= (int32_t)N - 200) break;
            vper[nv] = (int16_t)pp; acc2 += pp;
            vtgt[nv] = (float)SR / (float)pp;
            ++nv;
        }
        memcpy(work, orig, N * sizeof *orig);
        spfy_prosody_warp_unit(work, N, vper, nv, vtgt, 4.0f, SR);
        check(memcmp(work, orig, N * sizeof *orig) == 0,
              "identity: VARYING periods, target == natural, bit-identical");
    }

    /* NOTE — the identity snap tolerance in psola_unit.c is NOT guarded
     * from here, deliberately. */

    double f_in = dom_f0(orig, N, 30, 160);
    for (int i = 0; i < n_marks; ++i) tgt[i] = natural * 1.2f;
    memcpy(work, orig, N * sizeof *orig);
    spfy_prosody_warp_unit(work, N, periods, n_marks, tgt, 4.0f, SR);
    double f_out = dom_f0(work, N, 30, 160);
    printf("      F0 %.1f Hz -> %.1f Hz (target %.1f)\n",
           f_in, f_out, natural * 1.2);
    check(f_out > f_in * 1.10 && f_out < f_in * 1.32,
          "shift: +20%% target raises measured F0 into range");
    check(memcmp(work, orig, N * sizeof *orig) != 0,
          "shift: output actually changed");

    for (int i = 0; i < n_marks; ++i) tgt[i] = natural * 4.0f;
    memcpy(work, orig, N * sizeof *orig);
    spfy_prosody_warp_unit(work, N, periods, n_marks, tgt, 2.0f, SR);
    double f_clamp = dom_f0(work, N, 20, 160);
    double want = f_in * pow(2.0, 2.0 / 12.0);
    printf("      clamped F0 %.1f Hz (expected ~%.1f for a +2 st clamp)\n",
           f_clamp, want);
    /* Two-sided: an upper bound alone is satisfied by the clamp doing
     * nothing, or by an octave-down measurement error. */
    check(f_clamp <= want * 1.08 && f_clamp >= want * 0.92,
          "clamp: +2 st clamp lands at +2 st, not lower or higher");

    /* 6. */
    {
        const double r_req = 1.07;
        /* Ask the limiter, do not re-derive it: the soft tanh means the
         * ratio actually applied is not the one requested (1.070 -> 1.068),
         * and a hand-computed expectation would charge that difference to
         * placement. */
        const double r = spfy_prosody_limit_ratio(r_req, 4.0, 1);
        const double step = (double)period / r;
        const int tlen = period;
        double *tmpl = malloc((size_t)tlen * sizeof *tmpl);
        if (!tmpl) return 1;
        for (int k = 0; k < tlen; ++k) tmpl[k] = buzz_pulse((double)k, period);

        /* FLOOR -- what the locator reports on a PERFECT result. */
        /* Errors are scored as JITTER -- the spread of the position error,
         * not its size. */
        double err[128];
        int ne;

        /* FLOOR -- what the locator reports on a PERFECT result. */
        make_buzz_at(work, N, step, (double)period);
        ne = 0;
        for (int i = 3; i < 40 && ne < 128; ++i) {
            double t_want = (double)period + (double)i * step;
            if (t_want + tlen >= (double)N) break;
            err[ne++] = locate_pulse(work, N, tmpl, tlen, t_want, 6) - t_want;
        }
        double floor_jit = spread(err, ne);
        printf("      locator floor on an IDEAL result: jitter %.4f samples "
               "(n=%d)\n", floor_jit, ne);
        /* It has to resolve well inside the effect it is being pointed at:
         * a +-0.5 sample rounding is 0.408 RMS on the period, so anything
         * above ~0.1 cannot tell the arms apart and the rest of this is
         * noise. */
        check(floor_jit < 0.10,
              "placement: locator resolves better than 0.1 sample");

        double jit[2] = { 0.0, 0.0 };
        for (int arm = 0; arm < 2; ++arm) {
            set_smooth(arm ? "1" : "0");
            for (int i = 0; i < n_marks; ++i) tgt[i] = (float)(natural * r_req);
            memcpy(work, orig, N * sizeof *orig);
            spfy_prosody_warp_unit(work, N, periods, n_marks, tgt, 4.0f, SR);
            const double last = (double)period * (double)n_marks;
            ne = 0;
            for (int i = 3; ne < 128; ++i) {
                double t_want = (double)period + (double)i * step;
                if (t_want + 2.0 * (double)period >= last) break;
                if (t_want + tlen >= (double)N) break;
                err[ne++] = locate_pulse(work, N, tmpl, tlen, t_want, 5) - t_want;
            }
            jit[arm] = spread(err, ne);
        }
        set_smooth(NULL);
        printf("      realised pulse JITTER: level 0 %.4f, level 1 %.4f "
               "samples (instrument floor %.4f)\n",
               jit[0], jit[1], floor_jit);
        /* Two-sided on purpose. */
        check(jit[0] > 3.0 * floor_jit,
              "placement: level 0 really does misplace grains (arm A can fail)");
        check(jit[1] < floor_jit * 1.5,
              "placement: level 1 lands at the instrument floor");

        /* Moving a grain by a fraction of a sample means resampling it, and
         * a bad resampler trades the jitter for something worse: `frac`
         * changes from grain to grain, so any passband droop becomes timbre
         * modulation at the... */
        {
            /* Band stops at 3600, not 4000, and that is a property of the
             * OPERATION: a half-sample delay has exactly zero gain at fs/2
             * for any symmetric filter, so no tap count makes the last few
             * hundred Hz flat. */
            double fq[9] = { 200, 300, 700, 1200, 1900, 2600, 3100, 3400, 3600 };
            double db[9];
            double worst = 0.0;
            for (int q = 0; q <= 20; ++q) {
                spfy_psola_frac_response(-0.5 + 0.05 * (double)q, 9, fq, SR, db);
                for (int z = 0; z < 9; ++z)
                    if (fabs(db[z]) > worst) worst = fabs(db[z]);
            }
            double lin = 20.0 * log10(fabs(cos(M_PI * 3400.0 / SR)));
            printf("      placer response: worst |dev| %.3f dB over 200-3600 Hz, "
                   "frac -0.5..+0.5 (linear interp: %.1f dB at 3400)\n",
                   worst, lin);
            check(worst < 0.25,
                  "placement: interpolator is flat across the material's band");
            /* The bar is only meaningful if something plausible fails it. */
            check(lin < -6.0,
                  "placement: that 0.25 dB bar is one linear interpolation fails");
        }
        free(tmpl);
    }

    /* 7. */
    {
        float natural2 = (float)SR / (float)period;
        for (int i = 0; i < n_marks; ++i)
            tgt[i] = (float)(natural2 * pow(2.0, 0.05 / 12.0));

        memcpy(work, orig, N * sizeof *orig);
        int rt = spfy_prosody_lp_roundtrip(work, N, periods, n_marks);
        double rt_snr = (rt == 0) ? snr_db(orig, work, N) : -99.0;
        printf("      LP analysis->residual->synthesis, no grain movement: "
               "%.1f dB SNR\n", rt_snr);
        check(rt == 0, "LP-PSOLA: the fit is stable on the test signal");
        /* 25 dB is ~5% error. */
        check(rt_snr > 25.0,
              "LP-PSOLA: analysis/synthesis round-trip is faithful");

        set_env("SPFY_PSOLA_METHOD", "lp");
        memcpy(work, orig, N * sizeof *orig);
        spfy_prosody_warp_unit(work, N, periods, n_marks, tgt, 4.0f, SR);
        int lp_ran = memcmp(work, orig, N * sizeof *orig) != 0;
        set_env("SPFY_PSOLA_METHOD", NULL);
        check(lp_ran, "LP-PSOLA: the LP path actually ran");
        /* And it must not have quietly become plain TD: identical output
         * would pass the SNR bar while proving nothing. */
        set_env("SPFY_PSOLA_METHOD", "lp");
        memcpy(work, orig, N * sizeof *orig);
        spfy_prosody_warp_unit(work, N, periods, n_marks, tgt, 4.0f, SR);
        int16_t *lpbuf = malloc(N * sizeof *lpbuf);
        if (lpbuf) memcpy(lpbuf, work, N * sizeof *work);
        set_env("SPFY_PSOLA_METHOD", NULL);
        memcpy(work, orig, N * sizeof *orig);
        spfy_prosody_warp_unit(work, N, periods, n_marks, tgt, 4.0f, SR);
        check(lpbuf && memcmp(lpbuf, work, N * sizeof *work) != 0,
              "LP-PSOLA: differs from TD, i.e. did not fall back");
        free(lpbuf);
    }

    if (argc > 1) {
        spfy_pmarks_t pm;
        int rc = spfy_pmarks_load(argv[1], &pm);
        if (rc != SPFY_OK) {
            printf("  [SKIP] pmarks_load(%s) rc=%d\n", argv[1], rc);
        } else {
            printf("      pmarks: rate=%u n_units=%u n_data=%zu\n",
                   pm.rate, pm.n_units, pm.n_data);
            check(pm.rate == 8000, "pmarks: rate is 8000");
            check(pm.n_units == 169579u, "pmarks: 169579 units (Tom)");
            const int16_t *p = NULL;
            long total = 0, nz = 0;
            for (uint32_t u = 0; u < pm.n_units; ++u) {
                int c = spfy_pmarks_get(&pm, u, &p);
                total += c;
                if (c) ++nz;
            }
            printf("      marks total=%ld, units with marks=%ld\n", total, nz);
            check(total > 500000 && total < 1200000,
                  "pmarks: mark count in the expected band");
            spfy_pmarks_free(&pm);
        }
    } else {
        printf("  [SKIP] no pm stem given; pass one to test the loader\n");
    }

    /* 6. */
    {
        enum { NHP = 40 };
        uint32_t dur[NHP];
        uint8_t  acc[NHP];
        int8_t   typ[NHP], bt[NHP];
        for (int i = 0; i < NHP; ++i) {
            dur[i] = 400;
            acc[i] = 0; typ[i] = 0; bt[i] = 0;
        }
        acc[14] = acc[15] = 1;
        acc[22] = acc[23] = 1;
        acc[34] = acc[35] = 1;
        bt[35] = 1;

        spfy_contour_params_t p;
        spfy_contour_defaults(&p);
        spfy_contour_t c;
        int rc = spfy_contour_build(&c, &p, dur, NHP, acc, typ, NULL, NULL, bt, NULL, SR);
        check(rc == 0, "contour: builds from a halfphone timeline");
        check(c.n_acc == 3, "contour: three accent groups found");
        check(c.have_fall == 1, "contour: L-L% detected");

        double total = (double)NHP * 400.0;
        float f_start = spfy_contour_at(&c, 0.0, 118.0f);
        float f_ra    = spfy_contour_at(&c, c.n_acc ? c.pos[0] : 0.0, 118.0f);
        float f_end   = spfy_contour_at(&c, total, 118.0f);
        printf("      contour: start %.1f Hz, RA peak %.1f Hz, end %.1f Hz\n",
               f_start, f_ra, f_end);
        /* Threshold is 1.08 (~+1.3 st), not 1.15. */
        check(f_ra > f_start * 1.08,
              "contour: accent peak rises clearly above the onset");
        check(f_end < f_start,
              "contour: ends below the onset (declination + L-L%)");

        /* The failure this whole exercise exists to fix: the realised peak
         * used to land on "This" (10% of the phrase) instead of radar. */
        float best = 0.0f; double best_t = 0.0;
        for (double t = 0.0; t <= total; t += 200.0) {
            float f = spfy_contour_at(&c, t, 118.0f);
            if (f > best) { best = f; best_t = t; }
        }
        printf("      contour peak at %.0f%% of the phrase (radar is ~%.0f%%)\n",
               100.0 * best_t / total, 100.0 * c.pos[0] / total);
        check(fabs(best_t - c.pos[0]) < 2.0 * 400.0,
              "contour: global peak sits on the FIRST accent, not the onset");

        /* Zeroed parameters must give a flat contour -- the identity path
         * the byte-exact audit depends on. */
        spfy_contour_params_t z = p;
        z.accent_st = 0.0f; z.decl_st = 0.0f; z.fall_st = 0.0f;
        spfy_contour_t cz;
        spfy_contour_build(&cz, &z, dur, NHP, acc, typ, NULL, NULL, bt, NULL, SR);
        int flat = 1;
        for (double t = 0.0; t <= total; t += 400.0)
            if (fabs(spfy_contour_st_at(&cz, t)) > 1e-6) flat = 0;
        check(flat, "contour: zeroed params give exactly 0 st (identity)");
        spfy_contour_free(&cz);
        spfy_contour_free(&c);
    }

    /* ---- soft limiter -------------------------------------------------
     * These must FAIL against the old hard clamp, or they are not testing
     * anything: a hard clamp returns EXACTLY 2^(4/12) for any over-large
     * request, so... */
    {
        printf("\n-- soft pitch limiter --\n");
        const double mx = 4.0;
        const double lim = pow(2.0, mx / 12.0);
        const double big = pow(2.0, 8.0 / 12.0);

        double hard = spfy_prosody_limit_ratio(big, mx, 0);
        double softr = spfy_prosody_limit_ratio(big, mx, 1);
        check(fabs(hard - lim) < 1e-12,
              "limiter: hard mode clamps exactly at max_st");
        check(softr < lim - 1e-6,
              "limiter: soft mode stays STRICTLY inside max_st "
              "(fails against a hard clamp)");
        check(softr > pow(2.0, 2.5 / 12.0),
              "limiter: soft mode still realises most of a large request");

        check(spfy_prosody_limit_ratio(1.0, mx, 1) == 1.0,
              "limiter: soft mode is EXACTLY 1.0 at unity");
        check(spfy_prosody_limit_ratio(1.0, mx, 0) == 1.0,
              "limiter: hard mode is EXACTLY 1.0 at unity");

        /* Symmetry and monotonicity -- a limiter that reorders targets
         * would turn a falling contour into a non-monotone one. */
        double up = spfy_prosody_limit_ratio(big, mx, 1);
        double dn = spfy_prosody_limit_ratio(1.0 / big, mx, 1);
        check(fabs(up * dn - 1.0) < 1e-12,
              "limiter: soft mode is symmetric in the log domain");
        int mono = 1;
        double prev = 0.0;
        for (double st = -10.0; st <= 10.0; st += 0.25) {
            double v = spfy_prosody_limit_ratio(pow(2.0, st / 12.0), mx, 1);
            if (v <= prev) mono = 0;
            prev = v;
        }
        check(mono, "limiter: soft mode is strictly monotone");

        /* The measured phrase-final case: -3.92 st asked on the Indiana tail. */
        double tail = spfy_prosody_limit_ratio(pow(2.0, -3.92 / 12.0), mx, 1);
        double tail_st = 12.0 * log2(tail);
        printf("      phrase-final -3.92 st -> %.2f st\n", tail_st);
        check(tail_st > -3.3 && tail_st < -2.5,
              "limiter: the -3.92 st tail lands near -3 st");
    }

    free(orig); free(work); free(periods); free(tgt);
    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASS",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
