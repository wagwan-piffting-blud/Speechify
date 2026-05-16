'use strict';
/*
 * viterbi_consts_hook.js -- per-utterance Viterbi structure capture.
 *
 * Function-entry hook on `FUN_08e8b620` (USelGraph::ViterbiWithJoinCache).
 * On each call (one per utterance), reads the constants the join-cost
 * formula depends on:
 *
 *   *(this+4)             : ptr to weight/config struct
 *   *(this+4) + 0x28      : gate_weight (f32)
 *   *(this+4) + 0x84      : miss_offset (f32)
 *   *(this+0x18)          : slot array ptr (per-slot HP cand objects)
 *   *(this+0x20)          : voice ptr (used to look up ccos curve etc.)
 *
 * Plus the ccos-gate curve referenced via `*(int **)(*this + 200)`:
 *
 *   piVar5 = *(int **)(*this + 200)        // 200 = 0xc8 = voice+0xc8
 *   piVar5[0] : curve length (n_entries)
 *   piVar5[1] : offset constant
 *   piVar5[2] : ptr to f32[n_entries] curve
 *
 * All function-entry; no register dereferences without
 * Process.findRangeByAddress validation.
 */

var ADDR_VITERBI = ptr('0x08E8B620');

var stats = { calls: 0, sent: 0, errors: 0 };
var sentOnce = false;     /* dump constants once per session */

function rangeOK(addr) {
    try { var r = Process.findRangeByAddress(addr);
          return r !== null && r.protection.indexOf('r') !== -1; }
    catch (e) { return false; }
}
function safeReadU32(addr) {
    if (!rangeOK(addr)) return null;
    try { return addr.readU32(); } catch (e) { return null; }
}
function safeReadF32(addr) {
    if (!rangeOK(addr)) return null;
    try { return addr.readFloat(); } catch (e) { return null; }
}

Interceptor.attach(ADDR_VITERBI, {
    onEnter: function () {
        stats.calls++;
        if (sentOnce) return;
        var thisPtr = this.context.ecx.toUInt32();
        if (thisPtr < 0x100000) return;

        var t = ptr(thisPtr);
        var weight_ptr = safeReadU32(t.add(4));      /* this+4 -> weight struct */
        var voice_ptr  = safeReadU32(t.add(0x20));   /* this+0x20 (might differ) */
        /* Some fields are dereferenced via *this (NOT this+4) per the
         * Viterbi decompile: `iVar3 = *this; iVar10 = *(int *)(iVar3 + 0x80)`
         * `piVar5 = *(int **)(*this + 200)`. So *this is also a ptr. */
        var first_field = safeReadU32(t);

        var out = {
            type: 'viterbi_consts',
            this_ptr: thisPtr,
            weight_ptr: weight_ptr,
            voice_ptr: voice_ptr,
            first_field: first_field
        };

        if (weight_ptr !== null && weight_ptr >= 0x100000) {
            var w = ptr(weight_ptr);
            out.gate_weight = safeReadF32(w.add(0x28));
            out.miss_offset = safeReadF32(w.add(0x84));
            /* Also probe nearby fields for context (offsets we might
             * confuse with weight struct fields). */
            out.weight_dump = {};
            for (var off = 0x00; off <= 0xa0; off += 4) {
                out.weight_dump['+0x' + off.toString(16)] =
                    safeReadF32(w.add(off));
            }
            /* Also read as ints to check for ptrs / non-float fields. */
            out.weight_dump_u32 = {};
            for (var off2 = 0x00; off2 <= 0xa0; off2 += 4) {
                out.weight_dump_u32['+0x' + off2.toString(16)] =
                    safeReadU32(w.add(off2));
            }
        }

        /* ccos curve at voice+0xc8 -> ptr -> {n, offset, curve_ptr}. */
        if (first_field !== null && first_field >= 0x100000) {
            var ff = ptr(first_field);
            var curveStruct = safeReadU32(ff.add(0xc8));
            out.curve_struct_ptr = curveStruct;
            if (curveStruct !== null && curveStruct >= 0x100000) {
                var cs = ptr(curveStruct);
                out.curve_n        = safeReadU32(cs);
                out.curve_offset_k = safeReadU32(cs.add(4));
                out.curve_data_ptr = safeReadU32(cs.add(8));
                if (out.curve_data_ptr !== null &&
                    out.curve_data_ptr >= 0x100000 &&
                    out.curve_n !== null && out.curve_n > 0 &&
                    out.curve_n < 1024) {
                    var cd = ptr(out.curve_data_ptr);
                    var curve = [];
                    for (var ci = 0; ci < out.curve_n; ++ci) {
                        var v = safeReadF32(cd.add(ci * 4));
                        curve.push(v);
                    }
                    out.curve_data = curve;
                }
            }
        }

        send(out);
        sentOnce = true;
    }
});

rpc.exports = {
    stats: function () { return stats; },
    flush: function () { return stats; },
    reset: function () {
        stats = { calls: 0, sent: 0, errors: 0 };
        sentOnce = false;     /* allow re-dump on next call */
    }
};

send({ type: 'ready', hook: 'viterbi_consts', addr: '0x08E8B620' });
