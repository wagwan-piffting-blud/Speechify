#ifndef SPFY_WSOLA_WSOLA_H
#define SPFY_WSOLA_WSOLA_H

#include <stddef.h>
#include <stdint.h>

#include "wav.h"

/* Streaming WSOLA-lite synthesizer (M4 iter 1).
 *
 * The Speechify engine's SWIttsWsola::concat (0x08EE65E0) joins per-unit
 * audio spans with a 10 ms (80-sample @ 8 kHz) Hanning-windowed
 * overlap-add. We don't yet model the engine's time-stretch + selective
 * F0 smoothing pipeline (gap #3); for now this module replaces the
 * naive linear xfade and the cross-rec linear blend used by spfy_concat
 * with proper Hann OLA across every unit boundary, plus an optional
 * cross-correlation alignment step that nudges each new chunk by ±5 ms
 * to maximise similarity in the overlap region. That alignment is what
 * stops voiced joins from clicking when pitch periods don't line up.
 *
 * Streaming model: caller pushes raw decoded samples chunk-by-chunk via
 * spfy_wsola_push_unit(); the streamer holds the last OLA_SAMPLES of the
 * previous chunk as a "tail", windows it with the falling half of a
 * Hann, windows the new chunk's head with the rising half, sums, and
 * writes them out together. Middle samples of each chunk are written
 * verbatim. The final flush emits the held tail unwindowed.
 *
 * No memory is owned across calls except the tail buffer (statically
 * sized inside the struct).
 */

/* 80-sample OLA was the original tuning based on the Ghidra-inferred
 * "10 ms Hanning OLA" in SWIttsWsola::concat. Empirical A/B against
 * engine WAV output (2026-06-08, "The quick brown fox… Speechify engine
 * using the Tom voice"): same UIDs as engine (audit-verified) but our
 * emit was 6.41 s vs engine's 6.84 s - losing 80 samples (10 ms) per
 * cross-rec join across 66 joins = 660 ms cumulative.
 *
 * Engine's effective per-join sample loss is ~26 samples (3.25 ms), not
 * 80. The "10 ms" docstring appears to describe the Hanning blend
 * window SHAPE, not the duration cost per join. OLA=26 reproduces
 * engine duration to within 1 ms with NO change to UID selection
 * (audit bit-identical: PATH UID 6929/7594 = 91.2% before and after).
 *
 * SPFY_WSOLA_OLA env var still overrides at runtime; SPFY_WSOLA_OLA=80
 * reverts to the legacy "too-fast" output for diagnostic purposes. */
/* Engine geometry, read off the WSOLA constructor (FUN_08ee2680 @ 0x08EE2680)
 * and the blend (FUN_08ee1240 @ 0x08EE1240). At 8 kHz the base window is
 * W = this+4 = 0x50 = 80, and everything else is derived from it:
 *
 *   this+4   = W     = 80   <- max lag AND the crossfade length
 *   this+8   = W>>1  = 40   <- the HOP: the blend's 80 samples go out as 2x40
 *   this+0xc = W*2   = 160  <- lag-search correlation window / tail retained
 *   this+0x10 = W*3  = 240  <- frame: 160 of correlation + 80 of lag headroom
 *
 * The 16 kHz branch sets W = 0xa0 = 160 and derives identically, which is how
 * we know these are relationships and not five unrelated constants.
 *
 * ⚠ THE CROSSFADE IS `this+4` = W, NOT `this+8`. FUN_08ee1240 loops to
 * this+4 and mixes head[i]*win[i] + tail[i]*win[i+W]. this+8 is only the
 * emission hop. Setting this to 40 was a misread of that relationship.
 *
 * Was 26 (~3.25 ms), which is not an engine number at all. */
#define SPFY_WSOLA_OLA_SAMPLES_DEFAULT 80u
#define SPFY_WSOLA_MAX_LAG_DEFAULT     80u   /* ±10 ms; matches engine's
                                              * lag search range
                                              * (state+0x04 = 80 in
                                              * FUN_08EE1330). Was 40
                                              * pre-2026-05-14. */
#define SPFY_WSOLA_OLA_SAMPLES_MAX    320u
#define SPFY_WSOLA_MAX_LAG_MAX        160u

