'use strict';
/*
 * anchor_cost4_hook.js -- capture engine's per-cand cost4 (4-cell ccos)
 * AND the surviving-after-histogram-prune set, for v7 ground-truth diff.
 *
 * Target: FUN_08e8adc0 @ 0x08E8ADC0 in SWIttsUSel.dll
 *         The histogram-pruner / 4-cell-ccos scorer.
 *         Fills caller-supplied cost & pid arrays in-place, returns
 *         survivor count in EAX.
 *
 * Calling convention: __fastcall (ECX = param_1=last_hp, EDX = iVar6).
 *   [esp+ 4] log_ctx
 *   [esp+ 8] USelNet ptr
 *   [esp+12] cost_arr  <-- target: per-cand float cost4 array (afStack_13880)
 *   [esp+16] pid_arr   <-- target: per-cand pid array (local_9c40)
 *   [esp+20] anchor_type (2=Syl, 4=Word)
 *   [esp+24] slot_idx
 *   [esp+28] first_hp
 *
 * Returns (EAX): number of surviving cands AFTER histogram pruning. Survivors'
 * (cost4, pid) live in cost_arr[0..eax-1], pid_arr[0..eax-1].
 *
 * Why this is enough to settle v7 cases text_004/009/018/022/024:
 *   - We get engine's authoritative cost4 per-survivor.
 *   - Cross-reference with our computed cost4 via pid -> (ss, se).
 *   - If our cost4 disagrees, the bug is in compute_4cell_ccos.
 *   - If cost4 matches but engine kept a cand we pruned, the bug is the
 *     histogram-prune threshold (already RE-fixed once; could be more).
 *
 * Safety: function-boundary only; reads only from caller-provided arrays
 * via the saved [esp+12]/[esp+16] addresses.
 */

var ADDR_HIST_PRUNE = ptr('0x08E8ADC0');

var BATCH_N   = 1;
var TOTAL_CAP = 5000;
var MAX_DUMP  = 200;       /* per-call survivor cap (in case of huge slot) */

var stats = { calls: 0, sent: 0, dropped: 0, ptr_invalid: 0, read_errors: 0 };
var batch = [];

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
    send({ type: 'anchor_cost4_batch', samples: batch });
    stats.sent += batch.length;
    batch = [];
}

rpc.exports = {
    flush: function () { flush(); return stats; },
    drain: function () { flush(); return stats; },
    reset: function () { stats.calls = 0; stats.dropped = 0; batch = []; }
};

Interceptor.attach(ADDR_HIST_PRUNE, {
    onEnter: function () {
        stats.calls++;
        if (stats.calls > TOTAL_CAP) { stats.dropped++; return; }

        var esp = this.context.esp;
        /* __fastcall: ECX=last_hp, EDX=iVar6, then stack from [esp+4]. */
        var last_hp     = this.context.ecx.toUInt32();
        var ivar6       = this.context.edx.toUInt32();
        var p_log       = safeReadU32(esp.add(4));
        var p_net       = safeReadU32(esp.add(8));
        var p_costs     = safeReadU32(esp.add(12));
        var p_pids      = safeReadU32(esp.add(16));
        var p_type      = safeReadU32(esp.add(20));
        var p_slot_idx  = safeReadU32(esp.add(24));
        var p_first_hp  = safeReadU32(esp.add(28));

        this.snap = {
            n: stats.calls,
            type: (p_type !== null) ? (p_type >>> 0) : null,
            first_hp: (p_first_hp !== null) ? (p_first_hp >>> 0) : null,
            last_hp: last_hp,
            slot_idx: (p_slot_idx !== null) ? (p_slot_idx >>> 0) : null,
            ivar6: ivar6,
            net_ptr: (p_net !== null) ? (p_net >>> 0) : null
        };
        this.costs_addr = p_costs;
        this.pids_addr  = p_pids;
    },
    onLeave: function (retval) {
        if (this.snap === undefined) return;
        var n_surv = retval.toUInt32();
        this.snap.n_survivors = n_surv;

        if (this.costs_addr && this.costs_addr >= 0x100000 &&
            this.pids_addr && this.pids_addr >= 0x100000 && n_surv > 0) {
            var cap = (n_surv > MAX_DUMP) ? MAX_DUMP : n_surv;
            var costs_p = ptr(this.costs_addr);
            var pids_p  = ptr(this.pids_addr);
            var pairs = [];
            for (var i = 0; i < cap; ++i) {
                var c = safeReadF32(costs_p.add(i * 4));
                var p = safeReadU32(pids_p.add(i * 4));
                pairs.push([c, p]);
            }
            this.snap.surv_cost_pid = pairs;
            this.snap.dump_capped = (n_surv > MAX_DUMP);
        }

        batch.push(this.snap);
        if (batch.length >= BATCH_N) flush();
    }
});

send({ type: 'ready', target: 'FUN_08e8adc0', addr: '0x08E8ADC0' });
