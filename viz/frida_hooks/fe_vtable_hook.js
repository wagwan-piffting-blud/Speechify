'use strict';
/*
 * fe_vtable_hook.js  -- FE F1 vtable surface tracer (2026-05-06)
 *
 * Purpose: capture which methods on the SWIttsFe-en-US plugin vtable the
 * engine actually calls during synthesis, with timing and args. Used to
 * narrow the ECI API down to the methods we must re-implement for the
 * port.
 *
 * Strategy:
 *   1. Hook SWIttsFe-en-US!getObject to grab the returned plugin pointer.
 *   2. Read its vtable pointer; dump the first MAX_SLOTS entries on first
 *      capture (so we have a static map of slot -> function ptr).
 *   3. Attach a per-slot trampoline that logs (slot_index, args[0..3])
 *      to the host. Throttled by BATCH_N to keep IPC overhead bounded.
 *
 * Anchor (from Ghidra):
 *   ADDR_GET_OBJECT  = 0x0836bd70 (export Ordinal_1)
 *   VTABLE_LITERAL   = 0x08395980 (PTR_LAB_08395980, in .rdata)
 *   Module image base is computed from getObject's address minus its RVA.
 *
 * We compute slot count by reading sequential 4-byte function pointers
 * until one falls outside the .text segment (07dd1000-08392fff).
 *
 * Usage:
 *   frida -p $(pgrep Speechify.exe) -l fe_vtable_hook.js \
 *     -o build/fe_vtable_trace.jsonl
 *
 *   send `dump_vtable` rpc to grab the static map without tracing.
 */

var MODULE_NAME      = 'SWIttsFe-en-US.dll';
var GET_OBJECT_RVA   = 0x59bd70;        // 0x0836bd70 - module_base 0x07dd0000

var TEXT_LO = 0x07dd1000;
var TEXT_HI = 0x08392fff;

var MAX_SLOTS = 256;
var BATCH_N   = 64;
var stats     = { utts: 0, calls: 0, sent: 0, dropped: 0, slots: 0 };
var batch     = [];
var fe_obj    = null;
var vtable    = null;
var slot_addrs = [];

function flush() {
    if (batch.length === 0) return;
    send({ type: 'fe_vt_batch', items: batch });
    stats.sent += batch.length;
    batch = [];
}

function dump_vtable_static(vt_ptr) {
    var entries = [];
    for (var i = 0; i < MAX_SLOTS; ++i) {
        var p = null;
        try { p = vt_ptr.add(i * 4).readU32(); } catch (e) { break; }
        if (p === null) break;
        if (p < TEXT_LO || p > TEXT_HI) break;        /* end of vtable */
        entries.push({ slot: i, fn: '0x' + p.toString(16) });
    }
    return entries;
}

function attach_slot(slot_idx, fn_ptr) {
    try {
        Interceptor.attach(ptr(fn_ptr), {
            onEnter: function (args) {
                stats.calls++;
                if (batch.length >= BATCH_N) flush();
                /* args[0] is `this` (FE plugin); args[1..3] are method args.
                 * Read raw u32; the consumer interprets per-slot.
                 * Slot args[1]/[2] may be string ptrs -- try reading as a
                 * NUL-terminated ASCII string with a length cap. */
                var a1 = null, a2 = null, a3 = null;
                try { a1 = args[1].toUInt32(); } catch (e) {}
                try { a2 = args[2].toUInt32(); } catch (e) {}
                try { a3 = args[3].toUInt32(); } catch (e) {}
                var s1 = null, s2 = null;
                if (a1 && a1 > 0x10000) {
                    try { s1 = ptr(a1).readUtf8String(64); } catch (e) {}
                }
                if (a2 && a2 > 0x10000) {
                    try { s2 = ptr(a2).readUtf8String(64); } catch (e) {}
                }
                batch.push({
                    slot: slot_idx,
                    a1: a1, a2: a2, a3: a3,
                    s1: s1, s2: s2,
                });
            }
        });
    } catch (e) {
        /* Some slots may be no-ops or thunks that error on attach;
         * skip silently. */
    }
}

/* Module API moved between Frida 16 and 17. Try the new API first
 * (Process.findModuleByName -> Module.base), fall back to the legacy
 * Module.findBaseAddress for older runtimes. */
function find_module_base(name) {
    try {
        if (typeof Process.findModuleByName === 'function') {
            var m = Process.findModuleByName(name);
            return m ? m.base : null;
        }
    } catch (e) {}
    try {
        if (typeof Module.findBaseAddress === 'function') {
            return Module.findBaseAddress(name);
        }
    } catch (e) {}
    return null;
}

