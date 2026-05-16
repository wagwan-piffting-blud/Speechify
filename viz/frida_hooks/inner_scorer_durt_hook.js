'use strict';
/*
 * inner_scorer_durt_hook.js — preselect-time durt CART output capture.
 *
 * Closes plan 02-05 D-17 Path R: the engine evaluates the durt CART
 * INSIDE the inner-scorer (via FUN_08e87d90) at preselect time, BEFORE
 * the synth-time durt CART that cart_walks_hook.js instruments. The
 * Path A diagnostic (2026-05-09) localised the silence-sentinel UID
 * 169578 +2.4321 drift to the D-cost component; spfy's D-cost formula
 * and inputs match the engine bit-exactly EXCEPT for (durt_mean,
 * durt_inv_std), which spfy reads from cart_walks JSONL. If the engine's
 * preselect-time CART output differs from the synth-time output, this
 * hook captures the missing values so spfy_hp_score_test can use the
 * right ones.
 *
 * --- Reverse engineering basis (Ghidra MCP) ---
 *
 * Function:  FUN_08e87d90  @  0x08E87D90  in SWIttsUSel.dll
 *            "InnerScorerDurtCart" — binary CART walker over 0x18-byte
 *            nodes. Leaf detection: *(int*)(node + 0x8) < 0. At leaf,
 *            mean = *(float*)(node + 0x10), inv_std = *(float*)(node + 0x14).
 *
 * Calling convention: __fastcall
 *   eax       = in_EAX (param_1) — voice-side context ptr
 *   edx       = param_2          — node-table base offset
 *   [esp+ 4]  = param_3
 *   [esp+ 8]  = param_4 — feature ptr (slot's durt feature struct)
 *
 * Decompile:
 *   iVar1 = *(int*)(*(int*)(in_EAX + 0x10) + param_2*4);   // root node ptr
 *   iVar2 = *(int*)(iVar1 + 8);                            // node[+0x8]
 *   iVar4 = iVar1;
 *   while (-1 < iVar2) {
 *     iVar3 = FUN_08e87c90(param_4, param_3);              // question
 *     if (iVar3 == 0) { iVar2 = *(int*)(iVar4 + 0xC); }   // no-branch
 *     iVar4 = iVar1 + iVar2 * 0x18;                        // child
 *     iVar2 = *(int*)(iVar4 + 8);
 *   }
 *   return iVar4;                                          // leaf node ptr
 *
 * --- Slot tracking ---
 *
 * FUN_08e87d90 has no slot in its args. We coordinate via a parallel
 * attach on FUN_08e88de0 (the InnerScorer caller) which DOES carry the
 * slot in its esp+8 arg. Each FUN_08e88de0 onEnter sets the module-
 * level current_slot; the next FUN_08e87d90 leave fires within that
 * scope. (FUN_08e88de0 also calls FUN_08e87e10 for f0 CART, which we
 * do NOT instrument — separate hook for f0 if ever needed.)
 *
 * --- Safety ---
 *
 * Function-entry-and-leave attaches only. NO writes to engine memory
 * (the cand_buf debug-flag flip lives in the other hook). Output is
 * the same batched-send pattern as inner_scorer_hook.js.
 */

var ADDR_INNER_SCORER = ptr('0x08E88DE0');
var ADDR_DURT_CART    = ptr('0x08E87D90');

var BATCH_N    = 64;
var TOTAL_CAP  = 50000;
var stats      = { is_calls: 0, durt_calls: 0, sent: 0, dropped: 0,
                   ptr_invalid: 0, read_errors: 0,
                   no_slot: 0, leaf_invalid: 0 };
var batch      = [];

/* Slot tracked across the FUN_08e88de0 ↔ FUN_08e87d90 boundary. */
var current_slot = null;
/* Sequential call counter; useful for utt segmentation if multiple utts
 * appear in one capture (slot drops back to 0 after a higher slot). */
