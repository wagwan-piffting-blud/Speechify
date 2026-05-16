'use strict';
/*
 * wsola_unit_probe_hook.js -- dump WsolaUnit + sub-unit + pmark data
 * at FUN_08EE2960 entry, for use deciding the PSOLA port scope.
 *
 * Per reveng/DLL_ANALYSIS.md, FUN_08EE2960 (Process Single Unit) is
 * called once per output unit during SWIttsWsolaConcat. Its arg2 is a
 * WsolaUnit struct (0x2c bytes) holding the per-unit synthesis state,
 * and the per-unit metadata struct used by the voiced-join precondition
 * (FUN_08EE20C0) is reachable via WSOLA state at this+0x11 / this+0x44.
 *
 * --- What we want to know ---
 *
 *  (Q1) Where do engine's pitch marks live?
 *       - VCF says Tom has `pmindex`/`pmdata` = (none), so pmarks
 *         aren't loaded from external files.
 *       - Hypothesis A: pmarks are computed at runtime per-unit.
 *       - Hypothesis B: pmarks are encoded in the VDB sub-unit data
 *         (each sub-unit having a pitch float at +0x18/+0x1c).
 *       - Hypothesis C: pmarks are computed once per voice load and
 *         live somewhere in the in-memory unit table.
 *  (Q2) If the answer is B, can we read sub-unit pitch from the VDB
 *       layout we already parse? (probably not without RE additions)
 *
 * --- Output schema (per FUN_08EE2960 entry) ---
 *
 *   {type: "wsola_unit", n_call: N, this_ptr: ..., state_ptr: ...,
 *    unit_id, duration, start_pos, pitch_mod, pitch_scale,
 *    n_sub_units, sub_units: [{samples, pitch_a, pitch_b}, ...],
 *    meta: {n_pmarks, pmark_table: [{pos, period_random, period_left,
 *                                    period_right}, ...]}
 *   }
 *
 * --- Safety ---
 *
 * Function-entry hook only (per [[feedback-frida-entry-only]]). Cap at
 * TOTAL_CAP units across the session to bound trace size. All pointer
 * reads guarded.
 */

var ADDR_FUN = ptr('0x08EE2960');
var TOTAL_CAP = 200;
var stats = { calls: 0, sent: 0, dropped: 0,
              ptr_invalid: 0, read_errors: 0 };

function rangeOK(addr) {
    try {
        var r = Process.findRangeByAddress(addr);
        return r !== null && r.protection.indexOf('r') !== -1;
    } catch (e) { return false; }
}
function safeReadU32(addr) {
    if (!rangeOK(addr)) { stats.ptr_invalid++; return null; }
    try { return addr.readU32(); }
    catch (e) { stats.read_errors++; return null; }
}
function safeReadS32(addr) {
    if (!rangeOK(addr)) { stats.ptr_invalid++; return null; }
    try { return addr.readS32(); }
    catch (e) { stats.read_errors++; return null; }
}
function safeReadF32(addr) {
    if (!rangeOK(addr)) { stats.ptr_invalid++; return null; }
    try { return addr.readFloat(); }
    catch (e) { stats.read_errors++; return null; }
}
function safeReadF64(addr) {
    if (!rangeOK(addr)) { stats.ptr_invalid++; return null; }
    try { return addr.readDouble(); }
    catch (e) { stats.read_errors++; return null; }
}