function install_hooks(mod_base) {
    var get_obj_addr = mod_base.add(GET_OBJECT_RVA);
    Interceptor.attach(get_obj_addr, {
        onEnter: function (args) {
            this.out_pp = args[1];     /* second arg is `int **out` */
        },
        onLeave: function (rv) {
            stats.utts++;
            if (fe_obj !== null) return;        /* only first call sets up */
            try {
                var pp = ptr(this.out_pp.toUInt32()).readU32();
                if (pp === 0) return;
                fe_obj = ptr(pp);
                vtable = ptr(fe_obj.readU32());
                var entries = dump_vtable_static(vtable);
                stats.slots = entries.length;
                send({ type: 'fe_vtable_dump',
                       fe_obj: '0x' + fe_obj.toUInt32().toString(16),
                       vtable: '0x' + vtable.toUInt32().toString(16),
                       entries: entries });
                for (var i = 0; i < entries.length; ++i) {
                    slot_addrs.push(entries[i].fn);
                    attach_slot(entries[i].slot, parseInt(entries[i].fn, 16));
                }
            } catch (e) {
                console.error('[fe_vtable] setup error: ' + e);
            }
        }
    });
    send({ type: 'ready', hook: 'fe_vtable',
           module: MODULE_NAME, batch_n: BATCH_N,
           mod_base: '0x' + mod_base.toString(16) });
}

/* Diagnostics: send a "boot" message immediately so the host knows the
 * script ran. Also list every loaded module name so we can see if the
 * FE DLL is in fact loaded under a different filename. */
function list_modules() {
    try {
        if (typeof Process.enumerateModules === 'function') {
            return Process.enumerateModules().map(function (m) {
                return m.name;
            });
        }
    } catch (e) {}
    return [];
}
send({ type: 'boot',
       has_findModuleByName: (typeof Process.findModuleByName === 'function'),
       has_findBaseAddress:  (typeof Module.findBaseAddress === 'function'),
       initial_mod: (function () {
            var m = find_module_base(MODULE_NAME);
            return m ? '0x' + m.toString(16) : null;
       })(),
       modules: list_modules() });

/* Hook LoadLibrary* to detect ALL DLL loads (we'll see the full
 * sequence even if the FE comes in via a static-import chain from
 * another module loaded at runtime). */
function hook_loadlibrary() {
    var loaders = ['LoadLibraryExW', 'LoadLibraryW',
                   'LoadLibraryExA', 'LoadLibraryA'];
    var hooked = 0;
    loaders.forEach(function (nm) {
        var p = null;
        try {
            if (typeof Module.getGlobalExportByName === 'function') {
                p = Module.getGlobalExportByName(nm);
            } else if (typeof Module.findExportByName === 'function') {
                p = Module.findExportByName(null, nm);
            }
        } catch (e) { p = null; }
        if (!p) return;
        Interceptor.attach(p, {
            onEnter: function (args) {
                this.is_w = nm.endsWith('W');
                try {
                    this.name = this.is_w
                        ? args[0].readUtf16String()
                        : args[0].readUtf8String();
                } catch (e) { this.name = null; }
            },
            onLeave: function (rv) {
                if (!this.name) return;
                send({ type: 'load_seen', loader: nm, name: this.name,
                       handle: '0x' + rv.toUInt32().toString(16) });
                /* If the FE just loaded, install hooks immediately. */
                if (this.name.toLowerCase().indexOf('swittsfe-en-us') >= 0) {
                    var m = find_module_base(MODULE_NAME);
                    if (m !== null && fe_obj === null) install_hooks(m);
                }
            }
        });
        hooked++;
    });
    return hooked;
}
var n_loader_hooks = hook_loadlibrary();
send({ type: 'loader_hooks_installed', n: n_loader_hooks });

/* Also poll, in case the DLL was already loaded before script attached. */
var poll_attempts = 0;
var iv = setInterval(function () {
    poll_attempts++;
    var m = find_module_base(MODULE_NAME);
    if (m !== null && fe_obj === null) {
        install_hooks(m);
    }
    if (poll_attempts > 800 || fe_obj !== null) {
        clearInterval(iv);
    }
}, 25);

/* Auto-flush on process exit so the JSONL is complete before we detach. */
try {
    var exitProc = Module.getGlobalExportByName('ExitProcess');
    if (exitProc) {
        Interceptor.attach(exitProc, {
            onEnter: function () {
                flush();
                send({ type: 'process_exit', stats: stats });
            }
        });
    }
} catch (e) {}

rpc.exports = {
    stats: function () { flush(); return stats; },
    flush: function () { flush(); return stats; },
};