/* ==================================================================
 * ENGINE-FAITHFUL PATH  (spfy_wsola_push_engine / spfy_wsola_flush)
 * ==================================================================
 *
 * The whole algorithm, read out of SWIttsWsola.dll and then verified against
 * the engine's own output cursor. For Tom the engine runs with time-scaling
 * OFF (this+0x2c == 0) and plain-WSOLA mode (this+0x3614 == 1), which is a
 * much simpler machine than the streaming OLA this module used to implement.
 *
 * Geometry, all derived from the base window W (0x50 @ 8 kHz, 0xa0 @ 16 kHz):
 *
 *     W      = this+4    = 80    blend length AND max lag
 *     hop    = this+8    = 40    W/2
 *     corr   = this+0xc  = 160   lag-search window AND history length
 *     frame  = this+0x10 = 240   corr + W, the head window
 *     stride = this+0x28 = 2     lag-search decimation
 *
 * Driver (FUN_08ee3aa0 "Wsola::process"):
 *
 *     load unit 0;  emit body
 *     for each later unit:  load;  fill history;  JOIN;  emit body
 *     finally: emit `hop` samples from the history buffer
 *
 * Per unit the decoded span is laid out exactly as FUN_08ee2960 builds it:
 *
 *     [ pre ][ content ][ over-read >= corr ]
 *
 * with pre = min(W, local_pos * sps) -- CLAMPED at a recording's start, which
 * is why uid 0 (local_pos 0) gets none.
 *
 * JOIN (FUN_08ee3560):
 *     lag  = argmax NCC over k in [0, W] step `stride`, correlating `corr`
 *            history samples against buf[k..]
 *     emit W blended samples: hist[j]*w_out[j] + buf[lag+j]*w_in[j]
 *     read_pos = 2*hop + lag
 *
 * BODY (FUN_08ee36e0, time-scale off -- the lag loop never runs for Tom, it
 * is a straight copy):
 *     emit (pre + content - hop) - read_pos samples from buf[read_pos]
 *
 * HISTORY (FUN_08ee2d60): copy `corr` samples starting where emission
 * stopped, i.e. at (pre + content - hop). So the crossfade's tail side is the
 * unit's last `hop` samples FOLLOWED BY what comes next in the recording --
 * not the unit's own last W samples, which is what we used to blend.
 *
 * Net per-unit output is therefore `content + hop - lag` for a joined unit
 * and `content - hop` for the first, plus one final `hop` at the end. Both
 * were confirmed exact against the engine's cursor on six texts.
 *
 * ⚠ There is no "align" concept in the engine: every unit after the first is
 * joined. Runs of consecutive UIDs are collapsed into ONE unit upstream,
 * which is where same-recording contiguity is actually handled. */
#define SPFY_WSOLA_CORR_MAX           320u

/* The base window for a sample rate. */
static inline uint32_t spfy_wsola_w_for_rate(uint32_t sample_rate)
{
    if (sample_rate == 0u) return SPFY_WSOLA_OLA_SAMPLES_DEFAULT;
    uint32_t w = (uint32_t)((uint64_t)SPFY_WSOLA_OLA_SAMPLES_DEFAULT
                            * sample_rate / 8000u);
    if (w > SPFY_WSOLA_OLA_SAMPLES_MAX) w = SPFY_WSOLA_OLA_SAMPLES_MAX;
    return w ? w : 1u;
}

/* Backward-compat aliases (some call sites still reference the old names;
 * treated as defaults). */
#define SPFY_WSOLA_OLA_SAMPLES SPFY_WSOLA_OLA_SAMPLES_DEFAULT
#define SPFY_WSOLA_MAX_LAG     SPFY_WSOLA_MAX_LAG_DEFAULT

typedef struct {
    spfy_wav_writer_t *wav;
    /* Last OLA samples of the previous chunk, NOT yet windowed. */
    int16_t  tail[SPFY_WSOLA_OLA_SAMPLES_MAX];
    size_t   tail_n;
    uint32_t ola_samples;
    uint32_t max_lag;
    /* How much of each unit is held back, as opposed to how much is
     * CROSSFADED. The engine keeps these separate and we did not:
     *   this+0xc = W*2 = 160  samples retained  (this field)
     *   this+4   = W   =  80  samples blended   (ola_samples)
     * FUN_08ee2d60 emits `total - this+8 - this+0xc` and copies this+0xc
     * into the tail buffer, and the measured engine loss is 160 per join,
     * not 80 - captured geometry gives output = sum(dur) - 160*(N-1) - 80,
     * which lands within 2 samples of the real 19134 on text_002. */
    uint32_t tail_keep;
    uint32_t next_pre;
    uint64_t n_pushes;
    uint64_t n_aligned;
    int32_t  last_lag;
    /* Set when `tail` holds SYNTHETIC content (an inserted inter-word or
     * inter-phrase gap) rather than real recorded audio. */
    int      tail_synthetic;
    /* Set when the NEXT pushed chunk is synthetic gap fill. */
    int      next_push_synthetic;
    /* Overlap length to use at a synthetic (gap) join, 0 = use ola_samples. */
    uint32_t gap_ola;
    /* One-shot override for how much tail THIS push holds back, so the push
     * before a gap can retain enough real speech to fade across. */
    uint32_t save_override;

    uint32_t W;
    uint32_t hop;
    uint32_t corr;       /* this+0xc  lag window and history length   */
    uint32_t stride;     /* this+0x28 lag-search decimation           */
    /* The `corr` samples that follow this unit's emitted body. Blend tail
     * side is hist[0..W); the lag search uses all `corr`. Zero-padded when
     * the recording ran out, matching FUN_08ee2d60. */
    int16_t  hist[SPFY_WSOLA_CORR_MAX];
    size_t   hist_n;
    /* Crossfade windows, built once at W. FUN_08ee11e0 ACCUMULATES 1.0f/W
     * rather than computing i*(1.0f/W); 1/80 is inexact in binary and the
     * engine's rounding is the one to match. */
    float    win_in [SPFY_WSOLA_OLA_SAMPLES_MAX];
    float    win_out[SPFY_WSOLA_OLA_SAMPLES_MAX];
    int      engine_mode;
} spfy_wsola_streamer_t;

