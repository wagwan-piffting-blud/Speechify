/* Viterbi DP for unit selection.
 *
 * See viterbi.h for the API contract and an overview of how this fits the
 * engine pipeline. This file is the DP only -- target costs come in
 * pre-scored, join costs come in via callback.
 *
 * Implementation notes:
 *
 *   - The forward table is allocated as a single flat long-double buffer
 *     plus a u32 predecessor buffer. Per-slot widths can vary (PRSL pools
 *     are not uniform), so we keep a small offsets[] table that maps slot
 *     index to row start. Compared to the obvious 2D-array shape this
 *     halves the allocator pressure and gives clean cache behaviour.
 *
 *   - Long-double accumulator. The target cost arrives as f32 (caller has
 *     already done the long-double summation in cost_d/f0/sp/s). Joins are
 *     also f32 (engine stores them f32). The forward sum is long double;
 *     the per-slot best is long double. The final cast to float happens
 *     once at return.
 *
 *   - "Forbidden" transitions: any join < 0 OR target_cost < 0 is treated
 *     as forbidden and the transition is skipped. We don't synthesise a
 *     fake +infinity because long double infinities and NaN are awkward
 *     to keep out of subsequent comparisons; explicit skip is simpler.
 *
 *   - If a slot has no reachable predecessor, we report SPFY_E_OOB. The
 *     out_path is left untouched in that case.
 */

#include "viterbi.h"

#include "../../include/spfy/spfy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FORBIDDEN_THRESH 0.0f      /* values < this in target/join => skip */

int spfy_viterbi_run(const spfy_viterbi_slot_t *slots,
                     uint32_t                   n_slots,
                     spfy_viterbi_join_fn       join,
                     void                      *join_user,
                     uint32_t                  *out_path,
                     float                     *out_total_cost)
{
    if (!slots || n_slots == 0)            return SPFY_E_INVAL;
    if (!out_total_cost && !out_path)      return SPFY_E_INVAL;

    /* Validate + compute total candidate count and per-slot offsets. */
    size_t total_cands = 0;
    for (uint32_t s = 0; s < n_slots; ++s) {
        if (slots[s].n_cands == 0)         return SPFY_E_INVAL;
        if (!slots[s].cands)               return SPFY_E_INVAL;
        if (!slots[s].target_cost)         return SPFY_E_INVAL;
        total_cands += slots[s].n_cands;
    }

    size_t       *offsets = calloc(n_slots, sizeof *offsets);
    long double  *fwd     = calloc(total_cands, sizeof *fwd);
    uint32_t     *pred    = calloc(total_cands, sizeof *pred);
    uint8_t      *valid   = calloc(total_cands, sizeof *valid);
    if (!offsets || !fwd || !pred || !valid) {
        free(offsets); free(fwd); free(pred); free(valid);
        return SPFY_E_NOMEM;
    }
    {
        size_t off = 0;
        for (uint32_t s = 0; s < n_slots; ++s) {
            offsets[s] = off;
            off       += slots[s].n_cands;
        }
    }

    /* Slot 0: forward = target_cost, no predecessor. */
    {
        const spfy_viterbi_slot_t *s0 = &slots[0];
        for (uint32_t c = 0; c < s0->n_cands; ++c) {
            float t = s0->target_cost[c];
            if (t < FORBIDDEN_THRESH) continue;
            fwd[offsets[0] + c]   = (long double)t;
            pred[offsets[0] + c]  = 0;     /* unused for s == 0 */
            valid[offsets[0] + c] = 1;
        }
    }

    /* Slots 1..N-1: forward[s][c] = T[s][c]
     *                            + min over p of (forward[s-1][p] + join(p,c)) */
    for (uint32_t s = 1; s < n_slots; ++s) {
        const spfy_viterbi_slot_t *sp = &slots[s - 1];
        const spfy_viterbi_slot_t *sc = &slots[s];
        size_t off_p = offsets[s - 1];
        size_t off_c = offsets[s];

        for (uint32_t c = 0; c < sc->n_cands; ++c) {
            float t = sc->target_cost[c];
            if (t < FORBIDDEN_THRESH) {
                valid[off_c + c] = 0;
                continue;
            }
            long double best  = 0.0L;
            uint32_t    bestp = 0;
            int         have  = 0;
            uint32_t    cuid  = sc->cands[c];

            for (uint32_t p = 0; p < sp->n_cands; ++p) {
                if (!valid[off_p + p]) continue;
                uint32_t puid = sp->cands[p];
                float    j    = join ? join(puid, cuid, join_user) : 0.0f;
                if (j < FORBIDDEN_THRESH) continue;
                long double cand = fwd[off_p + p]
                                 + (long double)j;
                if (!have || cand < best) {
                    best  = cand;
                    bestp = p;
                    have  = 1;
                }
            }
            if (!have) {
                valid[off_c + c] = 0;
                continue;
            }
            fwd[off_c + c]   = best + (long double)t;
            pred[off_c + c]  = bestp;
            valid[off_c + c] = 1;
        }
    }

    /* Pick best end-of-sequence candidate. */
    uint32_t    last      = n_slots - 1;
    size_t      off_last  = offsets[last];
    long double end_best  = 0.0L;
    uint32_t    end_idx   = 0;
    int         end_have  = 0;
    for (uint32_t c = 0; c < slots[last].n_cands; ++c) {
        if (!valid[off_last + c]) continue;
        long double v = fwd[off_last + c];
        if (!end_have || v < end_best) {
            end_best = v;
            end_idx  = c;
            end_have = 1;
        }
    }
    if (!end_have) {
        free(offsets); free(fwd); free(pred); free(valid);
        return SPFY_E_OOB;
    }

    /* Backtrack to recover path. Write out_path[s] = chosen UID at slot s. */
    if (out_path) {
        uint32_t cur = end_idx;
        for (int32_t s = (int32_t)last; s >= 0; --s) {
            out_path[s] = slots[s].cands[cur];
            if (s > 0) cur = pred[offsets[s] + cur];
        }
    }
    if (out_total_cost) *out_total_cost = (float)end_best;

    free(offsets); free(fwd); free(pred); free(valid);
    return SPFY_OK;
}

