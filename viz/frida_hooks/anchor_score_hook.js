'use strict';
/*
 * anchor_score_hook.js -- capture the 3 unknowns blocking 100% anchor pre_dp
 * formula reproduction.
 *
 * Target: FUN_08e8ce60 @ 0x08E8CE60 in SWIttsUSel.dll
 *         Anchor cand scorer (called per anchor slot from PostScoringAdj
 *         FUN_08e8d210 with type=2 syl or type=4 word).
 *
 * Calling convention: __cdecl, args on stack:
 *   [esp+ 4] param_1  log_ctx
 *   [esp+ 8] param_2  anchor_slot ptr
 *   [esp+12] param_3  USelNet ptr
 *   [esp+16] param_4  ?
 *   [esp+20] param_5  type (2=Syl, 4=Word)
 *   [esp+24] param_6  first_hp_idx
 *   [esp+28] param_7  last_hp_idx
 *
 * Captures (one-shot per session):
 *   1. weight+0x3c (anchor FLAG scaler; differs from IS's +0x38=0.25)
 *   2. _DAT_08e971d8 (histogram bin width)
 *   3. _DAT_08e98a24 (histogram norm scaler)
 *   4. _DAT_08e9857c (emphasis const)
 *   5. weight+0x10..+0x44 (full weight struct dump for cross-check)
 *
 * Per-call (capped):
 *   - type, first_hp, last_hp, anchor_slot uid/jk
 *   - workspace+0x18 syl_idx array snapshot at this anchor slot
 *
 * Safety: function-entry only, per the project's hot-path safety policy.
 */

var ADDR_ANCHOR_SCORER = ptr('0x08E8CE60');
var ADDR_DAT_971D8     = ptr('0x08E971D8');
var ADDR_DAT_98A24     = ptr('0x08E98A24');
var ADDR_DAT_9857C     = ptr('0x08E9857C');
var ADDR_DAT_98580     = ptr('0x08E98580');

var BATCH_N   = 1;     /* flush per call -- few anchor slots per utt */
var TOTAL_CAP = 5000;

var stats = { calls: 0, sent: 0, dropped: 0, ptr_invalid: 0, read_errors: 0 };
var batch = [];
var sentConstsOnce = false;

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

function flush() {
    if (batch.length === 0) return;
    send({ type: 'anchor_score_batch', samples: batch });
    stats.sent += batch.length;
    batch = [];
}

rpc.exports = {
    flush: function () { flush(); return stats; },
    drain: function () { flush(); return stats; },
    reset: function () { stats.calls = 0; stats.dropped = 0; batch = []; }
};