Interceptor.attach(ADDR_FUN, {
    onEnter: function () {
        stats.calls++;
        if (stats.calls > TOTAL_CAP) { stats.dropped++; return; }

        var state_ptr = this.context.ecx.toUInt32();
        /* Capture output cursor at entry; leave delta will be the
         * number of samples emitted by this process_unit call. */
        this.cursor_enter = safeReadS32(ptr(state_ptr + 0x35f4));
        this.state_at_entry = state_ptr;
        /* __thiscall: ecx = this; args on stack. After Frida's onEnter,
         * the RET addr is at [esp+0], so caller's arg1 is at [esp+4],
         * arg2 (WsolaUnit ptr) at [esp+8]. */
        var espVal = this.context.esp.toUInt32();
        var arg1 = safeReadU32(ptr(espVal + 4));
        var arg2 = safeReadU32(ptr(espVal + 8));

        var snap = {
            type:      'wsola_unit',
            n_call:    stats.calls,
            state_ptr: state_ptr,
            arg1:      arg1,
            arg2:      arg2
        };

        /* Dump WsolaUnit (arg2) per DLL_ANALYSIS layout:
         *   [+0x08] unit_id
         *   [+0x0c] duration (WSOLA ticks)
         *   [+0x10] start position (WSOLA ticks)
         *   [+0x18] / [+0x1c] float pitch modifier / scale
         *   [+0x24] sub-unit count
         *   [+0x28] sub-unit array ptr */
        if (arg2 !== null && arg2 >= 0x100000) {
            var u = ptr(arg2);
            snap.unit = {
                id:           safeReadU32(u.add(0x08)),
                duration:     safeReadS32(u.add(0x0c)),
                start_pos:    safeReadS32(u.add(0x10)),
                pitch_mod:    safeReadF32(u.add(0x18)),
                pitch_scale:  safeReadF32(u.add(0x1c)),
                n_sub_units:  safeReadS32(u.add(0x24)),
                sub_units_p:  safeReadU32(u.add(0x28)),
                /* Bonus dump of every 4-byte slot in the struct for
                 * any field we might have mislabeled. */
                raw_u32: []
            };
            for (var off = 0; off < 0x2c; off += 4) {
                snap.unit.raw_u32.push(safeReadU32(u.add(off)));
            }

            /* Walk sub-unit array (0x30 bytes each) — likely the source
             * of per-pitch-period data engine uses for PSOLA. */
            snap.unit.sub_units = [];
            if (snap.unit.n_sub_units !== null && snap.unit.n_sub_units > 0
                && snap.unit.n_sub_units < 128
                && snap.unit.sub_units_p !== null
                && snap.unit.sub_units_p >= 0x100000) {
                var su = ptr(snap.unit.sub_units_p);
                for (var i = 0; i < snap.unit.n_sub_units; ++i) {
                    var sui = su.add(i * 0x30);
                    var entry = {
                        i: i,
                        ptr: (snap.unit.sub_units_p + i * 0x30) >>> 0,
                        samples:  safeReadU32(sui.add(0x08)),
                        pitch_a:  safeReadF32(sui.add(0x18)),
                        pitch_b:  safeReadF32(sui.add(0x1c)),
                        raw_u32:  []
                    };
                    for (var off2 = 0; off2 < 0x30; off2 += 4) {
                        entry.raw_u32.push(safeReadU32(sui.add(off2)));
                    }
                    snap.unit.sub_units.push(entry);
                }
            }
        }

        /* Try to reach the per-unit metadata struct used by
         * voiced-join precondition (FUN_08EE20C0): state+0x44 points
         * to a struct array; state+0x11*4 (which the precondition
         * reads via param_1[0x11]) indexes the "current" unit. */
        if (state_ptr >= 0x100000) {
            var st = ptr(state_ptr);
            var meta_arr = safeReadU32(st.add(0x44));
            var cur_idx  = safeReadU32(st.add(0x11 * 4));
            snap.state = {
                meta_arr_ptr: meta_arr,
                cur_idx:      cur_idx,
                /* state+0xd83 is max_pitch_period per FUN_08EE20C0 */
                max_pitch_period: safeReadS32(st.add(0xd83 * 4)),
                /* state+0x3614 is the mode flag (0=Selective F0,1=Plain) */
                mode_flag:    safeReadU32(st.add(0x3614))
            };

            /* The precondition computes:
             *   puVar1 = *(iVar7+-8)*0x30 + -0x30 + *(iVar7+-4)
             *   puVar2 = *(int *)(iVar7 + 0x28)
             * where iVar7 = state+0x11. So state+0x11+(-2) (= +0xc bytes
             * from iVar7) and state+0x11+(-1) (= +0x10 from iVar7) are
             * used. Best-effort: grab the bytes around state+0x11 as
             * raw u32 so we can see what's there. */
            snap.state.iVar7_window = [];
            for (var k = -4; k <= 12; ++k) {
                snap.state.iVar7_window.push({
                    off: (0x11 * 4) + k * 4,
                    val: safeReadU32(st.add(0x11 * 4 + k * 4))
                });
            }

            /* The unit-meta struct (puVar1 from FUN_08EE20C0) has
             *   [4] = n_pmarks (offset +0x10)
             *   [5] = pmark_array_ptr (offset +0x14)
             *   pmark stride = 0x1c (28 bytes / 7 ints)
             *
             * We don't fully know puVar1's address yet, but if
             * cur_idx is sane we can try state+0x44 + cur_idx*0x30
             * as a candidate. */
            if (meta_arr !== null && meta_arr >= 0x100000
                && cur_idx !== null && cur_idx > 0 && cur_idx < 1024) {
                /* Try the previous slot (cur_idx-1) since the
                 * precondition reads puVar1 from prev unit. */
                var prev_meta = ptr(meta_arr + (cur_idx - 1) * 0x30);
                var n_pmarks  = safeReadU32(prev_meta.add(0x10));
                var pmark_p   = safeReadU32(prev_meta.add(0x14));
                snap.state.prev_meta = {
                    ptr:        (meta_arr + (cur_idx - 1) * 0x30) >>> 0,
                    n_pmarks:   n_pmarks,
                    pmark_arr:  pmark_p,
                    raw_u32: []
                };
                for (var off3 = 0; off3 < 0x30; off3 += 4) {
                    snap.state.prev_meta.raw_u32.push(
                        safeReadU32(prev_meta.add(off3)));
                }
                if (n_pmarks !== null && n_pmarks > 0 && n_pmarks < 64
                    && pmark_p !== null && pmark_p >= 0x100000) {
                    snap.state.prev_meta.pmarks = [];
                    var pm = ptr(pmark_p);
                    for (var pi = 0; pi < n_pmarks; ++pi) {
                        var pme = pm.add(pi * 0x1c);
                        snap.state.prev_meta.pmarks.push({
                            i: pi,
                            raw_u32: [
                                safeReadU32(pme.add(0x00)),
                                safeReadU32(pme.add(0x04)),
                                safeReadU32(pme.add(0x08)),
                                safeReadU32(pme.add(0x0c)),
                                safeReadU32(pme.add(0x10)),
                                safeReadU32(pme.add(0x14)),
                                safeReadU32(pme.add(0x18))
                            ]
                        });
                    }
                }
            }
        }

        send(snap);
        stats.sent++;
    },
    onLeave: function () {
        if (stats.calls > TOTAL_CAP) return;
        if (typeof this.cursor_enter !== 'number') return;
        if (typeof this.state_at_entry !== 'number') return;
        var cursor_leave = safeReadS32(ptr(this.state_at_entry + 0x35f4));
        var delta_samples = (cursor_leave !== null && this.cursor_enter !== null)
                            ? (cursor_leave - this.cursor_enter)
                            : null;
        send({type: 'wsola_unit_leave',
              n_call: stats.calls,
              cursor_enter: this.cursor_enter,
              cursor_leave: cursor_leave,
              delta_samples: delta_samples});
    }
});

