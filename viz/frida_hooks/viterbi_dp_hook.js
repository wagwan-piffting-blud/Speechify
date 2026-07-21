'use strict';
/*
 * viterbi_dp_hook.js -- engine Viterbi DP entry/exit dump.
 *
 * Hooks FUN_08e8b620 (USelGraph::ViterbiWithJoinCache) at entry and
 * exit, dumping the full per-slot HP cand state for every utterance.
 *
 * Closes the M3.4r blocker: per-cand totals are bit-exact at
 * InnerScorer time (M3.4q), but our DP still mismatches engine on
 * ~20% of slots. The hypothesis is that PostScoringAdj rewrites cand
 * totals between InnerScorer and Viterbi; this hook reads cand+0x2c
 * (= "pre-DP cost", which is what Viterbi feeds the recurrence)
 * directly so we can A/B against our score_candidate output.
 *
 * --- Reverse engineering basis ---
 *
 * Function:  FUN_08e8b620  @  0x08E8B620  in SWIttsUSel.dll.
 * Decompile (Ghidra MCP, 2026-05-05) gives us:
 *
 *   this (USelGraph):
 *     this+0x0c  n_slots                       (s32)
 *     this+0x10  best last-slot index          (output)
 *     this+0x18  -> slot_ptr_array             (array of slice ptrs,
 *                                              n_slots entries, 4B stride)
 *     this+0x2c  best cand idx in last slot    (output)
 *     this+0x30  log_id (debug)
 *
 *   USelSlice (slot, *(this+0x18)[k]):
 *     slice+0x2c  n_cands                       (s32, # HP cand ptrs)
 *     slice+0x34  -> cand_ptr_array             (array of HP cand obj
 *                                              ptrs, 4B stride)
 *     slice+0x38  n_predecessors                (for join cache)
 *
 *   HP cand object (slice+0x34[i]):
 *     cand+0x0c  uid                            (u32; engine reads this
 *                                              to index voice+0x20 unit
 *                                              table at *0x18 stride)
 *     cand+0x20  Viterbi total                  (f32; updated during DP,
 *                                              read at exit for the
 *                                              chosen-path argmin)
 *     cand+0x24  predecessor cand ptr           (backpointer, set by DP)
 *     cand+0x2c  pre-DP cost                    (f32; set by PostScoringAdj
 *                                              or InnerScorer, copied to
 *                                              cand+0x20 at slot 0 init)
 *
 * --- Safety ---
 *
 * Function-entry hook (entry + leave on the function symbol; no
 * mid-instruction Interceptor.attach). This function fires ONCE per
 * utterance, not per slot/cand, so it's well outside the hot-loop
 * danger zone. All pointer reads are guarded with rangeOK + try/catch.
 */

var ADDR_VITERBI = ptr('0x08E8B620');

var TOTAL_CAP = 100;            /* utterances captured per session */
var stats = { calls: 0, sent: 0, dropped: 0,
              ptr_invalid: 0, read_errors: 0 };

/* Page-keyed cache for rangeOK. Process.findRangeByAddress walks the
 * target's memory map on EVERY call, and dumpGraph issues one read per
 * candidate -- thousands per utterance. Measured 2026-07-20: this hook
 * alone was 53% of master-capture wall clock (13.8 s of 26.2 s over 10
 * phrases) almost entirely inside findRangeByAddress.
 *
 * The DP structures live in a handful of heap regions, so caching by 4 KB
 * page collapses those thousands of lookups to a few. The try/catch in
 * the readers below is the real protection and is untouched: if a cached
 * page is later unmapped, the read still fails safe into read_errors.
 * Cleared by reset() between utterances. */
var _rangeCache = {};

function rangeOK(addr) {
    try {
        var key = addr.shr(12).shl(12).toString();
        var hit = _rangeCache[key];
        if (hit !== undefined) return hit;
        var r = Process.findRangeByAddress(addr);
        var ok = r !== null && r.protection.indexOf('r') !== -1;
        _rangeCache[key] = ok;
        return ok;
    } catch (e) { return false; }
}
function safeReadU32(addr) {
    if (!rangeOK(addr)) { stats.ptr_invalid++; return null; }
    try { return addr.readU32(); }
    catch (e) { stats.read_errors++; return null; }
}
function safeReadF32(addr) {
    if (!rangeOK(addr)) { stats.ptr_invalid++; return null; }
    try { return addr.readFloat(); }
    catch (e) { stats.read_errors++; return null; }
}

