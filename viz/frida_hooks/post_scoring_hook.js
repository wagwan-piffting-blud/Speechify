'use strict';
/*
 * post_scoring_hook.js -- diagnostic hook on FUN_08e8d210
 * (USelGraph::Populate, the PostScoringAdj phase). Reads cand_buf state
 * BEFORE and AFTER the function to determine whether PostScoringAdj
 * mutates slice's cand_buf+0x04 (our hypothesis for the +18.85 invariant
 * offset on silence boundary slots).
 *
 * Function-entry only. We log:
 *   - this (slice ptr)
 *   - this+0x18 (cand_buf base)
 *   - n_cands (this+0x00)
 *   - first cand's bytes 0..23 BEFORE and AFTER the call.
 */

var ADDR_POST = ptr('0x08E8D210');

var stats = { calls: 0, sent: 0 };

function rangeOK(addr) {
    try { var r = Process.findRangeByAddress(addr);
          return r !== null && r.protection.indexOf('r') !== -1; }
    catch (e) { return false; }
}
function safeReadU32(addr) {
    if (!rangeOK(addr)) return null;
    try { return addr.readU32(); } catch (e) { return null; }
}

function dumpCand(slicePtr) {
    var n_cands = safeReadU32(slicePtr);
    var cb = safeReadU32(slicePtr.add(0x18));
    if (n_cands === null || cb === null || cb < 0x100000) return null;
    var out = { n_cands: n_cands, cb: cb, cand0: [] };
    for (var b = 0; b < 24; ++b) {
        try { out.cand0.push(ptr(cb).add(b).readU8()); }
        catch (e) { return null; }
    }
    return out;
}

Interceptor.attach(ADDR_POST, {
    onEnter: function () {
        stats.calls++;
        send({ type: 'post_scoring_entry', n_call: stats.calls,
               this_ptr: this.context.ecx.toUInt32() });
        var thisPtrVal = this.context.ecx.toUInt32();
        if (thisPtrVal < 0x100000) return;
        this.this_ptr = thisPtrVal;
    },
    onLeave: function () {
        send({ type: 'post_scoring_leave', this_ptr: this.this_ptr });
    }
});

rpc.exports = {
    stats: function () { return stats; },
    flush: function () { return stats; },
    reset: function () { stats = { calls: 0, sent: 0 }; }
};

send({ type: 'ready', hook: 'post_scoring', addr: '0x08E8D210' });
