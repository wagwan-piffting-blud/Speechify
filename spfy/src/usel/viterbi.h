#ifndef SPFY_USEL_VITERBI_H
#define SPFY_USEL_VITERBI_H

#include <stddef.h>
#include <stdint.h>

/* Viterbi DP for unit selection.
 *
 * The engine's USel runs a forward DP across N target halfphone slots. At
 * each slot s a small candidate pool (PRSL output, typically O(10-200)
 * uids) is scored against the target via target costs (D + F0 + SP + S).
 * Adjacent slots' candidates are tied with a join cost (precomputed in the
 * VIN `hash` chunk; misses fall back to the ccos gate / a default).
 *
 * This module is a pure DP -- it does not evaluate target costs or join
 * costs itself. Callers pre-score per-slot target costs (one f32 per
 * candidate) and supply a callback for the (prev_uid, curr_uid) join
 * cost. That keeps the DP isolated from the cost-pipeline glue and easy
 * to unit-test with synthetic numbers.
 *
 * Accumulator is `long double` to preserve x87 80-bit precision (matches
 * MSVC 7.1 (2003) FP semantics; see plan: bit-exact FP strategy). Final
 * total cost is cast to f32 at return for storage parity with the engine.
 *
 * Per-slot best-cost storage is also long double; predecessors are
 * candidate indices (u32) into the previous slot's `cands` array. Backtrack
 * walks predecessors to recover the chosen-UID sequence.
 *
 * Complexity: O(sum_s n_cands[s] * n_cands[s-1]) join evaluations + same
 * number of accumulator adds. Tom's PRSL pools average ~14 candidates so
 * this is well under a millisecond per utterance.
 */

typedef struct {
    const uint32_t *cands;        /* candidate UIDs (caller-owned, alias) */
    const float    *target_cost;  /* one f32 per cand; T[c] = D+F0+SP+S */
    uint32_t        n_cands;
} spfy_viterbi_slot_t;

/* Join-cost callback. Returns the engine-equivalent join cost f32 for the
 * directed transition prev_uid -> curr_uid. The caller is responsible for
 * the lookup (typically spfy_hash_lookup; on miss, the engine's ccos gate
 * fallback or a default). Must be deterministic. May be invoked many times
 * per slot; keep it cheap.
 *
 * Returning a very large value (e.g. 1e30f) effectively forbids the
 * transition. Returning a negative value is treated as forbidden.
 */
typedef float (*spfy_viterbi_join_fn)(uint32_t prev_uid,
                                      uint32_t curr_uid,
                                      void    *user);

/* Run the DP. On SPFY_OK:
 *   - out_path[s] is set to the chosen UID at slot s (s in [0, n_slots))
 *   - *out_total_cost is the path's total cost (f32 cast of long double)
 *
 * out_path must point to at least n_slots u32 elements; pass NULL if you
 * only want the cost.
 *
 * Errors:
 *   SPFY_E_INVAL: bad args (NULL slots, n_slots == 0, slot with no cands)
 *   SPFY_E_NOMEM: alloc failure
 *   SPFY_E_OOB:   no reachable path (e.g. every transition into slot s is
 *                 forbidden). out_path is left untouched in this case.
 */
int spfy_viterbi_run(const spfy_viterbi_slot_t *slots,
                     uint32_t                   n_slots,
                     spfy_viterbi_join_fn       join,
                     void                      *join_user,
                     uint32_t                  *out_path,
                     float                     *out_total_cost);

/* --------------------------------------------------------------------- */
/* DAG variant: per-slot predecessor list                                */
/* --------------------------------------------------------------------- */
/*
 * The linear `spfy_viterbi_run` above assumes slot s's predecessors are
 * exactly slot s-1's cands. The engine's actual DP (FUN_08e8b620) reads
 * `slice+0x3c` -- an inline array of n=slice+0x38 predecessor slice
 * pointers -- so a slot's predecessors form an arbitrary sub-DAG of
 * earlier slots. This is what allows BuildGraph to collapse long
 * same-recording runs into single anchor slots whose only predecessor
 * is the run's start anchor (skipping all in-between halfphone slots).
 *
 * `spfy_viterbi_run_dag` accepts the same per-slot cand+target_cost
 * arrays as the linear version, plus a per-slot predecessor list of
 * slot indices. Slots whose `n_preds == 0` (or whose predecessors all
 * have `n_cands == 0`) are treated as source slots: forward = target.
 *
 * The chosen path is reported as parallel arrays of slot indices and
 * UIDs (length = number of non-empty slots on the path). Caller pre-
 * allocates `out_path_slot` and `out_path_uid` arrays of at least
 * `n_slots` elements; the actual length lands in `*out_path_len`.
 *
 * Slot order assumption: callers must pass slots in topological order
 * (every entry in slots[s].preds must have value < s). The engine's
 * BuildGraph + LinkGraph naturally produces this ordering via post-
 * order tree traversal -- so the slot index returned by the
 * viterbi_dp Frida hook satisfies this invariant.
 */

