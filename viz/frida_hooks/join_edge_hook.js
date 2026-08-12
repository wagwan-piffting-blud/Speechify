'use strict';
/*
 * join_edge_hook.js -- the engine's per-EDGE join cost along the CHOSEN path.
 *
 * Function-entry attach on FUN_08e8b620 (USelGraph::ViterbiWithJoinCache) at
 * 0x08E8B620 in SWIttsUSel.dll, reading state in onLeave. Same safety profile
 * as viterbi_c7c_hook.js: one attach, at a function entry, never mid-body --
 * the x87 loops in this DLL do not survive a relocated trampoline.
 *
 * --- Why this exists (2026-08-10) ---
 *
 * felix/fr_053 diverges from the engine with EVERY per-slot scoring input
 * identical (all five sp targets, ctx, durt, f0tr) and IDENTICAL candidate
 * pools (54 = 54, verified with SPFY_FULL_POOL_DUMP=1). Both sides pick a
 * contiguous same-recording run; they pick DIFFERENT ones. That leaves the
 * join cost, and nothing in the trace carried per-edge join costs to compare
 * against.
 *
 * --- The measurement, and why it needs no mid-function hook ---
 *
 * The DP accumulates, per accepted transition:
 *
 *     dp_20[curr] = dp_20[prev] + join(prev -> curr) + target_cost(curr)
 *
 * so the edge cost is recoverable by subtraction from state that is already
 * settled when the function returns:
 *
 *     join(prev -> curr) = dp_20[curr] - dp_20[prev] - pre_dp[curr]
 *
 * This hook walks the predecessor chain back from the best final candidate
 * and emits ONLY that path, so the arithmetic above can be done offline.
 * viterbi_c7c_hook dumps the entire graph (every candidate of every slot),
 * which for a 42-slot phrase with ~54 candidates each is ~2200 records to
 * carry 42 numbers.
 *
 * ⚠ The identity is an inference, not a read. VALIDATE IT ON TOM FIRST: tom
 * is byte-identical to the engine, so his derived per-edge costs must match
 * spfy's own SPFY_JOIN_DUMP figures. If they do not, the identity is wrong
 * (an extra term, or a different accumulation order) and no felix number
 * derived from it means anything.
 *
 * --- Cand offsets (from viterbi_c7c_hook.js, engine memory layout) ---
 *
 *   cand+0x0c  uid                (u32)
 *   cand+0x10  join_key           (u32, = uid for HP leaves)
 *   cand+0x20  dp_20, Viterbi cum (f32)
 *   cand+0x24  predecessor ptr    (u32)
 *   cand+0x2c  pre-DP target cost (f32)
 *   cand+0x6c  c6c = f0_end       (u32)  } inputs to the join gate,
 *   cand+0x7c  c7c run-length     (u32)  } carried so a mismatched
 *   cand+0x80  c80 run-length     (u32)  } curve index can be seen
 *
 * --- Output schema ---
 *
 *   {type: "join_edge", n_call: N, this_ptr: P, n_slots: N,
 *    best_slot: K, best_idx: I,
 *    path: [{slot, i, uid, join_key, dp_20, pre_dp, c6c, c7c, c80,
 *            cand_ptr, predec}, ...]}        // path[0] is the FIRST slot
 */

var ADDR_VITERBI = ptr('0x08E8B620');

var TOTAL_CAP = 40;
var stats = { calls: 0, sent: 0, dropped: 0,
              ptr_invalid: 0, read_errors: 0, no_path: 0 };

function rangeOK(addr) {
    try {
        var r = Process.findRangeByAddress(addr);
        return r !== null && r.protection.indexOf('r') !== -1;
    } catch (e) { return false; }
}
function rdU32(addr) {
    if (!rangeOK(addr)) { stats.ptr_invalid++; return null; }
    try { return addr.readU32(); }
    catch (e) { stats.read_errors++; return null; }
}
function rdF32(addr) {
    if (!rangeOK(addr)) { stats.ptr_invalid++; return null; }
    try { return addr.readFloat(); }
    catch (e) { stats.read_errors++; return null; }
}

/* cand_ptr -> (slot, index), built once per call so the predecessor chain
 * can be resolved to slot positions. The engine stores a raw pointer in
 * cand+0x24, not an index. */
function buildPtrMap(thisP, n_slots) {
    var map = {};
    var slot_arr = rdU32(thisP.add(0x18));
    if (slot_arr === null || slot_arr < 0x100000) return null;
    var slot_arr_p = ptr(slot_arr);
    for (var k = 0; k < n_slots; ++k) {
        var slice = rdU32(slot_arr_p.add(k * 4));
        if (slice === null || slice < 0x100000) continue;
        var sliceP = ptr(slice);
        var n_c = rdU32(sliceP.add(0x2c));
        var arr  = rdU32(sliceP.add(0x34));
        if (n_c === null || n_c <= 0 || n_c > 4096 ||
            arr === null || arr < 0x100000) continue;
        var arrP = ptr(arr);
        for (var i = 0; i < n_c; ++i) {
            var cp = rdU32(arrP.add(i * 4));
            if (cp === null || cp < 0x100000) continue;
            map[cp >>> 0] = { slot: k, i: i };
        }
    }
    return map;
}

