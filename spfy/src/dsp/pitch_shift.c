/* TD-PSOLA pitch shift. See pitch_shift.h for the high-level contract.
 *
 * Implementation notes:
 *
 *   Frame size is 256 samples (32 ms @ 8 kHz); hop is 80 (10 ms). Period
 *   search range is 20..160 samples (200..50 Hz F0 @ 8 kHz). Voicing
 *   threshold: autocorr peak / lag-0 must exceed 0.35 — below that, we
 *   treat the frame as unvoiced and use a pseudo-period of 80 samples
 *   so the OLA still reconstructs the waveform smoothly.
 *
 *   Pitch marks are placed at signed-peak extrema within each period.
 *   The "signed peak" rule (max of x and -x separately, take whichever
 *   is larger in absolute value within a period window) gives stable
 *   glottal-closure-instant approximations for voiced speech and is
 *   robust enough for unvoiced/silence pass-through.
 *
 *   Resynthesis walks the input pitch-mark list at a rate of
 *   period / beta in output-time, where beta = 2^(semitones/12). When
 *   beta > 1 (pitch up) consecutive output marks are closer together
 *   than the input's, so successive grains overlap more and the same
 *   input grain is copied multiple times. When beta < 1 (pitch down),
 *   output marks are spaced wider and some input grains are skipped.
 *
 *   The Hann window normalizer accumulator avoids amplitude drift at
 *   pitch boundaries — output samples are divided by the sum of Hann
 *   weights that overlapped them, with a small epsilon floor so silence
 *   regions don't divide by zero.
 */

#include "pitch_shift.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Tuning constants. All sample units assume 8 kHz; we scale where the
 * caller passes a different rate. */
#define PS_FRAME_LEN          256
#define PS_HOP                 80
#define PS_MIN_LAG_8K          20      /* 400 Hz */
#define PS_MAX_LAG_8K         160      /* 50  Hz */
#define PS_VOICING_THRESHOLD   0.35f
#define PS_UNVOICED_PSEUDO     80      /* 10 ms grains for unvoiced */
#define PS_MAX_PERIOD         200      /* hard cap so 1 mark != huge window */
#define PS_SILENCE_RMS         50.0f   /* below this, no need to PSOLA */

/* Autocorrelation pitch detector on a frame. Returns the lag that
 * maximises sum(x[i]*x[i+lag]) within [min_lag, max_lag], plus the
 * normalised peak strength (peak / R(0)) as *out_strength. */
static int detect_period(const int16_t *frame, int n,
                         int min_lag, int max_lag,
                         float *out_strength)
{
    /* R(0): squared energy. Use double to avoid overflow. */
    double r0 = 1e-9;
    for (int i = 0; i < n; ++i) {
        double v = (double)frame[i];
        r0 += v * v;
    }
    int best_lag = min_lag;
    double best_r = -1e30;
    for (int lag = min_lag; lag <= max_lag; ++lag) {
        if (lag >= n) break;
        double r = 0.0;
        int N = n - lag;
        for (int i = 0; i < N; ++i) {
            r += (double)frame[i] * (double)frame[i + lag];
        }
        /* Length-normalised correlation: comparable across lags. */
        r *= (double)n / (double)N;
        if (r > best_r) { best_r = r; best_lag = lag; }
    }
    *out_strength = (float)(best_r / r0);
    return best_lag;
}

/* Build per-sample period array via piecewise-linear interpolation
 * between frame-center estimates. Also detects silence on a per-frame
 * basis (signaled via a 0 period in the resulting array). */