/* Walk the USelGraph at `this_ptr` and return a {slots: [...]} snapshot.
 *   stage = 'enter'  -> read cand+0x2c (pre-DP cost)
 *   stage = 'leave'  -> read cand+0x20 (post-DP total) + cand+0x24
 *                       (predecessor) + chosen-path output fields. */
function dumpGraph(this_ptr, stage) {
    if (this_ptr < 0x100000) return null;
    var thisP = ptr(this_ptr);

    var n_slots = safeReadU32(thisP.add(0x0c));
    if (n_slots === null || n_slots <= 0 || n_slots > 4096) return null;

    var slot_arr = safeReadU32(thisP.add(0x18));
    if (slot_arr === null || slot_arr < 0x100000) return null;

    var snap = { stage: stage, this_ptr: this_ptr >>> 0,
                 n_slots: n_slots, slots: [] };

    if (stage === 'leave') {
        var best_slot = safeReadU32(thisP.add(0x10));
        var best_idx  = safeReadU32(thisP.add(0x2c));
        snap.best_slot = best_slot;
        snap.best_idx  = best_idx;
    }

    var slot_arr_p = ptr(slot_arr);
    for (var k = 0; k < n_slots; ++k) {
        var slice_ptr = safeReadU32(slot_arr_p.add(k * 4));
        if (slice_ptr === null || slice_ptr < 0x100000) {
            snap.slots.push({slot: k, error: 'bad_slice_ptr'});
            continue;
        }
        var sliceP   = ptr(slice_ptr);
        var n_cands  = safeReadU32(sliceP.add(0x2c));
        var cand_arr = safeReadU32(sliceP.add(0x34));
        /* Capture predecessor slice list (slice+0x3c is an INLINE array
         * of n=slice+0x38 predecessor slice ptrs, per FUN_08e8b620
         * disasm `local_38 = sliceP+0x3c; iVar13 = *local_38;
         * local_38 += 4`). Even if cands are bad we want the predec
         * topology -- it's set by BuildGraph regardless of AddUnits. */
        var n_preds  = safeReadU32(sliceP.add(0x38));
        var preds = [];
        if (n_preds !== null && n_preds >= 0 && n_preds < 64) {
            for (var pp = 0; pp < n_preds; ++pp) {
                var pred_ptr = safeReadU32(sliceP.add(0x3c + pp * 4));
                preds.push(pred_ptr);
            }
        }
        if (n_cands === null || n_cands < 0 || n_cands > 4096 ||
            cand_arr === null || cand_arr < 0x100000) {
            snap.slots.push({slot: k, error: 'bad_cand_array',
                             slice_ptr: slice_ptr >>> 0,
                             n_preds: n_preds, preds: preds});
            continue;
        }
        var cand_arr_p = ptr(cand_arr);
        var cands = [];
        for (var i = 0; i < n_cands; ++i) {
            var cand_p_val = safeReadU32(cand_arr_p.add(i * 4));
            if (cand_p_val === null || cand_p_val < 0x100000) {
                cands.push({i: i, error: 'bad_cand_ptr'});
                continue;
            }
            var candP = ptr(cand_p_val);
            var uid     = safeReadU32(candP.add(0x0c));
            /* cand+0x10 is the "join_key" used as the iVar9 in the
             * Viterbi DP's hash lookup -- for halfphone leaves it
             * equals uid (cand+0xc), but for tree-internal anchors
             * (representing multi-unit runs collapsed by BuildGraph)
             * it's the run's TAIL uid. Without capturing this we
             * cannot reproduce the engine's hash hits on inter-slot
             * transitions whose prev-side is an anchor cand. (M3.4r) */
            var join_k  = safeReadU32(candP.add(0x10));
            /* Smooth-miss inputs (M3.4r Phase A.5):
             *   cand+0x68 ("c68"): boundary threshold; if <0x15 the
             *                       cand inherits the run state from
             *                       pred, else resets it.
             *   cand+0x78 ("c78"): run-counter reset value used when
             *                       cand "breaks" the run (only with
             *                       weight+0x94 == 0; for Tom weight+0x94
             *                       == 1 so this is unused).
             *   cand+0x6c ("c6c"): used as the "curr" key in smooth
             *                       miss curve indexing:
             *                       i = c6c - offset_k - pred+0x7c. */
            var c68 = safeReadU32(candP.add(0x68));
            var c78 = safeReadU32(candP.add(0x78));
            var c6c = safeReadU32(candP.add(0x6c));
            var entry   = {i: i, cand_ptr: cand_p_val >>> 0,
                           uid: uid, join_key: join_k,
                           c68: c68, c78: c78, c6c: c6c};
            if (stage === 'enter') {
                entry.pre_dp = safeReadF32(candP.add(0x2c));
                entry.cur_20 = safeReadF32(candP.add(0x20));
            } else {
                entry.dp_20  = safeReadF32(candP.add(0x20));
                entry.predec = safeReadU32(candP.add(0x24));
                entry.pre_dp = safeReadF32(candP.add(0x2c));
            }
            cands.push(entry);
        }
        snap.slots.push({slot: k, slice_ptr: slice_ptr >>> 0,
                         n_cands: n_cands, cands: cands,
                         n_preds: n_preds, preds: preds});
    }

    /* If we're at leave, walk back from (best_slot, best_idx) to build
     * the engine's chosen path (uid sequence) for easy A/B vs our DP. */
    if (stage === 'leave' && typeof snap.best_slot === 'number' &&
        typeof snap.best_idx === 'number' &&
        snap.best_slot < n_slots) {
        var path_uids = [];
        var cur_slot = snap.best_slot;
        var cur_idx  = snap.best_idx;
        var hops = 0;
        while (cur_slot >= 0 && hops < n_slots + 4) {
            var s = snap.slots[cur_slot];
            if (!s || !s.cands || cur_idx >= s.cands.length) break;
            var c = s.cands[cur_idx];
            path_uids.unshift({slot: cur_slot, uid: c.uid});
            if (cur_slot === 0) break;
            /* Predecessor is a cand ptr; find its index in slot k-1.
             * Engine sets cand+0x24 = predecessor cand ptr. */
            var prev_cand_ptr = c.predec;
            if (!prev_cand_ptr) break;
            var prev_slot = snap.slots[cur_slot - 1];
            if (!prev_slot || !prev_slot.cands) break;
            var found = -1;
            for (var pj = 0; pj < prev_slot.cands.length; ++pj) {
                if (prev_slot.cands[pj].cand_ptr === (prev_cand_ptr >>> 0)) {
                    found = pj; break;
                }
            }
            if (found < 0) break;
            cur_slot = cur_slot - 1;
            cur_idx  = found;
            hops++;
        }
        snap.chosen_path = path_uids;
    }

    return snap;
}

