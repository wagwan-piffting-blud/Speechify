#ifndef SPFY_USEL_VITERBI_H
#define SPFY_USEL_VITERBI_H

#include <stddef.h>
#include <stdint.h>

/* Viterbi DP for unit selection. */

typedef struct {
    const uint32_t *cands;
    const float    *target_cost;
    uint32_t        n_cands;
} spfy_viterbi_slot_t;

/* Join-cost callback. */
typedef float (*spfy_viterbi_join_fn)(uint32_t prev_uid,
                                      uint32_t curr_uid,
                                      void    *user);

/* Run the DP. */
int spfy_viterbi_run(const spfy_viterbi_slot_t *slots,
                     uint32_t                   n_slots,
                     spfy_viterbi_join_fn       join,
                     void                      *join_user,
                     uint32_t                  *out_path,
                     float                     *out_total_cost);

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
    const uint32_t *cands;
    /* per-cand "join key" used as the prev-side argument to join_cb. */
    const uint32_t *join_keys;
    const float    *target_cost;
    uint32_t        n_cands;
    const uint32_t *preds;
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
                         float                         *out_total_cost,
                         /* cfg+0x94 = tts.voiceCfg.GET_RID_OF_PATH_F0, read PER VOICE. */
                         int                            path_f0_flag);

#endif
