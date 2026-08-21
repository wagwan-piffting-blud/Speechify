#ifndef SPFY_VB_JOIN_COST_H
#define SPFY_VB_JOIN_COST_H

#include <stddef.h>
#include <stdint.h>

/* Join cost, ported from the shipped edgeFrames scorer in SWIttsUSel.dll.
 * Derivation and addresses: spfy/src/vb/SPEC_S4_hash.md, "The join cost
 * formula". This is the computation the offline builder precomputed into the
 * `hash` chunk.
 *
 * Frame layout, dim floats per frame, two frames per unit:
 *   [0]      F0 at the edge. ABSOLUTE difference, gated on both sides being
 *            voiced. Comes straight from the unit record -- f0_start for the
 *            start-edge frame, f0_end for the end-edge frame -- so it needs no
 *            acoustic estimation. 0 means unvoiced and fails the gate.
 *   [1]      DEAD. load_edge_frames computes a weight for it and then stores 0
 *            over it, logging "joinweights[1] = *DISABLED*". Kept in the layout
 *            so indices match the vendor's.
 *   [2..]    spectral. The vendor's representation is NOT recoverable from a
 *            reader -- the reader only ever consumes floats. We use 12 MFCC,
 *            which is what Exp 65 substituted successfully via a Frida cave.
 */

#define SPFY_JC_DIM_F0     0u
#define SPFY_JC_DIM_DEAD   1u
#define SPFY_JC_DIM_SPEC   2u

/* Voicing-class weight/offset pairs, from the VCF (V0/V1/V2_JCW, V0/V1/V2_JCO).
 * Selected by how many of the two phones are voiced. */
typedef struct {
    float w[3];
    float o[3];
} spfy_jc_voicing;

typedef struct {
    uint32_t     dim;
    uint32_t     n_units;
    const float *frames;   /* n_units * 2 * dim, unit-major, start then end */
    float       *weights;  /* dim */
    float        f0_gate;  /* T; the engine compares strictly greater */
    /* Gauge on `raw`. The vendor's term scales linearly with the spectral
     * features' own spread, and their representation is unrecoverable from a
     * reader, so absolute magnitude is a free parameter that must be
     * calibrated per voice. spfy_vb_joincost reports the value that aligns
     * the median with the voice's own cached distribution. 1.0 = uncalibrated. */
    float        raw_scale;
} spfy_jc_t;

/* Derive `weights` exactly as load_edge_frames does: inverse standard
 * deviation per dimension over all 2*n_units frames, with dim 0 excluding
 * unvoiced frames from its count, dim 1 forced to zero, and dims >= 2
 * additionally divided by (2*dim - 4).
 *
 * k0 and kspec are the two multiplicative constants the vendor applies
 * (DAT_08e96bc8 for dim 0; dims >= 2 carry none, pass 1.0f). */
int spfy_jc_derive_weights(spfy_jc_t *jc, float k0);

/* The per-frame-pair kernel: |dX0|*w0 when both voiced, plus sum of squared
 * weighted differences over the remaining dimensions. */
float spfy_jc_kernel(const spfy_jc_t *jc, const float *x, const float *y);

/* Raw 3-point boundary distance between left unit's end and right unit's
 * start, with the seam term doubled. */
float spfy_jc_raw(const spfy_jc_t *jc, uint32_t uid_left, uint32_t uid_right);

/* The value the vendor stores in `hash`:
 *   0                                       if uid_right == uid_left + 1
 *   raw * join_weight + join_offset         otherwise
 * join_weight/join_offset are JOIN_COST_WEIGHT / JOIN_COST_OFFSET from the VCF
 * -- the cache path applies neither at lookup time, so they must be baked in
 * here. */
float spfy_jc_cached_value(const spfy_jc_t *jc,
                           uint32_t uid_left, uint32_t uid_right,
                           float join_weight, float join_offset);

/* The runtime edgeFrames path instead applies the voicing-class pair. Provided
 * for parity checking against a voice that ships edge frames. */
float spfy_jc_runtime_value(const spfy_jc_t *jc,
                            uint32_t uid_left, uint32_t uid_right,
                            int left_voiced, int right_voiced,
                            const spfy_jc_voicing *v);

#endif
