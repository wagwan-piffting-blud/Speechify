'use strict';
/*
 * wsola_cursor_hook.js -- the engine's per-span output cursor.
 *
 * Answers one question the wsola_in capture cannot: how many samples does
 * the engine actually emit per span on the TIME-SCALED path? Every duration
 * model fitted from outside has had to guess a per-span constant, and three
 * different guesses each produced a plausible-looking but wrong curve.
 *
 * Anchor: FUN_08ee1700 @ 0x08EE1700, the tail-save that FUN_08ee3aa0 calls
 * once after every span's body. __fastcall, so `this` is in ECX.
 *
 * SAFETY: function ENTRY only, and once per span -- a handful of calls per
 * phrase, not a hot loop. That is the standing rule after the 2026-05-05
 * server kills, and this hook is on the right side of it.
 *
 * Fields, all per-span state documented in spfy/src/wsola/wsola.h:
 *   +0x35f4  running TOTAL output samples emitted    <- the answer
 *   +0x35a8  read_pos (input cursor)
 *   +0x35b4  out_pos  (output cursor, per span)
 *   +0x35bc  current unit index within the span
 *   +0x35c0  unit count for the span
 *   +0x35dc  out_end for the current unit (samples)
 *   +0x35e0  accumulated target (float, ms)
 *   +0x35e4  current scale (float)
 *   +0x2c    time-scaling armed
 */

var ADDR_TAIL_SAVE = ptr('0x08EE1700');

function isReadable(addr, nbytes) {
    if (addr.isNull()) return false;
    var r = Process.findRangeByAddress(addr);
    if (!r) return false;
    if (r.protection.charAt(0) !== 'r') return false;
    return addr.add(nbytes).compare(r.base.add(r.size)) <= 0;
}

var BATCH_N = 64;
var TOTAL_CAP = 20000;
var stats = { spans: 0, sent: 0, dropped: 0 };
var batch = [];

function flush() {
    if (batch.length === 0) return;
    send({ type: 'wsola_cursor_batch', samples: batch });
    stats.sent += batch.length;
    batch = [];
}

Interceptor.attach(ADDR_TAIL_SAVE, {
    onEnter: function () {
        stats.spans++;
        if (stats.spans > TOTAL_CAP) { stats.dropped++; return; }
        var self = this.context.ecx;
        if (!isReadable(self, 0x3620)) { stats.dropped++; return; }
        try {
            batch.push({
                span:    stats.spans,
                armed:   self.add(0x2c).readU8(),
                emitted: self.add(0x35f4).readS32(),
                read_pos: self.add(0x35a8).readS32(),
                out_pos: self.add(0x35b4).readS32(),
                unit_i:  self.add(0x35bc).readS32(),
                unit_n:  self.add(0x35c0).readS32(),
                out_end: self.add(0x35dc).readS32(),
                acc_ms:  self.add(0x35e0).readFloat(),
                scale:   self.add(0x35e4).readFloat(),
            });
        } catch (e) { stats.dropped++; return; }
        if (batch.length >= BATCH_N) flush();
    }
});

rpc.exports = {
    stats: function () { return stats; },
    flush: function () { flush(); return stats; },
    reset: function () { flush(); stats = { spans: 0, sent: 0, dropped: 0 }; },
};

send({ type: 'ready', hook: 'wsola_cursor', batch_n: BATCH_N, cap: TOTAL_CAP });
