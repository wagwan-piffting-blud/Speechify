#ifndef SPFY_VB_EDGE_FRAMES_H
#define SPFY_VB_EDGE_FRAMES_H

#include <stdint.h>

#include "join_cost.h"
#include "../voice/voice.h"

/* Build the per-unit edge frames the join cost consumes.
 *
 * Two frames per unit, matching load_edge_frames: the START edge and the END
 * edge. Layout per frame (see join_cost.h):
 *   [0]     F0, taken directly from the unit record (f0_start / f0_end).
 *           No estimation -- the engine's own edge codes are already there,
 *           and 0 already means unvoiced.
 *   [1]     dead, always 0.
 *   [2..]   cfg.n_cep cepstral coefficients over a window anchored at the edge.
 *
 * The representation of dims >= 2 is a HYPOTHESIS, not a reader-recoverable
 * fact, so it is parameterised rather than fixed: spfy_vb_jcfit scores a
 * candidate against the costs the vendor cached in `hash`, per pair. The
 * defaults below reproduce the original hard-coded 12-MFCC build.
 *
 * Festival's clunits -- the acknowledged ancestor, see SPEC_S4_hash.md --
 * carries F0 in channel 0 of the join track (cl_maybe_fix_pitch_c0 overwrites
 * coefficient 0 with the pitch-mark reciprocal) and mel-cepstra after it. That
 * makes "F0 then cepstra, energy adjacent" the family to search first. */

typedef enum {
    SPFY_VB_ANCHOR_EDGE = 0,   /* start: [start, start+win)  end: [end-win, end) */
    SPFY_VB_ANCHOR_CENTER = 1  /* window centred on the boundary sample */
} spfy_vb_anchor_t;