typedef struct {
    const uint32_t *cands;        /* n_cands UIDs (caller-owned, alias) */
    /* per-cand "join key" used as the prev-side argument to join_cb.
     * For halfphone-leaf cands this is the same as `cands` (= uid).
     * For tree-internal anchor cands representing a multi-unit run
     * (BuildGraph collapses long same-rec runs into a single anchor),
     * this is the run's TAIL uid -- the natural join point. The
     * engine reads this from cand+0x10 ("join_key") whereas the
     * "uid" used as the curr-side hash key comes from cand+0xc. If
     * NULL, falls back to `cands` (legacy halfphone-only callers). */
    const uint32_t *join_keys;
    const float    *target_cost;  /* one f32 per cand (= engine pre_dp) */
    uint32_t        n_cands;
    const uint32_t *preds;        /* n_preds slot indices (alias) */
    uint32_t        n_preds;
    /* Optional per-cand static state for engine-faithful join cost
     * (FUN_08e8b620 reads cand+0x68/+0x6c/+0x70/+0x78). All four are
     * parallel arrays of length n_cands. NULL means callers don't want
     * the engine F0-curve gate logic — the DAG falls back to the
     * legacy callback that ignores per-cand state.
     *
     *   c68 = unit_mem+0x11 (f0_mid). Used as gate threshold (>= 21
     *         disables run-length inheritance).
     *   c6c = unit_mem+0x10 (f0_end). Used as the curr-side argument to
     *         the F0-prob curve lookup.
     *   c70 = unit_mem+0x0f (f0_start). Stored on cand+0x70.
     *   c78 = unit_mem+0x0f (f0_start) — engine init for cand+0x78,
     *         which becomes the run-length COUNT seed when the cand's
     *         own c68 >= 21. Same byte as c70.
     *
     * For a multi-UID anchor cand the engine populates these from the
     * RUN-TAIL unit (cand+0x10). Pass se_unit's bytes for anchor cands. */
    const uint8_t  *c68;
    const uint8_t  *c6c;
    const uint8_t  *c70;
    const uint8_t  *c78;
} spfy_viterbi_dag_slot_t;

/* DAG join-cost callback. Same intent as spfy_viterbi_join_fn but with
 * slot/cand indices passed in so the callback can maintain per-cand
 * DP-state (e.g. the engine's "smooth miss" run-counter fields
 * cand+0x7c, cand+0x80 -- see FUN_08e8b620 disasm). prev_uid here is
 * the prev cand's join_key (cand+0x10 in the engine), curr_uid is the
 * curr cand's primary uid.
 *
 * `prev_c7c` and `prev_c80` are the predecessor cand's run-length
 * state propagated from the chosen sub-path. They are 0 unless the
 * caller populated `c68/c6c/c70/c78` on every slot, in which case the
 * DAG implementation maintains and forwards them along the chosen
 * pred path. The callback can use them to gate the engine's F0-prob
 * curve (see FUN_08e8b620): the curve fires when curr.c6c > 20 AND
 * prev_c80 < 15 AND prev_c7c > 20.
 *
 * `curr_c6c` is the curr cand's f0_end byte (= +0x10 in the engine's
 * unit-mem layout). Forwarded here so the callback doesn't have to
 * reach back through user data to fetch it. */
typedef float (*spfy_viterbi_dag_join_fn)(uint32_t prev_uid_join_key,
                                          uint32_t curr_uid,
                                          uint32_t prev_slot,
                                          uint32_t prev_idx,
                                          uint32_t curr_slot,
                                          uint32_t curr_idx,
                                          int32_t  prev_c7c,
                                          int32_t  prev_c80,
                                          uint32_t curr_c6c,
                                          void    *user);

int spfy_viterbi_run_dag(const spfy_viterbi_dag_slot_t *slots,
                         uint32_t                       n_slots,
                         spfy_viterbi_dag_join_fn       join,
                         void                          *join_user,
                         uint32_t                      *out_path_slot,
                         uint32_t                      *out_path_uid,
                         uint32_t                      *out_path_len,
                         float                         *out_total_cost);

#endif
