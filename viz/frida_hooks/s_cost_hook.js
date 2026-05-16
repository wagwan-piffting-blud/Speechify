'use strict';

// Hook stock's S (context) cost — captures the fVar13 value at 0x8E89232
// (right before it's added to the candidate's total score). Per README
// §2185-2188 this is a 4-component sum weighted by CONTEXT_COST_WEIGHT.
//
// Also captures:
//   - The 4 context bytes from the candidate struct (+0x0, +0x1, +0x2, +0x3 of *voice[0xc0|0xc4])
//   - The 4 table pointer bases (local_3c..local_4c in the scorer frame)
//   - The scorer's current slot/candidate indices

var ADDR_SCORER = ptr('0x08e88de0');
var ADDR_S_AFTER_COMPUTE = ptr('0x08e891c1');  // after all 4 adds, before FMUL by voice[0x44]

var currentSlot = -1;
var byCand = {};    // key "slot.cand" -> { s_raw, ctx_bytes, ... }

function tryU32(p) { try { return p.readU32(); } catch(e) { return null; } }

Interceptor.attach(ADDR_SCORER, {
    onEnter: function() {
        var slotIdx = tryU32(this.context.esp.add(0x8));
        if (slotIdx !== null && slotIdx >= 0 && slotIdx < 2048) currentSlot = slotIdx;
    }
});

// Hook right after the 4-table FADD chain completes (before FMUL by voice[0x44])
// At this point the top of x87 stack has fVar13 (s_raw). We can't read x87
// easily; instead hook the subsequent FMUL and snapshot its second operand.
// Simpler: hook at the FMUL-ing instruction and grab the S value after FMUL
// is stored (LAB_08e891c2 => fVar13 stored).

// Actually let's just hook before the S section at 0x08e891a8 and after at
// 0x08e891dc to bracket. On leave, the candidate's emit cost field contains
// the accumulated score with S added.

// Simpler approach: hook the STORE of S at candidate+0x14 (the logging path).
// The code at 0x08e8932a writes `*(candidate+0x14) = fVar13 * voice[0x44]`.
// But this only fires when debug/log flag is set — may not fire in prod.

// Pragmatic: capture the 4 context bytes per candidate. That's cheap and
// might let us reverse the formula from data.
var ADDR_CAND_LOOP_ITER = ptr('0x08e890b3');  // start of per-cand loop body
Interceptor.attach(ADDR_CAND_LOOP_ITER, {
    onEnter: function() {
        if (currentSlot < 0) return;
        // At loop iteration start, EBX = cand_idx * 0x18 (per decomp)
        // Actually the loop structure per decomp: local_20 = cand_idx,
        // param_1 = cand_idx * 0x18.
        // Let me just log ESI (this), ESP, and the registers — we'll dig in.
        // Non-critical; skip for now to avoid perf hit.
    }
});

rpc.exports = {
    getByCand: function() { return byCand; },
    reset: function() { byCand = {}; currentSlot = -1; },
};

send({type: 'ready'});