function candRecord(cand_ptr, slot, i) {
    var p = ptr(cand_ptr);
    return {
        slot: slot, i: i, cand_ptr: cand_ptr >>> 0,
        uid:      rdU32(p.add(0x0c)),
        join_key: rdU32(p.add(0x10)),
        dp_20:    rdF32(p.add(0x20)),
        predec:   rdU32(p.add(0x24)),
        pre_dp:   rdF32(p.add(0x2c)),
        c6c:      rdU32(p.add(0x6c)),
        c7c:      rdU32(p.add(0x7c)),
        c80:      rdU32(p.add(0x80)),
    };
}

Interceptor.attach(ADDR_VITERBI, {
    onEnter: function () {
        stats.calls++;
        if (stats.calls > TOTAL_CAP) { stats.dropped++; this.skip = 1; return; }
        this.this_ptr = this.context.ecx.toUInt32();
    },
    onLeave: function () {
        if (this.skip || !this.this_ptr || this.this_ptr < 0x100000) return;
        var thisP = ptr(this.this_ptr);

        var n_slots = rdU32(thisP.add(0x0c));
        if (n_slots === null || n_slots <= 0 || n_slots > 4096) return;

        var best_slot = rdU32(thisP.add(0x10));
        var best_idx  = rdU32(thisP.add(0x2c));
        var map = buildPtrMap(thisP, n_slots);
        if (!map) return;

        /* Start from the engine's own recorded winner. Falls back to the
         * minimum dp_20 of the last populated slot when those fields are
         * not the ones believed -- reported either way so the caller can
         * tell which happened rather than trusting a silent fallback. */
        var start = null, how = 'best_fields';
        var slot_arr = rdU32(thisP.add(0x18));
        var slot_arr_p = ptr(slot_arr);
        if (best_slot !== null && best_slot < n_slots) {
            var slice = rdU32(slot_arr_p.add(best_slot * 4));
            if (slice !== null && slice >= 0x100000) {
                var arr = rdU32(ptr(slice).add(0x34));
                var n_c = rdU32(ptr(slice).add(0x2c));
                if (arr !== null && n_c !== null && best_idx !== null &&
                    best_idx < n_c) {
                    var cp = rdU32(ptr(arr).add(best_idx * 4));
                    if (cp !== null && cp >= 0x100000) start = cp >>> 0;
                }
            }
        }
        if (start === null) {
            how = 'min_dp_last_slot';
            for (var k = n_slots - 1; k >= 0 && start === null; --k) {
                var sl = rdU32(slot_arr_p.add(k * 4));
                if (sl === null || sl < 0x100000) continue;
                var nn = rdU32(ptr(sl).add(0x2c));
                var aa = rdU32(ptr(sl).add(0x34));
                if (nn === null || nn <= 0 || aa === null) continue;
                var bestv = null;
                for (var j = 0; j < nn; ++j) {
                    var q = rdU32(ptr(aa).add(j * 4));
                    if (q === null || q < 0x100000) continue;
                    var v = rdF32(ptr(q).add(0x20));
                    if (v === null) continue;
                    if (bestv === null || v < bestv) { bestv = v; start = q >>> 0; }
                }
            }
        }
        if (start === null) { stats.no_path++; return; }

        /* Walk predecessors back to the head, guarding against cycles. */
        var path = [], seen = {}, cur = start;
        for (var step = 0; step < n_slots + 4; ++step) {
            if (!cur || cur < 0x100000 || seen[cur]) break;
            seen[cur] = 1;
            var loc = map[cur] || { slot: -1, i: -1 };
            var rec = candRecord(cur, loc.slot, loc.i);
            path.push(rec);
            if (!rec.predec || rec.predec < 0x100000) break;
            cur = rec.predec >>> 0;
        }
        path.reverse();

        send({ type: 'join_edge', n_call: stats.calls,
               this_ptr: this.this_ptr >>> 0, n_slots: n_slots,
               best_slot: best_slot, best_idx: best_idx,
               start_how: how, path: path });
        stats.sent++;
    }
});

rpc.exports = {
    stats: function () { return stats; },
    flush: function () { return stats; },
    reset: function () {
        stats = { calls: 0, sent: 0, dropped: 0,
                  ptr_invalid: 0, read_errors: 0, no_path: 0 };
    }
};

send({ type: 'ready', hook: 'join_edge', addr: '0x08E8B620',
       cap: TOTAL_CAP });