Interceptor.attach(ADDR_ANCHOR_SCORER, {
    onEnter: function () {
        stats.calls++;
        if (stats.calls > TOTAL_CAP) { stats.dropped++; return; }

        var esp = this.context.esp;
        var p1 = safeReadU32(esp.add(4));      /* log_ctx */
        var p2 = safeReadU32(esp.add(8));      /* anchor_slot */
        var p3 = safeReadU32(esp.add(12));     /* USelNet */
        var p4 = safeReadU32(esp.add(16));
        var p5 = safeReadU32(esp.add(20));     /* type */
        var p6 = safeReadU32(esp.add(24));     /* first_hp */
        var p7 = safeReadU32(esp.add(28));     /* last_hp */

        if (p3 === null || p3 < 0x100000) { return; }
        var net = ptr(p3);

        /* Per FUN_08e8ce60 decompile:
         *   iVar2 = *(int *)(param_3 + 4);   // weight struct
         *   iVar3 = *(int *)(param_3 + 8);   // weight struct (second)
         * Try BOTH and see which yields a sensible weight+0x44=1.0
         * (CONTEXT_COST_WEIGHT) value -- our ground truth.
         */
        var snap = {
            n: stats.calls,
            type: (p5 !== null) ? (p5 >>> 0) : null,
            first_hp: (p6 !== null) ? (p6 >>> 0) : null,
            last_hp: (p7 !== null) ? (p7 >>> 0) : null,
            anchor_slot: (p2 !== null) ? (p2 >>> 0) : null
        };

        if (p2 !== null && p2 >= 0x100000) {
            var ap = ptr(p2);
            snap.slot_n_cands = safeReadU32(ap.add(0x2c));
            /* anchor cand list ptr at slot+0x34 (per FUN_08e8ce60 decompile) */
            snap.cands_ptr = safeReadU32(ap.add(0x34));
        }

        /* One-shot constants dump on first call. */
        if (!sentConstsOnce) {
            var consts = {
                DAT_971d8: safeReadF32(ADDR_DAT_971D8),
                DAT_98a24: safeReadF32(ADDR_DAT_98A24),
                DAT_9857c: safeReadF32(ADDR_DAT_9857C),
                DAT_98580: safeReadF32(ADDR_DAT_98580)
            };

            /* Try iVar3 = *(USelNet+8) as weight ptr */
            var w_ptr_v = safeReadU32(net.add(8));
            var weights_v8 = null;
            if (w_ptr_v !== null && w_ptr_v >= 0x100000) {
                var wp = ptr(w_ptr_v);
                weights_v8 = {
                    base_ptr: w_ptr_v >>> 0,
                    /* SP weights at +0x10..+0x20 */
                    w_10: safeReadF32(wp.add(0x10)),
                    w_14: safeReadF32(wp.add(0x14)),
                    w_18: safeReadF32(wp.add(0x18)),
                    w_1c: safeReadF32(wp.add(0x1c)),
                    w_20: safeReadF32(wp.add(0x20)),
                    w_24: safeReadF32(wp.add(0x24)),  /* F0 */
                    w_28: safeReadF32(wp.add(0x28)),
                    w_2c: safeReadF32(wp.add(0x2c)),
                    w_30: safeReadF32(wp.add(0x30)),
                    w_34: safeReadF32(wp.add(0x34)),  /* DUR */
                    w_38: safeReadF32(wp.add(0x38)),  /* IS FLAG */
                    w_3c: safeReadF32(wp.add(0x3c)),  /* ANCHOR FLAG -- TARGET */
                    w_40: safeReadF32(wp.add(0x40)),
                    w_44: safeReadF32(wp.add(0x44)),  /* CONTEXT_COST */
                    w_48: safeReadF32(wp.add(0x48)),
                    w_4c: safeReadF32(wp.add(0x4c)),  /* PRUNE_THRESH */
                    w_50: safeReadF32(wp.add(0x50)),
                    w_54: safeReadF32(wp.add(0x54)),  /* type=2 norm */
                    w_58: safeReadF32(wp.add(0x58)),  /* type=2 norm2 */
                    w_5c: safeReadF32(wp.add(0x5c)),  /* type=4 norm */
                    w_60: safeReadF32(wp.add(0x60)),  /* type=4 norm2 */
                    w_80: safeReadF32(wp.add(0x80)),  /* MISS_F0 */
                    w_84: safeReadF32(wp.add(0x84)),  /* miss_offset */
                    w_8c: safeReadU32(wp.add(0x8c)),  /* f0tr gate */
                    w_90: safeReadU32(wp.add(0x90)),
                    w_98: safeReadU32(wp.add(0x98))   /* emphasis */
                };
            }

            send({ type: 'anchor_consts', consts: consts,
                   weights_via_USelNet_8: weights_v8 });
            sentConstsOnce = true;
        }

        /* workspace+0x18 syl_idx array dump per call. */
        var syl_arr_ptr_v = safeReadU32(net.add(0x18));
        if (syl_arr_ptr_v !== null && syl_arr_ptr_v >= 0x100000 &&
            p7 !== null && (p7 >>> 0) < 1000) {
            var syl_idx = [];
            var sp = ptr(syl_arr_ptr_v);
            var n = (p7 >>> 0) + 1;     /* indices 0..last_hp */
            for (var i = 0; i < n; ++i) {
                var v = safeReadU32(sp.add(i * 4));
                syl_idx.push(v);
            }
            snap.syl_idx_0_to_last = syl_idx;
        }

        /* workspace+0x14 per-hp ctx block dump. Stride 0x28 per hp.
         * Per FUN_08e8adc0 disasm: slot 0 row uses workspace[first_hp+0x4],
         * slot 1 uses [+0x8], slot 2 uses [last_hp+0x10], slot 3 uses [+0x14].
         * Dump bytes at offsets 0..0x18 to see the actual layout for the
         * first_hp/last_hp this anchor references. */
        var ws_block_ptr_v = safeReadU32(net.add(0x14));
        if (ws_block_ptr_v !== null && ws_block_ptr_v >= 0x100000 &&
            p6 !== null && p7 !== null) {
            var wsb = ptr(ws_block_ptr_v);
            var first_block = wsb.add((p6 >>> 0) * 0x28);
            var last_block = wsb.add((p7 >>> 0) * 0x28);
            var dump_block = function (b) {
                var bytes = [];
                for (var i = 0; i < 0x18; ++i) {
                    try { bytes.push(b.add(i).readU8()); }
                    catch (e) { bytes.push(null); }
                }
                return bytes;
            };
            snap.ws_first_hp_bytes = dump_block(first_block);
            snap.ws_last_hp_bytes = dump_block(last_block);
        }

        /* Save slot ptr for onLeave to read final n_cands. */
        this.snap = snap;
        this.anchor_slot_ptr = (p2 !== null) ? p2 : 0;
    },
    onLeave: function (retval) {
        if (this.snap === undefined) return;
        if (this.anchor_slot_ptr && this.anchor_slot_ptr >= 0x100000) {
            try {
                this.snap.final_n_cands = ptr(this.anchor_slot_ptr).add(0x2c).readU32();
                /* Read first cand's uid + jk if cands were created. */
                if (this.snap.final_n_cands && this.snap.final_n_cands > 0) {
                    var cands_ptr = ptr(this.anchor_slot_ptr).add(0x34).readU32();
                    if (cands_ptr >= 0x100000) {
                        var first_cand_ptr = ptr(cands_ptr).readU32();
                        if (first_cand_ptr >= 0x100000) {
                            this.snap.first_cand_uid = ptr(first_cand_ptr).add(0xc).readU32();
                            this.snap.first_cand_jk = ptr(first_cand_ptr).add(0x10).readU32();
                        }
                    }
                }
            } catch (e) {}
        }
        batch.push(this.snap);
        if (batch.length >= BATCH_N) flush();
    }
});

send({ type: 'ready' });
