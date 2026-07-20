/* spfy_wsola -- streaming Hann OLA synth for unit-concat audio (M4).
 *
 * See wsola.h for the model. Single-precision throughout for the OLA
 * coefficients (the engine ran on x87 float; long double here would be
 * gratuitous and slower). Output samples are clamped to int16 range.
 */

#include "wsola.h"

#include <spfy/spfy.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Pre-computed Hann coefficients sized for the active OLA window. Re-
 * computed when ola_samples changes between streams. */
static float    g_hann_in [SPFY_WSOLA_OLA_SAMPLES_MAX];
static float    g_hann_out[SPFY_WSOLA_OLA_SAMPLES_MAX];
static uint32_t g_hann_ready_n = 0;

static void hann_init(uint32_t n)
{
    if (g_hann_ready_n == n) return;
    const float pi = 3.14159265358979323846f;
    for (uint32_t i = 0; i < n; ++i) {
        float t = (float)i / (float)n;
        float c = cosf(pi * t);
        g_hann_in [i] = 0.5f * (1.0f - c);
        g_hann_out[i] = 0.5f * (1.0f + c);
    }
    g_hann_ready_n = n;
}

#include <stdio.h>     /* env-var diagnostic */
static uint32_t parse_env_u32(const char *name, uint32_t def, uint32_t mx)
{
    const char *v = getenv(name);
    if (!v || !*v) return def;
    long n = strtol(v, NULL, 10);
    if (n <= 0) return def;
    if ((uint32_t)n > mx) return mx;
    return (uint32_t)n;
}

static int16_t clip_s16(float x)
{
    if (x >  32767.0f) return  32767;
    if (x < -32768.0f) return -32768;
    return (int16_t)lrintf(x);
}

/* Find the lag k in [0, MAX_LAG] that maximises the normalised
 * cross-correlation between the tail's overlap region and head[k..].
 *
 * Engine plain-WSOLA path (FUN_08EE1330 @ 0x08EE1330) scores each lag
 * as:
 *   ncc(k) = (sum_i tail[i] * head[i+k]) / sqrt(sum_i head[i+k]^2)
 *
 * The denominator makes the score amplitude-invariant: lags where the
 * new chunk is merely *louder* don't win over lags where the phase
 * matches better. This is the key quality difference vs a raw dot
 * product (our prior behaviour, which preferred high-energy lags on
 * voiced onsets and caused audible clicks at cross-recording joins).
 *
 * The engine's incremental-energy update (subtract leaving sample^2,
 * add entering sample^2) is preserved so the per-lag cost stays
 * O(1) after the first. SPFY_WSOLA_LAG_RAW=1 reverts to raw
 * correlation for A/B. */
static int32_t find_best_lag(const int16_t *tail, size_t tail_n,
                             const int16_t *head, size_t head_n,
                             uint32_t ola_samples, uint32_t max_lag)
{
    if (tail_n < ola_samples) return 0;
    int32_t lag_hi = (int32_t)max_lag;
    if ((int32_t)head_n < (int32_t)ola_samples + lag_hi)
        lag_hi = (int32_t)head_n - (int32_t)ola_samples;
    if (lag_hi < 0) return 0;

    const int16_t *t0 = tail + (tail_n - ola_samples);
    static int lag_raw = -1;
    if (lag_raw < 0)
        lag_raw = (getenv("SPFY_WSOLA_LAG_RAW") != NULL);

    /* Initial sums at lag 0. */
    long double he0 = 0.0L;  /* head[0..ola)^2 */
    long double cs0 = 0.0L;  /* tail . head[0..ola) */
    for (uint32_t i = 0; i < ola_samples; ++i) {
        long double h = (long double)head[i];
        he0 += h * h;
        cs0 += (long double)t0[i] * h;
    }
    /* Engine's epsilon floor on energy (avoid div-by-zero on near-
     * silent new chunk). 1.0L is conservative — matches our prior
     * "<= 1.0L treat as zero" semantics. */
    long double he_eps = 1.0L;

    int32_t    best_lag = 0;
    long double best_score = lag_raw
        ? cs0
        : (cs0 / sqrtl(he0 < he_eps ? he_eps : he0));
    long double he = he0;

    for (int32_t k = 1; k <= lag_hi; ++k) {
        /* Incremental energy update: leaving sample is head[k-1],
         * entering sample is head[k + ola_samples - 1]. */
        long double leaving = (long double)head[k - 1];
        long double entering = (long double)head[k + (int32_t)ola_samples - 1];
        he = he - leaving * leaving + entering * entering;

        long double cs = 0.0L;
        const int16_t *h0 = head + k;
        for (uint32_t i = 0; i < ola_samples; ++i) {
            cs += (long double)t0[i] * (long double)h0[i];
        }
        long double score = lag_raw
            ? cs
            : (cs / sqrtl(he < he_eps ? he_eps : he));
        if (score > best_score) {
            best_score = score;
            best_lag   = k;
        }
    }
    return (best_score > 0.0L) ? best_lag : 0;
}

