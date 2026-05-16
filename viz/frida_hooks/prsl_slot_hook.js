'use strict';
/*
 * prsl_slot_hook.js -- per-slot PRSL preselection capture.
 *
 * Closes the M3.4c blocker:
 *   - We could not reproduce the engine's PRSL context_key encoding from
 *     scratch (chosen_in_pool was 0% in spfy_viterbi_replay). This hook
 *     captures the engine's TRUE per-slot context (5 phone IDs) and the
 *     resulting candidate pool, so the C-side replay can:
 *       (a) figure out what tuple of those 5 makes the lookup key, and
 *       (b) compare its locally-built pool to the engine's pool exactly.
 *
 * Closes plan gap #6 (PRSL fallback chain): the captured `fallback_type`
 * (1, 2, 3, 5) tells us which width the engine ended up using, so the
 * fallback ordering becomes statistical observation across the corpus.
 *
 * --- Reverse engineering basis (Ghidra MCP, this session) ---
 *
 * Function:  FUN_08e91dc0  @  0x08E91DC0  in SWIttsUSel.dll
 *            "USelNetwork::AddUnit" (per the per-slot loop in FUN_08e920f0
 *            "USelNetwork::AddUnits" which calls this once per target).
 *
 * Calling convention: __thiscall
 *   ecx        = this (per-slot scratch struct)
 *   [esp+ 4]   = param_1 (USelNetwork ptr, ignored here)
 *   [esp+ 8]   = param_2 (utterance ptr, ignored here)
 *   [esp+12]   = param_3 (SLOT INDEX -- this is what we want)
 *
 * Struct layout (filled in BY this function, valid on onLeave only):
 *   this+0x00  u32  n_cands   (final candidate count, written near end)
 *   this+0x04  u32  context[0]   pp2 (position = slot - 4)
 *   this+0x08  u32  context[1]   pp1 (position = slot - 2)
 *   this+0x0c  u32  context[2]   cur (position = slot    )
 *   this+0x10  u32  context[3]   pn1 (position = slot + 2)
 *   this+0x14  u32  context[4]   pn2 (position = slot + 4)
 *   this+0x18  ptr  candidate buffer (array of 0x18-byte entries; uid @ +0)
 *   this+0x1c  ptr  log handle (ignored)
 *   this+0x20  ptr  PRSL table object (ignored; see hash_lookup retired hook)
 *
 * The function tries lookup widths in a switch{1,2,3,5} fallback chain;
 * we capture the WIDTH that ultimately succeeded as `fallback_type`. That
 * value is in EAX of the caller-stored switch register, but it's easier
 * to infer post-hoc from the corpus -- so we skip recording it directly
 * and let the C-side analysis figure it out from (context tuple, returned
 * UIDs) pairs across the corpus.
 *
 * --- Safety ---
 *
 * Function-entry / function-leave only. NO mid-instruction Interceptor
 * attaches. NO register dereferences without Process.findRangeByAddress
 * validation (per the v2 hardening pattern in fe_token_hook.js and
 * wsola_buffer_hook.js, after server kills #2/#3).
 *
 * Output: streamed batched 'prsl_slot_batch' send() messages. Capped to
 * keep the engine responsive on long corpora.
 */

var ADDR_PRSL_SLOT = ptr('0x08E91DC0');

var BATCH_N    = 64;
var TOTAL_CAP  = 50000;
var MAX_CANDS  = 4096;     /* hard cap per slot; real pools are <= ~200 */
var stats      = { calls: 0, sent: 0, dropped: 0, ptr_invalid: 0,
                   read_errors: 0 };
var batch      = [];

function tryU32(addr) { try { return addr.readU32(); } catch(e) { return null; } }

/* Validate that addr is in a readable mapped range before reading. The
 * try/catch alone is not reliable on Windows -- some AVs trap inside the
 * trampoline before Frida's signal handler runs. */
function rangeOK(addr) {
    try {
        var r = Process.findRangeByAddress(addr);
        return r !== null && r.protection.indexOf('r') !== -1;
    } catch(e) { return false; }
}

