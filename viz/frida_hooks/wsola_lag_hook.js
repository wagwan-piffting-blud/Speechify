'use strict';
/*
 * wsola_lag_hook.js -- the join lag search's full inputs and its answer.
 *
 * Anchor: FUN_08ee1330 @ 0x08EE1330, called once per join from FUN_08ee3560.
 *   __thiscall FUN_08ee1330(this /*ECX*\/, hist_ptr, head_ptr) -> lag
 * this+0xc = corr (history length), this+0x10 = frame (head samples copied),
 * this+0x4 = W (max lag), this+0x28 = stride.
 *
 * Dumps hist[0..corr) and head[0..frame) as int16 so a spfy join dump
 * (SPFY_WSOLA_DUMP_JOIN) can be diffed sample-for-sample. Fires once per
 * join -- a few dozen per utterance -- but each event is ~800 bytes, so run
 * it over one short text at a time.
 */

var ADDR_LAG = ptr('0x08EE1330');
var TOTAL_CAP = 400;
var stats = { calls: 0, sent: 0, dropped: 0 };

function readS16s(p, n) {
    var out = new Array(n);
    for (var i = 0; i < n; ++i) out[i] = p.add(i * 2).readS16();
    return out;
}

Interceptor.attach(ADDR_LAG, {
    onEnter: function (args) {
        this.rec = null;
        stats.calls++;
        if (stats.calls > TOTAL_CAP) { stats.dropped++; return; }
        try {
            var self = this.context.ecx;
            var esp = this.context.esp;
            var hist = ptr(esp.add(4).readU32());
            var head = ptr(esp.add(8).readU32());
            var corr = self.add(0xc).readS32();
            var frame = self.add(0x10).readS32();
            this.rec = {
                type: 'wsola_lag',
                call: stats.calls,
                W: self.add(0x4).readS32(),
                corr: corr,
                frame: frame,
                stride: self.add(0x28).readS32(),
                hist: readS16s(hist, corr),
                head: readS16s(head, frame),
            };
        } catch (e) { stats.dropped++; this.rec = null; }
    },
    onLeave: function (retval) {
        if (!this.rec) return;
        this.rec.lag = retval.toInt32();
        send(this.rec);
        stats.sent++;
    }
});

rpc.exports = {
    stats: function () { return stats; },
    flush: function () { return stats; },
    reset: function () { stats = { calls: 0, sent: 0, dropped: 0 }; },
};

send({ type: 'ready', hook: 'wsola_lag', cap: TOTAL_CAP });
