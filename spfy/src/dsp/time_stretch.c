/* WSOLA time-stretch. See time_stretch.h for the high-level contract.
 *
 * Constants tuned for the engine's native 8 kHz 16-bit mono output:
 *
 *   frame_len = 256 samples (32 ms)  — long enough for one period of
 *                                       even the deepest male voiced
 *                                       speech (~50 Hz F0).
 *   hop_len   = 64 samples (8 ms)    — gives Hann COLA at OLA factor 4.
 *   search_n  = 50 samples           — +-6 ms window for NCC alignment.
 *
 * For other sample rates we proportionally scale. */

#include "time_stretch.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TS_FRAME_LEN   256
#define TS_HOP         64
#define TS_SEARCH      50
#define TS_HOP_OUT     TS_HOP    /* output advance per frame is fixed */

static float *hann_window(int n)
{
    float *w = (float *)malloc((size_t)n * sizeof(float));
    if (!w) return NULL;
    for (int i = 0; i < n; ++i) {
        w[i] = 0.5f - 0.5f * cosf((float)(2.0 * M_PI) * (float)i
                                  / (float)(n - 1));
    }
    return w;
}

/* Normalised cross-correlation between two short windows. Used to find
 * the input offset (within +- search) whose first `tail_n` samples
 * align best with the previous output frame's tail. */
static int best_align(const int16_t *in, int n_in, int center,
                      const int16_t *tail, int tail_n, int search)
{
    int best = center;
    double best_score = -1e30;
    for (int off = -search; off <= search; ++off) {
        int pos = center + off;
        if (pos < 0 || pos + tail_n > n_in) continue;
        double s = 0.0, e1 = 0.0, e2 = 0.0;
        for (int i = 0; i < tail_n; ++i) {
            double a = (double)in[pos + i];
            double b = (double)tail[i];
            s  += a * b;
            e1 += a * a;
            e2 += b * b;
        }
        double denom = sqrt((e1 + 1e-9) * (e2 + 1e-9));
        double ncc = s / denom;
        if (ncc > best_score) { best_score = ncc; best = pos; }
    }
    return best;
}

int spfy_time_stretch_block(const int16_t *in, size_t n_in,
                            int16_t **out, size_t *out_n,
                            float factor, int sample_rate)
{
    if (!in || !out || !out_n || n_in == 0) return -1;
    if (factor < 0.25f) factor = 0.25f;
    if (factor > 4.0f)  factor = 4.0f;

    /* Pass-through fast path. */
    if (fabsf(factor - 1.0f) < 0.001f) {
        int16_t *cp = (int16_t *)malloc(n_in * sizeof(int16_t));
        if (!cp) return -1;
        memcpy(cp, in, n_in * sizeof(int16_t));
        *out = cp; *out_n = n_in; return 0;
    }

    int frame_len = TS_FRAME_LEN;
    int hop_in    = (int)((float)TS_HOP * factor + 0.5f);
    int hop_out   = TS_HOP;
    int search    = TS_SEARCH;
    if (hop_in < 1) hop_in = 1;

    /* Scale to non-8 kHz rates. */
    float sr_scale = (float)sample_rate / 8000.0f;
    frame_len = (int)((float)frame_len * sr_scale + 0.5f);
    hop_in    = (int)((float)hop_in    * sr_scale + 0.5f);
    hop_out   = (int)((float)hop_out   * sr_scale + 0.5f);
    search    = (int)((float)search    * sr_scale + 0.5f);
    if (hop_in  < 1) hop_in  = 1;
    if (hop_out < 1) hop_out = 1;
    if (frame_len < 32) frame_len = 32;

    float *win = hann_window(frame_len);
    if (!win) return -1;

    size_t out_cap = (size_t)((double)n_in / (double)factor + (double)frame_len + 16.0);
    double *acc = (double *)calloc(out_cap, sizeof(double));
    double *wgt = (double *)calloc(out_cap, sizeof(double));
    int16_t *tail = (int16_t *)malloc((size_t)frame_len * sizeof(int16_t));
    if (!acc || !wgt || !tail) {
        free(win); free(acc); free(wgt); free(tail);
        return -1;
    }

    /* First frame: copy from start without alignment search. */
    int in_pos = 0;
    int out_pos = 0;
    for (int i = 0; i < frame_len; ++i) {
        if ((size_t)(out_pos + i) >= out_cap) break;
        if (in_pos + i >= (int)n_in) break;
        double w = (double)win[i];
        acc[out_pos + i] += (double)in[in_pos + i] * w;
        wgt[out_pos + i] += w;
    }
    /* Tail of previous synthesis frame, used to align the next input
     * frame: last (frame_len - hop_out) samples of the output frame
     * we just laid down. */
    int tail_n = frame_len - hop_out;
    for (int i = 0; i < tail_n; ++i) {
        tail[i] = in[in_pos + hop_out + i];
    }
    out_pos += hop_out;
    in_pos  += hop_in;

    while (in_pos + frame_len < (int)n_in
           && (size_t)(out_pos + frame_len) < out_cap) {
        /* Search around `in_pos` for the best alignment with `tail`. */
        int aligned = best_align(in, (int)n_in, in_pos, tail, tail_n, search);
        for (int i = 0; i < frame_len; ++i) {
            double w = (double)win[i];
            acc[out_pos + i] += (double)in[aligned + i] * w;
            wgt[out_pos + i] += w;
        }
        for (int i = 0; i < tail_n; ++i) {
            tail[i] = in[aligned + hop_out + i];
        }
        out_pos += hop_out;
        in_pos  += hop_in;
    }

    /* Normalize. */
    size_t real_out_n = (size_t)out_pos;
    int16_t *o = (int16_t *)malloc(real_out_n * sizeof(int16_t));
    if (!o) {
        free(win); free(acc); free(wgt); free(tail);
        return -1;
    }
    for (size_t i = 0; i < real_out_n; ++i) {
        double v;
        if (wgt[i] > 1e-3) {
            v = acc[i] / wgt[i];
        } else {
            v = 0.0;
        }
        if (v >  32767.0) v =  32767.0;
        if (v < -32768.0) v = -32768.0;
        o[i] = (int16_t)v;
    }
    free(win); free(acc); free(wgt); free(tail);
    *out = o;
    *out_n = real_out_n;
    return 0;
}
