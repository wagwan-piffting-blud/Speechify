'use strict';
/*
 * fe_token_hook.js  (v2 -- strict pointer validation, 2026-05-05)
 *
 * Closes plan FE-track gap #10. SWIttsFe-en-US.dll is a black box; we
 * recover its OUTPUT format by hooking the boundary where it hands off
 * to SWIttsUSel. Per viterbi_hook.js this orchestrator entry fires once
 * per synthesis, so call rate is low.
 *
 * Anchor (SWIttsUSel.dll):
 *   0x08E819E0   SWIttsUSel orchestrator entry.
 *
 * v1 crashed Speechify.exe across the corpus because the "blocks" probe
 * called `ptr(v).readByteArray(0x40)` on any u32 register/stack value
 * > 0x100000. Many such values are integers, not pointers; the read
 * AV'd. The try/catch did NOT reliably recover from Windows access
 * violations.
 *
 * v2 fix: every memory dereference is gated by Process.findRangeByAddress
 * which queries the live memory map without dereferencing. Only proceed
 * if the address falls in a readable mapped range AND the read won't
 * exit the range. Same pattern as wsola_buffer v2.
 */

var ADDR_USEL = ptr('0x08E819E0');

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

function safeReadBytes(addr_u32, n) {
    if (!isReadable(addr_u32, n)) return null;
    try { return ptr(addr_u32).readByteArray(n); } catch (e) { return null; }
}

var calls = 0;
var TOTAL_CAP = 4096;

Interceptor.attach(ADDR_USEL, {
    onEnter: function () {
        calls++;
        if (calls > TOTAL_CAP) return;

        var ctx = this.context;
        var esp = ctx.esp;
        var probe = { call: calls, ecx: ctx.ecx.toUInt32(), stack: {} };
        for (var i = 1; i <= 8; ++i) {
            probe.stack['esp+' + (i * 4)] = safeReadU32(esp.add(i * 4).toUInt32());
        }
        /* For each slot that points into a readable range, dump 0x40 bytes.
         * Strict validation -- bytes only get read if the entire read window
         * fits inside one readable range. */
        var blocks = {};
        var seen = {};
        var slots = [probe.ecx, probe.stack['esp+4'], probe.stack['esp+8'],
                     probe.stack['esp+12']];
        for (var i = 0; i < slots.length; ++i) {
            var v = slots[i];
            if (!v || seen[v]) continue;
            seen[v] = 1;
            var bytes = safeReadBytes(v, 0x40);
            if (bytes !== null) {
                blocks['@0x' + v.toString(16)] = Array.from(new Uint8Array(bytes));
            }
        }
        probe.blocks = blocks;
        send({ type: 'fe_tokens', probe: probe });
    }
});

send({ type: 'ready', hook: 'fe_token', cap: TOTAL_CAP });
