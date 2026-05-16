'use strict';
/*
 * cart_walks_surgical_hook.js — surgical bracketing of inner_scorer +
 * anchor cost functions to disambiguate walker call sites.
 *
 * Why: cart_walks_safe / cart_walker_args attribute every durt/f0tr
 * walker call to `current_slot` set by inner_scorer entry, but walks can
 * fire inside anchor scoring funcs (FUN_08e89530 anchor D-cost,
 * FUN_08e893b0 anchor F0-cost) which are CALLED FROM inner_scorer. Those
 * walks bleed into the per-slot "first walk" master_compare picks,
 * causing audit diffs (e.g. nat_036 slot 7 first durt walk reports
 * phone_idx=0 mean=89.02 but mean=89.02 only exists in durt tree 36 =
 * "t", not tree 0 = "aa" — proving the walk wasn't slot-init).
 *
 * This hook maintains a per-thread mode STACK so each walker call is
 * tagged with the exact code path it came from:
 *
 *   slot_init   — walker inside inner_scorer body, no anchor func active
 *   anchor_orch — walker inside FUN_08e8ce60 body  (anchor scoring orch)
 *   anchor_d    — walker inside FUN_08e89530 body  (anchor D-cost)
 *   anchor_f0   — walker inside FUN_08e893b0 body  (anchor F0-cost)
 *   anchor_sp   — walker inside FUN_08e897b0 body  (anchor SP-cost)
 *   outside     — walker NOT inside inner_scorer (PostScoringAdj etc;
 *                 these are still emitted but flagged so consumers can
 *                 drop them).
 *
 * Per-walk record:
 *   { slot, slot_call_idx, walk_idx, tree, mode, phone_idx,
 *     q1..q9, leaf_mean, leaf_var, retval_ptr, ebx, ecx }
 *
 * Output type: "cart_walk_surgical_batch" (batched), each entry typed
 * "cart_walk_surgical".
 *
 * Safety:
 *   - Function-entry only (no mid-instruction hooks).
 *   - Reads guarded by Process.findRangeByAddress.
 *   - TOTAL_CAP bounded; per-slot capture is bounded by inner_scorer
 *     onEnter/onLeave pair so stale slot attribution can't drift.
 */

var ADDR_INNER_SCORER  = ptr('0x08E88DE0');
var ADDR_ANCHOR_ORCH   = ptr('0x08E8CE60');  /* FUN_08e8ce60 anchor scoring entry */
var ADDR_ANCHOR_DCOST  = ptr('0x08E89530');  /* FUN_08e89530 anchor D-cost */
var ADDR_ANCHOR_F0COST = ptr('0x08E893B0');  /* FUN_08e893b0 anchor F0-cost */
var ADDR_ANCHOR_SPCOST = ptr('0x08E897B0');  /* FUN_08e897b0 anchor SP-cost */
var ADDR_DURT_WALKER   = ptr('0x08E87D90');
var ADDR_F0TR_WALKER   = ptr('0x08E87E10');

var BATCH_N   = 64;
var TOTAL_CAP = 50000;

var stats = { inner: 0, durt: 0, f0tr: 0, dropped: 0, sent: 0,
              ptr_invalid: 0, read_errors: 0 };
var batch = [];

/* Per-process tracking. The engine is single-threaded for our use case
 * (one synth at a time), so global state is fine. If we ever multi-
 * thread, switch to ThreadId keyed maps. */
var current_slot     = -1;
var slot_call_idx    = 0;       /* incremented each time we enter inner_scorer */
var walk_idx         = 0;       /* incremented each walker call inside scorer */
var mode_stack       = [];      /* top = most recent active mode */
var in_inner_scorer  = false;
/* Track each walker call's record between onEnter and onLeave so we can
 * back-patch the leaf values on leave. Use the saved-thread-call-context
 * `this` map provided by Frida. */

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
    send({ type: 'cart_walk_surgical_batch', samples: batch });
    stats.sent += batch.length;
    batch = [];
}

function current_mode() {
    if (mode_stack.length === 0) return in_inner_scorer ? 'slot_init' : 'outside';
    return mode_stack[mode_stack.length - 1];
}

/* ---------------------------------------------------------------- */
/* inner_scorer: bracket per-slot work                               */
/* ---------------------------------------------------------------- */
Interceptor.attach(ADDR_INNER_SCORER, {
    onEnter: function () {
        stats.inner++;
        /* slot is the first stack arg (per cart_walker_args_hook). */
        var slotV = safeReadU32(this.context.esp.add(8));
        if (slotV !== null && slotV < 4096) {
            current_slot = slotV >>> 0;
        }
        slot_call_idx += 1;
        walk_idx = 0;
        in_inner_scorer = true;
    },
    onLeave: function () {
        in_inner_scorer = false;
        /* Drain any unclosed mode entries from this frame (defensive). */
        mode_stack.length = 0;
    }
});

/* ---------------------------------------------------------------- */
/* Anchor cost orchestrator + per-component cost funcs               */
/* Each pushes its mode on enter, pops on leave.                     */
/* ---------------------------------------------------------------- */
function push_mode(name) {
    return {
        onEnter: function () { mode_stack.push(name); },
        onLeave: function () {
            /* Pop only if our name is on top (defensive against re-entrancy). */
            for (var i = mode_stack.length - 1; i >= 0; i--) {
                if (mode_stack[i] === name) {
                    mode_stack.splice(i, 1);
                    break;
                }
            }
        }
    };
}
Interceptor.attach(ADDR_ANCHOR_ORCH,   push_mode('anchor_orch'));
Interceptor.attach(ADDR_ANCHOR_DCOST,  push_mode('anchor_d'));
Interceptor.attach(ADDR_ANCHOR_F0COST, push_mode('anchor_f0'));
Interceptor.attach(ADDR_ANCHOR_SPCOST, push_mode('anchor_sp'));

