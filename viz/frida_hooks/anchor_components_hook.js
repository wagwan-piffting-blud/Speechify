'use strict';
/*
 * anchor_components_hook.js -- per-cand component breakdown for the
 * anchor cost outer driver FUN_08e8ce60. Localises the 3.06 anchor-TC
 * undercount that drives the last 47 wrong UIDs (project_final_uid_
 * gap_2026_05_13.md). text_013 ("Three.") slot 11 is the canonical
 * drill-down: engine pre_dp=8.475, ours=5.416, CCOS bit-exact at
 * 3.4386, so the 3.06 gap lives in FLAG+SP+D+F0 collectively.
 *
 * Engine sequence inside FUN_08e8ce60 (per RESUME.md attack plan):
 *   1. CCOS  via FUN_08e8adc0   -> cost_array_out[k]; caller stores
 *                                  cand[k].tc = ccos
 *   2. FLAG  inline             -> cand[k].tc += (int)(sum_b17 *
 *                                  weight+0x3c * 0.01)
 *   3. SP    via FUN_08e897b0   -> cand[k].tc += sp
 *   4. D     via FUN_08e89530   -> cand[k].tc += d   (gated on
 *                                  voice+0xd0 != 0)
 *   5. F0    via FUN_08e893b0   -> cand[k].tc += f0  (gated on
 *                                  voice+0xcc != 0)
 *
 * Strategy: bracket FUN_08e8ce60 onEnter/onLeave and snapshot the
 * active cand's tc (+0x2c) at each subroutine entry. Component cost =
 * tc_at_next_sync_point - tc_at_this_sync_point. This avoids any mid-
 * instruction probes (project safety policy: onEnter/onLeave only,
 * per anchor_score_hook.js:30 and ccos_apply_hook.js:67).
 *
 * Cand identification: each subroutine takes the cand pointer as one
 * of its stack args. findCandArg() walks the first 6 stack slots and
 * picks the one whose deref looks like a cand struct (uid @+0xc in
 * range, finite float @+0x2c). This is robust to arg-order variation
 * across the three subfunctions.
 *
 * Output: one `anchor_components` message per FUN_08e8ce60 call.
 * Caller (run_frida_capture.py --filter "^text_013$") provides the
 * phrase identifier; slot index is derivable from first_hp/last_hp
 * relative to the utterance HP layout (or via call ordering -- the
 * driver fires once per anchor slot).
 *
 * Schema:
 *   {
 *     "type": "anchor_components",
 *     "frame": {
 *       "n_call":     int,    // monotonically increasing
 *       "type":       int,    // 2=Syl, 4=Word
 *       "first_hp":   int,
 *       "last_hp":    int,
 *       "anchor_slot": uint,  // ptr (hex when printed)
 *       "net":        uint,
 *       "w_3c":       float,  // weight+0x3c (FLAG scaler) for offline
 *                             // FLAG reproduction
 *       "voice_d0_gate": uint, // 0 => D skipped per engine
 *       "voice_cc_gate": uint, // 0 => F0 skipped per engine
 *       "events":     [        // ordered sync-point snapshots
 *         {"kind": "ccos_leave"},  // no cand_ptr; ccos written by
 *                                  // caller in subsequent store
 *         {"kind": "sp_enter"|"d_enter"|"f0_enter",
 *          "cand_ptr": uint, "uid": int, "jk": int, "tc": float}
 *       ],
 *       "n_kept":     int,
 *       "final_cands": [
 *         {"kept_idx": int, "ptr": uint, "uid": int, "jk": int,
 *          "tc": float}     // tc is anchor pre_dp endpoint
 *       ]
 *     },
 *     "stats": {...}
 *   }
 *
 * Offline reduction per (kept_idx, uid):
 *   ccos    = first sp_enter.tc            // post-CCOS+FLAG endpoint
 *                                          //   minus FLAG... actually
 *                                          //   inline FLAG happens
 *                                          //   between CCOS-store and
 *                                          //   SP-call, so first
 *                                          //   sp_enter.tc = ccos+flag
 *   flag    = sp_enter.tc - ccos_known     // ccos_known from
 *                                          //   ccos_apply hook or
 *                                          //   from the matching
 *                                          //   ccos_apply trace file
 *   sp      = d_enter.tc - sp_enter.tc     // (if D gated off, use
 *                                          //   final_cands.tc instead
 *                                          //   of d_enter.tc)
 *   d       = f0_enter.tc - d_enter.tc     // similarly degrade if
 *                                          //   F0 gated off
 *   f0      = final_cands.tc - f0_enter.tc
 *   total   = final_cands.tc
 *
 * Safety: function-entry/leave only. No Stalker, no instruction-level
 * Interceptor.attach. FUN_08e8ce60 is called once per anchor slot
 * (low frequency); subroutines fire a few times per anchor. No hot-
 * path concern.
 */

