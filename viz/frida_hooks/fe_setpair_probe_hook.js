'use strict';
/*
 * fe_setpair_probe_hook.js — find where utt_ptr lives in the engine's
 * `user_arg` struct. Strategy:
 *
 *   1. Hook FE's setPair_E/F/G/H + installHookA/B vtable wrappers
 *      (slots 41/43/45/46/47/48) at entry. Capture (cb_ptr, user_arg).
 *      These are the registrations — they tell us the engine's
 *      callback addresses + the engine's `self` pointer (user_arg).
 *
 *   2. For each unique (cb_ptr, user_arg) pair, attach a fresh
 *      Interceptor on the cb_ptr ENTRY. That's where the engine's
 *      adapter receives control when the FE wants to emit on that
 *      channel.
 *
 *   3. In each cb's onEnter, dump the FIRST few u32s of user_arg
 *      (the engine's state struct). Also capture utt_ptr from the
 *      previously-seen SWIttsUSelUnitSelection call (if any) and
 *      search the user_arg deref for it.
 *
 *   4. Cross-reference: the offset at which utt_ptr appears in
 *      user_arg = the offset where the engine stashes utt. That's
 *      the field we need to mimic in our fe_engine_stubs.c.
 *
 * Output: per-call records, one per cb invocation:
 *   { type: 'setpair_cb_entry', slot, cb_va, user_arg,
 *     user_arg_dump: [...u32s...], esp_dump: [...u32s...],
 *     utt_hit_offset: <offset or null> }
 */

var SLOT_VAS = {
    41: '0x0836d150',  // setPair_E
    43: '0x0836d1c0',  // setPair_F
    45: '0x0836d230',  // setPair_G
    46: '0x0836d250',  // setPair_H
    47: '0x0836d270',  // installHookA
    48: '0x0836d290',  // installHookB
};
var SLOT_NAMES = {
    41:'setPair_E', 43:'setPair_F', 45:'setPair_G',
    46:'setPair_H', 47:'installHookA', 48:'installHookB',
};

/* Resolve SWIttsUSelUnitSelection dynamically — engine is ASLR'd so
 * the hardcoded VA from earlier captures will be stale next session. */
var ADDR_USEL = null;
try {
    ADDR_USEL = Module.findExportByName('SWIttsEngine.dll',
                                        'SWIttsUSelUnitSelection');
} catch (e) {}
if (!ADDR_USEL) {
    /* Fallback: brute scan modules for the export name. */
    var mods = Process.enumerateModules();
    for (var i = 0; i < mods.length; i++) {
        try {
            var exps = mods[i].enumerateExports();
            for (var j = 0; j < exps.length; j++) {
                if (exps[j].name.indexOf('USelUnitSelection') !== -1) {
                    ADDR_USEL = exps[j].address;
                    break;
                }
            }
        } catch (e) {}
        if (ADDR_USEL) break;
    }
}
var g_last_utt_ptr = null;

var USER_ARG_DUMP_U32S = 0x80;   /* 512 bytes of user_arg */
var ESP_DUMP_U32S      = 0x10;   /* 64 bytes of caller stack */

function rangeOK(addr) {
    try {
        var r = Process.findRangeByAddress(addr);
        return r !== null && r.protection.indexOf('r') !== -1;
    } catch (e) { return false; }
}

function readU32Array(p, n) {
    var arr = [];
    if (!p || !rangeOK(p)) return arr;
    try {
        var bytes = new Uint8Array(p.readByteArray(n * 4));
        for (var i = 0; i + 4 <= bytes.length; i += 4) {
            var v = (bytes[i] | (bytes[i+1]<<8) | (bytes[i+2]<<16) |
                     (bytes[i+3]<<24)) >>> 0;
            arr.push(v);
        }
    } catch (e) {}
    return arr;
}

function findU32(arr, needle) {
    if (needle === null) return null;
    for (var i = 0; i < arr.length; i++) {
        if (arr[i] === needle) return i * 4;  /* byte offset */
    }
    return null;
}

/* For each cb_ptr we see registered, attach a probe to it (idempotent). */
var g_probed = {};
var stats = { regs: 0, probes_attached: 0, cb_calls: 0, sent: 0, errs: 0 };

