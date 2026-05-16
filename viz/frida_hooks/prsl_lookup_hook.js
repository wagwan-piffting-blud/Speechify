'use strict';
/*
 * prsl_lookup_hook.js -- log every PRSL triphone-context candidate query.
 *
 * Closes plan gaps #5 and #6:
 *   #5 (PRSL out-of-pool UIDs)  -- ~39 of 178 slots in our Python repro
 *      reference UIDs outside the reconstructed candidate pool. Live trace
 *      tells us whether they're sentinels, secondary-pool refs, or
 *      pool-reconstruction misses.
 *   #6 (PRSL fallback chain)    -- when the triphone key
 *      `left_hp*10000 + center_hp*100 + right_hp` misses, the backoff
 *      ordering (bigram? unigram? hash-bucket?) is undocumented. Trigger
 *      misses deliberately and observe.
 *
 * Anchor (SWIttsUSel.dll, confirmed via reveng/DLL_ANALYSIS.md):
 *   0x8E89A70   PRSL lookup -- returns candidate UIDs for a triphone key.
 *
 * Calling convention is presumed __thiscall (typical MSVC C++ method):
 *   ecx = this (PRSL object); args via stack starting at esp+4.
 * Concrete arg layout TBD -- log on enter:
 *   - ecx (this), edx, [esp+4..esp+0x18] candidate args (probably context
 *     key and output buffer)
 * On leave:
 *   - retval (probably u32 hit-count or status)
 *
 * Output: streamed batched 'prsl_lookup_batch' send() messages. We avoid
 * ring + synchronous drain RPC because that stalls under back-pressure.
 */

var ADDR_PRSL_LOOKUP = ptr('0x8E89A70');

var BATCH_N = 64;
var TOTAL_CAP = 50000;
var stats = { calls: 0, sent: 0, dropped: 0 };
var batch = [];

function tryU32(addr) { try { return addr.readU32(); } catch (e) { return null; } }

function flush() {
    if (batch.length === 0) return;
    send({ type: 'prsl_lookup_batch', samples: batch });
    stats.sent += batch.length;
    batch = [];
}

Interceptor.attach(ADDR_PRSL_LOOKUP, {
    onEnter: function () {
        stats.calls++;
        if (stats.calls > TOTAL_CAP) { stats.dropped++; return; }
        var ctx = this.context;
        var esp = ctx.esp;
        var args = {
            ecx: ctx.ecx.toUInt32(),
            edx: ctx.edx.toUInt32(),
        };
        for (var i = 1; i <= 6; ++i) {
            args['esp+' + (i * 4)] = tryU32(esp.add(i * 4));
        }
        this.snapshot = { n: stats.calls, args: args };
    },
    onLeave: function (retval) {
        if (!this.snapshot) return;
        this.snapshot.retval = retval.toUInt32();
        batch.push(this.snapshot);
        if (batch.length >= BATCH_N) flush();
    }
});

rpc.exports = {
    stats: function () { return stats; },
    flush: function () { flush(); return stats; },
    reset: function () {
        flush();
        stats = { calls: 0, sent: 0, dropped: 0 };
    },
};

send({ type: 'ready', hook: 'prsl_lookup', batch_n: BATCH_N, cap: TOTAL_CAP });
