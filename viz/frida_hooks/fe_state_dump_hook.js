'use strict';
/*
 * fe_state_dump_hook.js — capture FE state + setPair args at multiple
 * points in the call sequence to identify what data the engine
 * consumes during init that flips verbose output.
 *
 * - At setPair_E/F/G/H entry (slots 41/43/45/46): probe args[0] data
 *   pointer immediately (before it's freed/moved). Determine module
 *   and dump first 256 bytes.
 * - At slot 42 entry: dump full state + ctrl as before.
 */

var ADDR_SLOT5   = ptr('0x0836cb90');   /* feedConfigA */
var ADDR_SLOT11  = ptr('0x0836cd30');   /* runOrAbort */
var ADDR_SLOT41  = ptr('0x0836d150');   /* setPair_E */
var ADDR_SLOT42  = ptr('0x0836d170');   /* delegateB_call */
var ADDR_SLOT43  = ptr('0x0836cf20');   /* setPair_F (slot 39 wrapper actually but let's see) */
var DUMP_BYTES   = 0x800;
var PROBE_BYTES  = 256;

function rangeOK(a) {
    try { var r = Process.findRangeByAddress(a);
          return r !== null && r.protection.indexOf('r') !== -1; }
    catch (e) { return false; }
}
function dumpU32s(addr, nbytes) {
    if (!rangeOK(addr)) return null;
    try {
        var bytes = new Uint8Array(addr.readByteArray(nbytes));
        var out = [];
        for (var i = 0; i + 4 <= bytes.length; i += 4) {
            out.push((bytes[i] | (bytes[i+1]<<8) | (bytes[i+2]<<16) | (bytes[i+3]<<24)) >>> 0);
        }
        return out;
    } catch (e) { return null; }
}
function probeAddr(a, nbytes) {
    if (!a || a.isNull()) return null;
    var mod = null, rng = null;
    try { mod = Process.findModuleByAddress(a); } catch (e) {}
    try { rng = Process.findRangeByAddress(a); } catch (e) {}
    return {
        addr: a.toString(),
        module: mod ? mod.name : null,
        offset: mod ? '+0x' + a.sub(mod.base).toString(16) : null,
        prot: rng ? rng.protection : null,
        head: dumpU32s(a, nbytes),
    };
}

var setpair_seen = {};

/* Per fe_vtable_trace.js's SLOTS table */
[
    { addr: ptr('0x0836d150'), name: 'setPair_E   (slot 41)' },
    { addr: ptr('0x0836d1c0'), name: 'setPair_F   (slot 43)' },
    { addr: ptr('0x0836d230'), name: 'setPair_G   (slot 45)' },
    { addr: ptr('0x0836d250'), name: 'setPair_H   (slot 46)' },
    { addr: ptr('0x0836d270'), name: 'installHookA (slot 47)' },
    { addr: ptr('0x0836d290'), name: 'installHookB (slot 48)' },
].forEach(function (s) {
    try {
        Interceptor.attach(s.addr, {
            onEnter: function (args) {
                if (setpair_seen[s.name]) return;
                setpair_seen[s.name] = 1;
                var esp = this.context.esp;
                /* __stdcall(this, arg0, arg1) — args at esp+4, +8, +c */
                try {
                    var arg0 = ptr(esp.add(0x4).readU32());
                    var arg1 = ptr(esp.add(0x8).readU32());
                    var arg2 = ptr(esp.add(0xc).readU32());
                    send({
                        type: 'setpair_call',
                        slot: s.name,
                        this_ptr: arg0.toString(),
                        arg0: probeAddr(arg1, PROBE_BYTES),
                        arg1: probeAddr(arg2, PROBE_BYTES),
                    });
                } catch (e) {
                    send({ type: 'setpair_err', name: s.name, err: e.toString() });
                }
            },
        });
    } catch (e) {
        send({ type: 'attach_err', addr: s.addr.toString(), err: e.toString() });
    }
});

var dumped42 = false;
Interceptor.attach(ADDR_SLOT42, {
    onEnter: function (args) {
        if (dumped42) return;
        dumped42 = true;
        try {
            var iobj = args[0];
            if (!rangeOK(iobj)) return;
            var state = ptr(iobj.add(0x8).readU32() >>> 0);
            var ctrl_u32 = state.add(0x6c).readU32();
            var ctrl = (ctrl_u32 >>> 0) ? ptr(ctrl_u32 >>> 0) : null;

            send({
                type:       'state_dump',
                iobj_addr:  iobj.toString(),
                state_addr: state.toString(),
                ctrl_addr:  ctrl ? ctrl.toString() : null,
                state_words: dumpU32s(state, DUMP_BYTES),
                ctrl_words:  ctrl ? dumpU32s(ctrl, DUMP_BYTES) : null,
            });
        } catch (e) {
            send({ type: 'error', msg: e.toString() });
        }
    },
});

rpc.exports = {
    stats: function () { return { dumped: dumped42, setpair: setpair_seen }; },
    flush: function () { return { dumped: dumped42 }; },
    reset: function () { dumped42 = false; setpair_seen = {}; },
};

send({ type: 'ready', hook: 'fe_state_dump' });