void spfy_wsola_init (spfy_wsola_streamer_t *s, spfy_wav_writer_t *wav);

/* Declare that the currently-held tail is synthetic gap fill, not recorded
 * speech. */
void spfy_wsola_mark_tail_synthetic(spfy_wsola_streamer_t *s);

/* Mirror of the above for the ENTRY side of a gap: declare that the NEXT
 * chunk pushed is synthetic fill, while the held tail is real speech. */
void spfy_wsola_mark_next_push_synthetic(spfy_wsola_streamer_t *s);

/* Overlap length (samples) to use at synthetic gap joins. */
void spfy_wsola_set_gap_ola(spfy_wsola_streamer_t *s, uint32_t n);

/* Ask the NEXT push to hold back `n` samples of tail instead of the usual
 * ola_samples. */
void spfy_wsola_request_tail_save(spfy_wsola_streamer_t *s, uint32_t n);
void spfy_wsola_set_next_pre(spfy_wsola_streamer_t *s, uint32_t n);

/* Push one chunk of decoded s16 samples into the stream. */
int  spfy_wsola_push_unit(spfy_wsola_streamer_t *s,
                          const int16_t *samples, size_t n,
                          int align);

/* PSOLA-aware variant. Same as spfy_wsola_push_unit but takes the F0
 * (raw Hz byte, 0=unvoiced/silence) at the boundary on each side:
 *   f0_tail = previous unit's f0_end
 *   f0_head = new unit's   f0_start
 *
 * When both are nonzero AND align != 0, the streamer switches to
 * "Selective F0 smoothing" mode: overlap window length is grown to
 * 2 * T0 (= 2 * sample_rate / avg_f0), guaranteeing at least one full
 * pitch period of crossfade on each side. This is the engine's WSOLA
 * Mode 0 (FUN_08ee1160 with flag at state+0x3614=0). It defeats the
 * audible click that plain fixed-window OLA produces at voiced cross-
 * recording joins where pitch periods don't naturally align.
 *
 * Falls back to plain WSOLA when either side is unvoiced, when
 * SPFY_WSOLA_NO_PSOLA env is set, or when 2*T0 fits inside the default
 * window (no benefit to widening).
 *
 * Duration-preserving overread: `nominal_n` is the unit's intended
 * output sample count (= dur * sample_rate / 1000). When `n` exceeds
 * `nominal_n` (caller over-decoded by up to max_lag samples), the
 * extra samples act as a "look-ahead reservoir": after lag-shift, the
 * function truncates to exactly `nominal_n` samples of output. This
 * matches engine's behaviour where each unit always emits its dur
 * samples regardless of join-alignment lag - eliminates cumulative
 * timing drift on dense cross-recording join clusters. Pass
 * nominal_n == 0 to disable (output = n samples after lag).
 */
int  spfy_wsola_push_unit_psola(spfy_wsola_streamer_t *s,
                                const int16_t *samples, size_t n,
                                size_t nominal_n,
                                int align,
                                uint8_t f0_tail,
                                uint8_t f0_head,
                                uint32_t sample_rate);

/* ENGINE-FAITHFUL push. One call per WsolaUnit, in order.
 *
 * `buf` is the unit's decoded span laid out as FUN_08ee2960 builds it:
 *     [pre][content][over-read], buf_n = the whole thing.
 * `pre`       samples of recording history before the unit (clamped to what
 *             the recording actually had -- 0 at local_pos 0).
 * `content_n` the unit's TARGET length in samples (after any pau resize).
 *
 * The over-read should be at least `corr` samples or the history (and hence
 * the next join's lag search) runs against zero padding, exactly as the
 * engine's does when a recording ends.
 *
 * Emits `content_n + hop - lag` for a joined unit, `content_n - hop` for the
 * first. See the geometry block at the top of this header. */
int  spfy_wsola_push_engine(spfy_wsola_streamer_t *s,
                            const int16_t *buf, size_t buf_n,
                            size_t pre, size_t content_n);

/* Emit any held tail samples and finalise. Required before WAV close.
 * After this call the streamer state is reset to "no tail".
 *
 * On the engine path this emits `hop` samples from the history buffer, which
 * is what FUN_08ee3aa0 does after its unit loop in plain-WSOLA mode. */
int  spfy_wsola_flush(spfy_wsola_streamer_t *s);

#endif
