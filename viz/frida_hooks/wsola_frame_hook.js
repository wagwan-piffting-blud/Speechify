'use strict';
/*
 * wsola_frame_hook.js -- per-RESYNC-frame decisions on the time-scaled path.
 *
 * The span cursor (wsola_cursor_hook) proved the frame COUNTS match; what it
 * cannot show is which input position each frame was taken from. That is
 * decided inside FUN_08ee36e0's loop and only becomes visible as an argument
 * to the blend.
 *
 * Anchor: FUN_08ee1240 @ 0x08EE1240, the crossfade.
 *   __thiscall FUN_08ee1240(this /*ECX*\/, dest, hist_ptr, head_ptr)
 * so esp+4 = dest, esp+8 = hist_ptr, esp+0xc = head_ptr. Offsets are taken
 * against the span buffer base (this+0x34) to give buffer indices:
 *   rp = (hist_ptr - base)/2      the current input cursor
 *   np = (head_ptr - base)/2      the position the lag search picked
 *
 * Also grabs this+0x35b0 (ideal) and this+0x35b4 (out_pos) so each frame's
 * decision can be replayed against ours arithmetic-for-arithmetic.
 *
 * ⚠ SAFETY: this is a function ENTRY hook, but it is NOT once-per-span --
 * it fires once per resync frame, a few hundred per utterance. That is well
 * short of the Viterbi inner-loop rates that killed the server on
 * 2026-05-05, but it is not free either, so TOTAL_CAP is deliberately tight
 * and this should be run over ONE short utterance at a time, never the whole
 * corpus.
 */

var ADDR_BLEND = ptr('0x08EE1240');

var BATCH_N = 128;
var TOTAL_CAP = 6000;
var stats = { frames: 0, sent: 0, dropped: 0 };
var batch = [];

function flush() {
    if (batch.length === 0) return;
    send({ type: 'wsola_frame_batch', samples: batch });
    stats.sent += batch.length;
    batch = [];
}

Interceptor.attach(ADDR_BLEND, {
    onEnter: function (args) {
        stats.frames++;
        if (stats.frames > TOTAL_CAP) { stats.dropped++; return; }
        try {
            var self = this.context.ecx;
            var base = self.add(0x34).readU32();
            if (!base) { stats.dropped++; return; }
            var esp = this.context.esp;
            var hist = esp.add(8).readU32();
            var head = esp.add(0xc).readU32();
            batch.push({
                f:  stats.frames,
                rp: (hist - base) / 2,
                np: (head - base) / 2,
                ideal:   self.add(0x35b0).readS32(),
                out_pos: self.add(0x35b4).readS32(),
                unit_i:  self.add(0x35bc).readS32(),
                scale:   self.add(0x35e4).readFloat(),
                armed:   self.add(0x2c).readU8(),
            });
        } catch (e) { stats.dropped++; return; }
        if (batch.length >= BATCH_N) flush();
    }
});

rpc.exports = {
    stats: function () { return stats; },
    flush: function () { flush(); return stats; },
    reset: function () { flush(); stats = { frames: 0, sent: 0, dropped: 0 }; },
};

send({ type: 'ready', hook: 'wsola_frame', batch_n: BATCH_N, cap: TOTAL_CAP });
