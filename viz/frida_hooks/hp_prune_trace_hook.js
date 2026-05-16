// hp_prune_trace_hook.js
// Trace engine FUN_08e88830 (histogram-prune for HP cands).
// FUNCTION-ENTRY ONLY hook — mid-function trampolines AV the engine
// (per project Frida rules; see feedback-frida-entry-only memory).
//
// On entry: capture inputs.
//   - thisPtr in ECX (thiscall)
//   - this[0]   = n_cands_in
//   - this[6]   = cand_buf ptr (this+0x18)
//   - stack:    return addr, then param_1 (HP_MAX int), param_2 (HP_THRESH float),
//               param_3 (HP_SLOPE float), param_4 (best float).
//   - Snapshot all pre-prune cand TCs (uid + tc at +0x04 of each 0x18-stride cand).
//
// On leave: capture outputs.
//   - this[0]   = n_cands_out (post-prune)
//   - Snapshot all post-prune cand TCs.
//
// bin_dist and threshold are INFERRED downstream from the diff:
//   threshold ∈ [max(kept_tc), min(dropped_tc)]
//   bin_dist  = threshold - best (lies in the same range)

var ADDR_PRUNE_FN = ptr('0x08E88830');

function tryU32(p) { try { return p.readU32(); } catch (e) { return null; } }
function tryF32(p) { try { return p.readFloat(); } catch (e) { return null; } }

function snapshotCands(thisPtr, n) {
    if (n === null || n <= 0 || n > 200) return [];
    var cb_ptr = tryU32(thisPtr.add(0x18));
    if (cb_ptr === null || cb_ptr < 0x100000) return [];
    var arr = ptr(cb_ptr);
    var out = [];
    for (var i = 0; i < n; i++) {
        var base = arr.add(i * 0x18);
        var uid = tryU32(base);
        var tc  = tryF32(base.add(0x04));
        out.push({ uid: uid, tc: tc });
    }
    return out;
}

var callIdx = 0;
var ctxStack = [];

Interceptor.attach(ADDR_PRUNE_FN, {
    onEnter: function (args) {
        callIdx++;
        var thisPtr = this.context.ecx;
        var esp = this.context.esp;
        // thiscall: ESP+0 is return addr. params start at ESP+4.
        var n_in      = tryU32(thisPtr);
        var hp_max    = tryU32(esp.add(4));
        var hp_thresh = tryF32(esp.add(8));
        var hp_slope  = tryF32(esp.add(12));
        var best      = tryF32(esp.add(16));
        var tcs_in    = snapshotCands(thisPtr, n_in);

        ctxStack.push({
            idx: callIdx,
            thisPtr: thisPtr,
            n_in: n_in,
            best: best,
            tcs_in: tcs_in
        });

        send({
            type: 'hp_prune_enter',
            idx: callIdx,
            this_ptr: thisPtr.toString(),
            n_in: n_in,
            hp_max: hp_max,
            hp_thresh: hp_thresh,
            hp_slope: hp_slope,
            best: best,
            tcs_in: tcs_in
        });
    },
    onLeave: function (retval) {
        var ctx = ctxStack.pop();
        if (!ctx) return;
        var n_out = tryU32(ctx.thisPtr);
        var tcs_out = snapshotCands(ctx.thisPtr, n_out);
        send({
            type: 'hp_prune_leave',
            idx: ctx.idx,
            n_in: ctx.n_in,
            n_out: n_out,
            best: ctx.best,
            tcs_out: tcs_out
        });
    }
});

send({ type: 'ready', hook: 'hp_prune_trace' });
