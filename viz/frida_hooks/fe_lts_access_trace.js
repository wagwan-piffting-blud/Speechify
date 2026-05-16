'use strict';
/*
 * fe_lts_access_trace.js — Memory-access trace on the LTS rule index.
 *
 * Goal: identify the compiled-code locations that READ from the runtime
 * phoneme blob (state[+0x28], 18,870 bytes). Each access fault tells us
 * `(reader_code_address, blob_offset)` — the offset reveals which rule
 * is being consulted, and the address pinpoints the dispatch / match /
 * emit functions in Ghidra.
 *
 * Workflow:
 *   1. Hook FUN_0836835a (FE-init call 1) to capture the engine state
 *      pointer. On exit, read state[+0x28] = malloc'd phoneme blob.
 *   2. Enable MemoryAccessMonitor on the blob's page range.
 *   3. Each read triggers an event — log (from_address, accessed_offset).
 *   4. RPC `arm()` re-enables monitoring after a faulted page (because
 *      MemoryAccessMonitor disables a page once it traps on it).
 *
 * Notes:
 *   - Frida's MemoryAccessMonitor uses Windows guard-page tricks. After
 *     a page is faulted-on, normal access resumes for that page until
 *     re-armed. So on a fresh `arm()`, we get fresh events for ALL pages.
 *   - Per-page granularity (0x1000 = 4096 bytes), so the blob spans
 *     `ceil(18870/4096) = 5 pages`.
 *   - Same RPC-driven workflow as our other hooks (fe_token_hook,
 *     anchor_score_hook). Drives via `run_frida_capture.py` driver.
 *
 * Companion: fe_registry_init_hook.js captures registry-B base; this
 * hook is independent but the two could be merged later.
 */

var TARGET_MODULE = 'SWIttsFe-en-US.dll';
/* RVAs derived from absolute Ghidra addresses minus preferred base 0x07dd0000. */
var RVA_INIT1     = 0x0059835a;   /* FUN_0836835a — FE init call 1, allocs state[+0x28] */
var RVA_DISPATCH  = 0x005c1d20;   /* FUN_08391d20 — runtime LTS dispatch */
var BLOB_SIZE = 0x49b6;           /* 18,870 */
var BLOB_OFFSET_IN_STATE = 0x28;

var captured = {
    state_ptr:  null,
    blob_base:  null,
    accesses:   [],
    arm_count:  0,
    init_calls: 0,
    init_done:  false,
};

/* Threshold: registry-A (553) + registry-B (173) = 726 init calls.
 * After this many FUN_08391d20 calls, init is complete; all subsequent
 * events are runtime LTS dispatch. */
var INIT_DISPATCH_THRESHOLD = 553 + 173;

var ACC_LOG_CAP = 4096;

function moduleBase() {
    var m = Process.findModuleByName(TARGET_MODULE);
    return m ? m.base : null;
}

function armMonitor(blob_base, blob_size) {
    captured.arm_count++;
    /* Round to page boundary. Frida's MemoryAccessMonitor wants page-
     * aligned ranges. */
    var page_sz = Process.pageSize;
    var base_addr_u = blob_base.toUInt32();
    var aligned_base = ptr(base_addr_u & ~(page_sz - 1));
    var end_u = base_addr_u + blob_size;
    var aligned_end = ptr((end_u + page_sz - 1) & ~(page_sz - 1));
    var aligned_size = aligned_end.sub(aligned_base).toUInt32();

    var ranges = [{ base: aligned_base, size: aligned_size }];
    try {
        MemoryAccessMonitor.enable(ranges, {
            onAccess: function (details) {
                if (captured.accesses.length >= ACC_LOG_CAP) return;
                var from   = details.from;       /* code addr that did the read */
                var addr   = details.address;    /* accessed memory addr */
                var offset = addr.toUInt32() - blob_base.toUInt32();
                /* Capture the calling code bytes RIGHT NOW (while the
                 * function is still live in memory). Read 32 bytes
                 * surrounding the from address so we can disassemble. */
                var code_bytes = null, code_module = null;
                try {
                    var probe = from.sub(8);
                    var bytes = probe.readByteArray(40);
                    code_bytes = Array.prototype.map.call(
                        new Uint8Array(bytes),
                        function (b) { return b.toString(16).padStart(2,'0'); }
                    ).join(' ');
                } catch (e) { code_bytes = '<unreadable>'; }
                try {
                    var m = Process.findModuleByAddress(from);
                    code_module = m ? (m.name + '+0x' +
                        (from.toUInt32() - m.base.toUInt32()).toString(16))
                        : null;
                } catch (e) {}
                var ev = {
                    type:       captured.init_done ? 'lts_access_runtime'
                                                    : 'lts_access_init',
                    arm:        captured.arm_count,
                    from:       from.toString(),
                    from_module: code_module,
                    code_bytes_around_from: code_bytes,
                    op:         details.operation,
                    offset:     offset,
                    rangeIndex: details.rangeIndex,
                    pageIndex:  details.pageIndex,
                    init_done:  captured.init_done,
                };
                captured.accesses.push(ev);
                send(ev);
            }
        });
        send({ type: 'monitor_armed',
               base:      aligned_base.toString(),
               size:      aligned_size,
               arm_count: captured.arm_count });
    } catch (e) {
        send({ type: 'error', where: 'armMonitor',
               message: e.toString() });
    }
}