function attachCbProbe(slot, cb_va) {
    if (cb_va === 0) return;
    var key = '0x' + cb_va.toString(16) + ':' + slot;
    if (g_probed[key]) return;
    g_probed[key] = 1;
    try {
        Interceptor.attach(ptr(cb_va), {
            onEnter: function (args) {
                stats.cb_calls++;
                try {
                    var esp = this.context.esp;
                    var esp_dump = readU32Array(esp, ESP_DUMP_U32S);
                    /* the first stack arg is the engine's `self`
                     * (user_arg) — i.e. esp[+4] for __cdecl. We
                     * also probe esp[+8] / [+c] / [+10] for other args. */
                    var arg0 = esp_dump[1] || 0;  /* esp+4 */
                    var arg1 = esp_dump[2] || 0;  /* esp+8 */
                    var arg2 = esp_dump[3] || 0;  /* esp+c */
                    var arg3 = esp_dump[4] || 0;  /* esp+10 */
                    var ua_dump = (arg0 && (arg0 >>> 0) > 0x100000)
                        ? readU32Array(ptr(arg0 >>> 0), USER_ARG_DUMP_U32S)
                        : [];
                    var utt_hit = findU32(ua_dump, g_last_utt_ptr);
                    /* Read up to 128 bytes from arg1/arg2/arg3 as hex,
                     * IF they look like heap pointers. arg2 in particular
                     * has been seen to point at FE-side data buffers
                     * carrying phoneme records. */
                    function readHexFrom(va, n) {
                        if (va < 0x100000) return null;
                        var p = ptr(va >>> 0);
                        if (!rangeOK(p)) return null;
                        try {
                            var bytes = new Uint8Array(p.readByteArray(n));
                            var hex = '';
                            var lut = '0123456789abcdef';
                            for (var i = 0; i < bytes.length; i++) {
                                hex += lut[(bytes[i]>>4)&0xf] + lut[bytes[i]&0xf];
                            }
                            return hex;
                        } catch (e) { return null; }
                    }
                    /* Also read a printable ASCII view of arg2 (the
                     * suspected data buffer) to make per-cb content
                     * grep-able. */
                    function readAsciiFrom(va, n) {
                        var hex = readHexFrom(va, n);
                        if (!hex) return null;
                        var s = '';
                        for (var i = 0; i < hex.length; i += 2) {
                            var b = parseInt(hex.substr(i, 2), 16);
                            s += (b >= 0x20 && b < 0x7f) ? String.fromCharCode(b) : '.';
                        }
                        return s;
                    }
                    send({
                        type: 'setpair_cb_entry',
                        slot: slot,
                        slot_name: SLOT_NAMES[slot] || ('slot'+slot),
                        cb_va: '0x' + cb_va.toString(16),
                        args: ['0x'+(arg0>>>0).toString(16),
                               '0x'+(arg1>>>0).toString(16),
                               '0x'+(arg2>>>0).toString(16),
                               '0x'+(arg3>>>0).toString(16)],
                        arg1_hex: readHexFrom(arg1, 64),
                        arg2_hex: readHexFrom(arg2, 512),
                        arg2_ascii: readAsciiFrom(arg2, 512),
                        arg3_hex: readHexFrom(arg3, 64),
                        user_arg_dump_u32: ua_dump.map(function (v) {
                            return '0x'+v.toString(16); }),
                        esp_dump_u32:      esp_dump.map(function (v) {
                            return '0x'+v.toString(16); }),
                        utt_hit_offset: utt_hit,
                        utt_ptr_known:  g_last_utt_ptr ? '0x'+g_last_utt_ptr.toString(16) : null,
                    });
                    stats.sent++;
                } catch (e) { stats.errs++; }
            }
        });
        stats.probes_attached++;
    } catch (e) {
        send({ type: 'cb_probe_err', cb_va: '0x'+cb_va.toString(16), err: e.toString() });
    }
}

/* Hook the FE-side registration sites — these tell us cb addresses. */
Object.keys(SLOT_VAS).forEach(function (slot) {
    var slot_n = parseInt(slot, 10);
    try {
        Interceptor.attach(ptr(SLOT_VAS[slot]), {
            onEnter: function (args) {
                stats.regs++;
                try {
                    var cb   = args[1].toUInt32() >>> 0;
                    var ua   = args[2].toUInt32() >>> 0;
                    /* Also peek at args[3] = `type` per the vfn3
                     * understanding we just landed. */
                    var ty   = 0;
                    try { ty = this.context.esp.add(0x10).readU32() >>> 0; } catch (e) {}
                    send({
                        type: 'setpair_register',
                        slot: slot_n,
                        slot_name: SLOT_NAMES[slot_n] || ('slot'+slot_n),
                        cb_va:    '0x'+cb.toString(16),
                        user_arg: '0x'+ua.toString(16),
                        type:     '0x'+ty.toString(16),
                    });
                    attachCbProbe(slot_n, cb);
                } catch (e) { stats.errs++; }
            }
        });
    } catch (e) {
        send({ type: 'reg_hook_err', slot: slot_n, err: e.toString() });
    }
});

/* Capture utt_ptr from the engine's call to SWIttsUSelUnitSelection. */
if (ADDR_USEL) {
    try {
        Interceptor.attach(ADDR_USEL, {
            onEnter: function () {
                try {
                    var esp = this.context.esp;
                    var utt = esp.add(0xc).readU32() >>> 0;  /* arg3 = utt */
                    if (utt > 0x100000) {
                        g_last_utt_ptr = utt;
                        send({ type: 'usel_utt_seen',
                               utt_ptr: '0x'+utt.toString(16) });
                    }
                } catch (e) {}
            }
        });
        send({ type: 'usel_hook_ok',
               va: '0x'+ADDR_USEL.toString(16) });
    } catch (e) {
        send({ type: 'usel_hook_err', err: e.toString() });
    }
} else {
    send({ type: 'usel_not_found' });
}

rpc.exports = {
    stats: function () { return stats; },
    flush: function () { return stats; },
    reset: function () {
        stats = { regs:0, probes_attached:0, cb_calls:0, sent:0, errs:0 };
        g_last_utt_ptr = null;
        g_probed = {};
    },
};

send({ type: 'ready', hook: 'fe_setpair_probe',
       slots: Object.keys(SLOT_VAS) });