void spfy_wsola_init(spfy_wsola_streamer_t *s, spfy_wav_writer_t *wav)
{
    memset(s, 0, sizeof *s);
    s->wav         = wav;
    s->ola_samples = parse_env_u32("SPFY_WSOLA_OLA",
                                   SPFY_WSOLA_OLA_SAMPLES_DEFAULT,
                                   SPFY_WSOLA_OLA_SAMPLES_MAX);
    s->max_lag     = parse_env_u32("SPFY_WSOLA_LAG",
                                   SPFY_WSOLA_MAX_LAG_DEFAULT,
                                   SPFY_WSOLA_MAX_LAG_MAX);
    if (getenv("SPFY_WSOLA_VERBOSE"))
        fprintf(stderr, "[wsola] ola=%u lag=%u\n", s->ola_samples, s->max_lag);
    hann_init(s->ola_samples);
}

/* Core OLA blend with caller-specified overlap length. eff_ola is the
 * effective overlap window in samples; can be > s->ola_samples for
 * PSOLA voiced joins (= 2 * T0) or == s->ola_samples for plain WSOLA.
 * After this returns, s->tail holds exactly s->ola_samples of new tail
 * regardless of eff_ola, so the next push sees a consistent state. */
static int do_ola_blend(spfy_wsola_streamer_t *s,
                        const int16_t *new_p, size_t new_n,
                        uint32_t eff_ola,
                        int energy_norm_on)
{
    if (eff_ola < 1) eff_ola = 1;
    if (eff_ola > SPFY_WSOLA_OLA_SAMPLES_MAX) eff_ola = SPFY_WSOLA_OLA_SAMPLES_MAX;
    if (new_n < eff_ola) {
        /* Not enough new content for full overlap; degrade to mix-into-
         * tail (matches the original short-chunk path). */
        size_t mix_n = new_n;
        for (size_t i = 0; i < mix_n; ++i) {
            float t = (float)s->tail[i];
            float h = (float)new_p[i];
            float wi = 0.5f - 0.5f * cosf(3.14159265358979323846f
                                          * (float)i / (float)mix_n);
            float v = t * (1.0f - wi) + h * wi;
            s->tail[i] = clip_s16(v);
        }
        return SPFY_OK;
    }

    /* Build a local Hann pair sized for eff_ola. For eff_ola ==
     * s->ola_samples this is identical to g_hann_in/out (which we
     * could reuse) but keeping a local copy avoids re-init churn when
     * PSOLA toggles per-push. eff_ola_max=320 -> stack buffers fine. */
    float hann_in [SPFY_WSOLA_OLA_SAMPLES_MAX];
    float hann_out[SPFY_WSOLA_OLA_SAMPLES_MAX];
    const float pi = 3.14159265358979323846f;
    for (uint32_t i = 0; i < eff_ola; ++i) {
        float t = (float)i / (float)eff_ola;
        float c = cosf(pi * t);
        hann_in [i] = 0.5f * (1.0f - c);
        hann_out[i] = 0.5f * (1.0f + c);
    }

    /* Tail may be shorter than eff_ola (transient state). Pad front
     * with zeros conceptually — the same convention as the plain-OLA
     * path. */
    size_t tail_have = s->tail_n;
    size_t tail_pad_lead = (tail_have < eff_ola)
                         ? (eff_ola - tail_have) : 0;

    /* Energy normalisation (matches engine amp_mods=1). On by default;
     * SPFY_WSOLA_NO_ENERGY_NORM disables. Computed over the overlap
     * window only — that's where amplitude jumps are most audible. */
    float new_scale = 1.0f;
    if (energy_norm_on) {
        long double te = 0.0L, he = 0.0L;
        for (uint32_t i = (uint32_t)tail_pad_lead; i < eff_ola; ++i) {
            float t = (float)s->tail[i - tail_pad_lead];
            te += (long double)t * (long double)t;
        }
        for (uint32_t i = 0; i < eff_ola; ++i) {
            float h = (float)new_p[i];
            he += (long double)h * (long double)h;
        }
        if (te > 1.0L && he > 1.0L) {
            new_scale = (float)sqrtl(te / he);
            if (new_scale < 0.5f) new_scale = 0.5f;
            if (new_scale > 2.0f) new_scale = 2.0f;
        }
    }

    /* Crossfade across eff_ola samples; emit immediately. */
    int16_t mix[SPFY_WSOLA_OLA_SAMPLES_MAX];
    for (uint32_t i = 0; i < eff_ola; ++i) {
        float t = (i < tail_pad_lead)
                  ? 0.0f
                  : (float)s->tail[i - tail_pad_lead];
        float h = (float)new_p[i] * new_scale;
        float v = t * hann_out[i] + h * hann_in[i];
        mix[i]  = clip_s16(v);
    }
    int rc = spfy_wav_write(s->wav, mix, eff_ola);
    if (rc != SPFY_OK) return rc;

    /* Body region: everything between the overlap and the next-tail. */
    int16_t scaled_buf[SPFY_WSOLA_OLA_SAMPLES_MAX * 8];
    const int16_t *body_src = new_p + eff_ola;
    uint32_t save_n = s->ola_samples;  /* Tail save uses default size for
                                          consistency with next push. */
    if (new_n > eff_ola + save_n) {
        size_t body = new_n - eff_ola - save_n;
        if (new_scale != 1.0f) {
            size_t cap = sizeof(scaled_buf) / sizeof(scaled_buf[0]);
            size_t emitted = 0;
            while (emitted < body) {
                size_t k = (body - emitted < cap) ? (body - emitted) : cap;
                for (size_t i = 0; i < k; ++i)
                    scaled_buf[i] = clip_s16(
                        (float)body_src[emitted + i] * new_scale);
                rc = spfy_wav_write(s->wav, scaled_buf, k);
                if (rc != SPFY_OK) return rc;
                emitted += k;
            }
        } else {
            rc = spfy_wav_write(s->wav, body_src, body);
            if (rc != SPFY_OK) return rc;
        }
        if (new_scale != 1.0f) {
            for (size_t i = 0; i < save_n; ++i)
                s->tail[i] = clip_s16(
                    (float)new_p[new_n - save_n + i] * new_scale);
        } else {
            memcpy(s->tail, new_p + new_n - save_n,
                   save_n * sizeof *new_p);
        }
        s->tail_n = save_n;
    } else if (new_n > eff_ola) {
        size_t held = new_n - eff_ola;
        if (held > save_n) {
            /* Hold only the most recent save_n. */
            const int16_t *src = body_src + (held - save_n);
            if (new_scale != 1.0f) {
                for (size_t i = 0; i < save_n; ++i)
                    s->tail[i] = clip_s16((float)src[i] * new_scale);
            } else {
                memcpy(s->tail, src, save_n * sizeof *src);
            }
            s->tail_n = save_n;
        } else {
            if (new_scale != 1.0f) {
                for (size_t i = 0; i < held; ++i)
                    s->tail[i] = clip_s16((float)body_src[i] * new_scale);
            } else {
                memcpy(s->tail, body_src, held * sizeof *body_src);
            }
            s->tail_n = held;
        }
    } else {
        s->tail_n = 0;
    }
    return SPFY_OK;
}

