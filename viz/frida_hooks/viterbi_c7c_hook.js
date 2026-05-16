'use strict';
/*
 * viterbi_c7c_hook.js — capture per-cand run-length state c7c/c80.
 *
 * Function-entry hook on FUN_08e8b620 (USelGraph::ViterbiWithJoinCache)
 * at 0x08E8B620 in SWIttsUSel.dll. Same safety profile as
 * viterbi_dp_hook.js — fires ONCE per utterance, no mid-instruction
 * trampolines.
 *
 * --- Purpose (2026-05-14 evening) ---
 *
 * The 14 wrong UIDs that remain in the audit trace to a join-cost
 * mismatch at the slot 9→10 transition in mp_009 utt 1 (and analogous
 * slots in 4 other phrases). Engine computes join(147839 → 140957) as
 * exactly 1000.0; we compute 1000.13. The 0.13 difference is curve_val
 * = 0.6 * curve[idx], where idx = curr.c6c - sub_off - prev.c7c.
 *
 * We compute idx=59 (curve=4.99, but somehow our cost is only 1000.13
 * = curve_val=0.22 which matches curve[50]); engine effectively gets
 * idx=49 (curve=0.0). The discrepancy points to a different value of
 * `prev.c7c` between engine and us.
 *
 * The existing viterbi_dp_hook captures cand+0x68 (c68), +0x78 (c78),
 * +0x6c (c6c) on leave — but NOT +0x7c (c7c) or +0x80 (c80), which
 * are the actual run-length state values consumed by the next slot's
 * join cost. This hook captures those.
 *
 * --- Cand offsets (engine memory layout) ---
 *
 *   cand+0x0c  uid                   (u32)
 *   cand+0x10  join_key              (u32, = uid for HP leaves)
 *   cand+0x20  Viterbi cum cost      (f32, dp_20)
 *   cand+0x24  predecessor ptr       (u32)
 *   cand+0x2c  pre-DP target cost    (f32)
 *   cand+0x68  c68 = f0_mid          (u32)
 *   cand+0x6c  c6c = f0_end          (u32)
 *   cand+0x70  c70 = f0_start        (u32)
 *   cand+0x78  c78 = ?               (u32; engine zeros at init)
 *   cand+0x7c  c7c = run-length c7c  (u32; UPDATED by DP, read by join)
 *   cand+0x80  c80 = run-length c80  (u32; UPDATED by DP)
 *
 * Per FUN_08e8b620 decompile, when a transition is accepted:
 *   if (curr.c68 < 0x15):  c7c = prev.c7c, c80 = prev.c80 + 1
 *   else:                  c7c = curr.c68, c80 = curr.c78
 *
 * Join gate fires iff: curr.c6c > 20 AND prev.c80 < 15 AND prev.c7c > 20
 * Curve idx: clamp(curr.c6c - sub_off - prev.c7c, 0, max_idx-1)
 *
 * --- Output schema ---
 *
 * Each captured utterance emits two events: viterbi_c7c_enter and
 * viterbi_c7c_leave. Each event:
 *
 *   {type: "viterbi_c7c_leave" | "viterbi_c7c_enter",
 *    n_call: N, this_ptr: ..., n_slots: N,
 *    slots: [{slot: K, n_cands: N,
 *             cands: [{i, uid, join_key, c68, c78, c6c, c70, c7c, c80,
 *                      dp_20, predec, pre_dp}, ...]},
 *            ...]}
 *
 * --- Safety ---
 *
 * Function-entry hook only. All pointer reads guarded with rangeOK +
 * try/catch. Caps captures at TOTAL_CAP = 100 utterances per session.
 */

var ADDR_VITERBI = ptr('0x08E8B620');

var TOTAL_CAP = 100;
var stats = { calls: 0, sent: 0, dropped: 0,
              ptr_invalid: 0, read_errors: 0 };

function rangeOK(addr) {
    try {
        var r = Process.findRangeByAddress(addr);
        return r !== null && r.protection.indexOf('r') !== -1;
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
        snap.best_slot = safeReadU32(thisP.add(0x10));
        snap.best_idx  = safeReadU32(thisP.add(0x2c));
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
        if (n_cands === null || n_cands < 0 || n_cands > 4096 ||
            cand_arr === null || cand_arr < 0x100000) {
            snap.slots.push({slot: k, error: 'bad_cand_array',
                             slice_ptr: slice_ptr >>> 0});
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
            var entry = {
                i: i,
                cand_ptr: cand_p_val >>> 0,
                uid:      safeReadU32(candP.add(0x0c)),
                join_key: safeReadU32(candP.add(0x10)),
                c68:      safeReadU32(candP.add(0x68)),
                c6c:      safeReadU32(candP.add(0x6c)),
                c70:      safeReadU32(candP.add(0x70)),
                c78:      safeReadU32(candP.add(0x78)),
                c7c:      safeReadU32(candP.add(0x7c)),
                c80:      safeReadU32(candP.add(0x80)),
                pre_dp:   safeReadF32(candP.add(0x2c)),
            };
            if (stage === 'leave') {
                entry.dp_20  = safeReadF32(candP.add(0x20));
                entry.predec = safeReadU32(candP.add(0x24));
            }
            cands.push(entry);
        }
        snap.slots.push({slot: k, slice_ptr: slice_ptr >>> 0,
                         n_cands: n_cands, cands: cands});
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
            snap.type   = 'viterbi_c7c_enter';
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
            snap.type   = 'viterbi_c7c_leave';
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
    }
};

send({ type: 'ready', hook: 'viterbi_c7c', addr: '0x08E8B620',
       cap: TOTAL_CAP });
