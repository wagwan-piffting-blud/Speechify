'use strict';
/* One-shot read of the three CCOS histogram-prune globals at fixed VAs:
 *   _DAT_08e98528  INIT_THRESHOLD_OFFSET    (expected ~10000.0f)
 *   _DAT_08e98a24  BIN_SCALE_NUM            (expected ~49.0 for 50-bin histo)
 *   _DAT_08e971d8  BIN_WIDTH
 *
 * Used by plan 02-02 Task 4 to lock in the histogram-prune constants
 * before authoring spfy_anchor_prune().
 */

function safeReadF32(va) {
    try { return ptr(va).readFloat(); } catch (e) { return null; }
}
function safeReadU32(va) {
    try { return ptr(va).readU32(); } catch (e) { return null; }
}

var addrs = {
    INIT_THRESHOLD_OFFSET: '0x08e98528',
    BIN_SCALE_NUM:         '0x08e98a24',
    BIN_WIDTH:             '0x08e971d8'
};

var out = { type: 'ccos_globals' };
for (var name in addrs) {
    var va = addrs[name];
    out[name] = {
        addr:    va,
        f32:     safeReadF32(va),
        u32_hex: '0x' + (safeReadU32(va) >>> 0).toString(16)
    };
}

send(out);
send({ type: 'ready', stats: {} });
rpc.exports = {
    flush: function () { return out; },
    drain: function () { return out; },
    reset: function () {}
};
