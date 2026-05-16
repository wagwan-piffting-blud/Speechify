'use strict';
/*
 * fe_ctrl_watch_hook.js — for every FE vtable call, snapshot a small
 * window of ctrl bytes BEFORE and AFTER. Diff them. Log only the
 * deltas. This pinpoints which init step writes the verbose-mode
 * flag (ctrl+0x004) and starts populating the output buffer
 * (ctrl+0x3d0..+0x600).
 *
 * Output is per-call: { slot, name, deltas: [{off, before, after}, ...] }.
 * If no fields changed in our window, the call is silent.
 */

var SLOTS = [
    { slot:  1, va: '0x0836cac0', name: 'AddRef'           },
    { slot:  2, va: '0x0836d2e0', name: 'Release'          },
    { slot:  3, va: '0x0836cb10', name: 'initStage1'       },
    { slot:  4, va: '0x0836cb50', name: 'initStage2'       },
    { slot:  5, va: '0x0836cb90', name: 'feedConfigA'      },
    { slot:  6, va: '0x0836cbd0', name: 'feedConfigB'      },
    { slot:  9, va: '0x0836cc90', name: 'delegateA_call'   },
    { slot: 10, va: '0x0836cce0', name: 'getErrorMessage'  },
    { slot: 11, va: '0x0836cd30', name: 'runOrAbort'       },
    { slot: 25, va: '0x0836d040', name: 'setMode'          },
    { slot: 26, va: '0x0836d060', name: 'reset'            },
    { slot: 33, va: '0x0836d0f0', name: 'getKind'          },
    { slot: 41, va: '0x0836d150', name: 'setPair_E'        },
    { slot: 42, va: '0x0836d170', name: 'delegateB_call'   },
    { slot: 43, va: '0x0836d1c0', name: 'setPair_F'        },
    { slot: 44, va: '0x0836d1e0', name: 'delegateB_call2'  },
    { slot: 45, va: '0x0836d230', name: 'setPair_G'        },
    { slot: 46, va: '0x0836d250', name: 'setPair_H'        },
    { slot: 47, va: '0x0836d270', name: 'installHookA'     },
    { slot: 48, va: '0x0836d290', name: 'installHookB'     },
];

/* Watch windows (offsets within ctrl). Now spans the full 0x800 region
 * in two halves so we don't miss the verbose-output buffer if it's
 * elsewhere this session (different ASLR/heap layout). */
var WATCH_WINDOWS = [
    [0x000, 0x400],
    [0x400, 0x800],
];

/* Cap diff list per call — the per-byte changes inside a buffer can
 * easily be thousands; we just want to know IT changed and where. */
var MAX_DIFFS_PER_CALL = 24;

function rangeOK(a) {
    try { var r = Process.findRangeByAddress(a);
          return r !== null && r.protection.indexOf('r') !== -1; }
    catch (e) { return false; }
}

function snapshot_window(ctrl_ptr, lo, hi) {
    if (!rangeOK(ctrl_ptr.add(lo))) return null;
    try {
        var bytes = new Uint8Array(ctrl_ptr.add(lo).readByteArray(hi - lo));
        var arr = [];
        for (var i = 0; i + 4 <= bytes.length; i += 4) {
            arr.push((bytes[i] | (bytes[i+1]<<8) | (bytes[i+2]<<16) | (bytes[i+3]<<24)) >>> 0);
        }
        return { lo: lo, words: arr };
    } catch (e) { return null; }
}

function snapshot_all(ctrl_ptr) {
    var snaps = [];
    for (var i = 0; i < WATCH_WINDOWS.length; i++) {
        var w = WATCH_WINDOWS[i];
        var s = snapshot_window(ctrl_ptr, w[0], w[1]);
        if (s) snaps.push(s);
    }
    return snaps;
}

function diff_snapshots(before, after) {
    var diffs = [];
    if (!before || !after || before.length !== after.length) return diffs;
    for (var w = 0; w < before.length; w++) {
        var b = before[w], a = after[w];
        if (!b || !a || b.lo !== a.lo) continue;
        var n = Math.min(b.words.length, a.words.length);
        for (var i = 0; i < n; i++) {
            if (b.words[i] !== a.words[i]) {
                diffs.push({
                    off: '0x' + (b.lo + i * 4).toString(16),
                    before: '0x' + b.words[i].toString(16),
                    after:  '0x' + a.words[i].toString(16),
                });
            }
        }
    }
    return diffs;
}

