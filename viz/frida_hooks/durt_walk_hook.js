'use strict';
/*
 * durt_walk_hook.js -- capture per-call (is_first_half, leaf_mean, leaf_var)
 * for FUN_08e87d90 (engine's anchor-time durt CART walker) to verify the
 * hypothesis behind project_anchor_d_cost_bug_2026_05_13:
 *   - Per target_hp, walks produce up to 2 distinct (mean, var) pairs,
 *     selected by unit's is_first_half (mem+0x14, the EDX arg).
 *
 * Calling shape (verified via assembly @ 0x08e896d3):
 *   ECX = first stack arg (durt object)
 *   EDX = is_first_half byte
 *   EAX (implicit on entry, used inside) = per-target durt object ptr
 *     -> *(EAX+0x10) is the entry table indexed by is_first_half.
 *   Return: EAX = leaf node ptr. mean @+0x10, var @+0x14.
 *
 * Safety: function-entry/leave only.
 */

var ADDR_DURT_WALK = ptr('0x08E87D90');

var TOTAL_CAP = 5000;
var stats = { calls: 0, leaves: 0, dropped: 0 };

function rangeOK(addr) {
    try {
        var r = Process.findRangeByAddress(addr);
        return r !== null && r.protection.indexOf('r') !== -1;
    } catch (e) { return false; }
}

function safeReadU32(addr) {
    if (!rangeOK(addr)) return null;
    try { return addr.readU32(); }
    catch (e) { return null; }
}

function safeReadF32(addr) {
    if (!rangeOK(addr)) return null;
    try { return addr.readFloat(); }
    catch (e) { return null; }
}

Interceptor.attach(ADDR_DURT_WALK, {
    onEnter: function () {
        if (stats.calls >= TOTAL_CAP) { stats.dropped++; this.skip = true; return; }
        stats.calls++;
        this.state = {
            n_call: stats.calls,
            is_first_half: this.context.edx.toUInt32() & 0xff,
            in_eax: this.context.eax.toUInt32() >>> 0,
            ecx_arg: this.context.ecx.toUInt32() >>> 0
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
        send({
            type: 'durt_walk',
            n_call: this.state.n_call,
            is_first_half: this.state.is_first_half,
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
