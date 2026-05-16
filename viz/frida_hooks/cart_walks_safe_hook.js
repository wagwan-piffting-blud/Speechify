'use strict';
/*
 * cart_walks_safe_hook.js — function-entry-only replacement for the
 * retired cart_walks_hook.js (see SAFETY POLICY in run_frida_capture.py
 * and the retirement banner in cart_walks_hook.js).
 *
 * Captures exactly what spfy_viterbi_replay.c consumes from a cart_walks
 * JSONL: per-slot durt and f0tr leaf (mean, var). The original hook also
 * captured the visited (q_type, value) sequence by hooking the per-
 * question dispatcher (0x08e87c90) and ESI compare helper (0x08e87c70)
 * inside the CART scoring loop — those are the hot-path mid-instruction
 * hooks that AV'd the engine after enough trampoline trips. We omit them.
 *
 * Schema-compatible with cart_walks_hook.js for the fields downstream
 * code reads (slot, tree, leaf_mean, leaf_var). The "questions" array is
 * always empty []; the "n" counter still increments per walk so existing
 * cart_walks consumers don't see schema drift. spfy_viterbi_replay only
 * uses the fields it parses (slot/tree/leaf_mean/leaf_var per
 * parse_cart_walk_line in spfy_viterbi_replay.c:825).
 *
 * Hooks (function entries only — confirmed safe per cart_walker_args_hook.js
 * which hooks the same three sites without instability):
 *   0x08E87D90  durt walker entry  (FUN_08e87d90)
 *   0x08E87E10  f0tr walker entry  (FUN_08e87e10)
 *   0x08E88DE0  inner scorer entry (FUN_08e88de0) — current_slot tracker
 *
 * Leaf node layout (from the retired hook, validated 3366/3366):
 *   leaf_mean = float at retval + 0x10
 *   leaf_var  = float at retval + 0x14
 *
 * Output type stays "cart_walk" (singular) to match downstream parsers.
 */

var ADDR_INNER_SCORER = ptr('0x08E88DE0');
var ADDR_DURT_WALK    = ptr('0x08E87D90');
var ADDR_F0TR_WALK    = ptr('0x08E87E10');

var BATCH_N   = 64;
var TOTAL_CAP = 50000;

var stats = { walks: 0, sent: 0, dropped: 0,
              ptr_invalid: 0, read_errors: 0 };
var batch = [];

/* Track which slot we're scoring. Set on inner-scorer entry, cleared on
 * exit. Walker calls outside the inner scorer (PostScoringAdj, etc.)
 * have stale slot context and are dropped — same discipline as
 * cart_walker_args_hook.js. */
var current_slot    = -1;
var in_inner_scorer = false;

var current_walk = null;   /* { n, tree, slot, phone_idx, questions: [] } */

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
    send({ type: 'cart_walk_batch', samples: batch });
    stats.sent += batch.length;
    batch = [];
}

function start_walk(tree_name, phone_idx) {
    if (!in_inner_scorer) { stats.dropped++; current_walk = null; return; }
    if (stats.walks >= TOTAL_CAP) { stats.dropped++; current_walk = null; return; }
    current_walk = {
        n         : stats.walks + 1,
        tree      : tree_name,
        slot      : current_slot,
        phone_idx : phone_idx,
        questions : []
    };
}

function end_walk(retval) {
    if (!current_walk) return;
    stats.walks++;
    var rec = current_walk;
    current_walk = null;

    var mean = safeReadF32(retval.add(0x10));
    var v    = safeReadF32(retval.add(0x14));
    rec.leaf_mean = mean;
    rec.leaf_var  = v;

    batch.push(rec);
    if (batch.length >= BATCH_N) flush();
}

/* ------------------------------------------------------------------ */
/* Inner scorer: track current slot                                    */
/* ------------------------------------------------------------------ */
Interceptor.attach(ADDR_INNER_SCORER, {
    onEnter: function () {
        var slotV = safeReadU32(this.context.esp.add(8));
        if (slotV !== null && slotV < 4096) current_slot = slotV >>> 0;
        in_inner_scorer = true;
    },
    onLeave: function () {
        in_inner_scorer = false;
        /* Defensive: if a walk was somehow opened without a leave we
         * abandon it rather than emit a half-record. */
        current_walk = null;
    }
});

/* ------------------------------------------------------------------ */
/* durt walker entry / leave                                           */
/* ------------------------------------------------------------------ */
Interceptor.attach(ADDR_DURT_WALK, {
    onEnter: function () {
        /* phone_idx selects which of 47 trees; lives in EDX per the
         * retired hook's empirical confirmation 2026-05-05. */
        var phone_idx = this.context.edx.toUInt32() >>> 0;
        start_walk('durt', phone_idx);
    },
    onLeave: function (retval) {
        end_walk(retval);
    }
});

/* ------------------------------------------------------------------ */
/* f0tr walker entry / leave                                           */
/* ------------------------------------------------------------------ */
Interceptor.attach(ADDR_F0TR_WALK, {
    onEnter: function () {
        /* f0tr has only one tree; no phone_idx. */
        start_walk('f0tr', null);
    },
    onLeave: function (retval) {
        end_walk(retval);
    }
});

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */
rpc.exports = {
    stats: function () { return stats; },
    flush: function () { flush(); return stats; },
    reset: function () {
        flush();
        stats = { walks: 0, sent: 0, dropped: 0,
                  ptr_invalid: 0, read_errors: 0 };
        current_slot    = -1;
        in_inner_scorer = false;
        current_walk    = null;
    }
};

send({ type: 'ready', hook: 'cart_walks_safe',
       batch_n: BATCH_N, cap: TOTAL_CAP,
       inner_scorer: ADDR_INNER_SCORER.toString(),
       durt_walker : ADDR_DURT_WALK.toString(),
       f0tr_walker : ADDR_F0TR_WALK.toString() });
