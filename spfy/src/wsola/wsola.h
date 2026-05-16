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

#define SPFY_WSOLA_OLA_SAMPLES_DEFAULT 80u   /* 10 ms @ 8 kHz */
#define SPFY_WSOLA_MAX_LAG_DEFAULT     80u   /* ±10 ms; matches engine's
                                              * lag search range
                                              * (state+0x04 = 80 in
                                              * FUN_08EE1330). Was 40
                                              * pre-2026-05-14. */
#define SPFY_WSOLA_OLA_SAMPLES_MAX    320u   /* 40 ms cap @ 8 kHz */
#define SPFY_WSOLA_MAX_LAG_MAX        160u

/* Backward-compat aliases (some call sites still reference the old
 * names; treated as defaults). */
#define SPFY_WSOLA_OLA_SAMPLES SPFY_WSOLA_OLA_SAMPLES_DEFAULT
#define SPFY_WSOLA_MAX_LAG     SPFY_WSOLA_MAX_LAG_DEFAULT

typedef struct {
    spfy_wav_writer_t *wav;
    /* Last OLA samples of the previous chunk, NOT yet windowed.
     * Held verbatim so cross-correlation alignment can compare against
     * the next chunk's head. Sized to the max so runtime tuning works. */
    int16_t  tail[SPFY_WSOLA_OLA_SAMPLES_MAX];
    size_t   tail_n;            /* 0 means no tail (first push) */
    /* Active sizes, set at init from env vars (or defaults). */
    uint32_t ola_samples;       /* in [1 .. OLA_SAMPLES_MAX]   */
    uint32_t max_lag;           /* in [0 .. MAX_LAG_MAX]       */
    /* Stats. */
    uint64_t n_pushes;
    uint64_t n_aligned;         /* boundaries where lag != 0 */
    int32_t  last_lag;
} spfy_wsola_streamer_t;

void spfy_wsola_init (spfy_wsola_streamer_t *s, spfy_wav_writer_t *wav);

/* Push one chunk of decoded s16 samples into the stream.
 *
 * `align`: when nonzero, run a cross-correlation search over ±MAX_LAG
 * samples and shift the new chunk's start to maximise similarity with
 * the held tail. Use for voiced->voiced joins (cross-rec pairs and
 * long voiced spans). Same-rec spans should pass align=0 because the
 * source audio is already pitch-period-continuous.
 *
 * Returns SPFY_OK or SPFY_E_*.
 */
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
 * samples regardless of join-alignment lag — eliminates cumulative
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

/* Emit any held tail samples and finalise. Required before WAV close.
 * After this call the streamer state is reset to "no tail". */
int  spfy_wsola_flush(spfy_wsola_streamer_t *s);

#endif