var ADDR_ANCHOR_CE60 = ptr('0x08E8CE60');  /* anchor cost outer driver */
var ADDR_CCOS_ADC0   = ptr('0x08E8ADC0');  /* CCOS apply */
var ADDR_SP_97B0     = ptr('0x08E897B0');  /* SP cost subroutine */
var ADDR_D_9530      = ptr('0x08E89530');  /* D cost subroutine */
var ADDR_F0_93B0     = ptr('0x08E893B0');  /* F0 cost subroutine */

var TOTAL_CAP = 5000;

var stats = {
    anchor_calls: 0, anchor_leaves: 0, dropped: 0,
    ccos_calls: 0, sp_calls: 0, d_calls: 0, f0_calls: 0,
    read_errors: 0, ptr_invalid: 0
};

/* Single-threaded engine: one active anchor frame at a time. */
var frame = null;

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

function safeReadF32(addr) {
    if (!rangeOK(addr)) { stats.ptr_invalid++; return null; }
    try { return addr.readFloat(); }
    catch (e) { stats.read_errors++; return null; }
}

/*
 * Returns a {ptr, uid, jk, tc} snapshot if `p` looks like a cand
 * struct (uid in plausible range, tc finite). Otherwise null.
 * Tom voice cand uids run up to ~200000; cap at 0x300000 to allow
 * slack.
 */
function looksLikeCand(p) {
    if (!p || p < 0x100000) return null;
    var c = ptr(p);
    var uid = safeReadU32(c.add(0xc));
    var jk  = safeReadU32(c.add(0x10));
    var tc  = safeReadF32(c.add(0x2c));
    if (uid === null || jk === null || tc === null) return null;
    if (uid === 0 || uid > 0x300000) return null;
    if (!isFinite(tc)) return null;
    return { ptr: p >>> 0, uid: uid >>> 0, jk: jk >>> 0, tc: tc };
}

/* Snapshot all kept cands by walking slot+0x34 (cand array) with count
 * at slot+0x2c. The first findCandArg approach (stack-arg heuristic)
 * misidentified non-cand pointers (e.g. uid=17, tc=0). Walking the
 * kept-cand array directly is robust to subfunction arg-order
 * variation AND captures every cand's tc at the sync point -- so the
 * analyzer can attribute component costs per-cand by diffing across
 * consecutive sync points, regardless of whether the engine loops
 * cand-major or component-major. */
function snapshotKeptCands() {
    if (!frame || !frame.anchor_slot || frame.anchor_slot < 0x100000) {
        return { n_kept: -1, cands: [] };
    }
    var slot = ptr(frame.anchor_slot);
    var n_kept = safeReadU32(slot.add(0x2c));
    var cands_arr_ptr = safeReadU32(slot.add(0x34));
    if (n_kept === null || cands_arr_ptr === null ||
        cands_arr_ptr < 0x100000 || n_kept < 0 || n_kept >= 1000) {
        return { n_kept: -1, cands: [] };
    }
    var arr = ptr(cands_arr_ptr);
    var out = [];
    for (var i = 0; i < n_kept; ++i) {
        var cp = safeReadU32(arr.add(i * 4));
        if (cp === null || cp < 0x100000) {
            out.push({ kept_idx: i, ptr: null, uid: null,
                       jk: null, tc: null });
            continue;
        }
        var c = ptr(cp);
        out.push({
            kept_idx: i,
            ptr: cp >>> 0,
            uid: safeReadU32(c.add(0xc)),
            jk:  safeReadU32(c.add(0x10)),
            tc:  safeReadF32(c.add(0x2c))
        });
    }
    return { n_kept: n_kept, cands: out };
}

function recordSubcallEntry(kind) {
    if (!frame) return;
    var snap = snapshotKeptCands();
    frame.events.push({
        kind: kind,
        n_kept: snap.n_kept,
        cands: snap.cands
    });
}

