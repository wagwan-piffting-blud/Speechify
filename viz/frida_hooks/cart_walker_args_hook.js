'use strict';
/*
 * cart_walker_args_hook.js -- per-slot CART feature ground truth.
 *
 * Captures every CART walker entry (durt + f0tr) along with the 8
 * possible feature values the dispatcher could pick. The current
 * question's q_type then disambiguates which value the engine actually
 * compares against.
 *
 * --- Why we need this ---
 *
 * The retired cart_walks_hook.js gave us per-slot leaf means + the
 * sequence of (q_type, value) pairs visited along the walk. But it
 * didn't tell us WHICH workspace table each q_type's value comes from,
 * so we can't reproduce the per-slot feature precomputation from the
 * FE tree alone (Phase B4.3). This hook fills that gap.
 *
 * For each walker call we emit one record:
 *
 *   { tree: 'durt' | 'f0tr',
 *     slot: <int> (from inner_scorer onEnter, set on this.slot earlier),
 *     phone_idx: <int> (durt only; selects which of 47 trees),
 *     q1: <int>,    // workspace[0x28][slot]
 *     q2: <int>,    // workspace[0x2c][slot]
 *     q3: <int>,    // forest_8[slice+0x8] (durt) or 0 (f0tr)
 *     q4: <int>,    // forest_8[slice+0x10] (durt) or 0 (f0tr)
 *     q5: <int>,    // workspace[0x40][slot] (durt) or 0 (f0tr)
 *     q7: <int>,    // workspace[0x34][slot] (f0tr) or 0 (durt; EBX-zeroed)
 *     q8: <int>,    // workspace[0x38][slot]
 *     q9: <int>,    // workspace[0x3c][slot] (durt) or 0 (f0tr)
 *   }
 *
 * Joined with the existing cart_walks JSONL (per-slot question/value
 * sequence), this lets us correlate per-q_type values to per-slot
 * features, and thereby derive the feature-derivation kernels from FE
 * tree state. Together with fe_tree.jsonl (which has the FE-emitted
 * Word/Syl/Segment/Phrase tree per utterance), we have everything we
 * need to reverse-engineer each kernel.
 *
 * --- Reverse engineering basis ---
 *
 * Walker disasm + scorer push trace (see project_m3_4_status.md "CART
 * feature dispatcher decoded"). The q_type→stack-arg rotation inside
 * each walker is fixed; we read the values via direct stack reads
 * synthesized from each walker's individual ABI.
 *
 * For durt (FUN_08e87d90, 7 stack args at [ESP+4..0x1c]):
 *   q1 = arg1 = [ESP+0x04]
 *   q2 = arg2 = [ESP+0x08]
 *   q3 = arg3 = [ESP+0x0c]
 *   q4 = arg4 = [ESP+0x10]
 *   q5 = arg5 = [ESP+0x14]
 *   q9 = arg6 = [ESP+0x18]
 *   q8 = arg7 = [ESP+0x1c]
 *   plus phone_idx = EDX register
 *
 * For f0tr (FUN_08e87e10, 3 stack args + ECX):
 *   q1 = arg1 = [ESP+0x04]
 *   q2 = arg2 = [ESP+0x08]
 *   q8 = arg3 = [ESP+0x0c]
 *   q7 = ECX register
 *   q3 = q4 = q5 = q9 = 0 (walker hard-codes them)
 *
 * Both walkers receive EAX = forest ptr (durt: voice+0xd0,
 * f0tr: voice+0xcc). We capture EAX too as a sanity check.
 *
 * --- Safety ---
 *
 * Function-entry only (both walkers and the scorer). Bounded at
 * TOTAL_CAP records per session. All reads guarded by
 * Process.findRangeByAddress.
 */

var ADDR_INNER_SCORER = ptr('0x08E88DE0');
var ADDR_DURT_WALKER  = ptr('0x08E87D90');
var ADDR_F0TR_WALKER  = ptr('0x08E87E10');

var BATCH_N   = 64;
var TOTAL_CAP = 50000;
var stats = { inner: 0, durt: 0, f0tr: 0,
              sent: 0, dropped: 0,
              ptr_invalid: 0, read_errors: 0 };
var batch = [];

/* current_slot is set by inner_scorer onEnter and read by the walker
 * onEnter handlers (which fire inside the scorer's per-slot prologue).
 * The in_inner_scorer flag is set on InnerScorer entry and cleared on
 * exit, so we can distinguish per-slot AddUnits walks (correctly
 * attributed) from PostScoringAdj walks (FUN_08e893b0/08e89530 also
 * call the walkers, but `current_slot` would be stale). Walker events
 * fired with in_inner_scorer == false are dropped from the trace
 * because we cannot reliably attribute them to a slot. */
