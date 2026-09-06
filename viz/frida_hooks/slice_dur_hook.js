'use strict';
/*
 * slice_dur_hook.js -- the FE-supplied per-halfphone DURATION, as USel sees
 * it, straight out of the target-slice feature vectors.
 *
 * The question this answers: the engine's trailing pad target is one of six
 * distinct values across the 221-text corpus, all within 75 float32 ULPs of
 * 25.0, and the tagged text the FE prints shows only `?d`. Where does the
 * fraction come from?
 *
 * --- Reverse engineering basis (Ghidra MCP, SWIttsUSel.dll @ 0x08E80000) ---
 *
 * FUN_08e8de20 ("compute per-node mean target") accumulates, per node:
 *
 *     acc = 0.0f
 *     for (s = node->slice_begin; s <= node->slice_end; ++s)
 *         acc = (float)(acc + FUN_08e8f760(slices, s));       // f32 each add
 *     target = acc / node->n_halfphones;
 *
 * and FUN_08e8f760 is just an indexed read:
 *
 *     value = *(float *)( slices[0][idx] + *(int *)(slices[3] + 0xb0) * 4 )
 *
 * so `slices` is {[0] array of per-halfphone float vectors, [1] count,
 * [3] the loaded voice index}, and voice_index+0xb0 is the position of the
 * feature literally named "duration" in the VIN's `feat` chunk -- set by
 * FUN_08e84f00's strcmp chain at 0x08e85103.
 *
 * The vectors themselves are filled by FUN_08e8f2b0 (reached via
 * FUN_08e90da0) which walks the FE utterance's `Segment` relation and calls
 * each feature descriptor's extractor at +0xc; a type-3 (float) result has
 * its RAW BITS copied, two per segment -- left half then right half.
 *
 * --- Safety ---
 *
 * Function ENTRY only, on FUN_08e8de20, which runs ONCE per utterance --
 * not a hot loop, and nowhere near the Viterbi inner path that caused the
 * 2026-05-05 kills. The slice array is already populated at this point:
 * SWIttsUSelUnitSelection calls FUN_08e90da0 well before it.
 *
 * __thiscall: ecx = this, [esp+4] = param_1, [esp+8] = param_2 = slices.
 */

var ADDR_MEAN_TARGET = ptr('0x08E8DE20');

function isReadable(addr, nbytes) {
    if (addr.isNull()) return false;
    var r = Process.findRangeByAddress(addr);
    if (!r) return false;
    if (r.protection.charAt(0) !== 'r') return false;
    return addr.add(nbytes).compare(r.base.add(r.size)) <= 0;
}

var TOTAL_CAP = 4000;
var MAX_SLICES = 4096;
/* Dump every feature, not just duration. On by default -- the vectors are
 * ~16 floats per halfphone, a few hundred per utterance, which is nothing
 * next to the wsola_in captures. */
var DUMP_ALL = true;
var stats = { calls: 0, sent: 0, dropped: 0 };
var batch = [];

function flush() {
    if (batch.length === 0) return;
    send({ type: 'slice_dur_batch', samples: batch });
    stats.sent += batch.length;
    batch = [];
}

Interceptor.attach(ADDR_MEAN_TARGET, {
    onEnter: function () {
        stats.calls++;
        if (stats.calls > TOTAL_CAP) { stats.dropped++; return; }
        try {
            var slices = this.context.esp.add(8).readPointer();
            if (!isReadable(slices, 16)) { stats.dropped++; return; }

            var vec_arr = slices.readPointer();          /* [0] */
            var n       = slices.add(4).readS32();       /* [1] */
            var vindex  = slices.add(12).readPointer();  /* [3] */
            if (n < 0 || n > MAX_SLICES) { stats.dropped++; return; }
            if (!isReadable(vec_arr, n * 4)) { stats.dropped++; return; }
            if (!isReadable(vindex, 0xb4)) { stats.dropped++; return; }

            var dur_idx = vindex.add(0xb0).readS32();
            /* Companion indices, so a wrong-feature reading is visible
             * rather than silently plausible: +0x14 name, +0x18 filename,
             * +0xb4 pitch, +0xb8 pitch_z, +0x1c start, +0x2c n_features. */
            var n_feat = vindex.add(0x2c).readS32();
            if (dur_idx < 0 || n_feat <= 0 || dur_idx >= n_feat) {
                stats.dropped++;
                return;
            }

            var vals = [];
            var all = [];
            for (var i = 0; i < n; ++i) {
                var vec = vec_arr.add(i * 4).readPointer();
                if (!isReadable(vec, n_feat * 4)) { vals.push(null); continue; }
                vals.push(vec.add(dur_idx * 4).readFloat());
                /* SPFY_SLICE_ALL: the WHOLE feature vector as raw u32, so a
                 * value can be read as float or int offline. load_index also
                 * names a "start" feature (voice_index+0x1c), and the
                 * duration extractor is a difference of absolute times, so
                 * the absolute clock may be sitting right here. */
                if (DUMP_ALL) {
                    var row = [];
                    for (var k = 0; k < n_feat; ++k)
                        row.push(vec.add(k * 4).readU32());
                    all.push(row);
                }
            }
            batch.push({
                call: stats.calls,
                n_slices: n,
                dur_idx: dur_idx,
                n_feat: n_feat,
                pitch_idx: vindex.add(0xb4).readS32(),
                start_idx: vindex.add(0x1c).readS32(),
                name_idx: vindex.add(0x14).readS32(),
                file_idx: vindex.add(0x18).readS32(),
                dur: vals,
                all: DUMP_ALL ? all : undefined,
            });
        } catch (e) { stats.dropped++; return; }
        flush();
    }
});

rpc.exports = {
    stats: function () { return stats; },
    flush: function () { flush(); return stats; },
    reset: function () { flush(); stats = { calls: 0, sent: 0, dropped: 0 }; },
};

send({ type: 'ready', hook: 'slice_dur', cap: TOTAL_CAP });
