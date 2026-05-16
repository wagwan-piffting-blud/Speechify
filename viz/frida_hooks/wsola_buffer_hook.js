'use strict';
/*
 * wsola_buffer_hook.js  (v2 -- strict pointer validation, 2026-05-05)
 *
 * Closes plan gap #3 in part: captures the input unit lattice that WSOLA
 * concatenates per utterance. The output-buffer probe that crashed
 * Speechify.exe in v1 has been REMOVED -- it dereferenced arbitrary
 * pointer-sized values from the WSOLA object without validation. The
 * output side will be revisited via a separate dedicated hook once we
 * know which field actually holds the PCM buffer.
 *
 * Anchor (SWIttsWsola.dll, confirmed via viterbi_hook.js):
 *   ADDR_WSOLA_CONCAT = 0x08EE65E0   ; SWIttsWsola::concat -- top-level entry
 *
 * Safety: all memory reads go through safeReadU32() which first checks
 * the address against the readable-range allowlist captured at hook
 * load. Any miss is silently dropped.
 */

var ADDR_WSOLA_CONCAT = ptr('0x08EE65E0');

/* Per-address validity check via Process.findRangeByAddress(). The cached-
 * allowlist approach with Process.enumerateRanges() didn't work because
 * the protection-filter semantics ('r--' meaning "at least readable") in
 * Frida 17 effectively excluded RW pages -- the stack and most heap.
 * findRangeByAddress queries the live memory map per call which costs
 * more but is correct. We hook a per-utterance function so the call rate
 * is low (handful per phrase). */
function isReadable(addr_u32, nbytes) {
    if (addr_u32 === null || addr_u32 < 0x10000) return false;
    var r = Process.findRangeByAddress(ptr(addr_u32));
    if (!r) return false;
    if (r.protection.charAt(0) !== 'r') return false;
    var end_addr = ptr(addr_u32).add(nbytes);
    var rend = r.base.add(r.size);
    return end_addr.compare(rend) <= 0;
}

function safeReadU32(addr_u32) {
    if (!isReadable(addr_u32, 4)) return null;
    try { return ptr(addr_u32).readU32(); } catch (e) { return null; }
}

var BATCH_N = 32;
var TOTAL_CAP = 4096;
var stats = { utts: 0, sent: 0, dropped: 0 };
var batch = [];

function flush() {
    if (batch.length === 0) return;
    send({ type: 'wsola_in_batch', samples: batch });
    stats.sent += batch.length;
    batch = [];
}

Interceptor.attach(ADDR_WSOLA_CONCAT, {
    onEnter: function () {
        stats.utts++;
        if (stats.utts > TOTAL_CAP) { stats.dropped++; return; }

        var esp = this.context.esp;
        /* Stack arg 4 (per-decomp + viterbi_hook): pointer to a struct
         * with { count @ +0x04, units_array_ptr @ +0x08, stride 0x18 }. */
        var arg4_val = safeReadU32(esp.add(16).toUInt32());
        if (arg4_val === null)                      return;
        if (!isReadable(arg4_val, 0x10))            return;

        var arg4 = ptr(arg4_val);
        var count = safeReadU32(arg4.add(0x04).toUInt32());
        var arr   = safeReadU32(arg4.add(0x08).toUInt32());
        if (count === null || arr === null)         return;
        if (count < 1 || count > 1000)              return;
        if (!isReadable(arr, count * 0x18))         return;

        var aptr  = ptr(arr);
        var units = [];
        for (var i = 0; i < count; ++i) {
            var b = aptr.add(i * 0x18);
            units.push({
                uid: safeReadU32(b.toUInt32()),
                lp : safeReadU32(b.add(0x04).toUInt32()),
                dl : safeReadU32(b.add(0x08).toUInt32()),
            });
        }
        batch.push({ utt: stats.utts, n_units: count, units: units });
        if (batch.length >= BATCH_N) flush();
    }
    /* onLeave intentionally absent: v1 obj_probe sweep was unsafe. */
});

rpc.exports = {
    stats: function () { return stats; },
    flush: function () { flush(); return stats; },
    reset: function () {
        flush();
        stats = { utts: 0, sent: 0, dropped: 0 };
    },
};

send({
    type: 'ready', hook: 'wsola_buffer',
    batch_n: BATCH_N, cap: TOTAL_CAP,
});