/* Internal entry — both push_unit and push_unit_psola route here.
 * nominal_n=0 means "use all of n" (legacy behaviour); nominal_n>0
 * means "the unit's intended output is exactly nominal_n samples;
 * use lag offset into the buffer and truncate to nominal_n". */
static int push_unit_impl(spfy_wsola_streamer_t *s,
                          const int16_t *samples, size_t n,
                          size_t nominal_n,
                          int align,
                          uint8_t f0_tail, uint8_t f0_head,
                          uint32_t sample_rate)
{
    s->n_pushes++;
    if (n == 0) return SPFY_OK;
    s->last_lag = 0;
    /* Clamp nominal_n to what the buffer can actually serve at lag=0.
     * If nominal_n > n, caller forgot to over-decode — fall back to
     * "use whole buffer" behaviour (no truncation). */
    if (nominal_n == 0 || nominal_n > n) nominal_n = n;

    /* No prior tail: write everything except the last OLA samples
     * (those become the new tail). Truncate to nominal_n so over-
     * decoded reservoir at the end isn't emitted (lag=0 for the first
     * push, so no shift needed). */
    if (s->tail_n == 0) {
        size_t use_n = nominal_n;
        if (use_n <= s->ola_samples) {
            /* Tiny chunk: hold it all as tail; flush will emit later. */
            memcpy(s->tail, samples, use_n * sizeof *samples);
            s->tail_n = use_n;
            return SPFY_OK;
        }
        size_t body = use_n - s->ola_samples;
        int rc = spfy_wav_write(s->wav, samples, body);
        if (rc != SPFY_OK) return rc;
        memcpy(s->tail, samples + body,
               s->ola_samples * sizeof *samples);
        s->tail_n = s->ola_samples;
        return SPFY_OK;
    }

    /* align==0 contract: caller guarantees this chunk is source-contiguous
     * with the previous one (same recording, lp adjacent). Crossfading
     * here would mix tail-region samples with non-overlapping source
     * samples OLA_SAMPLES ahead, producing audible low-pass artifacts at
     * every contiguous-span join. Pure concat is the correct behaviour:
     * write the held tail verbatim, then write the new chunk minus the
     * new tail.
     *
     * Bug fix 2026-05-11 — was previously Hann-blending here, causing
     * the audible discontinuities the user identified at align=0
     * boundaries inside text_016. */
    if (!align) {
        int rc = spfy_wav_write(s->wav, s->tail, s->tail_n);
        if (rc != SPFY_OK) return rc;
        size_t use_n = nominal_n;
        if (use_n <= s->ola_samples) {
            /* New chunk fits entirely in the tail-buffer; hold it. */
            memcpy(s->tail, samples, use_n * sizeof *samples);
            s->tail_n = use_n;
            return SPFY_OK;
        }
        size_t body = use_n - s->ola_samples;
        rc = spfy_wav_write(s->wav, samples, body);
        if (rc != SPFY_OK) return rc;
        memcpy(s->tail, samples + body, s->ola_samples * sizeof *samples);
        s->tail_n = s->ola_samples;
        return SPFY_OK;
    }

    /* Optional alignment search. Discards the first `lag` samples of
     * the new chunk (or virtually shifts back when lag<0; we implement
     * shift-forward only since shift-back would imply re-emitting tail
     * content that was already written for the previous unit). When
     * align is 0 OR lag<0 we keep lag=0. */
    int32_t lag = 0;
    long double lag_score = 0.0L, lag0_score = 0.0L;
    if (align && n >= s->ola_samples + s->max_lag) {
        /* Compute lag-0 correlation as the baseline reference */
        const int16_t *t0 = s->tail + (s->tail_n - s->ola_samples);
        for (uint32_t i = 0; i < s->ola_samples; ++i)
            lag0_score += (long double)t0[i] * (long double)samples[i];
        lag = find_best_lag(s->tail, s->tail_n, samples, n,
                            s->ola_samples, s->max_lag);
        if (lag < 0) lag = 0;
        s->last_lag = lag;
        if (lag != 0) s->n_aligned++;
        /* Recompute the score at the chosen lag for diagnostic */
        const int16_t *h0 = samples + lag;
        for (uint32_t i = 0; i < s->ola_samples; ++i)
            lag_score += (long double)t0[i] * (long double)h0[i];
    }
    /* Compute the normalised cross-correlation at the chosen lag. The
     * lag-search picks the MAX-NCC lag; if even that NCC is poor, the
     * tail and head are decorrelated and a full OLA blend over
     * ola_samples will introduce destructive interference where Hann's
     * sin² · head — cos² · tail terms cancel pointwise. The audible
     * symptom is the energy-dip / "hitch" the user identified:
     * speech amplitude drops 15-25 dB for ~10 ms at every poorly-
     * correlated join, then resumes. Empirically ~480 such dips across
     * a 60 s passage vs oracle's ~280.
     *
     * Fix: shorten eff_ola dramatically when NCC at chosen lag is
     * close to zero (signals are decorrelated). A 16-sample (2 ms)
     * micro-fade preserves a soft crossfade for click avoidance but
     * collapses the destructive-interference window. */
    double ncc_chosen = 0.0;
    if (align && s->tail_n >= s->ola_samples) {
        const int16_t *t0 = s->tail + (s->tail_n - s->ola_samples);
        long double te = 0.0L, he = 0.0L;
        for (uint32_t i = 0; i < s->ola_samples; ++i)
            te += (long double)t0[i] * (long double)t0[i];
        const int16_t *h0 = samples + lag;
        for (uint32_t i = 0; i < s->ola_samples; ++i)
            he += (long double)h0[i] * (long double)h0[i];
        double denom = sqrt((double)te) * sqrt((double)he);
        ncc_chosen = denom > 1.0 ? (double)lag_score / denom : 0.0;
        if (getenv("SPFY_WSOLA_VERBOSE")) {
            double ncc_zero = denom > 1.0 ? (double)lag0_score / denom : 0.0;
            fprintf(stderr,
                    "[wsola] push n=%4zu align=1 lag=%+4d  "
                    "ncc(chosen)=%+0.3f  ncc(lag0)=%+0.3f\n",
                    n, lag, ncc_chosen, ncc_zero);
        }
    }
    const int16_t *new_p = samples + lag;
    /* Duration-preserving truncation: each cross-rec join emits
     * exactly nominal_n samples worth of output, regardless of lag.
     * The over-decoded "look-ahead" samples (n - nominal_n at most)
     * provide the headroom for the lag shift; anything past nominal_n
     * after the shift is discarded. Without this, our output ran
     * cumulatively shorter than the engine's by `sum(lag_k)` samples,
     * causing audible time-compression on dense cross-rec join
     * clusters (e.g. "dog" at the end of the pangram — 5 cross-rec
     * joins in 250 ms compressing ~30 ms of output). */
    size_t available = (size_t)((int64_t)n - (int64_t)lag);
    size_t new_n = (nominal_n < available) ? nominal_n : available;

    /* PSOLA voiced-join decision. When the engine has Selective F0
     * smoothing enabled (mode 0 in FUN_08ee1160, default when f0tr is
     * loaded) AND both sides of the join are voiced, the overlap window
     * is grown to ≥ 1 pitch period each side. We mirror that here.
     *
     * eff_ola defaults to s->ola_samples. If both f0_tail and f0_head
     * are nonzero (voiced) and PSOLA isn't disabled, eff_ola becomes
     * max(s->ola_samples, 2 * T0) where T0 = sample_rate / avg_f0.
     * For Tom @ 8 kHz, avg_f0 ≈ 118 Hz → T0 ≈ 68 samples →
     * eff_ola = max(80, 136) = 136 samples (17 ms).
     *
     * SPFY_WSOLA_NO_PSOLA disables the widening (reverts to plain WSOLA).
     */
    uint32_t eff_ola = s->ola_samples;
    int psola_active = 0;
    /* PSOLA default-OFF as of 2026-05-19 evening.
     *
     * Tom-family voices run the engine's PLAIN WSOLA mode
     * (state+0x3614=1; FUN_08EE3AA0 mode=1), verified via Frida probe
     * in [[project-engine-wsola-mode-finding-2026-05-14]]. The
     * Selective-F0-smoothing branch (which is what `eff_ola = 2*T0`
     * mirrored) never fires for Tom. Empirically: PSOLA widening
     * generated ~240 extra mini-dips per 60 s of audio relative to
     * plain WSOLA (484 vs 247 dips at default settings, vs oracle's
     * 281), because the wider Hann window at decorrelated voiced
     * boundaries lets the sin²/cos² mix cancel pointwise over a 17 ms
     * window instead of 10 ms.
     *
     * Re-enable per-voice with SPFY_WSOLA_PSOLA=1 (for voices that
     * actually run the engine's PSOLA branch — verify with the
     * wsola_unit_probe Frida hook first). */
    static int psola_enabled = -1;
    if (psola_enabled < 0) {
        const char *e = getenv("SPFY_WSOLA_PSOLA");
        if (e) {
            psola_enabled = atoi(e) ? 1 : 0;
        } else if (getenv("SPFY_WSOLA_NO_PSOLA")) {
            psola_enabled = 0;       /* legacy override, redundant now */
        } else {
            psola_enabled = 0;       /* new default */
        }
    }
    if (psola_enabled && f0_tail > 0 && f0_head > 0 && sample_rate > 0) {
        uint32_t avg_f0 = ((uint32_t)f0_tail + (uint32_t)f0_head + 1u) >> 1;
        if (avg_f0 >= 50 && avg_f0 <= 400) {   /* sane human-pitch range */
            uint32_t T0 = sample_rate / avg_f0;
            uint32_t want = 2u * T0;
            if (want > eff_ola) eff_ola = want;
            if (eff_ola > SPFY_WSOLA_OLA_SAMPLES_MAX)
                eff_ola = SPFY_WSOLA_OLA_SAMPLES_MAX;
            psola_active = (eff_ola > s->ola_samples);
        }
    }
    if (getenv("SPFY_WSOLA_VERBOSE") && psola_active) {
        fprintf(stderr,
                "[wsola] psola f0_tail=%u f0_head=%u eff_ola=%u (default=%u)\n",
                f0_tail, f0_head, eff_ola, s->ola_samples);
    }

    /* Low-NCC short-blend fallback. When the chosen lag's NCC is poor
     * (decorrelated tail vs head), collapse eff_ola to a 2 ms micro-
     * fade to avoid destructive-interference dips. Threshold of 0.2
     * keeps full blend on clearly-correlated joins (~70% of joins on
     * the Tom corpus) and uses the short fallback on the rest.
     * SPFY_WSOLA_LOW_NCC=<float> overrides; <=-1 disables. */
    static double low_ncc_thresh = -2.0;
    if (low_ncc_thresh < -1.5) {
        const char *e = getenv("SPFY_WSOLA_LOW_NCC");
        low_ncc_thresh = e ? atof(e) : 0.2;
    }
    if (align && low_ncc_thresh > -1.0 && ncc_chosen < low_ncc_thresh) {
        /* Micro-fade: 2 ms = 16 samples @ 8 kHz. Keep ≥ 8 samples for
         * any sane sample rate. Applies to both plain WSOLA and the
         * PSOLA-widened branch — many decorrelated joins occur at
         * voiced→unvoiced transitions where PSOLA still fires on the
         * voiced tail's f0 but the head is silence-bound. Don't shrink
         * past what eff_ola already is, so configured smaller OLAs
         * honour their setting. */
        uint32_t micro = sample_rate / 500;     /* 2 ms */
        if (micro < 8) micro = 8;
        if (micro < eff_ola) eff_ola = micro;
    }

    /* Energy normalisation default-OFF (2026-05-14 evening).
     *
     * Previously default-on as a defensive "match cross-rec volume"
     * step. WAV analysis at t=1080-1100ms in the Tom pangram (after
     * "jumps→over" join, file=4128 with prev=file=1019 high-rms)
     * showed energy_norm's 2.0x clamp doubling natural voiced peaks
     * past int16 range, producing hard-clipped samples (±32767) and
     * audible saturation buzz the user identified as "stuttery middle
     * region". Engine plain-WSOLA has no equivalent step.
     *
     * SPFY_WSOLA_ENERGY_NORM=1 explicitly enables. */
    static int energy_norm_on = -1;
    if (energy_norm_on < 0)
        energy_norm_on = (getenv("SPFY_WSOLA_ENERGY_NORM") != NULL)
            && (getenv("SPFY_WSOLA_NO_ENERGY_NORM") == NULL);

    return do_ola_blend(s, new_p, new_n, eff_ola, energy_norm_on);
}

int spfy_wsola_push_unit(spfy_wsola_streamer_t *s,
                         const int16_t *samples, size_t n,
                         int align)
{
    /* Plain WSOLA: no F0 info, no over-decode, so PSOLA path never
     * activates and nominal_n=0 means "use all of n" (legacy). */
    return push_unit_impl(s, samples, n, 0, align, 0, 0, 0);
}

int spfy_wsola_push_unit_psola(spfy_wsola_streamer_t *s,
                               const int16_t *samples, size_t n,
                               size_t nominal_n,
                               int align,
                               uint8_t f0_tail, uint8_t f0_head,
                               uint32_t sample_rate)
{
    return push_unit_impl(s, samples, n, nominal_n, align,
                          f0_tail, f0_head, sample_rate);
}

int spfy_wsola_flush(spfy_wsola_streamer_t *s)
{
    if (s->tail_n == 0) return SPFY_OK;
    int rc = spfy_wav_write(s->wav, s->tail, s->tail_n);
    s->tail_n = 0;
    return rc;
}
