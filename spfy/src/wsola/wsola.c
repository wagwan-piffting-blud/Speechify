/* spfy_wsola -- streaming Hann OLA synth for unit-concat audio (M4). */

#include "wsola.h"
#include "env.h"

#include <spfy/spfy.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Pre-computed Hann coefficients sized for the active OLA window. */
static float    g_hann_in [SPFY_WSOLA_OLA_SAMPLES_MAX];
static float    g_hann_out[SPFY_WSOLA_OLA_SAMPLES_MAX];
static uint32_t g_hann_ready_n = 0;

/* Crossfade window. Named hann_* for history; the engine's is LINEAR.
 *
 * FUN_08ee11e0 @ 0x08EE11E0 builds a table of 2W floats (allocated W<<3
 * bytes) by simple accumulation:
 *
 *     step = 1.0f / W;  rising = 0.0f;  falling = 1.0f;
 *     for (i = 0; i < W; ++i) {
 *         win[i + W] = falling;   falling -= step;   // applied to the TAIL
 *         win[i]     = rising;    rising  += step;   // applied to the HEAD
 *     }
 *
 * and FUN_08ee1240 mixes out[i] = head[i]*win[i] + tail[i]*win[i+W].
 *
 * So it is a straight linear crossfade, not raised-cosine. We used Hann,
 * which is smoother but is not what the engine emits, and no amount of
 * matching constants elsewhere can close a sample gap while the window
 * shape differs.
 *
 * ⚠ ACCUMULATED, not recomputed per index. `1.0f/W` is inexact in binary
 * (W=80 -> 0.0125), so summing it W times does NOT give the same floats as
 * i*(1.0f/W), and the engine's rounding is the one we have to match. Keep
 * the accumulation.
 *
 * SPFY_WSOLA_HANN=1 restores the raised-cosine window for A/B. */
static void hann_init(uint32_t n)
{
    if (g_hann_ready_n == n) return;
    static int want_hann = -1;
    if (want_hann < 0) want_hann = (spfy_env("SPFY_WSOLA_HANN") != NULL);
    if (want_hann) {
        const float pi = 3.14159265358979323846f;
        for (uint32_t i = 0; i < n; ++i) {
            float t = (float)i / (float)n;
            float c = cosf(pi * t);
            g_hann_in [i] = 0.5f * (1.0f - c);
            g_hann_out[i] = 0.5f * (1.0f + c);
        }
    } else {
        float step = 1.0f / (float)n;
        float rising = 0.0f, falling = 1.0f;
        for (uint32_t i = 0; i < n; ++i) {
            g_hann_out[i] = falling;
            g_hann_in [i] = rising;
            falling -= step;
            rising  += step;
        }
    }
    g_hann_ready_n = n;
}