function rearm() {
    /* Each synthesis triggers a fresh FE init (state recreated), so we
     * reset the init counters. The blob_base may also change (new heap
     * allocation) — but we only track the most-recent capture; if the
     * old one has been freed, MemoryAccessMonitor will simply not fault
     * on it anymore. */
    captured.init_calls = 0;
    captured.init_done  = false;
    if (captured.blob_base) {
        try { MemoryAccessMonitor.disable(); } catch (e) {}
        armMonitor(ptr(captured.blob_base), BLOB_SIZE);
    }
    return { armed: !!captured.blob_base, count: captured.arm_count };
}

rpc.exports = {
    /* Re-arm monitoring (call between synthesis runs to refresh page
     * permissions). */
    arm:   rearm,
    /* Driver compatibility: reset = re-arm (between corpus entries). */
    reset: function () { rearm(); return captured.arm_count; },
    /* Driver compatibility: flush is a no-op since we send() inline. */
    flush: function () { return captured.accesses.length; },
    /* Drain the accumulated access events (then clear). */
    drain: function () {
        var events = captured.accesses;
        captured.accesses = [];
        return events;
    },
    snapshot: function () {
        return {
            state_ptr: captured.state_ptr,
            blob_base: captured.blob_base,
            arm_count: captured.arm_count,
            n_events:  captured.accesses.length,
        };
    },
};

/* The static phoneme blob source lives at .data RVA 0x617778 (absolute
 * 0x083e7778 with default base). The FE init function memcpys from here
 * to a heap allocation. At runtime, the LTS dispatch may also read the
 * static copy directly OR read via state[+0x54] table pointers (which
 * point to other .data locations). */
var STATIC_PHON_BLOB_RVA = 0x617778;

var BLOB_SIGNATURE =
    '00 00 00 00 00 00 00 00 01 0c 00 00 00 30 00 00 ' +
    '00 ec 4f 02 00 00 00 00 00 00 00 00 00 00';

function scanForBlob() {
    /* Find ALL matches; prefer one outside any loaded module (= heap). */
    var modules = Process.enumerateModules();
    function isInModule(addr) {
        for (var i = 0; i < modules.length; ++i) {
            var m = modules[i];
            var addr_u = addr.toUInt32();
            var base_u = m.base.toUInt32();
            if (addr_u >= base_u && addr_u < base_u + m.size) return true;
        }
        return false;
    }
    var all_matches = [];
    /* Try multiple protection filters. */
    var protections = ['r--', 'rw-', 'rwx', 'r-x'];
    for (var pi = 0; pi < protections.length; ++pi) {
        var ranges = Process.enumerateRanges({
            protection: protections[pi],
            coalesce:   true,
        });
        for (var i = 0; i < ranges.length; ++i) {
            var r = ranges[i];
            if (r.size < BLOB_SIZE || r.size > 0x10000000) continue;
            try {
                var matches = Memory.scanSync(r.base, r.size, BLOB_SIGNATURE);
                for (var j = 0; j < matches.length; ++j) {
                    var a = matches[j].address;
                    all_matches.push({
                        addr:     a.toString(),
                        in_mod:   isInModule(a),
                        prot:     protections[pi],
                    });
                }
            } catch (e) {}
        }
    }
    send({ type: 'scan_results', matches: all_matches });
    /* Return first heap (non-module) match. */
    for (var k = 0; k < all_matches.length; ++k) {
        if (!all_matches[k].in_mod) return ptr(all_matches[k].addr);
    }
    /* Fallback to any match. */
    if (all_matches.length > 0) return ptr(all_matches[0].addr);
    return null;
}