static int *build_period_table(const int16_t *in, size_t n_in,
                               int sample_rate, int *n_marks_hint)
{
    int *period = (int *)malloc(n_in * sizeof(int));
    if (!period) return NULL;
    int min_lag = (int)((float)PS_MIN_LAG_8K * (float)sample_rate / 8000.0f);
    int max_lag = (int)((float)PS_MAX_LAG_8K * (float)sample_rate / 8000.0f);
    if (min_lag < 4) min_lag = 4;
    if (max_lag >= PS_FRAME_LEN) max_lag = PS_FRAME_LEN - 1;

    /* Frame center positions. */
    int n_frames = ((int)n_in - PS_FRAME_LEN) / PS_HOP + 1;
    if (n_frames < 1) n_frames = 1;
    int *frame_periods = (int *)malloc((size_t)n_frames * sizeof(int));
    int *frame_centers = (int *)malloc((size_t)n_frames * sizeof(int));
    if (!frame_periods || !frame_centers) {
        free(period); free(frame_periods); free(frame_centers);
        return NULL;
    }

    int total_marks_est = 0;
    for (int fi = 0; fi < n_frames; ++fi) {
        int fs = fi * PS_HOP;
        int fe = fs + PS_FRAME_LEN;
        if (fe > (int)n_in) fe = (int)n_in;
        int len = fe - fs;

        /* Frame RMS — silence check. */
        double sumsq = 0.0;
        for (int i = fs; i < fe; ++i) {
            double v = (double)in[i];
            sumsq += v * v;
        }
        float rms = (float)sqrt(sumsq / (len > 0 ? len : 1));

        if (rms < PS_SILENCE_RMS) {
            frame_periods[fi] = 0;
        } else {
            float strength = 0.0f;
            int lag = detect_period(&in[fs], len, min_lag, max_lag, &strength);
            if (strength < PS_VOICING_THRESHOLD) {
                /* Unvoiced — use uniform pseudo-period. */
                frame_periods[fi] = PS_UNVOICED_PSEUDO;
            } else {
                frame_periods[fi] = lag;
            }
        }
        frame_centers[fi] = fs + PS_FRAME_LEN / 2;
        if (frame_periods[fi] > 0) {
            total_marks_est += PS_HOP / (frame_periods[fi] ? frame_periods[fi] : PS_HOP) + 1;
        }
    }

    /* Interpolate per-sample. Silence (period==0) propagates as 0 in
     * the output table — caller passes through verbatim. */
    int last_fi = 0;
    for (size_t i = 0; i < n_in; ++i) {
        /* Find bracketing frames. */
        while (last_fi + 1 < n_frames &&
               frame_centers[last_fi + 1] <= (int)i) {
            ++last_fi;
        }
        int fi0 = last_fi;
        int fi1 = (last_fi + 1 < n_frames) ? last_fi + 1 : last_fi;
        int p0 = frame_periods[fi0];
        int p1 = frame_periods[fi1];
        if (p0 == 0 || p1 == 0) {
            period[i] = (p0 == 0 ? p1 : p0);
            if (period[i] == 0) period[i] = 0; /* both silence */
        } else if (fi0 == fi1) {
            period[i] = p0;
        } else {
            int x0 = frame_centers[fi0], x1 = frame_centers[fi1];
            float t = (float)((int)i - x0) / (float)(x1 - x0);
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            period[i] = (int)((float)p0 + (float)(p1 - p0) * t + 0.5f);
        }
        if (period[i] > PS_MAX_PERIOD) period[i] = PS_MAX_PERIOD;
    }

    free(frame_periods);
    free(frame_centers);
    *n_marks_hint = total_marks_est + 16;
    return period;
}

/* Find the local signed-peak (max of |x|) within +/- search around
 * `center`, clamped to [0, n-1]. Returns the absolute index. */
static int find_local_peak(const int16_t *in, int n, int center, int search)
{
    int lo = center - search; if (lo < 0) lo = 0;
    int hi = center + search; if (hi >= n) hi = n - 1;
    int best = center;
    int best_amp = -1;
    for (int i = lo; i <= hi; ++i) {
        int a = in[i] < 0 ? -in[i] : in[i];
        if (a > best_amp) { best_amp = a; best = i; }
    }
    return best;
}

/* Place pitch marks at successive signed-peak extrema, period-spaced.
 * Silent regions get uniform marks (still need OLA anchors). Returns
 * the mark count. *out_marks and *out_periods are caller-freed. */
static int place_pitch_marks(const int16_t *in, size_t n_in,
                             const int *period_per_sample,
                             int **out_marks, int **out_periods,
                             int cap)
{
    int *marks   = (int *)malloc((size_t)cap * sizeof(int));
    int *periods = (int *)malloc((size_t)cap * sizeof(int));
    if (!marks || !periods) { free(marks); free(periods); return -1; }
    int n_marks = 0;

    /* Start at the first non-silent position, or at index 0 if all
     * silent (output then matches input verbatim). */
    int pos = 0;
    while (pos < (int)n_in && period_per_sample[pos] == 0) pos += PS_HOP;
    if (pos >= (int)n_in) pos = 0;

    while (pos < (int)n_in) {
        int p = period_per_sample[pos];
        if (p == 0) p = PS_UNVOICED_PSEUDO;

        /* Refine to signed-peak within +/- p/4. */
        int peak = find_local_peak(in, (int)n_in, pos, p / 4);

        if (n_marks >= cap) {
            cap *= 2;
            int *nm = (int *)realloc(marks,   (size_t)cap * sizeof(int));
            int *np = (int *)realloc(periods, (size_t)cap * sizeof(int));
            if (!nm || !np) {
                free(nm ? nm : marks); free(np ? np : periods);
                return -1;
            }
            marks = nm; periods = np;
        }
        marks[n_marks]   = peak;
        periods[n_marks] = p;
        ++n_marks;

        pos = peak + p;
    }

    *out_marks   = marks;
    *out_periods = periods;
    return n_marks;
}

