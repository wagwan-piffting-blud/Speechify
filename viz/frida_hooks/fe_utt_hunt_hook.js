'use strict';
/*
 * fe_utt_hunt_hook.js — locate where the FE stashes its produced
 * Festival utterance pointer in iobj->state, so we can read it
 * directly from our hosted PE-loaded copy of the DLL (instead of
 * draining the lossy tagged-text serialization).
 *
 * Strategy:
 *   1. Hook SWIttsUSelUnitSelection (FUN_08e819e0) at entry — its
 *      param_3 is the FE-produced utterance pointer (verified by
 *      fe_tree_hook.js). Capture into a JS-side variable.
 *   2. Hook runOrAbort (slot 11 wrapper at 0x0836cd30) at onLeave —
 *      this is the last FE call before unit selection. At leave-time
 *      the utterance is fully built and the FE has stashed its pointer
 *      somewhere. Dump iobj, iobj->state, and the ctrl block at
 *      iobj->state[+0x6c] as raw u32s.
 *   3. JS-side correlation: for each utt_ptr captured in step 1, scan
 *      the most-recent state dump from step 2 for any u32 equal to
 *      utt_ptr. Emit (offset, container).
 *
 * If a stable offset emerges across phrases, the C-side fix is a
 * one-line read: read u32 at iobj->state[+offset] post-runOrAbort.
 *
 * Output: one JSON line per (runOrAbort_leave, USel_enter) pair, with:
 *   - utt_ptr: the engine's actual utterance pointer
 *   - hits:    list of (region, byte_offset) where utt_ptr was found
 *   - state_window: raw u32s for further analysis
 *
 * Safety: only function-entry / function-leave hooks. No mid-function
 * trampolines. All pointer reads are guarded with rangeOK + try/catch
 * (per the safety policy in run_frida_capture.py).
 */

var ADDR_USEL    = ptr('0x08E819E0');   /* SWIttsUSelUnitSelection */
var ADDR_RUNOR   = ptr('0x0836cd30');   /* slot 11: runOrAbort wrapper */

var DUMP_BYTES_IOBJ  = 0x40;            /* iobj is small */
var DUMP_BYTES_STATE = 0x2000;          /* widened 2026-05-12 from 0x400 */
var DUMP_BYTES_CTRL  = 0x2000;
var DUMP_BYTES_L1    = 0x200;           /* widened 2026-05-12 from 0x80 */
var DUMP_BYTES_L2    = 0x100;           /* level-2 deref window */

/* Heuristic for "looks like a heap pointer in this process" — captured
 * traces show iobj/state/ctrl/utt_ptr all in 0x02xxxxxx-0x07xxxxxx. */
function isHeapPtr(v) {
    return v >>> 0 >= 0x01000000 && v >>> 0 < 0x10000000;
}

/* Most-recent runOrAbort capture, replayed when USel fires. */
var pending = null;
var stats = { ro_calls: 0, usel_calls: 0, sent: 0, errs: 0 };

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
function safeReadHex(addr, n) {
    if (!rangeOK(addr)) return null;
    try { return addr.readByteArray(n); } catch (e) { return null; }
}

/* Collect every u32 in a hex byte array as an array of {off, val}. */
function asU32Array(bytes, base_off) {
    if (!bytes) return [];
    var u8 = new Uint8Array(bytes);
    var out = [];
    for (var i = 0; i + 4 <= u8.length; i += 4) {
        var v = (u8[i]) | (u8[i+1] << 8) | (u8[i+2] << 16) | (u8[i+3] << 24);
        v >>>= 0;
        out.push({ off: (base_off || 0) + i, val: v });
    }
    return out;
}

function findHits(needle_u32, regions) {
    var hits = [];
    for (var ri = 0; ri < regions.length; ri++) {
        var r = regions[ri];
        for (var i = 0; i < r.words.length; i++) {
            if (r.words[i].val === needle_u32) {
                hits.push({ region: r.name, off: r.words[i].off });
            }
        }
    }
    return hits;
}