Interceptor.attach(ADDR_VITERBI, {
    onEnter: function () {
        stats.calls++;
        if (stats.calls > TOTAL_CAP) { stats.dropped++; this.skip = 1; return; }
        var thisPtrVal = this.context.ecx.toUInt32();
        this.this_ptr  = thisPtrVal;
        var snap = dumpGraph(thisPtrVal, 'enter');
        if (snap) {
            snap.type  = 'viterbi_enter';
            snap.n_call = stats.calls;
            send(snap);
            stats.sent++;
        }
    },
    onLeave: function () {
        if (this.skip) return;
        if (!this.this_ptr) return;
        var snap = dumpGraph(this.this_ptr, 'leave');
        if (snap) {
            snap.type  = 'viterbi_leave';
            snap.n_call = stats.calls;
            send(snap);
            stats.sent++;
        }
    }
});

rpc.exports = {
    stats: function () { return stats; },
    flush: function () { return stats; },
    reset: function () {
        stats = { calls: 0, sent: 0, dropped: 0,
                  ptr_invalid: 0, read_errors: 0 };
        /* Drop the page cache too: the engine frees and reallocates the
         * DP arenas between utterances, so validity must be re-proven. */
        _rangeCache = {};
    }
};

send({ type: 'ready', hook: 'viterbi_dp', addr: '0x08E8B620',
       cap: TOTAL_CAP });
