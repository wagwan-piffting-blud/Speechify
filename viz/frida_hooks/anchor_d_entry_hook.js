'use strict';
/*
 * anchor_d_entry_hook.js -- dump the anchor D loop's inputs at FUN_08e89530
 * ENTRY, so the durt question tuple can be predicted from the engine's own
 * memory instead of inferred from the walk sequence.
 *
 * FUN_08e89530(this=ECX, net=[esp+4], param_3=[esp+8]), __thiscall, RET 8.
 *
 *   this+0x0c / this+0x10   first / last UNIT index (loop bounds)
 *   this+0x28  -> +0x68     first_hp   (local_34 starts here)
 *              -> +0x6c     last_hp    (advance bound, and the +2 index)
 *   this+0x00  -> +0x20     unit array base, stride 0x18, phone id at +0x14
 *
 * net arrays, all indexed by half-phone (proved by the preselect call site
 * FUN_08e88de0, which passes the same seven values at index = half-phone):
 *
 *   net+0x18  the int array the target-index advance walks for a DIFFERING
 *             entry -- contents are NOT in the decompile, dumped here
 *   net+0x28  q1   sp[1] sylType        (voice+0x268, 9x9)
 *   net+0x2c  q2   sp[0] sylInPhrase    (voice+0x0d8, 10x10)
 *   net+0x34       sp[2] sylInWord      (voice+0x3ac, 7x7)
 *   net+0x38  q8   sp[3] wordInPhrase   (voice+0x470, 7x7)
 *   net+0x3c  q9   sp[4] phoneInSyl     (voice+0x534, 7x7)
 *   net+0x40  q5
 *
 * The decompile says q8/q9 are read at iVar11, which is assigned ONLY inside
 * the two boundary branches: first_hp-2 for every unit except the last, where
 * it becomes last_hp+2. Both can land outside the anchor, and first_hp-2 can
 * go negative -- an unguarded read in the original. We therefore dump the
 * predicted values directly rather than trusting the index arithmetic.
 *
 * Safety: function entry only.
 */

var ADDR_ANCHOR_D = ptr('0x08E89530');

var TOTAL_CAP = 20000;
var stats = { calls: 0, dropped: 0 };

function rangeOK(addr) {
    try {
        var r = Process.findRangeByAddress(addr);
        return r !== null && r.protection.indexOf('r') !== -1;
    } catch (e) { return false; }
}

function rdU32(addr) {
    if (addr.isNull() || !rangeOK(addr)) return null;
    try { return addr.readU32() >>> 0; }
    catch (e) { return null; }
}

function rdS32(addr) {
    if (addr.isNull() || !rangeOK(addr)) return null;
    try { return addr.readS32(); }
    catch (e) { return null; }
}

/* array element at a possibly-negative index, exactly as the engine reads it */
function elem(base, idx) {
    if (base === null || base === 0) return null;
    return rdS32(ptr(base).add(4 * idx));
}

function window(base, lo, hi) {
    var out = [];
    for (var i = lo; i <= hi; i++) out.push(elem(base, i));
    return out;
}

Interceptor.attach(ADDR_ANCHOR_D, {
    onEnter: function () {
        if (stats.calls >= TOTAL_CAP) { stats.dropped++; return; }
        stats.calls++;

        var thiz = this.context.ecx;
        var esp = this.context.esp;
        var net = rdU32(esp.add(4));
        var p3 = rdU32(esp.add(8));
        if (net === null) return;

        var first_unit = rdS32(thiz.add(0x0c));
        var last_unit = rdS32(thiz.add(0x10));

        var span = rdU32(thiz.add(0x28));
        var first_hp = null, last_hp = null;
        if (span !== null) {
            first_hp = rdS32(ptr(span).add(0x68));
            last_hp = rdS32(ptr(span).add(0x6c));
        }

        var a18 = rdU32(ptr(net).add(0x18));
        var a28 = rdU32(ptr(net).add(0x28));
        var a2c = rdU32(ptr(net).add(0x2c));
        var a34 = rdU32(ptr(net).add(0x34));
        var a38 = rdU32(ptr(net).add(0x38));
        var a3c = rdU32(ptr(net).add(0x3c));
        var a40 = rdU32(ptr(net).add(0x40));

        /* phone id (+0x14), duration (+0x12) and the +0x15 flag that gates the
         * target-index advance, for every unit in the loop range */
        var phones = [], firsthalf = [], durs = [];
        var ub = rdU32(ptr(rdU32(thiz) === null ? 0 : rdU32(thiz)).add(0x20));
        if (ub !== null && first_unit !== null && last_unit !== null
            && last_unit - first_unit < 256) {
            for (var i = first_unit; i <= last_unit; i++) {
                var p = null, f = null, d = null;
                try {
                    p = ptr(ub).add(i * 0x18 + 0x14).readU8();
                    f = ptr(ub).add(i * 0x18 + 0x15).readU8();
                    d = ptr(ub).add(i * 0x18 + 0x12).readU8();
                } catch (e) { /* leave nulls */ }
                phones.push(p);
                firsthalf.push(f);
                durs.push(d);
            }
        }

        var lo = null, hi = null;
        if (first_hp !== null && last_hp !== null) { lo = first_hp - 3; hi = last_hp + 3; }

        send({
            type: 'anchor_d_entry',
            n_call: stats.calls,
            first_unit: first_unit, last_unit: last_unit,
            first_hp: first_hp, last_hp: last_hp,
            phones: phones,
            firsthalf: firsthalf,
            durs: durs,
            /* what the decompile says q8/q9 must be */
            pred_q8_nonlast: (first_hp === null) ? null : elem(a38, first_hp - 2),
            pred_q9_nonlast: (first_hp === null) ? null : elem(a3c, first_hp - 2),
            pred_q8_last: (last_hp === null) ? null : elem(a38, last_hp + 2),
            pred_q9_last: (last_hp === null) ? null : elem(a3c, last_hp + 2),
            /* the arrays over the anchor's span plus 3 either side */
            w18: (lo === null) ? null : window(a18, lo, hi),
            w28: (lo === null) ? null : window(a28, lo, hi),
            w2c: (lo === null) ? null : window(a2c, lo, hi),
            w34: (lo === null) ? null : window(a34, lo, hi),
            w38: (lo === null) ? null : window(a38, lo, hi),
            w3c: (lo === null) ? null : window(a3c, lo, hi),
            w40: (lo === null) ? null : window(a40, lo, hi),
            win_lo: lo,
            net: net, p3: p3,
            p3_count: (p3 === null) ? null : rdS32(ptr(p3).add(4))
        });
    }
});

rpc.exports = {
    drain: function () { return stats; },
    flush: function () { return stats; },
    reset: function () { stats = { calls: 0, dropped: 0 }; }
};

send({ type: 'ready' });