var current_slot     = -1;
var in_inner_scorer  = false;

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

function flush() {
    if (batch.length === 0) return;
    send({ type: 'cart_walker_args_batch', samples: batch });
    stats.sent += batch.length;
    batch = [];
}

/* ------------------------------------------------------------------ */
/* Inner scorer: track current slot                                    */
/* ------------------------------------------------------------------ */
Interceptor.attach(ADDR_INNER_SCORER, {
    onEnter: function () {
        stats.inner++;
        var slotV = safeReadU32(this.context.esp.add(8));
        if (slotV !== null && slotV < 4096) current_slot = slotV >>> 0;
        in_inner_scorer = true;
    },
    onLeave: function () {
        in_inner_scorer = false;
    }
});

/* ------------------------------------------------------------------ */
/* durt walker: 7 stack args                                           */
/* ------------------------------------------------------------------ */
Interceptor.attach(ADDR_DURT_WALKER, {
    onEnter: function () {
        /* Drop PostScoringAdj-time walks where current_slot is stale. */
        if (!in_inner_scorer) { stats.dropped++; return; }
        if (stats.durt + stats.f0tr >= TOTAL_CAP) {
            stats.dropped++; return;
        }
        stats.durt++;

        var ctx = this.context;
        var esp = ctx.esp;

        var rec = {
            n         : stats.durt + stats.f0tr,
            tree      : 'durt',
            slot      : current_slot,
            phone_idx : ctx.edx.toUInt32() >>> 0,
            forest    : ctx.eax.toUInt32() >>> 0,
            ecx       : ctx.ecx.toUInt32() >>> 0,
            ebx       : ctx.ebx.toUInt32() >>> 0,
            q1        : safeReadU32(esp.add(0x04)),
            q2        : safeReadU32(esp.add(0x08)),
            q3        : safeReadU32(esp.add(0x0c)),
            q4        : safeReadU32(esp.add(0x10)),
            q5        : safeReadU32(esp.add(0x14)),
            q9        : safeReadU32(esp.add(0x18)),
            q8        : safeReadU32(esp.add(0x1c)),
            q7        : 0   /* EBX is XOR'd to 0 inside walker */
        };

        batch.push(rec);
        if (batch.length >= BATCH_N) flush();
    }
});

/* ------------------------------------------------------------------ */
/* f0tr walker: 3 stack args + ECX                                     */
/* ------------------------------------------------------------------ */
Interceptor.attach(ADDR_F0TR_WALKER, {
    onEnter: function () {
        if (!in_inner_scorer) { stats.dropped++; return; }
        if (stats.durt + stats.f0tr >= TOTAL_CAP) {
            stats.dropped++; return;
        }
        stats.f0tr++;

        var ctx = this.context;
        var esp = ctx.esp;

        var rec = {
            n         : stats.durt + stats.f0tr,
            tree      : 'f0tr',
            slot      : current_slot,
            phone_idx : null,            /* f0tr has only 1 tree */
            forest    : ctx.eax.toUInt32() >>> 0,
            ecx       : ctx.ecx.toUInt32() >>> 0,
            ebx       : ctx.ebx.toUInt32() >>> 0,
            q1        : safeReadU32(esp.add(0x04)),
            q2        : safeReadU32(esp.add(0x08)),
            q8        : safeReadU32(esp.add(0x0c)),
            q7        : ctx.ecx.toUInt32() >>> 0,  /* EBX = ECX inside walker */
            q3        : 0,
            q4        : 0,
            q5        : 0,
            q9        : 0
        };

        batch.push(rec);
        if (batch.length >= BATCH_N) flush();
    }
});

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */
send({ type: 'cart_walker_args_init',
       inner_scorer: ADDR_INNER_SCORER.toString(),
       durt_walker : ADDR_DURT_WALKER.toString(),
       f0tr_walker : ADDR_F0TR_WALKER.toString() });

rpc.exports = {
    stats: function () { return stats; },
    flush: function () { flush(); return stats; },
    reset: function () {
        flush();
        stats = { inner: 0, durt: 0, f0tr: 0,
                  sent: 0, dropped: 0,
                  ptr_invalid: 0, read_errors: 0 };
        current_slot = -1;
    }
};

send({ type: 'ready', hook: 'cart_walker_args',
       cap: TOTAL_CAP });