var stats = { calls: 0, diffs_emitted: 0 };

/* One-shot raw dump of ctrl[0..0x800] at slot-3 (initStage1) ENTRY,
 * for differential-bisection work (see RESUME_K2.md). Fires once
 * per Frida session; subsequent slot-3 calls (e.g. across the
 * corpus) are not re-dumped — first phrase wins. */
var g_pre_init_dumped = false;
var g_pre_init_state_dumped = false;
var PRE_INIT_BYTES = 0x800;
var PRE_INIT_STATE_BYTES = 0x1300;   /* state block per RE notes ~0x1285 */
function hexdump_bytes(p, n) {
    var bytes = new Uint8Array(p.readByteArray(n));
    var hex = '';
    var lut = '0123456789abcdef';
    for (var i = 0; i < bytes.length; i++) {
        hex += lut[(bytes[i] >> 4) & 0xf] + lut[bytes[i] & 0xf];
    }
    return hex;
}

SLOTS.forEach(function (slot_def) {
    try {
        Interceptor.attach(ptr(slot_def.va), {
            onEnter: function (args) {
                stats.calls++;
                this.slot_def = slot_def;
                var iobj = args[0];
                if (!rangeOK(iobj)) return;
                try {
                    var state = ptr(iobj.add(0x8).readU32() >>> 0);
                    var ctrl_u32 = state.add(0x6c).readU32();
                    if (!ctrl_u32) return;
                    this.ctrl = ptr(ctrl_u32 >>> 0);
                    this.before = snapshot_all(this.ctrl);
                    /* One-shot pre-init raw dump on FIRST slot-3 entry. */
                    if (slot_def.slot === 3 && !g_pre_init_dumped) {
                        try {
                            if (rangeOK(this.ctrl.add(PRE_INIT_BYTES - 1))) {
                                var hex = hexdump_bytes(this.ctrl, PRE_INIT_BYTES);
                                send({ type: 'ctrl_pre_init_raw',
                                       n_bytes: PRE_INIT_BYTES,
                                       hex: hex });
                                g_pre_init_dumped = true;
                            }
                        } catch (e) {
                            send({ type: 'ctrl_pre_init_err', err: e.toString() });
                        }
                    }
                    /* Also one-shot dump state[0..PRE_INIT_STATE_BYTES] for
                     * the state-bisection experiment (T1). */
                    if (slot_def.slot === 3 && !g_pre_init_state_dumped) {
                        try {
                            if (rangeOK(state.add(PRE_INIT_STATE_BYTES - 1))) {
                                var shex = hexdump_bytes(state, PRE_INIT_STATE_BYTES);
                                send({ type: 'state_pre_init_raw',
                                       n_bytes: PRE_INIT_STATE_BYTES,
                                       hex: shex });
                                g_pre_init_state_dumped = true;
                            }
                        } catch (e) {
                            send({ type: 'state_pre_init_err', err: e.toString() });
                        }
                    }
                } catch (e) {}
            },
            onLeave: function (retval) {
                if (!this.ctrl || !this.before) {
                    /* Even if we couldn't read state, log that this call
                     * happened so the trace shows hook coverage. */
                    send({ type: 'ctrl_delta',
                           slot: this.slot_def.slot,
                           name: this.slot_def.name,
                           n_diffs: -1, error: 'no_baseline' });
                    return;
                }
                var after = snapshot_all(this.ctrl);
                var diffs = diff_snapshots(this.before, after);
                stats.diffs_emitted++;
                /* Always log, including 0-diff calls — that's important
                 * negative information ("we know this call is hooked
                 * and doesn't touch ctrl in our window"). */
                send({
                    type: 'ctrl_delta',
                    slot: this.slot_def.slot,
                    name: this.slot_def.name,
                    n_diffs: diffs.length,
                    diffs: diffs.slice(0, MAX_DIFFS_PER_CALL),
                });
            }
        });
    } catch (e) {
        send({ type: 'attach_err', va: slot_def.va, err: e.toString() });
    }
});

rpc.exports = {
    stats: function () { return stats; },
    flush: function () { return stats; },
    reset: function () { stats = { calls: 0, diffs_emitted: 0 }; },
};

send({ type: 'ready', hook: 'fe_ctrl_watch', n_slots: SLOTS.length,
       watch_windows: WATCH_WINDOWS });