/* ---------------------------------------------------------------- */
/* durt walker entry/leave                                           */
/* ---------------------------------------------------------------- */
Interceptor.attach(ADDR_DURT_WALKER, {
    onEnter: function () {
        if (stats.durt + stats.f0tr >= TOTAL_CAP) {
            stats.dropped++; this.rec = null; return;
        }
        stats.durt++;
        walk_idx += 1;
        var ctx = this.context;
        var esp = ctx.esp;
        /* Capture caller's return address for code-path attribution
         * (cross-check against mode). [ESP+0] = retaddr at entry. */
        var retaddr = safeReadU32(esp);
        this.rec = {
            type      : 'cart_walk_surgical',
            tree      : 'durt',
            slot      : current_slot,
            slot_call_idx: slot_call_idx,
            walk_idx  : walk_idx,
            mode      : current_mode(),
            in_inner  : in_inner_scorer ? 1 : 0,
            mode_stack: mode_stack.slice(),
            retaddr   : retaddr,
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
            q7        : 0   /* EBX is XOR'd to 0 inside walker — leaf walk sees 0 */
        };
    },
    onLeave: function (retval) {
        if (!this.rec) return;
        var rv = retval;
        this.rec.retval_ptr = rv.toUInt32() >>> 0;
        this.rec.leaf_mean = safeReadF32(rv.add(0x10));
        this.rec.leaf_var  = safeReadF32(rv.add(0x14));
        /* Leaf marker: rv+0x8 = yes_child, must be negative. */
        this.rec.leaf_yes  = safeReadU32(rv.add(0x08));
        batch.push(this.rec);
        if (batch.length >= BATCH_N) flush();
    }
});

/* ---------------------------------------------------------------- */
/* f0tr walker entry/leave                                           */
/* ---------------------------------------------------------------- */
Interceptor.attach(ADDR_F0TR_WALKER, {
    onEnter: function () {
        if (stats.durt + stats.f0tr >= TOTAL_CAP) {
            stats.dropped++; this.rec = null; return;
        }
        stats.f0tr++;
        walk_idx += 1;
        var ctx = this.context;
        var esp = ctx.esp;
        var retaddr = safeReadU32(esp);
        this.rec = {
            type      : 'cart_walk_surgical',
            tree      : 'f0tr',
            slot      : current_slot,
            slot_call_idx: slot_call_idx,
            walk_idx  : walk_idx,
            mode      : current_mode(),
            in_inner  : in_inner_scorer ? 1 : 0,
            mode_stack: mode_stack.slice(),
            retaddr   : retaddr,
            phone_idx : null,           /* f0tr has only 1 tree */
            forest    : ctx.eax.toUInt32() >>> 0,
            ecx       : ctx.ecx.toUInt32() >>> 0,
            ebx       : ctx.ebx.toUInt32() >>> 0,
            q1        : safeReadU32(esp.add(0x04)),
            q2        : safeReadU32(esp.add(0x08)),
            q8        : safeReadU32(esp.add(0x0c)),
            q7        : ctx.ecx.toUInt32() >>> 0,
            q3        : 0, q4: 0, q5: 0, q9: 0
        };
    },
    onLeave: function (retval) {
        if (!this.rec) return;
        var rv = retval;
        this.rec.retval_ptr = rv.toUInt32() >>> 0;
        this.rec.leaf_mean = safeReadF32(rv.add(0x10));
        this.rec.leaf_var  = safeReadF32(rv.add(0x14));
        this.rec.leaf_yes  = safeReadU32(rv.add(0x08));
        batch.push(this.rec);
        if (batch.length >= BATCH_N) flush();
    }
});

/* ---------------------------------------------------------------- */
/* Lifecycle                                                         */
/* ---------------------------------------------------------------- */
rpc.exports = {
    stats: function () { return stats; },
    flush: function () { flush(); return stats; },
    reset: function () {
        flush();
        stats = { inner: 0, durt: 0, f0tr: 0, dropped: 0, sent: 0,
                  ptr_invalid: 0, read_errors: 0 };
        current_slot     = -1;
        slot_call_idx    = 0;
        walk_idx         = 0;
        mode_stack.length = 0;
        in_inner_scorer  = false;
    }
};

send({
    type: 'ready',
    hook: 'cart_walks_surgical',
    batch_n: BATCH_N,
    cap: TOTAL_CAP,
    addrs: {
        inner_scorer  : ADDR_INNER_SCORER.toString(),
        anchor_orch   : ADDR_ANCHOR_ORCH.toString(),
        anchor_d      : ADDR_ANCHOR_DCOST.toString(),
        anchor_f0     : ADDR_ANCHOR_F0COST.toString(),
        anchor_sp     : ADDR_ANCHOR_SPCOST.toString(),
        durt_walker   : ADDR_DURT_WALKER.toString(),
        f0tr_walker   : ADDR_F0TR_WALKER.toString()
    }
});