typedef struct {
    uint32_t         win;      /* window length in samples, power of two */
    uint32_t         n_cep;    /* cepstral coefficients kept -> dim = 2 + n_cep */
    uint32_t         n_mel;    /* mel filterbank channels */
    float            preemph;  /* pre-emphasis coefficient; 0 disables */
    spfy_vb_anchor_t anchor;
    int              keep_c0;  /* 1: first kept coefficient is DCT bin 0 (energy),
                                  which is what Festival's channel layout implies
                                  once F0 stops overwriting it. 0: start at bin 1. */
    int              lifter;   /* sinusoidal liftering parameter; 0 disables */
    int              power;    /* 1: |X|^2 into the filterbank; 0: |X| */
    int              norm;     /* 1: normalise the spectral block to unit variance */
    /* 1: force dim 0 (the stored edge F0) to zero regardless of what the unit
     * records carry.
     *
     * For `--f0 render`, which populates f0_start/f0_end purely so WSOLA's
     * PSOLA crossfade can fire. Those same bytes are dim 0 of the S4 join
     * cost, so writing them silently re-costed the whole table and moved
     * 37-72% of picks -- the exact opposite of "render-only". Zeroing the
     * dimension here reproduces the `--f0 absent` cost exactly. */
    int              zero_f0_dim;
    /* Festival's pitch-synchronous analysis, which is where "dim 0 is F0" comes
     * from in the first place. sigpr_utt.cc: window length is
     * `factor * local pitch period` (DEFAULT_FRAME_FACTOR 2.0, hamming) and
     * `window_start = pos - window_size/2`, i.e. CENTRED ON A PITCH MARK. And
     * cldb.cc takes a unit's join coefficients as the sub-track from
     * index(unit->start) to index(unit->end), so the start frame is the first
     * mark inside the unit and the end frame the last -- which keeps the two
     * distinct, exactly as the 3-point cost requires. */
    float            pitch_factor; /* 0 = fixed window; else Festival's factor */
    int              pitch_align;  /* 0 at the raw boundary, 1 nearest epoch
                                      inside the unit, 2 anti-phase (CONTROL:
                                      if 1 and 2 score the same, the epoch
                                      detector is not finding anything and no
                                      conclusion about alignment is available) */
    int              pitch_f0;     /* 1: dim 0 becomes rate/period -- Festival's
                                      channel 0 -- instead of the unit record's
                                      f0 byte, which is a positional code */
    int              pitch_fixlen; /* 1: keep the analysis length at cfg.win in
                                      pitch mode, so OFFSET can be varied without
                                      also varying window length */
    float            pitch_offset; /* window centre shift, in LOCAL PERIODS */
    int              shift_start;  /* extra shift on the START edge only */
    int              shift_end;    /* extra shift on the END edge only. The two
                                      edges are separable: a frame grid that
                                      rounds index(start) UP and index(end) DOWN
                                      moves them in OPPOSITE directions. */
    int              shift_abs;    /* window centre shift, in SAMPLES, fixed path.
                                      Sweeping these two against each other is what
                                      separates glottal phase from plain timing:
                                      periods run 32-67 samples here, so the two
                                      parameterisations decorrelate across the
                                      corpus. */
    float            noise_snr; /* >0: add deterministic white noise at this SNR (dB)
                                   before analysis. The VDB is 8 kHz u-law, so the
                                   builder saw linear PCM we no longer have; this
                                   measures what one generation of that loss is
                                   WORTH in per-pair agreement, rather than
                                   assuming it explains a gap. */
    int              lpc;      /* >0: LPC of this order -> cepstrum, INSTEAD of the
                                  filterbank path. A quadratic form over log-mel
                                  spans every LINEAR transform of it, but LPC is a
                                  nonlinear function of the spectrum, so that whole
                                  family is invisible to the mel fit and has to be
                                  offered separately. */
    int              logmel;   /* 1: emit the log filterbank itself, no DCT, so a
                                  caller can FIT the vendor's transform instead of
                                  guessing it. Requires n_cep == n_mel. Any
                                  cepstral representation is a linear map of this
                                  block, so a quadratic form over it spans the
                                  whole family. */
    /* ⭐ MEASURED edge F0 for dim 0, INDEPENDENT of the stored bytes.
     * 2 bytes per uid (start, end) on the same quantiser as f0_start; NULL
     * falls back to the unit record.
     *
     * Why this exists: writing f0_start into the VIN collapses the "NAtional"
     * accent from +7.71 to ~+4.0, and that survives neutralising the target
     * cost, the edge weight, the join table, w_f0_miss AND the f0tr tree --
     * so the bytes have to stay 0. But dim 0 of the join cost reads those same
     * bytes, so with them zeroed the K-best partner ranking is PITCH-BLIND and
     * every seam is chosen on spectrum alone. Feeding the measurement in here
     * separates the two: the units stay f0-free (accent intact) while partner
     * selection can prefer a pitch-continuous join. */
    const uint8_t   *f0_edge;
    uint32_t         n_f0_edge;   /* units covered; 0 disables */
} spfy_vb_cfg_t;

/* The original hard-coded build: 32 ms Hamming at 8 kHz, 26 mel channels,
 * 12 coefficients starting at DCT bin 1, pre-emphasis 0.97, edge-anchored. */
void spfy_vb_cfg_default(spfy_vb_cfg_t *cfg);

#define SPFY_VB_MAX_WIN   2048u
#define SPFY_VB_MAX_MEL     64u
#define SPFY_VB_MAX_CEP     64u

typedef struct {
    float   *frames;     /* n_units * 2 * dim, owned */
    float   *weights;    /* dim, owned */
    uint32_t dim;
    uint32_t n_units;
    uint32_t n_missing;  /* units whose audio could not be resolved */
} spfy_vb_frames_t;

/* Decodes every unit's edge windows out of the VDB and fills `out`.
 * `sample_rate` is used only for the mel scale; the VDB byte arithmetic is
 * 8 bytes per millisecond as the engine does it. */
int  spfy_vb_frames_build_ex(const spfy_vin_t *vin, const spfy_vdb_t *vdb,
                             uint32_t sample_rate, const spfy_vb_cfg_t *cfg,
                             spfy_vb_frames_t *out);
int  spfy_vb_frames_build(const spfy_vin_t *vin, const spfy_vdb_t *vdb,
                          uint32_t sample_rate, spfy_vb_frames_t *out);
void spfy_vb_frames_free(spfy_vb_frames_t *f);

/* Point a join-cost context at built frames. */
void spfy_vb_frames_bind(const spfy_vb_frames_t *f, spfy_jc_t *jc);

#endif
