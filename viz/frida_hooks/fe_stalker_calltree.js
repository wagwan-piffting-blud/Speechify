'use strict';
/*
 * fe_stalker_calltree.js — full Stalker trace of FE function calls during
 * the runtime portion of synthesis (after FE init completes).
 *
 * Strategy:
 *   1. Track init phase via FUN_08391d20 call counter (726 init iterations
 *      = 553 reg-A + 173 reg-B). When that count is reached, init is done.
 *   2. After init_done, Stalker.follow the executing thread with
 *      events.call=true. Each call within SWIttsFe-en-US.dll is recorded.
 *   3. Stop tracing on detach OR after EVENT_CAP unique function-target
 *      addresses (whichever comes first).
 *
 * Output per unique target: { rva, hits, first_caller_rva }
 *   - rva: callee's offset within SWIttsFe-en-US.dll
 *   - hits: how many times we saw it called
 *   - first_caller_rva: the caller's RVA the first time we saw it (helps
 *     reconstruct the call graph rooted at the synthesis entry point)
 *
 * RPC:
 *   start()    : explicit start of Stalker (auto-starts at init_done too)
 *   stop()     : explicit stop
 *   summary()  : current call summary (rva -> {hits, first_caller_rva})
 *   drain()    : returns + clears the summary
 *
 * NOTE: Stalker is heavyweight. We exclude all non-FE modules to keep
 * volume manageable. Even so, expect >100k call events per synthesis.
 */

var TARGET_MODULE = 'SWIttsFe-en-US.dll';
var RVA_INIT1     = 0x0059835a;   /* FUN_0836835a */
var RVA_DISPATCH  = 0x005c1d20;   /* FUN_08391d20 */
var INIT_THRESHOLD = 553 + 173;
var EVENT_CAP = 100000;            /* per-stalk run, max unique calls */

var fe_base = null, fe_size = 0;
var stalking = false;
var follow_tid = null;
var init_calls = 0;
var init_done = false;
var summary = {};                  /* "rva" -> { hits, first_caller_rva } */
var total_call_events = 0;

function moduleBase() {
    var m = Process.findModuleByName(TARGET_MODULE);
    if (!m) return null;
    fe_base = m.base.toUInt32();
    fe_size = m.size;
    return m.base;
}

function inFe(addr_u32) {
    return addr_u32 >= fe_base && addr_u32 < fe_base + fe_size;
}

function startStalk(tid) {
    if (stalking) return;
    follow_tid = tid;
    stalking = true;
    summary = {};
    total_call_events = 0;
    /* Exclude only the heaviest modules (ntdll, kernel) to keep volume
     * tractable while still catching cross-module calls into our target. */
    var heavy = ['ntdll.dll', 'KERNEL32.DLL', 'KERNELBASE.dll',
                 'msvcrt.dll', 'MSVCR71.dll', 'ucrtbase.dll',
                 'apphelp.dll', 'sechost.dll', 'RPCRT4.dll', 'WS2_32.dll',
                 'msvcp_win.dll', 'wintypes.dll', 'win32u.dll',
                 'gdi32full.dll', 'GDI32.dll', 'USER32.dll', 'SHELL32.dll',
                 'SHFOLDER.dll', 'ADVAPI32.dll'];
    var mods = Process.enumerateModules();
    for (var i = 0; i < mods.length; ++i) {
        var m = mods[i];
        if (heavy.indexOf(m.name) >= 0) {
            try { Stalker.exclude({ base: m.base, size: m.size }); }
            catch (e) {}
        }
    }
    try {
        Stalker.follow(tid, {
            events: { call: true, ret: false, exec: false, block: false },
            onReceive: function (events) {
                if (Object.keys(summary).length >= EVENT_CAP) return;
                var arr;
                try { arr = Stalker.parse(events); }
                catch (e) { return; }
                for (var j = 0; j < arr.length; ++j) {
                    var e = arr[j];
                    if (e[0] !== 'call') continue;
                    var caller = e[1].toUInt32();
                    var target = e[2].toUInt32();
                    if (!inFe(target)) continue;
                    total_call_events++;
                    var rva = (target - fe_base).toString(16);
                    if (summary[rva]) {
                        summary[rva].hits++;
                    } else {
                        summary[rva] = {
                            hits: 1,
                            first_caller_rva: inFe(caller)
                                ? (caller - fe_base).toString(16)
                                : 'ext:' + caller.toString(16),
                        };
                        if (Object.keys(summary).length <= 20 ||
                            Object.keys(summary).length % 50 === 0) {
                            send({ type: 'fe_call_first',
                                   rva: rva,
                                   first_caller_rva:
                                       summary[rva].first_caller_rva,
                                   unique_count: Object.keys(summary).length });
                        }
                    }
                }
            },
        });
        send({ type: 'stalker_started', thread_id: tid });
    } catch (e) {
        send({ type: 'error', where: 'Stalker.follow',
               message: e.toString() });
        stalking = false;
    }
}

function stopStalk() {
    if (!stalking) return;
    try { Stalker.unfollow(follow_tid); } catch (e) {}
    stalking = false;
    send({ type: 'stalker_stopped',
           unique_targets: Object.keys(summary).length,
           total_call_events: total_call_events });
}

var followed_tids = [];

function startStalkAllThreads() {
    var threads = Process.enumerateThreads();
    for (var i = 0; i < threads.length; ++i) {
        var tid = threads[i].id;
        if (followed_tids.indexOf(tid) >= 0) continue;
        startStalk(tid);
        followed_tids.push(tid);
        /* Reset so each new follow appends to the existing summary. */
        stalking = false;
    }
    stalking = followed_tids.length > 0;
    return followed_tids;
}

rpc.exports = {
    start: function () {
        var tid = Process.getCurrentThreadId();
        startStalk(tid);
        return { stalking: stalking, tid: tid };
    },
    startAll: function () {
        return startStalkAllThreads();
    },
    stop: function () { stopStalk(); return { stalking: stalking }; },
    summary: function () { return summary; },
    drain: function () {
        var s = summary;
        summary = {};
        return s;
    },
    reset: function () {
        stopStalk();
        init_calls = 0;
        init_done = false;
        return 'reset';
    },
    flush: function () { return Object.keys(summary).length; },
};

(function () {
    var base = moduleBase();
    if (!base) {
        send({ type: 'error', message: 'no module ' + TARGET_MODULE });
        return;
    }
    send({ type: 'ready', module_base: base.toString(),
           module_size: fe_size });

    /* Hook FUN_08391d20 to count init iterations and auto-start
     * Stalker once init is complete. */
    var addr_dispatch = base.add(RVA_DISPATCH);
    Interceptor.attach(addr_dispatch, {
        onEnter: function () {
            init_calls++;
            if (!init_done && init_calls >= INIT_THRESHOLD) {
                init_done = true;
                send({ type: 'init_done', init_calls: init_calls });
                /* Start Stalker on THIS thread (the one running FE init). */
                startStalk(Process.getCurrentThreadId());
            }
        },
    });
})();
