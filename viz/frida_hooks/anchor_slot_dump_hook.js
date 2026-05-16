'use strict';
/*
 * anchor_slot_dump_hook.js — dump the slot arrays at FUN_08e8ce60 entry
 * to localize per-phrase slot-data divergence.
 *
 * Engine's anchor TC for SAME UID span (e.g. edge_024 ss=74338) varies
 * across phrases (n_call=4 tc=0.270 vs n_calls 20/36/53/71/89 tc=1.604).
 * The 6 events have different anchor_slot pointers — engine processes
 * each phrase's sub-tree independently. Slot fidelity audit is 100%
 * (per-slot HP-cand CART q-vectors match engine), so the divergence
 * must be in slot data fields NOT exposed via cart_walks_safe — most
 * likely workspace arrays at param_2+0x18..+0x40 on the USelNet
 * (filled by FUN_08e8cbb0 → FUN_08e8c7d0 → FUN_08e8a670/880).
 *
 * This hook captures, per FUN_08e8ce60 call:
 *   - anchor_slot pointer (param_2)
 *   - net pointer (param_3)
 *   - first_hp, last_hp (param_6/7)
 *   - param_3+0x18 array[first_hp..last_hp] (syl_idx_per_hp)
 *   - param_3+0x1c array[first_hp..last_hp] (word_idx_per_hp)
 *   - param_3+0x24 array[first_hp..last_hp] (stress_per_hp)
 *   - param_3+0x28 array[first_hp..last_hp] (stress code)
 *   - param_3+0x2c array[first_hp..last_hp] (position code)
 *   - param_3+0x30 array[first_hp..last_hp] (f0_curve_idx — F0 mean adj)
 *   - param_3+0x34/38/3c arrays[first_hp..last_hp] (sp_target indices)
 *
 * Safety: function-entry only, bounded reads.
 */

var ADDR_ANCHOR_CE60 = ptr('0x08E8CE60');
var TOTAL_CAP = 1000;

var stats = { calls: 0, dropped: 0, ptr_invalid: 0, read_errors: 0 };

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

function readArr(base_ptr, first, last) {
    if (!base_ptr || base_ptr < 0x100000) return null;
    var out = [];
    for (var i = first; i <= last; ++i) {
        var v = safeReadU32(ptr(base_ptr).add(i * 4));
        out.push(v === null ? -1 : (v | 0));   /* sign-extend for -1 sentinels */
    }
    return out;
}

Interceptor.attach(ADDR_ANCHOR_CE60, {
    onEnter: function () {
        if (stats.calls >= TOTAL_CAP) { stats.dropped++; return; }
        stats.calls++;

        var esp = this.context.esp;
        var p2 = safeReadU32(esp.add(8));    /* anchor_slot */
        var p3 = safeReadU32(esp.add(12));   /* USelNet */
        var p5 = safeReadU32(esp.add(20));   /* type */
        var p6 = safeReadU32(esp.add(24));   /* first_hp */
        var p7 = safeReadU32(esp.add(28));   /* last_hp */

        var first_hp = (p6 === null ? -1 : (p6 >>> 0));
        var last_hp  = (p7 === null ? -1 : (p7 >>> 0));

        var rec = {
            type: 'anchor_slot_dump',
            n_call: stats.calls,
            anchor_type: (p5 === null ? -1 : (p5 >>> 0)),
            first_hp: first_hp,
            last_hp: last_hp,
            anchor_slot: (p2 === null ? 0 : (p2 >>> 0)),
            net: (p3 === null ? 0 : (p3 >>> 0))
        };

        if (p3 && p3 >= 0x100000 && first_hp >= 0 && last_hp >= first_hp) {
            var net = ptr(p3);
            /* The arrays live at net+0x18..+0x40 — see FUN_08e8cbb0
             * decomp. Each is an int pointer. */
            var ptr_18 = safeReadU32(net.add(0x18));
            var ptr_1c = safeReadU32(net.add(0x1c));
            var ptr_24 = safeReadU32(net.add(0x24));
            var ptr_28 = safeReadU32(net.add(0x28));
            var ptr_2c = safeReadU32(net.add(0x2c));
            var ptr_30 = safeReadU32(net.add(0x30));
            var ptr_34 = safeReadU32(net.add(0x34));
            var ptr_38 = safeReadU32(net.add(0x38));
            var ptr_3c = safeReadU32(net.add(0x3c));

            rec.syl_idx     = readArr(ptr_18, first_hp, last_hp);
            rec.word_idx    = readArr(ptr_1c, first_hp, last_hp);
            rec.stress_raw  = readArr(ptr_24, first_hp, last_hp);
            rec.stress_code = readArr(ptr_28, first_hp, last_hp);
            rec.pos_code    = readArr(ptr_2c, first_hp, last_hp);
            rec.f0_curve_i  = readArr(ptr_30, first_hp, last_hp);
            rec.sp2         = readArr(ptr_34, first_hp, last_hp);
            rec.sp3         = readArr(ptr_38, first_hp, last_hp);
            rec.sp4         = readArr(ptr_3c, first_hp, last_hp);
        }

        send(rec);
    }
});

rpc.exports = {
    drain: function () { return stats; },
    flush: function () { return stats; },
    reset: function () {
        stats = { calls: 0, dropped: 0, ptr_invalid: 0, read_errors: 0 };
    }
};

send({ type: 'ready' });