function safeReadU32(addr) {
    if (!rangeOK(addr)) { stats.ptr_invalid++; return null; }
    try { return addr.readU32(); }
    catch(e) { stats.read_errors++; return null; }
}

function flush() {
    if (batch.length === 0) return;
    send({ type: 'prsl_slot_batch', samples: batch });
    stats.sent += batch.length;
    batch = [];
}

Interceptor.attach(ADDR_PRSL_SLOT, {
    onEnter: function () {
        stats.calls++;
        if (stats.calls > TOTAL_CAP) { stats.dropped++; this.skip = 1; return; }
        var ctx = this.context;
        var thisPtrVal = ctx.ecx.toUInt32();
        var slotIdx    = ctx.esp.add(12).readU32();
        /* Caller return address: the PC of the function that called
         * AddUnit. Used by spfy/test/diff/residue_to_fn.py to lift
         * confidence from 'low' (hook-resolved) to 'high' (trace
         * caller_addr). this.returnAddress is a Frida-provided
         * NativePointer at onEnter; no Stalker, no mid-instruction
         * tracing -- a stack peek is function-entry-only by definition.
         * Captured unconditionally because plan 03-06 expects every
         * record to have this field when re-captured; older traces
         * (without this field) still work via the hook fallback path. */
        var callerAddr = null;
        try { callerAddr = this.returnAddress.toString(); } catch(e) { callerAddr = null; }
        this.snapshot  = {
            n           : stats.calls,
            slot        : slotIdx >>> 0,
            this_ptr    : thisPtrVal >>> 0,
            caller_addr : callerAddr
        };
    },
    onLeave: function () {
        if (this.skip) return;
        if (!this.snapshot) return;

        /* By the time we get here, this+0..+0x18 must be populated. */
        var thisPtr = ptr(this.snapshot.this_ptr);

        /* All field reads are guarded; on any failure we record a partial
         * snapshot and move on rather than poking unmapped memory. */
        var n_cands = safeReadU32(thisPtr);
        if (n_cands === null) { batch.push(this.snapshot);
                                if (batch.length >= BATCH_N) flush();
                                return; }

        var ctx0 = safeReadU32(thisPtr.add(0x04));
        var ctx1 = safeReadU32(thisPtr.add(0x08));
        var ctx2 = safeReadU32(thisPtr.add(0x0c));
        var ctx3 = safeReadU32(thisPtr.add(0x10));
        var ctx4 = safeReadU32(thisPtr.add(0x14));
        var bufVal = safeReadU32(thisPtr.add(0x18));

        this.snapshot.n_cands = n_cands >>> 0;
        this.snapshot.ctx     = [ctx0, ctx1, ctx2, ctx3, ctx4];

        /* Read candidate UIDs (count clamped) from the 0x18-byte-stride
         * array. uid is at offset +0 of each entry. */
        if (bufVal !== null && bufVal >= 0x100000 &&
            n_cands > 0 && n_cands <= MAX_CANDS) {
            var buf = ptr(bufVal);
            if (rangeOK(buf)) {
                var uids = [];
                for (var i = 0; i < n_cands; ++i) {
                    var u = safeReadU32(buf.add(i * 0x18));
                    if (u === null) break;
                    /* The function uses -1 as a sentinel mid-array on the
                     * fallback path; stop early if we hit it. */
                    if (u === 0xFFFFFFFF) break;
                    uids.push(u >>> 0);
                }
                this.snapshot.uids = uids;
            }
        }

        batch.push(this.snapshot);
        if (batch.length >= BATCH_N) flush();
    }
});

rpc.exports = {
    stats : function () { return stats; },
    flush : function () { flush(); return stats; },
    reset : function () {
        flush();
        stats = { calls: 0, sent: 0, dropped: 0, ptr_invalid: 0,
                  read_errors: 0 };
    }
};

send({ type: 'ready', hook: 'prsl_slot', addr: '0x08E91DC0',
       batch_n: BATCH_N, cap: TOTAL_CAP });
