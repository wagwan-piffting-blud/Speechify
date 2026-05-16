'use strict';
/*
 * !! RETIRED 2026-05-05 -- DO NOT ATTACH UNLESS YOU HAVE A NEW STRATEGY !!
 *
 * Why retired: this hook attaches to 0x8E8B7E6 (a `cmp` instruction
 * inside the Viterbi candidate-scoring loop) which fires thousands of
 * times per phrase. Mid-instruction Frida instrumentation in such a hot
 * x87-FP-accumulator loop is unstable on Win32/x86 -- every trampoline
 * trip is a chance to perturb x87 stack state or EFLAGS in ways the
 * engine relies on. Symptom: SWIttsUSelUnitSelection AVs mid-synthesis
 * after a stochastic number of probes (1735 OK once, 512 then crashed).
 *
 * What we already learned from this hook (preserved in code + docs):
 *   1. cells_A stores uid_right_owner (NOT uid_left); cmp [esi+eax*8],ebx
 *      with ebx = uid_right. README_TECHNICAL hash section corrected.
 *   2. C hash loader at src/usel/hash.c -- 1735/1735 probes match
 *      bit-exact under spfy_hash_replay.
 *
 * Replacement strategy if more probe data is ever needed: dump the
 * entire hash chunk (rows + cells_A + cells_B) directly from the engine
 * at startup, OR attach at a function entry that wraps the Viterbi inner
 * loop and capture (uid_left, uid_right) lists in batch instead of
 * per-probe inside the loop.
 *
 * hash_lookup_hook.js -- captures every join-cost hash probe.
 *
 * Closes plan gap #1 (M3 blocker): the engine's COMPRESSED PERFECT HASH
 * lookup formula is documented (`cell[rows[uid_right] + uid_left]`), but the
 * BUILD-time algorithm that picks `rows[uid_right]` so all (left, right)
 * pairs collide-resolve correctly is unknown. Logging every observed probe
 * gives us thousands of (uid_left, row_offset, hit, stored, cost) tuples
 * which we use to fit / verify the build algorithm.
 *
 * Probe site (SWIttsUSel.dll, confirmed 2026-03-16; semantics CORRECTED
 * 2026-05-05 via this hook -- README_TECHNICAL had ebx and the cell key
 * mis-annotated):
 *   0x8E8B7E6:  cmp [esi + eax*8], ebx   ; the perfect-hash probe
 *   0x8E8B7E9:  jne miss
 *   0x8E8B7EB:  fld [esi + eax*8 + 4]    ; HIT: load f32 join cost
 *
 * Corrected register semantics (verified by live capture: eax != ebx on
 * many hits, so they cannot both be uid_left):
 *   eax = uid_left              (offset within the row; from candidate)
 *   esi = cellsBase + rows[uid_right] * 8
 *   ebx = uid_right             (verification key -- which uid_right "owns"
 *                                this cell after suffix-sharing collapse)
 *   edx = candidate struct base; [edx+0x10] = uid_left
 * Cell layout post-load is AoS:  struct Cell { u32 uid_right_owner; f32 cost; }
 * On hit:  cells_A[rows[uid_right] + uid_left] == uid_right
 *          cells_B[rows[uid_right] + uid_left] == precomputed join cost.
 *
 * Output: streamed batched 'hash_probe_batch' send() messages of up to
 * BATCH_N samples each. We do NOT use rpc.exports.drain() because
 * synchronously returning a multi-megabyte ring over the Frida channel
 * stalls under back-pressure -- batched send() messages flush as they're
 * produced and don't block the script.
 */

var ADDR_HASH_PROBE = ptr('0x8E8B7E6');

var BATCH_N = 256;            /* flush every N probes */
var TOTAL_CAP = 200000;       /* safety cap per session; drop after */
var stats = { probes: 0, hits: 0, misses: 0, dropped: 0, sent: 0 };
var batch = [];

function tryU32(addr) { try { return addr.readU32(); } catch (e) { return null; } }
function tryF32(addr) { try { return addr.readFloat(); } catch (e) { return null; } }

function flush() {
    if (batch.length === 0) return;
    send({ type: 'hash_probe_batch', samples: batch });
    stats.sent += batch.length;
    batch = [];
}

Interceptor.attach(ADDR_HASH_PROBE, {
    onEnter: function () {
        stats.probes++;
        if (stats.probes > TOTAL_CAP) { stats.dropped++; return; }

        var ctx = this.context;
        var uid_left  = ctx.eax.toUInt32();
        var uid_right = ctx.ebx.toUInt32();
        var esi       = ctx.esi;

        if (uid_left > 0x40000) { stats.dropped++; return; }
        var cell = esi.add(uid_left * 8);
        var stored = tryU32(cell);
        if (stored === null) { stats.dropped++; return; }

        var hit  = stored === uid_right;
        var cost = hit ? tryF32(cell.add(4)) : null;
        if (hit) stats.hits++; else stats.misses++;

        batch.push({
            n         : stats.probes,
            uid_left  : uid_left,
            uid_right : uid_right,
            esi       : esi.toString(),    /* hex; rows[uid_right]*8 = esi-cellsBase */
            stored    : stored,
            hit       : hit,
            cost      : cost,
        });
        if (batch.length >= BATCH_N) flush();
    }
});

rpc.exports = {
    stats: function () { return stats; },
    flush: function () { flush(); return stats; },
    reset: function () {
        flush();
        stats = { probes: 0, hits: 0, misses: 0, dropped: 0, sent: 0 };
    },
};

send({ type: 'ready', hook: 'hash_lookup', batch_n: BATCH_N, cap: TOTAL_CAP });