Interceptor.attach(ADDR_RUNOR, {
    onEnter: function (args) {
        /* `this` is at args[0] for __stdcall wrappers via the trampoline.
         * Save for onLeave. */
        this.iobj = args[0];
    },
    onLeave: function (retval) {
        stats.ro_calls++;
        try {
            var iobj = this.iobj;
            if (!rangeOK(iobj)) return;
            var state_ptr_u32 = safeReadU32(iobj.add(0x8));
            if (state_ptr_u32 === null) return;
            var state_ptr = ptr(state_ptr_u32 >>> 0);

            /* ctrl block lives at state[+0x6c] per fe_vtable_trace */
            var ctrl_ptr_u32 = safeReadU32(state_ptr.add(0x6c));
            var ctrl_ptr = ctrl_ptr_u32 ? ptr(ctrl_ptr_u32 >>> 0) : null;

            var iobj_words  = asU32Array(safeReadHex(iobj, DUMP_BYTES_IOBJ), 0);
            var state_words = asU32Array(safeReadHex(state_ptr, DUMP_BYTES_STATE), 0);
            var ctrl_words  = ctrl_ptr
                              ? asU32Array(safeReadHex(ctrl_ptr, DUMP_BYTES_CTRL), 0)
                              : [];

            var regions = [
                { name: 'iobj',  base: iobj.toString(),     words: iobj_words },
                { name: 'state', base: state_ptr.toString(), words: state_words },
                { name: 'ctrl',  base: ctrl_ptr ? ctrl_ptr.toString() : null,
                  words: ctrl_words },
            ];

            /* Level-1 pointer chase: every plausible-heap-pointer u32 in
             * the level-0 regions gets dereferenced and dumped. The
             * utterance pointer might live one hop away. */
            var seen = {};
            seen[iobj.toString()] = 1;
            seen[state_ptr.toString()] = 1;
            if (ctrl_ptr) seen[ctrl_ptr.toString()] = 1;
            var l1_regions = [];
            for (var ri = 0; ri < 3; ri++) {
                var r = regions[ri];
                for (var wi = 0; wi < r.words.length; wi++) {
                    var v = r.words[wi].val;
                    if (!isHeapPtr(v)) continue;
                    var key = v >>> 0;
                    var ks = '0x' + key.toString(16);
                    if (seen[ks]) continue;
                    seen[ks] = 1;
                    var l1_words = asU32Array(safeReadHex(ptr(key), DUMP_BYTES_L1), 0);
                    if (l1_words.length === 0) continue;
                    var l1_region = {
                        name: r.name + '+0x' + r.words[wi].off.toString(16) +
                              '->0x' + key.toString(16),
                        base: ks,
                        words: l1_words,
                    };
                    l1_regions.push(l1_region);
                    regions.push(l1_region);
                }
            }
            /* Level-2 pointer chase: every plausible-heap-pointer u32 in
             * the L1 regions gets dereferenced too — utt may live 2 hops
             * from iobj/state/ctrl. */
            for (var ri2 = 0; ri2 < l1_regions.length; ri2++) {
                var r2 = l1_regions[ri2];
                for (var wi2 = 0; wi2 < r2.words.length; wi2++) {
                    var v2 = r2.words[wi2].val;
                    if (!isHeapPtr(v2)) continue;
                    var key2 = v2 >>> 0;
                    var ks2 = '0x' + key2.toString(16);
                    if (seen[ks2]) continue;
                    seen[ks2] = 1;
                    var l2_words = asU32Array(safeReadHex(ptr(key2), DUMP_BYTES_L2), 0);
                    if (l2_words.length === 0) continue;
                    regions.push({
                        name: r2.name + '+0x' + r2.words[wi2].off.toString(16) +
                              '->0x' + key2.toString(16),
                        base: ks2,
                        words: l2_words,
                    });
                }
            }

            pending = {
                iobj_addr:  iobj.toString(),
                state_addr: state_ptr.toString(),
                ctrl_addr:  ctrl_ptr ? ctrl_ptr.toString() : null,
                regions: regions,
            };
        } catch (e) {
            stats.errs++;
            pending = null;
        }
    },
});

Interceptor.attach(ADDR_USEL, {
    onEnter: function () {
        stats.usel_calls++;
        try {
            /* SWIttsUSelUnitSelection is __cdecl. Per fe_tree_hook.js the
             * utterance is arg2 = [esp+0xc]. */
            var esp = this.context.esp;
            var utt_u32 = safeReadU32(esp.add(0xc));
            if (utt_u32 === null || utt_u32 < 0x100000) return;

            var hits = pending
                       ? findHits(utt_u32 >>> 0, pending.regions)
                       : [];

            send({
                type: 'utt_hunt',
                utt_ptr: utt_u32 >>> 0,
                hits: hits,
                pending: pending ? {
                    iobj_addr:  pending.iobj_addr,
                    state_addr: pending.state_addr,
                    ctrl_addr:  pending.ctrl_addr,
                } : null,
                stats: { ro_calls: stats.ro_calls, usel_calls: stats.usel_calls },
            });
            stats.sent++;
        } catch (e) {
            stats.errs++;
        }
    },
});

rpc.exports = {
    stats: function () { return stats; },
    flush: function () { return stats; },
    reset: function () { stats = { ro_calls: 0, usel_calls: 0, sent: 0, errs: 0 }; },
};

send({ type: 'ready', hook: 'fe_utt_hunt',
       run_or_va: ADDR_RUNOR.toString(),
       usel_va:   ADDR_USEL.toString() });
