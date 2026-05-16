'use strict';
/*
 * !! RETIRED 2026-05-05 -- DO NOT ATTACH UNLESS YOU HAVE A NEW STRATEGY !!
 *
 * Why retired: this hook attaches to the per-question dispatcher
 * (0x8E87C90) and compare helper (0x8E87C70) inside the CART scoring
 * loop. These fire many times per (slot, candidate) pair which in turn
 * fires per-halfphone in the Viterbi loop -- effectively a hot-path
 * mid-loop instrumentation under SWIttsUSelUnitSelection. Same
 * x87-FP/EFLAGS perturbation class as hash_lookup_hook.js. Cumulative
 * destabilisation eventually AVs the engine.
 *
 * What we already learned (preserved in code + docs):
 *   1. CART tree walker bit-exact match: 3366/3366 walks across the
 *      30-phrase corpus, validated by spfy_cart_replay against the C
 *      evaluator at src/cart/cart_eval.c.
 *   2. durt walker takes phone_idx in edx (verified empirically).
 *
 * Replacement strategy if more CART data is ever needed: dump the
 * entire f0tr/durt chunks from disk (we already do via VIN loader) and
 * walk in C using captured target features instead of live probes.
 *
 * cart_walks_hook.js -- log every CART tree walk (durt + f0tr).
 *
 * Closes plan gap #2 (M1). Emits one batched 'cart_walk' record per
 * leaf-hit, containing the full question sequence taken plus the final
 * (mean, variance). Compatible with spfy/test/oracle/run_frida_capture.py.
 *
 * Anchors (SWIttsUSel.dll, confirmed via DLL_ANALYSIS.md and
 * cart_accurate_hook.js):
 *   0x08E87C70   ESI-compare helper for q_type {3,4,5,8,9}
 *   0x08E87C90   question dispatcher entry  (q_type {1,2,7} caught here)
 *   0x08E87D90   durt walker
 *   0x08E87E10   f0tr walker
 *   0x08E88DE0   per-slot scorer (gives current slot index)
 *
 * Return-address-to-type table (inside dispatcher) is what disambiguates
 * which q_type a given ESI value belongs to.
 */

var ADDR_COMPARE   = ptr('0x08e87c70');
var ADDR_DISPATCH  = ptr('0x08e87c90');
var ADDR_DURT_WALK = ptr('0x08e87d90');
var ADDR_F0TR_WALK = ptr('0x08e87e10');
var ADDR_SCORER    = ptr('0x08e88de0');

var RETADDR_TO_TYPE = {};
RETADDR_TO_TYPE[ptr('0x08e87d27').toString()] = 8;
RETADDR_TO_TYPE[ptr('0x08e87d3e').toString()] = 3;
RETADDR_TO_TYPE[ptr('0x08e87d55').toString()] = 4;
RETADDR_TO_TYPE[ptr('0x08e87d6c').toString()] = 5;
RETADDR_TO_TYPE[ptr('0x08e87d87').toString()] = 9;

var BATCH_N = 64;
var TOTAL_CAP = 50000;
var stats = { walks: 0, sent: 0, dropped: 0, q_recorded: 0 };
var batch = [];

var current_slot = -1;
var current_walk = null;     /* { tree, slot, questions: [] } */

function tryU32(p) { try { return p.readU32(); } catch (e) { return null; } }
function tryF32(p) { try { return p.readFloat(); } catch (e) { return null; } }

function flush() {
    if (batch.length === 0) return;
    send({ type: 'cart_walk_batch', samples: batch });
    stats.sent += batch.length;
    batch = [];
}

Interceptor.attach(ADDR_SCORER, {
    onEnter: function () {
        var slot = tryU32(this.context.esp.add(0x8));
        if (slot !== null && slot >= 0 && slot < 2048) {
            current_slot = slot;
        }
    }
});

function start_walk(tree_name) {
    current_walk = {
        n: stats.walks + 1,
        tree: tree_name,
        slot: current_slot,
        questions: [],
    };
}

function end_walk(retval) {
    if (!current_walk) return;
    stats.walks++;
    if (stats.walks > TOTAL_CAP) { stats.dropped++; current_walk = null; return; }
    /* leaf at retval+0x10 (mean), retval+0x14 (variance) */
    try {
        current_walk.leaf_mean = tryF32(retval.add(0x10));
        current_walk.leaf_var  = tryF32(retval.add(0x14));
    } catch (e) {
        current_walk.leaf_mean = null;
        current_walk.leaf_var  = null;
    }
    batch.push(current_walk);
    current_walk = null;
    if (batch.length >= BATCH_N) flush();
}

Interceptor.attach(ADDR_DURT_WALK, {
    onEnter: function () {
        start_walk('durt');
        if (current_walk) {
            /* edx holds the phone_idx selecting which of 47 trees to walk.
             * Confirmed empirically (2026-05-05): for "Hello, world." the 8
             * distinct edx values were exactly {10,12,14,19,25,29,32,41} =
             * {d, eh, er, hh, l, ow, pau, w}, matching the expected phones. */
            current_walk.phone_idx = this.context.edx.toUInt32();
        }
    },
    onLeave: function (rv) { end_walk(rv); }
});

Interceptor.attach(ADDR_F0TR_WALK, {
    onEnter: function () { start_walk('f0tr'); },
    onLeave: function (rv) { end_walk(rv); }
});

/* dispatcher entry: catches q_type {1,2,7} that don't call the compare helper */
Interceptor.attach(ADDR_DISPATCH, {
    onEnter: function () {
        if (!current_walk) return;
        var eax = this.context.eax;
        var q_type = tryU32(eax);
        if (q_type === null) return;
        var val = null;
        if (q_type === 1) {
            val = tryU32(this.context.esp.add(0x4));
        } else if (q_type === 2) {
            val = this.context.ecx.toInt32() & 0xffffffff;
        } else if (q_type === 7) {
            val = 0;
        } else {
            return;
        }
        current_walk.questions.push({ q_type: q_type, value: val });
        stats.q_recorded++;
    }
});

/* compare helper: catches q_type {3,4,5,8,9} via return-address table */
Interceptor.attach(ADDR_COMPARE, {
    onEnter: function () {
        if (!current_walk) return;
        var ret_addr = tryU32(this.context.esp);
        if (ret_addr === null) return;
        var q_type = RETADDR_TO_TYPE[ptr(ret_addr).toString()];
        if (q_type === undefined) return;
        var val = this.context.esi.toInt32() & 0xffffffff;
        current_walk.questions.push({ q_type: q_type, value: val });
        stats.q_recorded++;
    }
});

rpc.exports = {
    stats: function () { return stats; },
    flush: function () { flush(); return stats; },
    reset: function () {
        flush();
        stats = { walks: 0, sent: 0, dropped: 0, q_recorded: 0 };
        current_slot = -1;
        current_walk = null;
    },
};

send({ type: 'ready', hook: 'cart_walks', batch_n: BATCH_N, cap: TOTAL_CAP });
