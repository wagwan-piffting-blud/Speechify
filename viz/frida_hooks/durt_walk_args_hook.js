'use strict';
/*
 * durt_walk_args_hook.js -- capture the FULL question tuple handed to the
 * engine's durt CART walker FUN_08e87d90, per call.
 *
 * ABI recovered from the disassembly (Ghidra, bin/SWIttsUSel.dll):
 *
 *   FUN_08e87d90:  EAX = model object; the forest table is *(EAX+0x10),
 *                        indexed by EDX.
 *                  EDX = phone id (CART forest index).
 *                  ECX = dead (overwritten by the first instruction).
 *                  7 caller-cleaned stack args at [esp+4 .. esp+0x1c].
 *
 *   The walker forwards those 7 to dispatcher FUN_08e87c90, which selects by
 *   the node's q_type. Following each q_type's compare operand back through
 *   both prologues gives:
 *
 *      stack arg  0    1    2    3    4    5    6
 *      question   q1   q2   q3   q4   q5   q9   q8
 *
 *   plus q7 <- EBX, which the walker zeroes (XOR EBX,EBX) before every
 *   dispatch, so q7 is always 0.
 *
 * returnAddress separates the three call sites without guessing:
 *   0x08e88f8b  preselect per-half-phone walk (FUN_08e88de0)
 *   0x08e896d8  anchor D walk                 (FUN_08e89530)
 *   0x08e8dc7d  third site                    (FUN_08e8d550)
 *
 * Safety: function-entry/leave only -- no mid-function trampolines.
 */

var ADDR_DURT_WALK = ptr('0x08E87D90');

var TOTAL_CAP = 20000;
var stats = { calls: 0, leaves: 0, dropped: 0 };

function rangeOK(addr) {
    try {
        var r = Process.findRangeByAddress(addr);
        return r !== null && r.protection.indexOf('r') !== -1;
    } catch (e) { return false; }
}

function safeReadF32(addr) {
    if (!rangeOK(addr)) return null;
    try { return addr.readFloat(); }
    catch (e) { return null; }
}

function argS32(esp, i) {
    try { return esp.add(4 + 4 * i).readS32(); }
    catch (e) { return null; }
}

Interceptor.attach(ADDR_DURT_WALK, {
    onEnter: function () {
        if (stats.calls >= TOTAL_CAP) { stats.dropped++; this.skip = true; return; }
        stats.calls++;
        var esp = this.context.esp;
        this.state = {
            n_call: stats.calls,
            ret: this.returnAddress.toUInt32() >>> 0,
            phone: this.context.edx.toUInt32() & 0xff,
            model: this.context.eax.toUInt32() >>> 0,
            /* stack arg index -> question number */
            q1: argS32(esp, 0),
            q2: argS32(esp, 1),
            q3: argS32(esp, 2),
            q4: argS32(esp, 3),
            q5: argS32(esp, 4),
            q9: argS32(esp, 5),
            q8: argS32(esp, 6)
        };
    },
    onLeave: function (retval) {
        if (this.skip || !this.state) return;
        stats.leaves++;
        var leaf_ptr = retval ? retval.toUInt32() : 0;
        var mean = null, vr = null;
        if (leaf_ptr >= 0x100000) {
            var lp = ptr(leaf_ptr);
            mean = safeReadF32(lp.add(0x10));
            vr   = safeReadF32(lp.add(0x14));
        }
        var s = this.state;
        send({
            type: 'durt_walk_args',
            n_call: s.n_call,
            ret: s.ret,
            phone: s.phone,
            model: s.model,
            q1: s.q1, q2: s.q2, q3: s.q3, q4: s.q4,
            q5: s.q5, q8: s.q8, q9: s.q9,
            leaf_ptr: leaf_ptr >>> 0,
            leaf_mean: mean,
            leaf_var: vr
        });
    }
});

rpc.exports = {
    drain: function () { return stats; },
    flush: function () { return stats; },
    reset: function () { stats = { calls: 0, leaves: 0, dropped: 0 }; }
};

send({ type: 'ready' });