(function () {
    var base = moduleBase();
    if (!base) {
        send({ type: 'error',
               message: 'module ' + TARGET_MODULE + ' not loaded' });
        return;
    }
    var addr_init1 = base.add(RVA_INIT1);
    send({ type: 'ready',
           module_base: base.toString(),
           init1_addr:  addr_init1.toString() });

    Interceptor.attach(addr_init1, {
        onEnter: function (args) {
            this.state_ptr = args[0];
        },
        onLeave: function () {
            try {
                var state = this.state_ptr;
                var blob = state.add(BLOB_OFFSET_IN_STATE).readPointer();
                captured.state_ptr = state.toString();
                captured.blob_base = blob.toString();
                send({ type: 'init_captured',
                       state_ptr: state.toString(),
                       blob_base: blob.toString(),
                       blob_size: BLOB_SIZE });
                armMonitor(blob, BLOB_SIZE);
            } catch (e) {
                send({ type: 'error', where: 'onLeave init1',
                       message: e.toString() });
            }
        }
    });

    /* Direct hook on the runtime LTS dispatch FUN_08391d20.
     * __fastcall: ECX = param_1_00, EDX = param_2_00 (caller_struct).
     * Stack: [esp+4]=param_1 (state ptr), [esp+8]=param_2 (flag).
     * Each call consults a (symbol_id, registry_idx) pair — symbol_id
     * lives at caller_struct+8, registry_idx is in EBX (unaff_EBX in
     * decomp). We capture both. */
    var addr_dispatch = base.add(RVA_DISPATCH);
    Interceptor.attach(addr_dispatch, {
        onEnter: function (args) {
            try {
                captured.init_calls++;
                /* Once we've seen the full 726-call init sequence, flip the
                 * gate AND re-arm MAM so post-init reads trigger fresh
                 * page faults (instead of being absorbed by already-faulted
                 * pages from the init memcpy). */
                if (!captured.init_done &&
                    captured.init_calls >= INIT_DISPATCH_THRESHOLD) {
                    captured.init_done = true;
                    send({ type: 'init_done',
                           init_calls: captured.init_calls });
                    if (captured.blob_base) {
                        try { MemoryAccessMonitor.disable(); } catch (e) {}
                        armMonitor(ptr(captured.blob_base), BLOB_SIZE);
                    }
                }
                /* Don't flood: log only the FIRST few init calls + every call
                 * AFTER init_done. */
                var should_log =
                    captured.init_done ||
                    captured.init_calls <= 4 ||
                    captured.init_calls === INIT_DISPATCH_THRESHOLD;
                if (!should_log) return;

                var ctx = this.context;
                var caller_struct = ctx.edx;
                var ebx = ctx.ebx.toUInt32();
                var symbol_byte = caller_struct.add(8).readU8();
                send({
                    type:         captured.init_done ? 'lts_dispatch_runtime'
                                                      : 'lts_dispatch_init',
                    n:            captured.init_calls,
                    symbol_id:    symbol_byte,
                    registry_idx: ebx,
                    state_ptr:    ctx.esp.add(4).readPointer().toString(),
                    flag:         ctx.esp.add(8).readU8(),
                });
            } catch (e) {
                send({ type: 'error', where: 'lts_dispatch onEnter',
                       message: e.toString() });
            }
        }
    });

    /* Watch the STATIC .data copy of the phoneme blob.
     *
     * The heap copy is the engine's working set — but per testing, after
     * init the runtime LTS dispatch does NOT read the heap copy (state
     * [+0x28]). Instead it likely reads the .data tables via state[+0x54]
     * registry pointers. By watching the .data static copy of the phoneme
     * blob, we catch any code that reads the rule index at runtime —
     * which gives us the dispatch function address. */
    try {
        var static_blob = base.add(STATIC_PHON_BLOB_RVA);
        captured.blob_base = static_blob.toString();
        send({ type: 'monitoring_static_blob',
               blob_base: static_blob.toString(),
               blob_size: BLOB_SIZE });
        armMonitor(static_blob, BLOB_SIZE);
    } catch (e) {
        send({ type: 'error', where: 'static_arm',
               message: e.toString() });
    }
})();
