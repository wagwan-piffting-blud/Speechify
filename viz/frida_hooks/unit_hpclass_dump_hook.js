'use strict';
/*
 * unit_hpclass_dump_hook.js -- one-shot dump of mem+0x13 (halfphone_class)
 * for ALL units in the unit_table. Engine-loaded values, NOT disk-derived.
 *
 * Hooks FUN_08e8ce60 (anchor scorer entry) -- on first call, walks
 * voice memory and dumps every unit's mem+0x13. Then can be used by
 * v7 as authoritative per-uid hp_class lookup.
 */

var ADDR_FUN = ptr('0x08E8CE60');
var dumped = false;

function rangeOK(addr) {
    try {
        var r = Process.findRangeByAddress(addr);
        return r !== null && r.protection.indexOf('r') !== -1;
    } catch (e) { return false; }
}
function safeReadU32(addr) {
    if (!rangeOK(addr)) return null;
    try { return addr.readU32(); } catch (e) { return null; }
}
function safeReadU8(addr) {
    if (!rangeOK(addr)) return null;
    try { return addr.readU8(); } catch (e) { return null; }
}

Interceptor.attach(ADDR_FUN, {
    onEnter: function () {
        if (dumped) return;
        var esp = this.context.esp;
        var net_ptr = safeReadU32(esp.add(0xc));   // param_3 = USelNet
        if (net_ptr === null || net_ptr < 0x100000) return;
        var voice_ptr = safeReadU32(ptr(net_ptr).add(4));
        if (voice_ptr === null || voice_ptr < 0x100000) return;
        var unit_tbl = safeReadU32(ptr(voice_ptr).add(0x20));
        if (unit_tbl === null || unit_tbl < 0x100000) return;

        // n_units from VIN parse: Tom=169579, Jill=191871.
        // Hardcoded; flip when targeting a different voice.
        var n_units = 169579;  /* TOM (default) */
        var hpclass = new Array(n_units);
        var ok_count = 0;
        for (var uid = 0; uid < n_units; ++uid) {
            var v = safeReadU8(ptr(unit_tbl).add(uid * 0x18 + 0x13));
            hpclass[uid] = (v !== null) ? v : -1;
            if (v !== null) ok_count++;
        }

        // Send in chunks (50000 per message to keep payload manageable)
        var chunk = 50000;
        for (var i = 0; i < n_units; i += chunk) {
            var part = hpclass.slice(i, Math.min(i + chunk, n_units));
            send({ type: 'unit_hpclass_chunk',
                   start: i, end: i + part.length,
                   hpclass: part });
        }
        send({ type: 'unit_hpclass_done',
               n_units: n_units, ok_count: ok_count,
               unit_table_base: unit_tbl >>> 0 });
        dumped = true;
    }
});

send({ type: 'ready' });

rpc.exports = {
    flush: function () { return { dumped: dumped }; },
    drain: function () { return { dumped: dumped }; },
    reset: function () { dumped = false; }
};