Interceptor.attach(ADDR_ANCHOR_CE60, {
    onEnter: function () {
        if (stats.anchor_calls >= TOTAL_CAP) {
            stats.dropped++;
            this.skipped = true;
            return;
        }
        stats.anchor_calls++;

        var esp = this.context.esp;
        var p2 = safeReadU32(esp.add(8));    /* anchor_slot */
        var p3 = safeReadU32(esp.add(12));   /* USelNet */
        var p5 = safeReadU32(esp.add(20));   /* type 2=Syl 4=Word */
        var p6 = safeReadU32(esp.add(24));   /* first_hp */
        var p7 = safeReadU32(esp.add(28));   /* last_hp */

        var w_3c = null, voice_d0 = null, voice_cc = null;
        if (p3 && p3 >= 0x100000) {
            var net = ptr(p3);
            var voice_ptr = safeReadU32(net.add(4));
            var weight_ptr = safeReadU32(net.add(8));
            if (weight_ptr && weight_ptr >= 0x100000) {
                w_3c = safeReadF32(ptr(weight_ptr).add(0x3c));
            }
            if (voice_ptr && voice_ptr >= 0x100000) {
                voice_d0 = safeReadU32(ptr(voice_ptr).add(0xd0));
                voice_cc = safeReadU32(ptr(voice_ptr).add(0xcc));
            }
        }

        frame = {
            n_call: stats.anchor_calls,
            type: (p5 === null ? -1 : (p5 >>> 0)),
            first_hp: (p6 === null ? -1 : (p6 >>> 0)),
            last_hp: (p7 === null ? -1 : (p7 >>> 0)),
            anchor_slot: (p2 === null ? 0 : (p2 >>> 0)),
            net: (p3 === null ? 0 : (p3 >>> 0)),
            w_3c: w_3c,
            voice_d0_gate: voice_d0,
            voice_cc_gate: voice_cc,
            events: []
        };
    },
    onLeave: function (retval) {
        if (this.skipped || !frame) return;
        stats.anchor_leaves++;

        /* After F0 (or the last gated component) the cand's tc is
         * its anchor pre_dp endpoint. Walk slot+0x34 (cand array)
         * with count at slot+0x2c. */
        var final_cands = [];
        var n_kept_v = null;
        if (frame.anchor_slot && frame.anchor_slot >= 0x100000) {
            var slot = ptr(frame.anchor_slot);
            n_kept_v = safeReadU32(slot.add(0x2c));
            var cands_arr_ptr = safeReadU32(slot.add(0x34));
            if (n_kept_v !== null && cands_arr_ptr &&
                cands_arr_ptr >= 0x100000 &&
                n_kept_v >= 0 && n_kept_v < 1000) {
                var arr = ptr(cands_arr_ptr);
                for (var i = 0; i < n_kept_v; ++i) {
                    var cp = safeReadU32(arr.add(i * 4));
                    var s = looksLikeCand(cp);
                    if (s !== null) {
                        s.kept_idx = i;
                        final_cands.push(s);
                    } else {
                        final_cands.push({ kept_idx: i, ptr: (cp || 0) >>> 0,
                                           uid: null, jk: null, tc: null });
                    }
                }
            }
        }
        frame.n_kept = (n_kept_v === null ? -1 : n_kept_v);
        frame.final_cands = final_cands;

        send({ type: 'anchor_components', frame: frame, stats: stats });
        frame = null;
    }
});

/* CCOS apply -- snapshot kept cands at FUN_08e8adc0 onLeave. At this
 * point the caller has NOT yet stored cost_array_out into cand.tc, so
 * the recorded tc is the pre-CCOS endpoint (typically 0, but recorded
 * for completeness). The first sp_enter snapshot gives the post-CCOS,
 * post-FLAG endpoint -- diff against ccos_leave to attribute CCOS+FLAG.
 * CCOS alone is recoverable from a concurrent ccos_apply trace. */
Interceptor.attach(ADDR_CCOS_ADC0, {
    onLeave: function () {
        if (!frame) return;
        stats.ccos_calls++;
        recordSubcallEntry('ccos_leave');
    }
});

Interceptor.attach(ADDR_SP_97B0, {
    onEnter: function () {
        if (!frame) return;
        stats.sp_calls++;
        recordSubcallEntry('sp_enter');
    }
});

Interceptor.attach(ADDR_D_9530, {
    onEnter: function () {
        if (!frame) return;
        stats.d_calls++;
        recordSubcallEntry('d_enter');
    }
});

Interceptor.attach(ADDR_F0_93B0, {
    onEnter: function () {
        if (!frame) return;
        stats.f0_calls++;
        recordSubcallEntry('f0_enter');
    }
});

rpc.exports = {
    drain: function () { return stats; },
    flush: function () { return stats; },
    reset: function () {
        stats = {
            anchor_calls: 0, anchor_leaves: 0, dropped: 0,
            ccos_calls: 0, sp_calls: 0, d_calls: 0, f0_calls: 0,
            read_errors: 0, ptr_invalid: 0
        };
        frame = null;
    }
};

send({ type: 'ready' });
