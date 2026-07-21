'use strict';
/*
 * fe_feedconfig_hook.js -- dump every string the engine feeds to the FE
 * via feedConfigA.
 *
 * Target: SWIttsEngine.dll!TTSFrontEnd::speak (FUN_06b03840, RVA 0x3840
 * at Ghidra base 0x06b00000). __thiscall: `this` in ECX, the string ptr
 * is the first stack arg [esp+4]. This is called for BOTH the ESPR mode
 * config header (\!SWIcv... \!SWIespr1 \!SWIwd1) AND the utterance text,
 * so we see the EXACT bytes + real gender/name/phoneset values + the
 * exact control-code escaping the FE actually accepts.
 *
 * Function-entry hook only (safe per run_frida_capture policy).
 */

var RVA = 0x3840;
var MOD = 'SWIttsEngine.dll';

var stats = { calls: 0, dumped: 0 };
var batch = [];

function rangeOK(addr) {
    try {
        var r = Process.findRangeByAddress(addr);
        return r !== null && r.protection.indexOf('r') !== -1;
    } catch (e) { return false; }
}

/* Read a NUL-terminated C string, escaping non-printables so backslashes
 * and control bytes are visible verbatim (critical for the escaping
 * question). Cap length to avoid dumping whole paragraphs of text. */
function readCStrEscaped(pval, cap) {
    if (pval === null || pval < 0x10000) return null;
    var p = ptr(pval >>> 0);
    if (!rangeOK(p)) return null;
    var out = '';
    for (var i = 0; i < cap; i++) {
        var b;
        try { b = p.add(i).readU8(); } catch (e) { break; }
        if (b === 0) break;
        if (b === 0x5c) out += '\\\\';                 // backslash -> \\
        else if (b >= 0x20 && b < 0x7f) out += String.fromCharCode(b);
        else out += '<' + b.toString(16) + '>';
    }
    return out;
}

var base = null;
Process.enumerateModules().forEach(function (m) {
    if (m.name.toLowerCase() === MOD.toLowerCase()) base = m.base;
});

if (base === null) {
    send({ type: 'fe_feedconfig_error', msg: MOD + ' not found' });
} else {
    var addr = base.add(RVA);
    Interceptor.attach(addr, {
        onEnter: function () {
            stats.calls++;
            var strval;
            try { strval = this.context.esp.add(4).readU32() >>> 0; }
            catch (e) { return; }
            var s = readCStrEscaped(strval, 512);
            if (s !== null) {
                stats.dumped++;
                batch.push({ call: stats.calls, len: s.length, str: s });
            }
        }
    });
    send({ type: 'ready', hook: 'fe_feedconfig', addr: addr.toString() });
}

function flush() {
    if (batch.length) { send({ type: 'fe_feedconfig_batch', samples: batch }); batch = []; }
}

rpc.exports = {
    stats: function () { return stats; },
    flush: function () { flush(); return stats; },
    reset: function () { flush(); stats = { calls: 0, dumped: 0 }; }
};