/* FUN_08EE1700 fires at the END of each unit iteration in FUN_08EE3AA0.
 * Dump multiple state offsets to find which one tracks cumulative output. */
var ADDR_1700 = ptr('0x08EE1700');
var post_unit_calls = 0;
Interceptor.attach(ADDR_1700, {
    onEnter: function () {
        post_unit_calls++;
        if (post_unit_calls > TOTAL_CAP) return;
        var state = this.context.ecx.toUInt32();
        if (state < 0x100000) return;
        /* Sweep candidate state offsets that might track output. */
        var dump = {};
        var offsets = [0x08, 0x0c, 0x10, 0x14, 0x35c4, 0x35c8, 0x35cc,
                       0x35d0, 0x35d4, 0x35e8, 0x35ec, 0x35f0, 0x35f4,
                       0x35f8, 0x35fc, 0x3600, 0x3604, 0x3608, 0x360c,
                       0x3610, 0x3614, 0x3618, 0x361c, 0x3620];
        for (var i = 0; i < offsets.length; ++i) {
            var off = offsets[i];
            dump['x' + off.toString(16)] = safeReadS32(ptr(state + off));
        }
        send({type: 'post_unit_sweep',
              n_call: post_unit_calls,
              state_ptr: state,
              fields: dump});
    }
});

/* Also hook FUN_08EE3560 (the OLA blend + emit) entry to count its
 * iVar1 arg (= samples emitted via state[+0x30] callback). FUN_08EE3560
 * is __fastcall with arg in ecx = state ptr. The output emission uses
 * state[+0x08] as n_samples — we capture it. */
var ADDR_3560 = ptr('0x08EE3560');
var ola_calls = 0;
Interceptor.attach(ADDR_3560, {
    onEnter: function () {
        ola_calls++;
        if (ola_calls > TOTAL_CAP) return;
        /* __fastcall: state in ECX */
        var state_ptr = this.context.ecx.toUInt32();
        if (state_ptr < 0x100000) return;
        var step_n = safeReadS32(ptr(state_ptr + 0x08));   /* nominal step */
        var x34 = safeReadU32(ptr(state_ptr + 0x34));
        send({type: 'ola_call',
              n_call: ola_calls,
              step_n: step_n});
    }
});

rpc.exports = {
    stats: function () { return stats; },
    flush: function () { return stats; },
    reset: function () {
        stats = { calls: 0, sent: 0, dropped: 0,
                  ptr_invalid: 0, read_errors: 0 };
    }
};

send({ type: 'ready', hook: 'wsola_unit_probe',
       addr: '0x08EE2960', cap: TOTAL_CAP });