/* --------------------------------------------------------------------- */
/* DAG variant                                                          */
/* --------------------------------------------------------------------- */

int spfy_viterbi_run_dag(const spfy_viterbi_dag_slot_t *slots,
                         uint32_t                       n_slots,
                         spfy_viterbi_dag_join_fn       join,
                         void                          *join_user,
                         uint32_t                      *out_path_slot,
                         uint32_t                      *out_path_uid,
                         uint32_t                      *out_path_len,
                         float                         *out_total_cost)
{
    if (!slots || n_slots == 0) return SPFY_E_INVAL;
    if (!out_total_cost && !(out_path_slot && out_path_uid && out_path_len))
        return SPFY_E_INVAL;

    /* Compute total candidate count and per-slot offsets. Slots with
     * n_cands==0 are valid graph nodes (e.g., word/syllable boundary
     * markers) and contribute nothing to the DP. */
    size_t total_cands = 0;
    for (uint32_t s = 0; s < n_slots; ++s) {
        if (slots[s].n_cands > 0 && !slots[s].cands)        return SPFY_E_INVAL;
        if (slots[s].n_cands > 0 && !slots[s].target_cost)  return SPFY_E_INVAL;
        if (slots[s].n_preds  > 0 && !slots[s].preds)       return SPFY_E_INVAL;
        total_cands += slots[s].n_cands;
    }

    size_t      *offsets   = calloc(n_slots,    sizeof *offsets);
    long double *fwd       = calloc(total_cands ? total_cands : 1, sizeof *fwd);
    uint32_t    *predslot  = calloc(total_cands ? total_cands : 1, sizeof *predslot);
    uint32_t    *predcidx  = calloc(total_cands ? total_cands : 1, sizeof *predcidx);
    uint8_t     *valid     = calloc(total_cands ? total_cands : 1, sizeof *valid);
    /* Engine-faithful run-length state propagated through the DP. Tracks
     * cand+0x7c (f0 history) and cand+0x80 (run-length count) for the
     * cand reached via its best predecessor path. Always allocated; if
     * the caller didn't supply per-cand state arrays we just leave them
     * at 0, which makes the F0-curve gate (`prev_c80 < 15 &&
     * prev_c7c > 20`) never fire — engine behaves identically. */
    int32_t     *fwd_c7c   = calloc(total_cands ? total_cands : 1, sizeof *fwd_c7c);
    int32_t     *fwd_c80   = calloc(total_cands ? total_cands : 1, sizeof *fwd_c80);
    if (!offsets || !fwd || !predslot || !predcidx || !valid
        || !fwd_c7c || !fwd_c80) {
        free(offsets); free(fwd); free(predslot); free(predcidx); free(valid);
        free(fwd_c7c); free(fwd_c80);
        return SPFY_E_NOMEM;
    }
    {
        size_t off = 0;
        for (uint32_t s = 0; s < n_slots; ++s) {
            offsets[s] = off;
            off       += slots[s].n_cands;
        }
    }

    /* Forward DP. Iterate slots in given order (caller guarantees topo). */
    for (uint32_t s = 0; s < n_slots; ++s) {
        const spfy_viterbi_dag_slot_t *sc = &slots[s];
        uint32_t nc = sc->n_cands;
        if (nc == 0) continue;

        /* Determine if this slot is a "source": no valid (non-empty)
         * predecessor. If so, fwd = target. */
        int has_live_pred = 0;
        for (uint32_t pi = 0; pi < sc->n_preds; ++pi) {
            uint32_t p = sc->preds[pi];
            if (p >= s)                continue;     /* topo violation */
            if (slots[p].n_cands == 0) continue;
            has_live_pred = 1;
            break;
        }

        if (!has_live_pred) {
            for (uint32_t c = 0; c < nc; ++c) {
                float t = sc->target_cost[c];
                if (t < FORBIDDEN_THRESH) continue;
                fwd[offsets[s] + c]      = (long double)t;
                predslot[offsets[s] + c] = s;
                predcidx[offsets[s] + c] = 0;
                valid[offsets[s] + c]    = 1;
                /* Engine pre-DP init in FUN_08e8b620 (lines 0x8e8b67e/681):
                 *   cand+0x7c = 0; cand+0x80 = 0;
                 * which is exactly the calloc default. The c68>=21 update
                 * branch fires only DURING DP transitions, not at source
                 * slot init. */
            }
            continue;
        }

        for (uint32_t c = 0; c < nc; ++c) {
            float t = sc->target_cost[c];
            if (t < FORBIDDEN_THRESH) {
                valid[offsets[s] + c] = 0;
                continue;
            }
            uint32_t    cuid    = sc->cands[c];
            long double best    = 0.0L;
            uint32_t    bestp_s = 0;
            uint32_t    bestp_c = 0;
            int         have    = 0;

            for (uint32_t pi = 0; pi < sc->n_preds; ++pi) {
                uint32_t p = sc->preds[pi];
                if (p >= s) continue;
                const spfy_viterbi_dag_slot_t *sp = &slots[p];
                if (sp->n_cands == 0) continue;
                size_t off_p = offsets[p];
                for (uint32_t cp = 0; cp < sp->n_cands; ++cp) {
                    if (!valid[off_p + cp]) continue;
                    /* prev side passes join_key (cand+0x10 in engine) if
                     * available, else uid as a halfphone-leaf fallback. */
                    uint32_t pkey = sp->join_keys ? sp->join_keys[cp]
                                                  : sp->cands[cp];
                    int32_t  pc7c = fwd_c7c[off_p + cp];
                    int32_t  pc80 = fwd_c80[off_p + cp];
                    uint32_t cc6c = sc->c6c ? (uint32_t)sc->c6c[c] : 0u;
                    float    j    = join
                        ? join(pkey, cuid, p, cp, s, c,
                               pc7c, pc80, cc6c, join_user)
                        : 0.0f;
                    if (j < FORBIDDEN_THRESH) continue;
                    long double cand = fwd[off_p + cp] + (long double)j;
                    if (!have || cand < best) {
                        best    = cand;
                        bestp_s = p;
                        bestp_c = cp;
                        have    = 1;
                    }
                }
            }
            if (!have) {
                valid[offsets[s] + c] = 0;
                continue;
            }
            fwd[offsets[s] + c]      = best + (long double)t;
            predslot[offsets[s] + c] = bestp_s;
            predcidx[offsets[s] + c] = bestp_c;
            valid[offsets[s] + c]    = 1;
            /* Engine-faithful run-length update from FUN_08e8b620.
             *   if (cand.c68 >= 21):
             *       cand.c7c = cand.c68
             *       if (weight+0x94 == 0): cand.c80 = cand.c78
             *       else:                  cand.c7c = c68; cand.c80 = 0
             *   else:
             *       cand.c7c = prev.c7c
             *       if (weight+0x94 == 0): cand.c80 = prev.c80 + 1
             *       else:                  cand.c80 = 100  (RESET sentinel)
             *
             * For Tom voice, weight+0x94 != 0 (verified 2026-05-14 via
             * Frida viterbi_c7c hook on mp_009 utt 1: engine has
             * uid=147839 with c68=0 -> c80=100, NOT c80=1). The c80=100
             * makes the next-slot join gate (`prev.c80 < 15`) FAIL,
             * suppressing the F0 curve cost — engine reports miss
             * cost = MISSING_JOIN (1000) exactly, no curve. We had
             * been running the +0x94==0 branch which propagates
             * c80=1 -> gate passes -> curve adds spurious 0.13.
             *
             * SPFY_C7C_LEGACY=1 reverts to the +0x94==0 branch (running
             * counter; matches our old behavior pre-2026-05-14-evening).
             */
            if (sc->c68 && sc->c78) {
                static int c7c_legacy = -1;
                if (c7c_legacy < 0)
                    c7c_legacy = (getenv("SPFY_C7C_LEGACY") != NULL);
                uint32_t my68 = sc->c68[c];
                if ((int32_t)my68 >= 21) {
                    fwd_c7c[offsets[s] + c] = (int32_t)my68;
                    fwd_c80[offsets[s] + c] = c7c_legacy
                                              ? (int32_t)sc->c78[c]
                                              : 0;
                } else {
                    fwd_c7c[offsets[s] + c] = fwd_c7c[offsets[bestp_s] + bestp_c];
                    fwd_c80[offsets[s] + c] = c7c_legacy
                        ? (fwd_c80[offsets[bestp_s] + bestp_c] + 1)
                        : 100;
                }
            }
            if (getenv("SPFY_DAG_FWD_DUMP")) {
                fprintf(stderr, "{\"fwd\":1,\"slot\":%u,\"c\":%u,"
                                "\"uid\":%u,\"pre_dp\":%.6f,"
                                "\"bestp_s\":%u,\"bestp_c\":%u,"
                                "\"fwd\":%.6f}\n",
                        s, c, cuid, (double)t,
                        bestp_s, bestp_c, (double)fwd[offsets[s] + c]);
            }
        }
    }

    /* Engine's DP picks argmin at the LAST non-empty slot reached. The
     * inner loop in FUN_08e8b620 only enters when n_cands > 0 and
     * updates this+0x10 = local_2c each iteration -- so the "last
     * processed slot" is the highest-index non-empty slot. */
    int32_t last = -1;
    for (int32_t s = (int32_t)n_slots - 1; s >= 0; --s) {
        if (slots[s].n_cands == 0) continue;
        size_t off_s = offsets[s];
        int    any   = 0;
        for (uint32_t c = 0; c < slots[s].n_cands; ++c) {
            if (valid[off_s + c]) { any = 1; break; }
        }
        if (any) { last = s; break; }
    }
    if (last < 0) {
        free(offsets); free(fwd); free(predslot); free(predcidx); free(valid);
        free(fwd_c7c); free(fwd_c80);
        return SPFY_E_OOB;
    }

    long double end_best = 0.0L;
    uint32_t    end_idx  = 0;
    int         end_have = 0;
    {
        size_t off_l = offsets[last];
        for (uint32_t c = 0; c < slots[last].n_cands; ++c) {
            if (!valid[off_l + c]) continue;
            long double v = fwd[off_l + c];
            if (!end_have || v < end_best) {
                end_best = v;
                end_idx  = c;
                end_have = 1;
            }
        }
    }
    if (!end_have) {
        free(offsets); free(fwd); free(predslot); free(predcidx); free(valid);
        free(fwd_c7c); free(fwd_c80);
        return SPFY_E_OOB;
    }

    /* Backtrack along predslot/predcidx until we hit a source (where
     * predslot points back to itself). Build the path in reverse, then
     * flip into out_path_slot / out_path_uid. */
    if (out_path_slot && out_path_uid && out_path_len) {
        uint32_t cur_s = (uint32_t)last;
        uint32_t cur_c = end_idx;
        uint32_t      *tmp_s = malloc(n_slots * sizeof *tmp_s);
        uint32_t      *tmp_u = malloc(n_slots * sizeof *tmp_u);
        if (!tmp_s || !tmp_u) {
            free(tmp_s); free(tmp_u);
            free(offsets); free(fwd); free(predslot); free(predcidx); free(valid);
            free(fwd_c7c); free(fwd_c80);
            return SPFY_E_NOMEM;
        }
        uint32_t hops = 0;
        for (;;) {
            tmp_s[hops] = cur_s;
            tmp_u[hops] = slots[cur_s].cands[cur_c];
            ++hops;
            if (hops > n_slots) break;
            uint32_t ps = predslot[offsets[cur_s] + cur_c];
            uint32_t pc = predcidx[offsets[cur_s] + cur_c];
            if (ps == cur_s) break;     /* source */
            cur_s = ps;
            cur_c = pc;
        }
        /* Reverse into output. */
        for (uint32_t i = 0; i < hops; ++i) {
            out_path_slot[i] = tmp_s[hops - 1 - i];
            out_path_uid [i] = tmp_u[hops - 1 - i];
        }
        *out_path_len = hops;
        free(tmp_s); free(tmp_u);
    }
    if (out_total_cost) *out_total_cost = (float)end_best;

    free(offsets); free(fwd); free(predslot); free(predcidx); free(valid);
    free(fwd_c7c); free(fwd_c80);
    return SPFY_OK;
}