#include <stdio.h>
static uint32_t parse_env_u32(const char *name, uint32_t def, uint32_t mx)
{
    const char *v = spfy_env(name);
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

/* Engine rounding, FUN_08ee1240's epilogue.
 *
 * It does NOT round-to-nearest-even the way lrintf does. It adds 0.5 for a
 * positive value / subtracts 0.5 for a negative one and then calls the
 * truncating converter FUN_08ee8828 -- i.e. round-half-AWAY-from-zero. On a
 * u-law-decoded blend that is a +/-1 difference on a large fraction of
 * samples, so it decides byte-identity.
 *
 * Clamp bounds are the same two constants the engine compares against before
 * it ever reaches the rounding. */
static int16_t clip_s16_engine(float x)
{
    if (x >  32767.0f) return  32767;
    if (x < -32768.0f) return -32768;
    return (int16_t)(x >= 0.0f ? (x + 0.5f) : (x - 0.5f));
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
        lag_raw = (spfy_env("SPFY_WSOLA_LAG_RAW") != NULL);

    /* Engine's search STRIDE (FUN_08ee1330 reads it from this+0x28): 2 at
     * 8 kHz, 4 at 16 kHz - i.e. W/40, where W is the base window that also
     * gives max_lag. It applies in BOTH places:
     *
     *   - the lag loop advances by `step`, so the engine only ever evaluates
     *     EVEN lags at 8 kHz. We were stepping by 1 and could therefore pick
     *     an odd lag the engine never considers, which changes the join
     *     sample-for-sample and (once the truncation below is removed) the
     *     output length too;
     *   - the correlation sum itself is DECIMATED by the same stride, so the
     *     engine scores ~half as many products as a dense sum would.
     *
     * SPFY_WSOLA_LAG_STEP=1 restores the dense search for A/B. */
    static int32_t step_env = -1;
    if (step_env < 0) {
        const char *e = spfy_env("SPFY_WSOLA_LAG_STEP");
        step_env = (e && *e) ? (int32_t)strtol(e, NULL, 10) : 0;
        if (step_env < 0) step_env = 0;
    }
    int32_t step = step_env ? step_env : (int32_t)(max_lag / 40u);
    if (step < 1) step = 1;

    long double he0 = 0.0L;
    long double cs0 = 0.0L;
    for (uint32_t i = 0; i < ola_samples; i += (uint32_t)step) {
        long double h = (long double)head[i];
        he0 += h * h;
        cs0 += (long double)t0[i] * h;
    }
    /* Engine's epsilon floor on energy (avoid div-by-zero on near- silent
     * new chunk). */
    long double he_eps = 1.0L;

    int32_t    best_lag = 0;
    long double best_score = lag_raw
        ? cs0
        : (cs0 / sqrtl(he0 < he_eps ? he_eps : he0));
    /* ⚠ The energy is recomputed per lag rather than updated incrementally.
     * The engine keeps a running update (subtract the leaving sample, add the
     * entering one) but advances the cursor by `step`, so it accounts for only
     * ONE of the `step` samples that actually enter and leave - an
     * approximation baked into its own arithmetic. Recomputing is exact and
     * cheap at this size; matching the engine's approximation bit-for-bit is
     * left until something is shown to depend on it, rather than reproducing
     * a rounding quirk on speculation. */
    for (int32_t k = step; k <= lag_hi; k += step) {
        const int16_t *h0 = head + k;
        long double cs = 0.0L, he = 0.0L;
        for (uint32_t i = 0; i < ola_samples; i += (uint32_t)step) {
            long double h = (long double)h0[i];
            he += h * h;
            cs += (long double)t0[i] * h;
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

void spfy_wsola_mark_tail_synthetic(spfy_wsola_streamer_t *s)
{
    if (s) s->tail_synthetic = 1;
}

void spfy_wsola_mark_next_push_synthetic(spfy_wsola_streamer_t *s)
{
    if (s) s->next_push_synthetic = 1;
}

void spfy_wsola_set_gap_ola(spfy_wsola_streamer_t *s, uint32_t n)
{
    if (!s) return;
    if (n > SPFY_WSOLA_OLA_SAMPLES_MAX) n = SPFY_WSOLA_OLA_SAMPLES_MAX;
    s->gap_ola = n;
}

void spfy_wsola_request_tail_save(spfy_wsola_streamer_t *s, uint32_t n)
{
    if (!s) return;
    if (n > SPFY_WSOLA_OLA_SAMPLES_MAX) n = SPFY_WSOLA_OLA_SAMPLES_MAX;
    s->save_override = n;
}

/* How many samples of PRE-ROLL the next push's buffer carries before the
 * unit's own audio. */
void spfy_wsola_set_next_pre(spfy_wsola_streamer_t *s, uint32_t n)
{
    if (!s) return;
    if (n > SPFY_WSOLA_OLA_SAMPLES_MAX) n = SPFY_WSOLA_OLA_SAMPLES_MAX;
    s->next_pre = n;
}

void spfy_wsola_set_span_limit(spfy_wsola_streamer_t *s, size_t limit)
{
    if (s) s->span_limit = limit;
}

void spfy_wsola_init(spfy_wsola_streamer_t *s, spfy_wav_writer_t *wav)
{
    memset(s, 0, sizeof *s);
    s->wav         = wav;
    /* Time-scaling starts DISARMED and at unity, which is the engine's
     * default-rate state (this+0x2c == 0). */
    s->unit_scale  = 1.0f;
    s->frame_trace = (spfy_env("SPFY_WSOLA_FRAME_TRACE") != NULL);
    /* W is the ONE rate-dependent constant; hop/corr/frame/stride all
     * derive from it (see the geometry block in wsola.h). */
    s->ola_samples = parse_env_u32("SPFY_WSOLA_OLA",
                                   spfy_wsola_w_for_rate(
                                       wav ? wav->sample_rate : 8000u),
                                   SPFY_WSOLA_OLA_SAMPLES_MAX);
    s->max_lag     = parse_env_u32("SPFY_WSOLA_LAG",
                                   SPFY_WSOLA_MAX_LAG_DEFAULT,
                                   SPFY_WSOLA_MAX_LAG_MAX);
    /* ⚠ Reverted from 2*ola_samples. The engine's tail BUFFER is this+0xc =
     * 2W, but that is search history for the lag correlation, not held-back
     * output: its per-join duration loss measured on the output cursor is a
     * median of 16 samples (the lag), not 160. Holding 2W back cost ~120 too
     * many samples per join. The 162.7/join figure that motivated 2W was an
     * artefact of averaging the pau units' huge shortfall into the total. */
    s->tail_keep   = parse_env_u32("SPFY_WSOLA_TAIL_KEEP",
                                   s->ola_samples,
                                   SPFY_WSOLA_OLA_SAMPLES_MAX);
    if (s->tail_keep < s->ola_samples) s->tail_keep = s->ola_samples;

    /* Engine geometry. */
    s->W      = s->ola_samples;
    s->hop    = s->W >> 1;
    s->corr   = s->W * 2u;
    if (s->corr > SPFY_WSOLA_CORR_MAX) s->corr = SPFY_WSOLA_CORR_MAX;
    s->stride = parse_env_u32("SPFY_WSOLA_LAG_STEP", s->W / 40u, 64u);
    if (s->stride < 1u) s->stride = 1u;
    /* FUN_08ee11e0 builds both halves of a 2W table:
     *     step        = float32(1 / W)          FDIVR then FSTP dword
     *     table[i]    = rising,  from 0.0, += step
     *     table[W+i]  = falling, from 1.0, -= step
     *
     * ⚠ The running values live in x87 REGISTERS, so the accumulation is
     * 80-bit and only the STORE rounds to float32. Accumulating in float
     * instead rounds at every step; 1/80 is inexact in binary, so the two
     * tables drift apart in the last bits and the blend lands +/-1 on a
     * scattering of samples. */
    {
        float step = (float)(1.0L / (long double)s->W);
        long double rising = 0.0L, falling = 1.0L;
        for (uint32_t i = 0; i < s->W && i < SPFY_WSOLA_OLA_SAMPLES_MAX; ++i) {
            s->win_in [i] = (float)rising;
            s->win_out[i] = (float)falling;
            rising  += (long double)step;
            falling -= (long double)step;
        }
    }
    if (spfy_env("SPFY_WSOLA_VERBOSE"))
        fprintf(stderr, "[wsola] W=%u hop=%u corr=%u stride=%u\n",
                s->W, s->hop, s->corr, s->stride);
    hann_init(s->ola_samples);
}

/* Lag search, FUN_08ee1330.
 *
 * score(k) = sum_i hist[i]*head[k+i] / sqrt(sum_i head[k+i]^2), with BOTH
 * sums decimated by `stride` and i running over `corr` samples -- not over
 * the blend width. k runs from `stride` to W in steps of `stride`, with k=0
 * as the incumbent, so only EVEN lags are ever considered at 8 kHz.
 *
 * ⚠ The energy is updated incrementally by ONE leaving and ONE entering
 * sample while the cursor advances by `stride`, so it is an approximation of
 * the true windowed energy. That is the engine's arithmetic and it changes
 * which lag wins, so it is reproduced rather than corrected.
 *
 * ⚠ PRECISION IS ASYMMETRIC, and it decides ties. Read off 0x08ee1479..
 * 0x08ee1585: the running energy and each lag's correlation sum live in x87
 * REGISTERS for the whole scan -- never stored -- so both accumulate at 80
 * bits. The winner, though, is kept in MEMORY as a float32
 * (`fstp dword ptr [esp+0x14]`), and each candidate is compared against it
 * while still in the register:
 *
 *     fcom  dword ptr [esp+0x14]   ; 80-bit ST0 vs the float32 incumbent
 *     fnstsw ax / test ah, 0x41 / jne  -> update only when C0 and C3 are both
 *                                        clear, i.e. STRICTLY greater
 *
 * So the comparison is strict -- a genuine tie keeps the earlier lag, exactly
 * as this code always did -- but two lags that TIE IN FLOAT32 need not tie at
 * 80 bits. Computing throughout in float made them tie, and the earlier lag
 * then won a race the engine gives to the later one. jill nat_007 is that
 * case: lags 72 and 74 both score 16.19f over a near-silent window, the
 * engine takes 74, and the 2-sample difference propagates to the end of the
 * utterance.
 *
 * SPFY_WSOLA_LAG_F32=1 rounds every intermediate back to float32. */
static int32_t find_lag_engine(const int16_t *hist, size_t hist_n,
                               const int16_t *head, size_t head_n,
                               uint32_t W, uint32_t corr, uint32_t stride)
{
    static int f32 = -1;
    if (f32 < 0) f32 = (spfy_env("SPFY_WSOLA_LAG_F32") != NULL);
    #define LAG_RND(v) (f32 ? (long double)(float)(v) : (long double)(v))
    if (stride < 1u) stride = 1u;
    /* The engine reads a `frame` = corr + W window of head; if the buffer
     * is short, shrink the lag range rather than read past it. */
    int32_t lag_hi = (int32_t)W;
    if (head_n < (size_t)corr) return 0;
    if (head_n - corr < (size_t)lag_hi) lag_hi = (int32_t)(head_n - corr);
    if (lag_hi < (int32_t)stride) return 0;

    /* ⚠ The energy floor is 250.0f (_DAT_08ee92c8), NOT 1.0. It clamps the
     * denominator for low-energy windows, which is precisely the
     * silence-to-speech join at the head of every phrase -- guessing 1.0
     * there picked a different lag on most short texts. Read from the DLL,
     * not inferred. Initial accumulators are 0.0f (_DAT_08ee92a0). */
    const long double ENERGY_EPS = 250.0L;
    long double energy = 0.0L, cs = 0.0L;
    for (uint32_t i = 0; i < corr; i += stride) {
        long double h = (long double)head[i];
        long double t = (i < hist_n) ? (long double)hist[i] : 0.0L;
        energy = LAG_RND(energy + h * h);
        cs     = LAG_RND(cs + t * h);
    }
    float best = (float)(cs / sqrtl(energy < ENERGY_EPS ? ENERGY_EPS : energy));
    int32_t best_lag = 0;

    size_t cur = 0;
    for (int32_t k = (int32_t)stride; k <= lag_hi; k += (int32_t)stride) {
        long double leave = (long double)head[cur];
        long double enter = (long double)head[cur + corr];
        energy = LAG_RND(enter * enter + (energy - leave * leave));
        cur += stride;
        long double c = 0.0L;
        for (uint32_t i = 0; i < corr; i += stride) {
            long double t = (i < hist_n) ? (long double)hist[i] : 0.0L;
            c = LAG_RND(c + (long double)head[cur + i] * t);
        }
        long double score =
            c / sqrtl(energy < ENERGY_EPS ? ENERGY_EPS : energy);
        if (f32) score = (long double)(float)score;
        /* `best` is the float32 the engine keeps in memory; `score` is
         * still in the register. */
        if (score > (long double)best) { best = (float)score; best_lag = k; }
    }
    #undef LAG_RND
    return best_lag;
}

void spfy_wsola_set_time_scale(spfy_wsola_streamer_t *s, int on)
{
    if (s) s->tscale_on = on ? 1 : 0;
}

void spfy_wsola_set_unit_scale(spfy_wsola_streamer_t *s, float scale)
{
    if (!s) return;
    /* ⚠ This is NOT the [1/3, 3] rate clamp. The Guide's 33..300 limit
     * applies to the REQUESTED RATE, which is already enforced when the tag
     * is parsed; the per-unit scale is natural/target, and its baseline is
     * only near 1.0 on average -- a unit whose natural length happens to sit
     * well off its target starts from 1.4 or 0.7, so \!rp33 legitimately
     * asks for less than 1/3 on some units. Clamping at 1/3 here quietly
     * capped \!rp33 at 2.72x when it should reach ~3x. These bounds exist
     * only to keep a corrupt target from producing an absurd stretch. */
    if (!(scale > 0.0f)) scale = 1.0f;
    if (scale < 0.1f) scale = 0.1f;
    if (scale > 10.0f) scale = 10.0f;
    s->unit_scale = scale;
}

void spfy_wsola_set_unit_plan(spfy_wsola_streamer_t *s,
                              const spfy_wsola_unit_t *units, size_t n)
{
    if (!s) return;
    s->plan   = (n && units) ? units : NULL;
    s->plan_n = s->plan ? n : 0u;
}

int spfy_wsola_label_is_plosive(const char *label)
{
    if (!label || !*label) return 0;
    char c = label[0];
    if (c != 'p' && c != 'd' && c != 'g' && c != 'b'
        && c != 't' && c != 'k' && c != 'c' && c != 'j') return 0;
    if (strncmp(label, "pau", 3) == 0) return 0;
    if (strncmp(label, "dh",  2) == 0) return 0;
    if (strncmp(label, "th",  2) == 0) return 0;
    return 1;
}

float spfy_wsola_unit_scale(const char *label,
                            uint32_t natural_ms, float target_ms)
{
    if (!(target_ms > 0.0f) || natural_ms == 0u) return 1.0f;
    /* FUN_08ee1640 compares the TRUNCATED target against the natural length,
     * and only takes the pass-through branch when the unit would grow. */
    if ((int32_t)target_ms >= (int32_t)natural_ms
        && spfy_wsola_label_is_plosive(label)
        && strncmp(label, "pau", 3) != 0)
        return 1.0f;
    return (float)natural_ms / target_ms;
}

/* The time-scaled body, FUN_08ee36e0's `this+0x2c != 0` branch.
 *
 * Emits `out_n` samples starting from input position `*io_pos`, walking in
 * W-sample frames. Per frame the engine compares the position it WANTS
 *
 *     ideal = body_in0 + (int)((float)out_rel * scale)      [0x08ee3512]
 *
 * against where it actually is. While the two agree to within a hop the
 * frame is copied verbatim; once they diverge it runs the ordinary NCC lag
 * search around `ideal - W - hop` and crossfades into the frame it finds,
 * which is how material gets inserted (scale < 1) or dropped (scale > 1)
 * without a pitch discontinuity.
 *
 * SCOPE: the engine's frame loop is bounded by `this+0x35bc < this+0x35c0`,
 * which are a unit index and count WITHIN THE CURRENT SPAN -- FUN_08ee2960
 * sets `this+0x44 = span`, `0x35c0 = span[+0x24]`, and zeroes 0x35bc / 0x35a8
 * / 0x35b4 / 0x35e0 on every span load. The engine's outer array is
 * 0x2c-stride spans; the 0x30-stride units sit inside one. spfy's
 * append_recording_span IS that span, so this loop's scope already matches
 * the engine's and no restructuring is owed.
 *
 * ⚠ What is NOT yet ported is the engine's per-unit scale STEP inside a
 * span: FUN_08ee33f0 advances `0x35bc` mid-frame and re-runs FUN_08ee1640
 * for the new unit, emitting that straddling frame as two hops
 * (`this+0x3600`). spfy passes one aggregate scale per span instead, so the
 * stretch is spread evenly across a run rather than stepping at each
 * internal unit edge. Totals land in the same place; individual unit
 * boundaries inside a recording do not. Reaching byte-parity on the rate
 * path means porting that step -- and capturing a rate-tagged oracle to
 * check it against, since every trace in traces_master is default-rate. */
static int tscale_frame(spfy_wsola_streamer_t *s,
                        const int16_t *buf, size_t buf_n, size_t content_end,
                        size_t *io_rp, size_t ideal, size_t fr);

static int emit_body_tscale(spfy_wsola_streamer_t *s,
                            const int16_t *buf, size_t buf_n,
                            size_t content_end,
                            size_t *io_pos, size_t out_n, float scale)
{
    const size_t W = s->W;
    const size_t body_in0 = *io_pos;
    size_t rp = body_in0;
    size_t out_rel = 0;

    while (out_rel < out_n) {
        size_t fr = W;
        if (fr > out_n - out_rel) fr = out_n - out_rel;

        /* Same float32 -> truncate shape as the engine's FILD/FMUL/__ftol. */
        long adv = (long)((float)out_rel * scale);
        long ideal_l = (long)body_in0 + adv;
        if (ideal_l < 0) ideal_l = 0;

        int rc = tscale_frame(s, buf, buf_n, content_end, &rp, (size_t)ideal_l, fr);
        if (rc == -1) break;
        if (rc == -2) return SPFY_E_IO;
        out_rel += fr;
    }

    *io_pos = rp;
    return SPFY_OK;
}

/* One time-scaled frame. Shared by both walks so they cannot drift apart.
 * Returns the input position the frame was taken from; `*io_rp` is the
 * current input cursor and is left at src + W. */
static int tscale_frame(spfy_wsola_streamer_t *s,
                        const int16_t *buf, size_t buf_n, size_t content_end,
                        size_t *io_rp, size_t ideal, size_t fr)
{
    const size_t W = s->W, hop = s->hop, corr = s->corr;
    size_t rp = *io_rp;
    int16_t mix[SPFY_WSOLA_OLA_SAMPLES_MAX];
    const int16_t *frame;

    /* ⚠ The engine compares `ideal` against this+0x35ac, and FUN_08ee33f0
     * sets that to `W + read_pos` -- not read_pos. Comparing against the
     * bare cursor shifts every sync/resync decision by a whole window. */
    long delta = (long)ideal - (long)(rp + W);
    /* FUN_08ee36e0: `(iVar5 < iVar1) && (-iVar1 <= iVar5)`. The upper bound
     * is exclusive and the lower is INCLUSIVE -- an exclusive lower bound
     * forces a resync on the exact -hop boundary, which changes that frame's
     * source and every frame after it. */
    int in_sync = (delta < (long)hop && delta >= -(long)hop);
    /* The engine's ONLY bail-out: `this[0x35c4] < this[4] + iVar1 + iVar4`,
     * i.e. not enough input left to reach the frame it wants.
     *
     * ⚠ There was a second clause here, `rp + corr > buf_n`, guarding the
     * lag search's own reach. It has no counterpart in the engine and it
     * forced the sync branch exactly where the stretch direction spends its
     * last frames -- which is where \!rp33/50/75 diverged. The reach is
     * handled by zero-padding below instead, matching the zero-filled slack
     * FUN_08ee2960 allocates past this+0x35c4. */
    /* ⛔ NOT against content_end. Tried 2026-09-04 on the theory that
     * this+0x35c4 is the content end: the rate gate collapsed 34 -> 11/37,
     * with rp50/rp100/rp150 and both span entries losing byte-identity at
     * NCC 0.04-0.9. content_end runs 2W short of buf_n, and bailing that
     * early takes the copy branch all over the stretch direction. The
     * over-allocated, zero-filled buffer end is the right bound. */
    /* FUN_08ee36e0: `this[0x35c4] < this[4] + iVar1 + iVar4`, i.e.
     * `limit < W + hop + ideal` -- the engine's ONLY bail-out.
     *
     * ⚠ The bound is this+0x35c4, the span's INPUT LIMIT, which FUN_08ee2960
     * clamps to what the source recording holds; the BUFFER stays longer
     * than that and zero-filled. Using the buffer length made a span at the
     * end of a recording keep crossfading past the real audio.
     * ⛔ It is not content_end either: 0x35c4 is content_end + corr, and
     * bailing at content_end took the gate from 34 to 11/37. */
    size_t lim = content_end ? content_end : buf_n;
    int short_input = ((long)lim < (long)ideal + (long)W + (long)hop);

    if (in_sync || short_input) {
        if (rp + fr > buf_n) {
            /* ⚠ Do NOT bail. FUN_08ee2960 over-allocates the span buffer by
             * `iVar9 + 100` samples and zero-fills past the content, so the
             * engine simply reads into that slack. Stopping the walk here
             * instead dropped whole frames in the STRETCH direction, where
             * the walk legitimately outruns the decoded span -- measured as
             * \!rp50 landing exactly 80 samples (one frame) short and
             * \!rp33 three frames short. */
            size_t have = (rp < buf_n) ? buf_n - rp : 0u;
            if (have > fr) have = fr;
            if (have) memcpy(mix, buf + rp, have * sizeof *buf);
            if (have < fr) memset(mix + have, 0, (fr - have) * sizeof *mix);
            frame = mix;
        } else {
            frame = buf + rp;
        }
    } else {
        long base_l = (long)ideal - (long)W - (long)hop;
        size_t base = (base_l < 0) ? 0u : (size_t)base_l;
        /* Zero-pad both correlation inputs so the search always spans the
         * full [0, W]. FUN_08ee2960 zero-fills past this+0x35c4, so the
         * engine correlates against zeros out there; find_lag_engine instead
         * clamps lag_hi to head_n - corr and silently searches a shorter
         * range, which picks a different lag on the last frames of a
         * stretched span. */
        static int16_t hpad[SPFY_WSOLA_CORR_MAX
                            + SPFY_WSOLA_OLA_SAMPLES_MAX];
        static int16_t tpad[SPFY_WSOLA_CORR_MAX];
        const int16_t *hist_p = buf + rp;
        if (rp + corr > buf_n) {
            size_t have = (rp < buf_n) ? buf_n - rp : 0u;
            if (have > corr) have = corr;
            if (have) memcpy(tpad, buf + rp, have * sizeof *buf);
            memset(tpad + have, 0, (corr - have) * sizeof *tpad);
            hist_p = tpad;
        }
        const int16_t *head_p = buf + base;
        size_t head_n = (base < buf_n) ? buf_n - base : 0u;
        const size_t need = corr + W;
        if (head_n < need) {
            if (head_n) memcpy(hpad, buf + base, head_n * sizeof *buf);
            memset(hpad + head_n, 0, (need - head_n) * sizeof *hpad);
            head_p = hpad;
            head_n = need;
        }
        int32_t lag = find_lag_engine(hist_p, corr, head_p, head_n,
                                      (uint32_t)W, (uint32_t)corr, s->stride);
        size_t np = base + (size_t)((lag > 0) ? lag : 0);
        if (np + W > buf_n || rp + W > buf_n) {
            size_t have = (rp < buf_n) ? buf_n - rp : 0u;
            if (have > fr) have = fr;
            if (have) memcpy(mix, buf + rp, have * sizeof *buf);
            if (have < fr) memset(mix + have, 0, (fr - have) * sizeof *mix);
            frame = mix;
        } else {
            for (size_t j = 0; j < W; ++j) {
                long double t = (long double)buf[rp + j];
                long double h = (long double)buf[np + j];
                long double acc = h * (long double)s->win_in[j]
                                + t * (long double)s->win_out[j];
                mix[j] = clip_s16_engine((float)acc);
            }
            /* SPFY_WSOLA_FRAME_TRACE: one line per RESYNC frame, matching
             * viz/frida_hooks/wsola_frame_hook.js field for field so the two
             * can be diffed directly. */
            if (s->frame_trace)
                fprintf(stderr, "[wsolaf] rp=%zu np=%zu ideal=%zu lag=%d\n",
                        rp, np, ideal, (int)lag);
            frame = mix;
            rp = np;
            s->n_aligned++;
        }
    }
    if (spfy_wav_write(s->wav, frame, fr) != SPFY_OK) return -2;
    *io_rp = rp + W;
    return 0;
}

/* The per-unit walk: FUN_08ee36e0's time-scaled body with FUN_08ee33f0's
 * unit stepping, which is what the span-aggregate version could not express.
 *
 * Bases are per-span, matching FUN_08ee2960's reset of 0x35b4 / 0x35e0 /
 * 0x35d8. For unit i:
 *
 *     in_base_i  = pre + sum_{j<i} nat_j        (0x35d0, running 0x35d4)
 *     out_base_i =       sum_{j<i} tgt_j        (0x35d8)
 *     out_end_i  = out_base_i + tgt_i           (0x35dc)
 *     scale_i    = nat_i / tgt_i                (0x35e4, via FUN_08ee1640)
 *
 * and the frame's wanted input position is FUN_08ee33f0 @ 0x08ee3512:
 *
 *     ideal = in_base_i + (int)((float)(out_pos - out_base_i) * scale_i)
 *
 * ⚠ The engine ALSO splits the frame that straddles a unit boundary into two
 * hops with the advance between them (`this+0x3600`). That is not reproduced
 * and does not need to be: both halves come from the same frame buffer, so
 * the SAMPLES are identical either way -- the split exists so FUN_08ee2e70
 * can fire its boundary notification at the right output offset. */
static int emit_body_tscale_plan(spfy_wsola_streamer_t *s,
                                 const int16_t *buf, size_t buf_n,
                                 size_t content_end,
                                 size_t *io_pos, size_t pre, size_t in_base0,
                                 uint32_t sps)
{
    const size_t W = s->W, hop = s->hop;
    const spfy_wsola_unit_t *pl = s->plan;
    const size_t n = s->plan_n;
    if (n == 0 || sps == 0) return SPFY_OK;

    /* Read straight off FUN_08ee3560's tail, which is the ONLY place the
     * join's contribution to the span budget is written down:
     *
     *     this+0x35d0 = hop + lag                   in_base for unit 0
     *     FUN_08ee1640(this, (0x35d4 - 0x35d0) >> shift)
     *     FUN_08ee33f0(this, 0x35d0 + hop, hop)     read_pos, OUT_POS = hop
     *
     * Two things fall out and both were wrong here before:
     *
     *  1. The join emits W samples but advances the output cursor by only
     *     `hop`. Counting W against the span's target over-deducts; counting
     *     zero under-deducts. The first cost ~3% duration at every rate; the
     *     second left ~9% of the output unscaled, which read as a slope
     *     (long when fast, short when slow) pivoting near \!rp90.
     *
     *  2. Unit 0's natural length is REDUCED to (0x35d4 - 0x35d0), i.e. by
     *     however much input the join already consumed. Its TARGET is not
     *     reduced, so unit 0 legitimately scales harder than its neighbours.
     *
     * Units after the first keep their true buffer positions: 0x35d4 is the
     * running base FUN_08ee2960 seeded at `pre`, and the join does not move
     * it. */
    /* Unit 0's natural is the span's own, less whatever input the join
     * already consumed -- FUN_08ee3560 passes (0x35d4 - 0x35d0) >> shift.
     * Its TARGET is not reduced, so unit 0 legitimately scales harder. */
    size_t in_base = in_base0;
    size_t next_base = pre + (size_t)pl[0].nat_ms * sps;      /* 0x35d4 */
    size_t nat_smp = (next_base > in_base) ? next_base - in_base : 1u;
    uint32_t nat_ms = (uint32_t)(nat_smp / sps);
    if (nat_ms == 0u) nat_ms = 1u;

    size_t rp = *io_pos;
    size_t out_pos = hop;          /* FUN_08ee33f0(this, ..., hop) */
    size_t ui = 0;
    size_t out_base = 0;
    float acc_ms = 0.0f;

    /* FUN_08ee1640, with the length the engine actually passes it. */
#define SPFY_UNIT_STEP(idx, neff)                                              do {                                                                           float _t = pl[idx].tgt_ms;                                                 if ((int32_t)_t >= (int32_t)(neff) && pl[idx].plosive) {                       cur_scale = 1.0f;  acc_ms += (float)(neff);                            } else {                                                                       cur_scale = (_t > 0.0f) ? (float)(neff) / _t : 1.0f;                       acc_ms += _t;                                                          }                                                                          out_end = (size_t)((float)sps * acc_ms);                               } while (0)

    float cur_scale = 1.0f;
    size_t out_end = 0;
    SPFY_UNIT_STEP(0, nat_ms);

    for (;;) {
        /* ⚠ ADVANCE BEFORE EMIT. FUN_08ee33f0 runs its unit-advance at the
         * end of the join/prologue, i.e. BEFORE the body loop, and
         * FUN_08ee36e0 guards its do-while with `if (0x35bc < 0x35c0)`. A
         * span whose first unit already satisfies the test therefore emits
         * ZERO body frames. Emitting first and checking after gets every
         * multi-frame span right and every zero-frame span wrong by exactly
         * one W -- measured as +80 on each of the two leading pau spans. */
        while (out_end <= out_pos + W) {
            if (++ui >= n) goto done;
            out_base   = out_end;
            in_base    = next_base;
            next_base += (size_t)pl[ui].nat_ms * sps;
            nat_ms     = pl[ui].nat_ms ? pl[ui].nat_ms : 1u;
            SPFY_UNIT_STEP(ui, nat_ms);
        }
        /* ⚠ WHOLE frames only, and the advance test is
         * `out_end <= out_pos + W` -- FUN_08ee33f0's
         * `if (this[0x35dc] <= iVar2)` with iVar2 = W + out_pos. Together
         * they stop the body ONE FRAME SHORT of out_end, which is why the
         * engine's per-span emission is always hop + k*W for integer k.
         * Emitting a partial final frame to "reach" out_end instead adds a
         * few samples per span and no amount of target tuning recovers it.
         *
         * Verified against the engine's own cursor (this+0x35f4, captured
         * per span via FUN_08ee1700) on all 19 spans of
         * "The national weather service has issued a warning." at \!rp100. */
        /* FUN_08ee33f0 @ 0x08ee3512, operand for operand:
         *
         *     ideal = in_base + (int)((float)(0x35b8 - out_base) * scale)
         *
         * and 0x35b8 is `W + out_pos`, NOT out_pos. Dropping that W leaves
         * every span the right LENGTH -- the frame count is set by out_end,
         * which does not involve it -- while sourcing each frame a window
         * early, so the samples differ with the durations exact. */
        /* FUN_08ee1640 stores the scale as a FLOAT32 (FILD / FDIV dword /
         * FSTP dword), then FUN_08ee33f0 multiplies by it on the x87 stack
         * in 80-bit and truncates once at the __ftol. Reproduce both halves:
         * scale narrowed to float, the product widened to long double.
         * Under -fexcess-precision=standard a plain float multiply rounds
         * the product too, which is one rounding more than the engine does. */
        long adv = (long)((long double)(out_pos + W - out_base)
                          * (long double)cur_scale);
        long ideal_l = (long)in_base + adv;
        if (ideal_l < 0) ideal_l = 0;
        /* The inputs to `ideal`, so a one-sample miss can be attributed to
         * the scale, the bases or the truncation rather than guessed at.
         * wsola_frame_hook.js reports scale and out_pos for the same frame. */
        if (s->frame_trace)
            fprintf(stderr, "[wsolab] out_pos=%zu out_base=%zu in_base=%zu "
                    "unit=%zu scale=%.9g adv=%ld ideal=%ld\n",
                    out_pos, out_base, in_base, ui, (double)cur_scale,
                    adv, ideal_l);

        int rc = tscale_frame(s, buf, buf_n, content_end, &rp, (size_t)ideal_l, W);
        if (rc == -1) break;                   /* ran out of input */
        if (rc == -2) return SPFY_E_IO;
        out_pos += W;
    }
done:
#undef SPFY_UNIT_STEP
    *io_pos = rp;
    return SPFY_OK;
}

int spfy_wsola_push_engine(spfy_wsola_streamer_t *s,
                           const int16_t *buf, size_t buf_n,
                           size_t pre, size_t content_n)
{
    if (!s || !buf || buf_n == 0) return SPFY_OK;
    s->n_pushes++;
    s->engine_mode = 1;
    const size_t W = s->W, hop = s->hop, corr = s->corr;
    /* SPFY_WSOLA_TRACE: one line per push carrying the OUTPUT CURSOR, so a
     * first-divergence sample index from audio_compare maps to a specific
     * join without re-deriving the emission arithmetic by hand. */
    static int trace = -1;
    if (trace < 0) trace = (spfy_env("SPFY_WSOLA_TRACE") != NULL);
    size_t out0 = s->wav ? (size_t)s->wav->n_samples_written : 0u;
    /* SPFY_WSOLA_DUMP_JOIN=<path>: the join's two inputs, per push, so the
     * engine's chosen lag can be SOLVED for offline (try every candidate k,
     * see which reproduces the reference's next W samples) rather than
     * inferred... */
    static const char *dump_path = NULL;
    static int dump_looked = 0;
    if (!dump_looked) {
        dump_looked = 1;
        dump_path = spfy_env("SPFY_WSOLA_DUMP_JOIN");
    }
    if (dump_path) {
        FILE *df = fopen(dump_path, "ab");
        if (df) {
            uint32_t h[9];
            h[0] = 0x314a5753u;
            h[1] = (uint32_t)(s->n_pushes - 1u);
            h[2] = (uint32_t)s->hist_n;
            h[3] = (uint32_t)pre;
            h[4] = (uint32_t)content_n;
            h[5] = (uint32_t)buf_n;
            h[6] = (uint32_t)corr;
            h[7] = (uint32_t)W;
            h[8] = (uint32_t)out0;
            fwrite(h, sizeof h, 1, df);
            fwrite(s->hist, sizeof *s->hist, s->hist_n, df);
            fwrite(buf, sizeof *buf, buf_n, df);
            fclose(df);
        }
    }

    size_t content_end = pre + content_n;          /* this+0x35cc */
    if (content_end > buf_n) content_end = buf_n;

    size_t read_pos;                               /* this+0x35a8 */
    if (s->hist_n == 0) {
        /* First unit: no join. FUN_08ee3670 emits `hop` samples from
         * buf[this+0x35d0] (= buf[pre]) and THEN sets the body read position
         * to pre + hop.
         *
         * ⚠ Those are two separate emissions, not one contiguous
         * buf[pre .. stop). They coincide whenever the body is non-empty --
         * which is why treating them as one write was byte-exact on tom --
         * but the prologue is UNCONDITIONAL while the body is guarded by
         * `stop > read_pos`. A phrase whose first unit is shorter than
         * 2*hop has no body at all, and the collapsed form then emitted
         * nothing where the engine emits `hop`.
         *
         * Measured on paulina es_042 push 16 (a uid-0 pau, content = 40 =
         * hop): the engine emits 41 verbatim samples of that buffer, which
         * decomposes as 40 of prologue plus the next join's blend sample 0
         * (= hist[0] = buf[pre + hop], since win_in[0] is 0). Both halves
         * of that only line up under this reading. */
        static int no_first_hop = -1;
        if (no_first_hop < 0)
            no_first_hop = (spfy_env("SPFY_WSOLA_NO_FIRST_HOP") != NULL);
        size_t n0 = no_first_hop ? 0u : hop;
        if (pre + n0 > buf_n) n0 = (buf_n > pre) ? buf_n - pre : 0u;
        if (n0) {
            int rc = spfy_wav_write(s->wav, buf + pre, n0);
            if (rc != SPFY_OK) return rc;
        }
        read_pos = no_first_hop ? pre : pre + hop;
        s->last_lag = 0;
    } else {
        int32_t lag = find_lag_engine(s->hist, s->hist_n, buf, buf_n,
                                      (uint32_t)W, (uint32_t)corr, s->stride);
        s->last_lag = lag;
        if (lag) s->n_aligned++;
        if (spfy_env("SPFY_WSOLA_VERBOSE"))
            fprintf(stderr, "[wsola] join lag=%d pre=%u content=%zu\n",
                    (int)lag, (unsigned)pre, content_n);
        /* FUN_08ee1240: W blended samples, written out as 2 x hop. */
        int16_t mix[SPFY_WSOLA_OLA_SAMPLES_MAX];
        size_t lag_u = (lag > 0) ? (size_t)lag : 0u;
        for (size_t j = 0; j < W; ++j) {
            long double t = (j < s->hist_n) ? (long double)s->hist[j] : 0.0L;
            long double h = (lag_u + j < buf_n)
                          ? (long double)buf[lag_u + j] : 0.0L;
            /* x87 keeps the two products and their sum in 80-bit and rounds
             * ONCE, at the FST to float32 -- so accumulate wide and narrow
             * at the end. */
            long double acc = h * (long double)s->win_in[j]
                            + t * (long double)s->win_out[j];
            mix[j] = clip_s16_engine((float)acc);
        }
        int rc = spfy_wav_write(s->wav, mix, W);
        if (rc != SPFY_OK) return rc;
        read_pos = 2u * hop + (size_t)lag;
    }

    /* Body: FUN_08ee36e0 with time-scaling off is a straight copy of
     * (this+0x35cc - hop) - this+0x35a8 samples.
     *
     * ⚠ That length can be NEGATIVE -- a short pau joined at a large lag
     * puts read_pos past the stop point. The engine guards with
     * `if (-1 < iVar4)`, emits nothing, AND does not advance its read cursor
     * (FUN_08ee33f0 is inside the same branch). So the cursor ends at
     * read_pos, not at stop, and that is where the next history is taken
     * from. Taking it from `stop` regardless picked a different lag at the
     * silence->speech join of most short texts. */
    size_t stop = (content_end > hop) ? (content_end - hop) : 0;
    size_t end_pos = read_pos;
    if (stop > read_pos) {
        if (s->tscale_on && s->plan && s->plan_n) {
            /* FUN_08ee3560 sets 0x35d0 = hop + lag on a join; the first span
             * of a phrase keeps FUN_08ee2960's `pre`. */
            size_t in_base0 = (s->hist_n == 0)
                            ? pre
                            : hop + (size_t)((s->last_lag > 0)
                                             ? s->last_lag : 0);
            uint32_t sps = s->wav ? (s->wav->sample_rate / 1000u) : 8u;
            if (sps == 0u) sps = 1u;
            end_pos = read_pos;
            /* this+0x35c4 -- the recording-clamped input limit, not the
             * zero-padded buffer length. */
            size_t span_lim = s->span_limit ? s->span_limit : buf_n;
            int rc = emit_body_tscale_plan(s, buf, buf_n, span_lim,
                                           &end_pos, pre, in_base0, sps);
            if (rc != SPFY_OK) return rc;
            /* ⚠ NOT forced to `stop`. FUN_08ee1700 takes the next join's
             * history from wherever the walk actually left the read cursor
             * (buf + 0x35a8), and a time-scaled walk does not end at the
             * straight-copy stop point. */
        } else if (s->tscale_on && s->unit_scale != 1.0f) {
            /* The span's WHOLE output budget is content_n / scale, and the
             * samples already emitted by the join (W) or the first-unit
             * prologue (hop) come OUT of that budget -- FUN_08ee33f0 sets
             * out_base right after the join, so the engine's frame loop runs
             * until out_pos reaches an out_end that counts those samples.
             *
             * ⚠ Budgeting the body from the remaining INPUT instead left the
             * join's W samples outside the stretch, so every span leaked an
             * un-stretched W. On a ~30-join utterance that is ~13% of the
             * output refusing to scale, which showed up as \!rp50 reaching
             * 1.902x instead of ~1.97x. */
            size_t pre_emit = (s->hist_n == 0) ? hop : W;
            size_t budget = (size_t)((float)content_n / s->unit_scale + 0.5f);
            size_t out_span = 0;
            if (budget > pre_emit + hop) out_span = budget - pre_emit - hop;
            end_pos = read_pos;
            size_t span_lim2 = s->span_limit ? s->span_limit : buf_n;
            int rc = emit_body_tscale(s, buf, buf_n, span_lim2, &end_pos,
                                      out_span, s->unit_scale);
            if (rc != SPFY_OK) return rc;
            /* The frame walk stops wherever the last whole frame landed;
             * the next join's history must still be taken from the end of
             * this unit's content, or a compressed unit would hand the join
             * material it never emitted. */
            if (end_pos < stop) end_pos = stop;
        } else {
            int rc = spfy_wav_write(s->wav, buf + read_pos, stop - read_pos);
            if (rc != SPFY_OK) return rc;
            end_pos = stop;
        }
    }

    /* History for the next join: FUN_08ee2d60 copies `corr` samples from
     * where emission stopped, zero-filling any shortfall. */
    size_t have = (buf_n > end_pos) ? (buf_n - end_pos) : 0;
    size_t hn   = (have < corr) ? have : corr;
    if (hn) memcpy(s->hist, buf + end_pos, hn * sizeof *buf);
    if (hn < corr) memset(s->hist + hn, 0, (corr - hn) * sizeof *s->hist);
    s->hist_n = corr;
    if (trace) {
        size_t out1 = s->wav ? (size_t)s->wav->n_samples_written : 0u;
        fprintf(stderr,
                "[wsolat] push=%llu out=%zu..%zu emit=%zu lag=%d pre=%zu "
                "content=%zu buf_n=%zu cend=%zu rpos=%zu stop=%zu\n",
                (unsigned long long)s->n_pushes - 1u, out0, out1, out1 - out0,
                (int)s->last_lag, pre, content_n, buf_n, content_end,
                read_pos, stop);
    }
    return SPFY_OK;
}

/* Core OLA blend with caller-specified overlap length. */
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

    /* Local window pair sized for eff_ola.
     *
     * ⚠ THIS IS THE WINDOW THAT ACTUALLY REACHES THE OUTPUT. g_hann_in/out
     * (built by hann_init) are NOT used here, so changing only that function
     * silently does nothing to the blend - which is exactly what happened
     * when the engine's linear window was first ported.
     *
     * Engine shape, FUN_08ee11e0: accumulate 1.0f/W, win[i] = i/W on the
     * head and 1 - i/W on the tail. Accumulated, not i*(1.0f/W): 1/80 is
     * inexact in binary and the engine's rounding is the one to match. */
    float hann_in [SPFY_WSOLA_OLA_SAMPLES_MAX];
    float hann_out[SPFY_WSOLA_OLA_SAMPLES_MAX];
    static int want_hann_b = -1;
    if (want_hann_b < 0) want_hann_b = (spfy_env("SPFY_WSOLA_HANN") != NULL);
    if (want_hann_b) {
        const float pi = 3.14159265358979323846f;
        for (uint32_t i = 0; i < eff_ola; ++i) {
            float t = (float)i / (float)eff_ola;
            float c = cosf(pi * t);
            hann_in [i] = 0.5f * (1.0f - c);
            hann_out[i] = 0.5f * (1.0f + c);
        }
    } else {
        float step = 1.0f / (float)eff_ola;
        float rising = 0.0f, falling = 1.0f;
        for (uint32_t i = 0; i < eff_ola; ++i) {
            hann_in [i] = rising;
            hann_out[i] = falling;
            rising  += step;
            falling -= step;
        }
    }

    /* ⚠ THE TAIL IS NOW LONGER THAN THE BLEND (tail_keep = 2W, blend = W).
     * Only its LAST eff_ola samples take part in the crossfade; everything
     * before that is ordinary audio that was simply held back, and must be
     * written out verbatim first or it is silently dropped. Blending from
     * the FRONT of a 2W tail would both lose W samples per join and fade the
     * wrong region. */
    size_t tail_lead = (s->tail_n > eff_ola) ? (s->tail_n - eff_ola) : 0;
    if (tail_lead) {
        int rc_lead = spfy_wav_write(s->wav, s->tail, tail_lead);
        if (rc_lead != SPFY_OK) return rc_lead;
    }
    const int16_t *tail_blend = s->tail + tail_lead;
    /* Tail may still be shorter than eff_ola (transient state). */
    size_t tail_have = s->tail_n - tail_lead;
    size_t tail_pad_lead = (tail_have < eff_ola)
                         ? (eff_ola - tail_have) : 0;

    /* Energy normalisation (matches engine amp_mods=1). */
    float new_scale = 1.0f;
    if (energy_norm_on) {
        long double te = 0.0L, he = 0.0L;
        for (uint32_t i = (uint32_t)tail_pad_lead; i < eff_ola; ++i) {
            float t = (float)tail_blend[i - tail_pad_lead];
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

    int16_t mix[SPFY_WSOLA_OLA_SAMPLES_MAX];
    for (uint32_t i = 0; i < eff_ola; ++i) {
        float t = (i < tail_pad_lead)
                  ? 0.0f
                  : (float)tail_blend[i - tail_pad_lead];
        float h = (float)new_p[i] * new_scale;
        float v = t * hann_out[i] + h * hann_in[i];
        mix[i]  = clip_s16(v);
    }
    int rc = spfy_wav_write(s->wav, mix, eff_ola);
    if (rc != SPFY_OK) return rc;

    int16_t scaled_buf[SPFY_WSOLA_OLA_SAMPLES_MAX * 8];
    const int16_t *body_src = new_p + eff_ola;
    /* Tail save size. */
    uint32_t save_n = s->save_override ? s->save_override : s->tail_keep;
    if (save_n > SPFY_WSOLA_OLA_SAMPLES_MAX)
        save_n = SPFY_WSOLA_OLA_SAMPLES_MAX;
    s->save_override = 0;
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

/* Internal entry - both push_unit and push_unit_psola route here. */
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
    /* Consume both synthetic-join flags: each describes only THIS push and
     * must not leak into the next. */
    const int synth_tail = s->tail_synthetic;
    const int synth_new  = s->next_push_synthetic;
    const int synth_join = synth_tail || synth_new;
    s->tail_synthetic = 0;
    s->next_push_synthetic = 0;
    /* Clamp nominal_n to what the buffer can actually serve at lag=0. */
    if (nominal_n == 0 || nominal_n > n) nominal_n = n;

    /* Pre-roll carried by this buffer (samples of the recording BEFORE the
     * unit's own start). */
    size_t pre = s->next_pre;
    s->next_pre = 0;
    if (pre > n) pre = 0;
    if (pre && nominal_n + pre > n) {
        nominal_n = (n > pre) ? (n - pre) : 0;
    }

    /* No prior tail: write everything except the last OLA samples (those
     * become the new tail). */
    if (s->tail_n == 0) {
        /* No tail to crossfade against, so the pre-roll has no job here -
         * skip it, exactly as the engine starts its first unit at
         * this+0x35d0 (= the pre-roll length) rather than at 0. */
        samples += pre;
        size_t use_n = nominal_n;
        if (use_n <= s->tail_keep) {
            memcpy(s->tail, samples, use_n * sizeof *samples);
            s->tail_n = use_n;
            return SPFY_OK;
        }
        size_t body = use_n - s->tail_keep;
        int rc = spfy_wav_write(s->wav, samples, body);
        if (rc != SPFY_OK) return rc;
        memcpy(s->tail, samples + body,
               s->tail_keep * sizeof *samples);
        s->tail_n = s->tail_keep;
        return SPFY_OK;
    }

    /* align==0 contract: caller guarantees this chunk is source-contiguous
     * with the previous one (same recording, lp adjacent). */
    if (!align) {
        /* Source-contiguous: pure concat, no crossfade, so the pre-roll is
         * audio the PREVIOUS unit already emitted. */
        samples += pre;
        /* Honour a pending tail-save override here too: the unit before a
         * gap is often source-contiguous (align==0), and if this path
         * handed over only ola_samples the widened gap fade would have
         * nothing to use. */
        uint32_t keep = s->save_override ? s->save_override : s->tail_keep;
        if (keep > SPFY_WSOLA_OLA_SAMPLES_MAX)
            keep = SPFY_WSOLA_OLA_SAMPLES_MAX;
        s->save_override = 0;
        int rc = spfy_wav_write(s->wav, s->tail, s->tail_n);
        if (rc != SPFY_OK) return rc;
        size_t use_n = nominal_n;
        if (use_n <= keep) {
            memcpy(s->tail, samples, use_n * sizeof *samples);
            s->tail_n = use_n;
            return SPFY_OK;
        }
        size_t body = use_n - keep;
        rc = spfy_wav_write(s->wav, samples, body);
        if (rc != SPFY_OK) return rc;
        memcpy(s->tail, samples + body, keep * sizeof *samples);
        s->tail_n = keep;
        return SPFY_OK;
    }

    /* Optional alignment search. */
    int32_t lag = 0;
    long double lag_score = 0.0L, lag0_score = 0.0L;
    /* `!synth_join`: correlating a word onset against synthetic gap fill
     * yields a meaningless lag and discards that many leading samples of
     * the resuming word. */
    if (align && !synth_join && n >= s->ola_samples + s->max_lag) {
        const int16_t *t0 = s->tail + (s->tail_n - s->ola_samples);
        for (uint32_t i = 0; i < s->ola_samples; ++i)
            lag0_score += (long double)t0[i] * (long double)samples[i];
        lag = find_best_lag(s->tail, s->tail_n, samples, n,
                            s->ola_samples, s->max_lag);
        if (lag < 0) lag = 0;
        s->last_lag = lag;
        if (lag != 0) s->n_aligned++;
        const int16_t *h0 = samples + lag;
        for (uint32_t i = 0; i < s->ola_samples; ++i)
            lag_score += (long double)t0[i] * (long double)h0[i];
    }
    /* Compute the normalised cross-correlation at the chosen lag. */
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
        if (spfy_env("SPFY_WSOLA_VERBOSE")) {
            double ncc_zero = denom > 1.0 ? (double)lag0_score / denom : 0.0;
            fprintf(stderr,
                    "[wsola] push n=%4zu align=1 lag=%+4d  "
                    "ncc(chosen)=%+0.3f  ncc(lag0)=%+0.3f\n",
                    n, lag, ncc_chosen, ncc_zero);
        }
    }
    const int16_t *new_p = samples + lag;
    /* Duration accounting: emit nominal_n, i.e. the lag shift costs nothing
     * extra. The over-decoded reservoir supplies the shifted samples.
     *
     * ⚠ THIS LOOKS WRONG AGAINST THE DECOMPILE AND IS NOT. FUN_08ee3560 sets
     * the read cursor to `this+8 + lag` after a join, which reads like a
     * `lag`-sized duration loss per join - and subtracting it here IS what
     * closed the length gap while our blend width was still wrong (26).
     * Once the blend was corrected to the engine's W=80 the two changes were
     * found to be compensating for each other. Measured on the seven
     * join-dominated corpus texts:
     *
     *     blend 26/40, subtract lag  ->  median 1.0141, mean|1-r| 0.0308
     *     blend 80,    subtract lag  ->  median 0.9848, mean|1-r| 0.0341
     *     blend 80,    emit nominal  ->  median 1.0105, mean|1-r| 0.0314  <-
     *
     * The engine's blend is duration-NEUTRAL: it emits W samples for the
     * overlap of a W tail with a W head, so the cursor advance already
     * accounts for the lag and charging it again double-counts. Our
     * ola_samples couples blend width to per-join cost where the engine keeps
     * them separate, which is what made the arithmetic look inconsistent.
     *
     * SPFY_WSOLA_SUB_LAG=1 restores the subtraction for A/B. */
    size_t available = (size_t)((int64_t)n - (int64_t)lag);
    /* The window starting at samples+lag must cover the pre-roll that the
     * crossfade eats PLUS the unit itself, less the `lag` samples the shift
     * skipped. */
    static int keep_nominal = -1;
    if (keep_nominal < 0)
        keep_nominal = (spfy_env("SPFY_WSOLA_KEEP_NOMINAL") != NULL);
    /* ⭐ THE +hop TERM IS THE ENGINE'S, AND IT IS WHY WE RAN SHORT.
     *
     * do_ola_blend writes tail_lead + eff_ola + body, i.e.
     *     tail_n + new_n - eff_ola - save_n
     * and with tail_n == save_n that reduces to new_n - eff_ola. So the net
     * cursor advance for this push is (span_n - lag) - ola_samples.
     *
     * The engine's advance, read off FUN_08ee3560 and then MEASURED against
     * its own output cursor, is
     *     target + hop - lag           hop = this+8 = W/2 = 40
     * exact (residual 0) on every joined unit across six texts. The join
     * stores `this+8 - lag` as the unit's length adjustment and sets the body
     * read position to `this+8 + lag`; both halves of the blend are emitted
     * (2 x 40), so the blend is duration-POSITIVE by one hop, not neutral.
     *
     * Setting span_n = nominal + pre + hop makes our advance
     *     (nominal + pre + hop - lag) - ola_samples = nominal + hop - lag
     * for pre == ola_samples, which is the engine's rule exactly. We were
     * emitting nominal - lag: one hop short at EVERY join, which is the
     * remaining shortfall after the pause fix (~40/join, ~168 over the four
     * joins of "One.").
     *
     * The over-read reservoir covers the extra: available = nominal + 2W -
     * lag against a need of nominal + W + hop - lag, so 40 samples spare.
     *
     * SPFY_WSOLA_NO_HOP=1 drops the term for A/B. */
    static int no_hop = -1;
    if (no_hop < 0) no_hop = (spfy_env("SPFY_WSOLA_NO_HOP") != NULL);
    size_t hop = no_hop ? 0u : (size_t)(s->ola_samples / 2u);
    size_t span_n = nominal_n + pre + hop;
    size_t emit_n = keep_nominal
                  ? span_n
                  : ((span_n > (size_t)lag) ? span_n - (size_t)lag : 0);
    size_t new_n = (emit_n < available) ? emit_n : available;

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
     * (state+0x3614=1; FUN_08EE3AA0 mode=1), verified via Frida probe.
     * The selective-F0-smoothing branch (which is what `eff_ola = 2*T0`
     * mirrored) never fires for Tom. Empirically: PSOLA widening
     * generated ~240 extra mini-dips per 60 s of audio relative to
     * plain WSOLA (484 vs 247 dips at default settings, vs oracle's
     * 281), because the wider Hann window at decorrelated voiced
     * boundaries lets the sin²/cos² mix cancel pointwise over a 17 ms
     * window instead of 10 ms.
     *
     * Re-enable per-voice with SPFY_WSOLA_PSOLA=1 (for voices that
     * actually run the engine's PSOLA branch - verify with the
     * wsola_unit_probe Frida hook first). */
    static int psola_enabled = -1;
    if (psola_enabled < 0) {
        const char *e = spfy_env("SPFY_WSOLA_PSOLA");
        if (e) {
            psola_enabled = atoi(e) ? 1 : 0;
        } else if (spfy_env("SPFY_WSOLA_NO_PSOLA")) {
            psola_enabled = 0;
        } else {
            psola_enabled = 0;
        }
    }
    if (psola_enabled && f0_tail > 0 && f0_head > 0 && sample_rate > 0) {
        uint32_t avg_f0 = ((uint32_t)f0_tail + (uint32_t)f0_head + 1u) >> 1;
        if (avg_f0 >= 50 && avg_f0 <= 400) {
            uint32_t T0 = sample_rate / avg_f0;
            uint32_t want = 2u * T0;
            if (want > eff_ola) eff_ola = want;
            if (eff_ola > SPFY_WSOLA_OLA_SAMPLES_MAX)
                eff_ola = SPFY_WSOLA_OLA_SAMPLES_MAX;
            psola_active = (eff_ola > s->ola_samples);
        }
    }
    if (spfy_env("SPFY_WSOLA_VERBOSE") && psola_active) {
        fprintf(stderr,
                "[wsola] psola f0_tail=%u f0_head=%u eff_ola=%u (default=%u)\n",
                f0_tail, f0_head, eff_ola, s->ola_samples);
    }

    /* Low-NCC short-blend fallback. */
    static double low_ncc_thresh = -2.0;
    if (low_ncc_thresh < -1.5) {
        const char *e = spfy_env("SPFY_WSOLA_LOW_NCC");
        low_ncc_thresh = e ? atof(e) : 0.2;
    }
    /* At a synthetic (gap) join, widen the overlap to gap_ola so the fade
     * spans several pitch periods instead of ~1. */
    if (synth_join && s->gap_ola > eff_ola) {
        uint32_t want = s->gap_ola;
        if (want > (uint32_t)s->tail_n) want = (uint32_t)s->tail_n;
        if (want > (uint32_t)new_n)      want = (uint32_t)new_n;
        if (want > eff_ola) eff_ola = want;
    }

    /* `!synth_join`: the low-NCC guard exists to stop Hann sin^2/cos^2
     * destructive interference between two decorrelated SPEECH signals. */
    if (align && !synth_join
        && low_ncc_thresh > -1.0 && ncc_chosen < low_ncc_thresh) {
        /* Micro-fade: 2 ms = 16 samples @ 8 kHz. */
        uint32_t micro = sample_rate / 500;
        if (micro < 8) micro = 8;
        if (micro < eff_ola) eff_ola = micro;
    }

    /* Energy normalisation default-OFF (2026-05-14 evening). */
    static int energy_norm_on = -1;
    if (energy_norm_on < 0)
        energy_norm_on = (spfy_env("SPFY_WSOLA_ENERGY_NORM") != NULL)
            && (spfy_env("SPFY_WSOLA_NO_ENERGY_NORM") == NULL);

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
    /* Engine path: FUN_08ee3aa0 finishes its unit loop by emitting `hop`
     * samples from the history buffer (plain-WSOLA mode, this+0x3614 == 1 --
     * the mode-0 branch runs FUN_08ee2d60 first and emits corr - hop
     * instead). Those `hop` samples are the tail the body loop held back. */
    if (s->engine_mode) {
        size_t n = s->hop;
        if (n > s->hist_n) n = s->hist_n;
        s->hist_n = 0;
        if (n == 0) return SPFY_OK;
        return spfy_wav_write(s->wav, s->hist, n);
    }
    if (s->tail_n == 0) return SPFY_OK;
    int rc = spfy_wav_write(s->wav, s->tail, s->tail_n);
    s->tail_n = 0;
    return rc;
}
