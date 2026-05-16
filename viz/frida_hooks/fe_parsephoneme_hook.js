'use strict';
/*
 * fe_parsephoneme_hook.js -- capture every phoneme NAME the engine
 * receives via ESPRparser::parsePhoneme (in SWIttsEngine.dll).
 *
 * The FE-USel boundary is text-based ESPR (Enhanced Speechify Phonetic
 * Representation). Each phoneme is encoded as `NAME(p<float>)` -- e.g.
 * `T(p0.5)`, `AA(p1.2)`. By hooking parsePhoneme we observe exactly
 * which NAMEs the engine accepts: the voice-specific phoneme inventory.
 *
 * Module: SWIttsEngine.dll
 * Function: ESPRparser::parsePhoneme @ 0x06b0b1f0  (RVA 0xb1f0)
 *
 * On x86-32, __fastcall puts the `this` pointer (parser state) in ECX.
 * Layout of parser state (from Ghidra):
 *   state[+0x00]: log handle
 *   state[+0x0c]: input cursor (char *)
 *   state[+0x14]: working string buffer (param_1[5])
 *
 * On entry, state[+0x0c] points at the start of the phoneme name (after
 * any leading whitespace -- but parsePhoneme also skips whitespace
 * itself before accumulating). We walk forward in memory until we hit
 * `(` (start of `(p...)` parameter), `]` (end of sylphones), `)` (in
 * f0 / pause), `<`, `>`, `.`, `;`, ` `, NUL, or any non-ASCII byte.
 * The chars collected are the phoneme NAME.
 *
 * Output: a `phon` event per parse, batched.
 */

var MODULE_NAME = 'SWIttsEngine.dll';
var PARSEPHONEME_RVA = 0xb1f0;

var BATCH_N = 32;
var stats = { calls: 0, sent: 0, errors: 0 };
var batch = [];
var hooked = false;

function flush() {
    if (batch.length === 0) return;
    send({ type: 'parsephon_batch', items: batch });
    stats.sent += batch.length;
    batch = [];
}

function isReadable(addr_u32, n) {
    if (addr_u32 < 0x10000) return false;
    var r;
    try { r = Process.findRangeByAddress(ptr(addr_u32)); } catch (e) { return false; }
    if (!r) return false;
    if (r.protection.charAt(0) !== 'r') return false;
    var end = ptr(addr_u32).add(n);
    var rend = r.base.add(r.size);
    return end.compare(rend) <= 0;
}

function read_phoneme_name(cursor_addr) {
    if (!isReadable(cursor_addr, 1)) return null;
    var p = cursor_addr;
    /* skip leading whitespace ourselves -- mirrors parsePhoneme entry. */
    while (true) {
        if (!isReadable(p, 1)) return null;
        var c = ptr(p).readU8();
        if (c === 0x20 || c === 0x09 || c === 0x0A || c === 0x0D) {
            p = p + 1; continue;
        }
        break;
    }
    var name = '';
    var max = 16;
    while (max-- > 0) {
        if (!isReadable(p, 1)) break;
        var c = ptr(p).readU8();
        if (c === 0) break;
        if (c === 0x28 /* ( */ || c === 0x5b /* [ */ || c === 0x5d /* ] */ ||
            c === 0x29 /* ) */ || c === 0x3c /* < */ || c === 0x3e /* > */ ||
            c === 0x2e /* . */ || c === 0x3b /* ; */ || c === 0x2c /* , */ ||
            c === 0x20 /* sp */ || c === 0x09 || c === 0x0A || c === 0x0D) break;
        if (c < 0x20 || c >= 0x7f) break;
        name += String.fromCharCode(c);
        p = p + 1;
    }
    return name.length > 0 ? name : null;
}

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

/* Track each unique parser-state pointer so we can emit one
 * 'espr_input' event per ESPR string seen. */
var seen_states = {};

function read_full_input(state, max_len) {
    /* state[+0x08] = ESPR input start. Walk forward up to max_len
     * looking for end-of-input (the engine sets state[+0x10] = len). */
    try {
        var input_start = ptr(state + 0x08).readU32();
        var input_len   = ptr(state + 0x10).readU32();
        if (input_len === 0 || input_len > 16384) return null;
        if (!isReadable(input_start, input_len)) return null;
        return ptr(input_start).readUtf8String(input_len);
    } catch (e) { return null; }
}

function install_hook(base) {
    if (hooked) return;
    var addr = base.add(PARSEPHONEME_RVA);
    Interceptor.attach(addr, {
        onEnter: function (args) {
            stats.calls++;
            try {
                /* fastcall: this in ECX. */
                var ecx = this.context.ecx;
                var state = ecx.toUInt32();

                /* On first parse for this state pointer, emit the full
                 * ESPR input string + diagnostic. */
                if (!(state in seen_states)) {
                    seen_states[state] = true;
                    var diag = { type: 'state_diag',
                                 state: '0x' + state.toString(16) };
                    try { diag.field_08 = '0x' + ptr(state + 0x08).readU32().toString(16); } catch (e) { diag.field_08 = 'read_err'; }
                    try { diag.field_0c = '0x' + ptr(state + 0x0c).readU32().toString(16); } catch (e) { diag.field_0c = 'read_err'; }
                    try { diag.field_10 = ptr(state + 0x10).readU32(); } catch (e) { diag.field_10 = -1; }
                    send(diag);
                    var full = read_full_input(state, 0);
                    if (full) {
                        send({ type: 'espr_input',
                               state: '0x' + state.toString(16),
                               text: full });
                    }
                }

                var cursor_addr = ptr(state + 0x0c).readU32();
                var name = read_phoneme_name(cursor_addr);
                if (name) {
                    batch.push({ name: name });
                    if (batch.length >= BATCH_N) flush();
                } else {
                    stats.errors++;
                }
            } catch (e) {
                stats.errors++;
            }
        }
    });
    hooked = true;
    send({ type: 'ready', hook: 'parsephoneme',
           module: MODULE_NAME, addr: '0x' + addr.toString(16) });
}

function list_modules() {
    try {
        if (typeof Process.enumerateModules === 'function') {
            return Process.enumerateModules().map(function (m) { return m.name; });
        }
    } catch (e) {}
    return [];
}

/* Boot signal so the host knows the hook ran. */
send({ type: 'boot',
       initial_modules: list_modules(),
       initial_target_loaded: find_module_base(MODULE_NAME) !== null });

/* Wait for the engine DLL to be loaded. */
function wait_and_install() {
    var b = find_module_base(MODULE_NAME);
    if (b !== null) { install_hook(b); return; }
    var attempts = 0;
    var iv = setInterval(function () {
        attempts++;
        var bb = find_module_base(MODULE_NAME);
        if (bb !== null) {
            clearInterval(iv);
            send({ type: 'load_seen',
                   attempts: attempts,
                   modules_now: list_modules() });
            install_hook(bb);
            return;
        }
        if (attempts > 800) {
            clearInterval(iv);
            send({ type: 'load_timeout',
                   attempts: attempts,
                   modules_at_timeout: list_modules() });
        }
    }, 25);
}
wait_and_install();

/* Hook ExitProcess to flush before detach. */
try {
    var ep = (typeof Module.getGlobalExportByName === 'function')
              ? Module.getGlobalExportByName('ExitProcess')
              : Module.findExportByName(null, 'ExitProcess');
    if (ep) {
        Interceptor.attach(ep, {
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