/* Binary search: index of the input mark whose position is closest to
 * `target`. n_marks must be >= 1. */
static int nearest_mark(const int *marks, int n_marks, int target)
{
    int lo = 0, hi = n_marks - 1;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (marks[mid] < target) lo = mid + 1; else hi = mid;
    }
    if (lo > 0) {
        int d0 = marks[lo]     - target;     if (d0 < 0) d0 = -d0;
        int d1 = target - marks[lo - 1];     if (d1 < 0) d1 = -d1;
        if (d1 < d0) return lo - 1;
    }
    return lo;
}

int spfy_pitch_shift_block(const int16_t *in, size_t n_in,
                           int16_t *out,
                           float semitones, int sample_rate)
{
    if (!in || !out || n_in == 0) return -1;
    if (fabsf(semitones) < 0.01f) {
        memcpy(out, in, n_in * sizeof(int16_t));
        return 0;
    }
    float beta = powf(2.0f, semitones / 12.0f);
    if (beta < 0.25f) beta = 0.25f;
    if (beta > 4.0f)  beta = 4.0f;

    int marks_cap = 0;
    int *period_per_sample = build_period_table(in, n_in, sample_rate,
                                                 &marks_cap);
    if (!period_per_sample) return -1;

    int *in_marks = NULL, *in_periods = NULL;
    int n_marks = place_pitch_marks(in, n_in, period_per_sample,
                                     &in_marks, &in_periods, marks_cap);
    free(period_per_sample);
    if (n_marks < 1) {
        free(in_marks); free(in_periods);
        memcpy(out, in, n_in * sizeof(int16_t));
        return 0;
    }

    /* Accumulators in double for headroom across hundreds of OLA hits. */
    double *acc = (double *)calloc(n_in, sizeof(double));
    double *wgt = (double *)calloc(n_in, sizeof(double));
    if (!acc || !wgt) {
        free(acc); free(wgt); free(in_marks); free(in_periods);
        return -1;
    }

    /* Output mark walk. Step is in_period[i] / beta where i is the
     * nearest input mark to the current output position. */
    int out_pos = in_marks[0];
    int safety_iters = (int)n_in * 4 + 16;   /* prevent runaway loops */
    while (out_pos < (int)n_in && safety_iters-- > 0) {
        int mi = nearest_mark(in_marks, n_marks, out_pos);
        int im = in_marks[mi];
        int tau = in_periods[mi];
        if (tau < 2) tau = 2;

        /* Hann-windowed grain copy with the standard 2-period support.
         * w(j) = 0.5 * (1 - cos(pi * (j + tau) / tau)) for j in [-tau, tau]. */
        for (int j = -tau; j <= tau; ++j) {
            int si = im + j;
            int oi = out_pos + j;
            if (si < 0 || si >= (int)n_in) continue;
            if (oi < 0 || oi >= (int)n_in) continue;
            float win = 0.5f - 0.5f * cosf((float)M_PI * (float)(j + tau)
                                            / (float)tau);
            acc[oi] += (double)in[si] * (double)win;
            wgt[oi] += (double)win;
        }
        int step = (int)((float)tau / beta + 0.5f);
        if (step < 1) step = 1;
        out_pos += step;
    }

    /* Normalize. wgt floor protects silence regions from div-by-zero. */
    for (size_t i = 0; i < n_in; ++i) {
        double v;
        if (wgt[i] > 1e-3) {
            v = acc[i] / wgt[i];
        } else {
            /* Outside any grain — fall back to input, preserves any
             * residual leading/trailing silence. */
            v = (double)in[i];
        }
        if (v >  32767.0) v =  32767.0;
        if (v < -32768.0) v = -32768.0;
        out[i] = (int16_t)v;
    }

    free(acc); free(wgt);
    free(in_marks); free(in_periods);
    return 0;
}

int spfy_pitch_shift_inplace(int16_t *samples, size_t n,
                             float semitones, int sample_rate)
{
    int16_t *tmp = (int16_t *)malloc(n * sizeof(int16_t));
    if (!tmp) return -1;
    int rc = spfy_pitch_shift_block(samples, n, tmp, semitones, sample_rate);
    if (rc == 0) memcpy(samples, tmp, n * sizeof(int16_t));
    free(tmp);
    return rc;
}