var current_n    = 0;
/* Detect utt boundary the same way prsl_slot/inner_scorer hooks do —
 * track max slot seen; if slot drops back to 0 while we've seen higher,
 * that's utt 1+. */
var utt          = 0;
var max_slot     = -1;

function rangeOK(addr) {
    try {
        var r = Process.findRangeByAddress(addr);
        return r !== null && r.protection.indexOf('r') !== -1;
    } catch(e) { return false; }
}

function safeReadF32(addr) {
    if (!rangeOK(addr)) { stats.ptr_invalid++; return null; }
    try { return addr.readFloat(); }
    catch(e) { stats.read_errors++; return null; }
}

function safeReadU32(addr) {
    if (!rangeOK(addr)) { stats.ptr_invalid++; return null; }
    try { return addr.readU32(); }
    catch(e) { stats.read_errors++; return null; }
}

function flush() {
    if (batch.length === 0) return;
    send({ type: 'inner_scorer_durt_batch', samples: batch });
    stats.sent += batch.length;
    batch = [];
}

/* InnerScorer attach — slot tracking only. Mirrors inner_scorer_hook.js
 * arg-extraction (esp+8 = slot). No memory writes.
 *
 * Clear current_slot on onLeave so post-inner-scorer FUN_08e87d90 calls
 * (synth-time, etc.) don't get mis-attributed to the last seen slot
 * (observed for slot 65 in text_002 — 54 trailing calls before the
 * fix). */
Interceptor.attach(ADDR_INNER_SCORER, {
    onEnter: function () {
        stats.is_calls++;
        var esp  = this.context.esp;
        var slot = safeReadU32(esp.add(8));
        if (slot === null) return;
        slot = slot >>> 0;
        /* Utt boundary: slot drops back to 0 after we've seen higher. */
        if (slot === 0 && max_slot > 0) {
            utt++;
            max_slot = 0;
        } else if (slot > max_slot) {
            max_slot = slot;
        }
        current_slot = slot;
    },
    onLeave: function () {
        current_slot = null;
    }
});

/* DurT CART attach — capture leaf node mean / inv_std. */
Interceptor.attach(ADDR_DURT_CART, {
    onEnter: function () {
        stats.durt_calls++;
        if (stats.durt_calls > TOTAL_CAP) {
            stats.dropped++;
            this.skip = 1;
            return;
        }
        if (current_slot === null) {
            stats.no_slot++;
            this.skip = 1;
            return;
        }
        this.snap_slot = current_slot;
        this.snap_utt  = utt;
        this.snap_n    = ++current_n;
    },
    onLeave: function (retval) {
        if (this.skip) return;
        var leafPtrVal = retval.toUInt32() >>> 0;
        if (leafPtrVal < 0x100000) { stats.leaf_invalid++; return; }
        var leaf = ptr(leafPtrVal);
        var mean    = safeReadF32(leaf.add(0x10));
        var inv_std = safeReadF32(leaf.add(0x14));
        if (mean === null || inv_std === null) {
            stats.leaf_invalid++;
            return;
        }
        batch.push({
            n:         this.snap_n,
            utt:       this.snap_utt,
            slot:      this.snap_slot,
            node_addr: leaf.toString(),
            mean:      mean,
            inv_std:   inv_std
        });
        if (batch.length >= BATCH_N) flush();
    }
});

rpc.exports = {
    stats : function () { return stats; },
    flush : function () { flush(); return stats; },
    reset : function () {
        flush();
        stats = { is_calls: 0, durt_calls: 0, sent: 0, dropped: 0,
                  ptr_invalid: 0, read_errors: 0,
                  no_slot: 0, leaf_invalid: 0 };
        current_slot = null;
        current_n    = 0;
        utt          = 0;
        max_slot     = -1;
    }
};

send({ type: 'ready', hook: 'inner_scorer_durt',
       is_addr:   ADDR_INNER_SCORER.toString(),
       durt_addr: ADDR_DURT_CART.toString(),
       batch_n: BATCH_N, cap: TOTAL_CAP });
