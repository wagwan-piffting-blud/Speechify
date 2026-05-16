'use strict';
/*
 * fe_vtable_trace.js — capture every FE-vtable call into SWIttsFe-en-US.dll
 * during a Speechify.exe synthesis.
 *
 * Goal: identify the per-utterance call sequence the engine uses against
 * the FE COM object — which slots configure, which carries the input
 * text, which delivers the per-slot output. This closes the Task-4
 * BLOCKED status in host_notes.md by replacing static guesses with the
 * empirical protocol.
 *
 * The 49-slot vtable lives at .rdata VA 0x08395980 (image base
 * 0x07dd0000, ASLR off). Each slot dispatches to a thin wrapper at
 * VA 0x0836cXXX / 0x0836dXXX (see host/vtable_inventory.md). We hook
 * every wrapper on entry + leave.
 *
 * Object layout (verified):
 *   this[+0x0] = vtable ptr
 *   this[+0x4] = refcount
 *   this[+0x8] = state ptr
 *   this[+0xc] = init_flag (u8)
 *   this[+0xd] = err_flag  (u8)
 *
 * State sub-block (state[+0x6c] -> ctrl block):
 *   ctrl[+0x2d4] = delegate-A ptr  (read by slots 9, 10)
 *   ctrl[+0x2dc] = delegate-B ptr  (read by slots 42, 44)
 *
 * Calling convention: every wrapper is __stdcall(this, [args]). Args
 * on stack are at esp+4 onwards (esp+0 is the return address). We
 * snapshot up to 3 user args (after `this`) on entry.
 */

var IMAGE_BASE_EXPECTED = ptr('0x07dd0000');

/* Slot # → { va, name, user_args }. Generated from
 * host/vtable_inventory.md. user_args excludes `this`. */
var SLOTS = [
    { slot:  0, va: '0x0836ca90', name: 'QueryInterface',     u: 2 },
    { slot:  1, va: '0x0836cac0', name: 'AddRef',             u: 0 },
    { slot:  2, va: '0x0836d2e0', name: 'Release',            u: 0 },
    { slot:  3, va: '0x0836cb10', name: 'initStage1',         u: 0 },
    { slot:  4, va: '0x0836cb50', name: 'initStage2',         u: 0 },
    { slot:  5, va: '0x0836cb90', name: 'feedConfigA',        u: 1 },
    { slot:  6, va: '0x0836cbd0', name: 'feedConfigB',        u: 1 },
    { slot:  7, va: '0x0836cc10', name: 'logEvent',           u: 2 },
    { slot:  8, va: '0x0836cc50', name: 'synth',              u: 0 },
    { slot:  9, va: '0x0836cc90', name: 'delegateA_call',     u: 3 },
    { slot: 10, va: '0x0836cce0', name: 'getErrorMessage',    u: 3 },
    { slot: 11, va: '0x0836cd30', name: 'runOrAbort',         u: 1 },
    { slot: 12, va: '0x0836cd60', name: 'notifyEvent',        u: 0 },
    { slot: 13, va: '0x0836cda0', name: 'cancel',             u: 0 },
    { slot: 14, va: '0x0836cde0', name: 'isReady',            u: 0 },
    { slot: 15, va: '0x0836ce20', name: 'predicateA',         u: 1 },
    { slot: 16, va: '0x0836ce60', name: 'predicateB',         u: 1 },
    { slot: 17, va: '0x0836cea0', name: 'predicateC',         u: 2 },
    { slot: 18, va: '0x0836cee0', name: 'setterA',            u: 2 },
    { slot: 19, va: '0x0836cf00', name: 'setPair_A',          u: 2 },
    { slot: 20, va: '0x0836cf60', name: 'setterB',            u: 2 },
    { slot: 21, va: '0x0836cf80', name: 'setterC',            u: 2 },
    { slot: 22, va: '0x0836cfa0', name: 'setPair_D',          u: 2 },
    { slot: 23, va: '0x0836cfc0', name: 'predicateD',         u: 1 },
    { slot: 24, va: '0x0836d000', name: 'predicateD_full',    u: 2 },
    { slot: 25, va: '0x0836d040', name: 'setMode',            u: 1 },
    { slot: 26, va: '0x0836d060', name: 'reset',              u: 0 },
    { slot: 27, va: '0x0836d090', name: 'Dict_load',          u: 1 },
    { slot: 28, va: '0x0836d0a0', name: 'Dict_fileLoad',      u: 1 },
    { slot: 29, va: '0x0836d0b0', name: 'Dict_free',          u: 1 },
    { slot: 30, va: '0x0836d0c0', name: 'Dict_activate',      u: 1 },
    { slot: 31, va: '0x0836d0d0', name: 'Dict_deactByName',   u: 1 },
    { slot: 32, va: '0x0836d0e0', name: 'Dict_deactivate',    u: 1 },
    { slot: 33, va: '0x0836d0f0', name: 'getKind',            u: 1 },
    { slot: 34, va: '0x0836d100', name: 'Dict_update',        u: 1 },
    { slot: 35, va: '0x0836d110', name: 'Dict_findFirst',     u: 1 },
    { slot: 36, va: '0x0836d120', name: 'Dict_findNext',      u: 1 },
    { slot: 37, va: '0x0836d130', name: 'Dict_lookup',        u: 1 },
    { slot: 38, va: '0x0836d140', name: 'Dict_prioLookup',    u: 1 },
    { slot: 39, va: '0x0836cf20', name: 'setPair_B',          u: 2 },
    { slot: 40, va: '0x0836cf40', name: 'setPair_C',          u: 2 },
    { slot: 41, va: '0x0836d150', name: 'setPair_E',          u: 2 },
    { slot: 42, va: '0x0836d170', name: 'delegateB_call',     u: 3 },
    { slot: 43, va: '0x0836d1c0', name: 'setPair_F',          u: 2 },
    { slot: 44, va: '0x0836d1e0', name: 'delegateB_call2',    u: 3 },
    { slot: 45, va: '0x0836d230', name: 'setPair_G',          u: 2 },
    { slot: 46, va: '0x0836d250', name: 'setPair_H',          u: 2 },
    { slot: 47, va: '0x0836d270', name: 'installHookA',       u: 3 },
    { slot: 48, va: '0x0836d290', name: 'installHookB',       u: 3 },
];

/* getObject — the entry that creates an FE COM object. Hooking it
 * gives us a clean "FE session opened" marker. */
var GETOBJECT_VA = ptr('0x0836bd70');

/* FUN_0836c420 — the indirect call inside delegate-A/B dispatch. The
 * caller's first arg is the delegate object pointer (= state[+0x2d4]
 * or state[+0x2dc]). Hooking it captures every delegate invocation
 * with arg0 = the delegate, args 1..3 = whatever the engine is reading
 * or writing through that delegate's vtable. */
var FUN_0836c420_VA = ptr('0x0836c420');

/* ============================================================
 * Per-session bookkeeping
 * ============================================================ */

/* Hard caps so a runaway hook doesn't drown the engine. Adjust if you
 * see clipping in the captures — per-utterance counts should be well
 * under these. */
var PER_SLOT_CAP = 1000;
var DELEGATE_CAP = 5000;
var TOTAL_CAP    = 20000;

var stats = {
    total_calls: 0,
    sent: 0,
    by_slot: {},
    delegate_hits: 0,
    getobject_hits: 0,
    ptr_invalid: 0,
    read_errors: 0,
};

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
function safeReadU8(addr) {
    if (!rangeOK(addr)) { stats.ptr_invalid++; return null; }
    try { return addr.readU8(); }
    catch (e) { stats.read_errors++; return null; }
}
function safeReadStr(addr, max) {
    if (!rangeOK(addr)) return null;
    try { return ptr(addr).readUtf8String(max || 256); }
    catch (e) {
        /* fall back to byte-by-byte ASCII (matches fe_tree_hook.js's
         * trick for engine-allocated strings that confuse readCString) */
        try {
            var chars = [];
            var p = ptr(addr);
            for (var b = 0; b < (max || 64); b++) {
                var c = p.add(b).readU8();
                if (c === 0) break;
                if (c < 32 || c > 126) return null;
                chars.push(c);
            }
            return String.fromCharCode.apply(null, chars);
        } catch (e2) { return null; }
    }
}

function read_this_block(this_ptr) {
    if (!this_ptr || this_ptr < 0x100000) return null;
    var p = ptr(this_ptr);
    return {
        this:     this_ptr >>> 0,
        refcount: safeReadU32(p.add(0x4)),
        state:    safeReadU32(p.add(0x8)),
        init_flag: safeReadU8(p.add(0xc)),
        err_flag:  safeReadU8(p.add(0xd)),
    };
}

function read_state_delegates(state_ptr) {
    /* state[+0x6c] is the ctrl block; ctrl[+0x2d4] / [+0x2dc] are the
     * delegate pointers. If state or ctrl looks invalid, return nulls.
     */
    if (!state_ptr || state_ptr < 0x100000) return null;
    var ctrl_pp = safeReadU32(ptr(state_ptr).add(0x6c));
    if (ctrl_pp === null || ctrl_pp < 0x100000) return null;
    return {
        ctrl:       ctrl_pp >>> 0,
        delegate_A: safeReadU32(ptr(ctrl_pp).add(0x2d4)),
        delegateA_state: safeReadU32(ptr(ctrl_pp).add(0x2d8)),
        delegate_B: safeReadU32(ptr(ctrl_pp).add(0x2dc)),
        delegateB_state: safeReadU32(ptr(ctrl_pp).add(0x2e0)),
    };
}

/* Read up to N user args from the stdcall stack frame. esp+0 is the
 * return addr, esp+4 is `this`, esp+8 onwards are user args. */
function read_args(esp, n) {
    var out = [];
    for (var i = 0; i < n && i < 4; i++) {
        out.push(safeReadU32(esp.add(8 + i * 4)));
    }
    return out;
}

/* Hook every vtable wrapper. */
function install_vtable_hooks() {
    for (var i = 0; i < SLOTS.length; i++) {
        (function (s) {
            stats.by_slot[s.slot] = 0;
            Interceptor.attach(ptr(s.va), {
                onEnter: function () {
                    if (stats.total_calls >= TOTAL_CAP) return;
                    if (stats.by_slot[s.slot] >= PER_SLOT_CAP) return;
                    stats.by_slot[s.slot]++;
                    stats.total_calls++;

                    var esp = this.context.esp;
                    var this_ptr = safeReadU32(esp.add(0x4));
                    var args = read_args(esp, s.u);
                    var obj = read_this_block(this_ptr);
                    var del = obj ? read_state_delegates(obj.state) : null;

                    /* For string-taking slots, try a few interpretations
                     * of arg[0]: as a direct C string, as a struct
                     * pointer where +0 / +4 might hold a string ptr. */
                    var args_str = null;
                    var args_blob = null;
                    if (args.length > 0 && args[0] && args[0] >= 0x100000) {
                        args_str = safeReadStr(args[0]);
                        if (!args_str) {
                            /* Try as a struct: read 4 dwords then string-probe them. */
                            try {
                                var p = ptr(args[0]);
                                var d0 = safeReadU32(p);
                                var d1 = safeReadU32(p.add(4));
                                var d2 = safeReadU32(p.add(8));
                                var d3 = safeReadU32(p.add(12));
                                args_blob = [d0, d1, d2, d3];
                                if (d0 && d0 >= 0x100000) {
                                    var sd0 = safeReadStr(d0);
                                    if (sd0) args_str = "(*arg)[+0] -> " + sd0;
                                }
                                if (!args_str && d1 && d1 >= 0x100000) {
                                    var sd1 = safeReadStr(d1);
                                    if (sd1) args_str = "(*arg)[+4] -> " + sd1;
                                }
                            } catch (e) {}
                        }
                    }

                    /* Track the call site (the engine's caller VA)
                     * so we can correlate slots to engine-side
                     * subsystems. */
                    var caller = null;
                    try { caller = this.returnAddress.toString(); } catch (e) {}

                    this._slot_evt = {
                        type: 'fe_vtable_call',
                        t: Date.now(),
                        slot: s.slot,
                        name: s.name,
                        this: obj ? obj.this : (this_ptr >>> 0),
                        state: obj ? obj.state : null,
                        refcount: obj ? obj.refcount : null,
                        init_flag: obj ? obj.init_flag : null,
                        err_flag: obj ? obj.err_flag : null,
                        ctrl: del ? del.ctrl : null,
                        delegate_A: del ? del.delegate_A : null,
                        delegate_B: del ? del.delegate_B : null,
                        args: args,
                        args_str: args_str,
                        args_blob: args_blob,
                        caller: caller,
                    };
                    /* For slot 42 / 44 (delegate-B reads), we want to
                     * see what GETS WRITTEN INTO the buffer. We need
                     * onLeave to read the buffer. Stash the buf ptr +
                     * max + out_len_ptr. */
                    if (s.slot === 42 || s.slot === 44) {
                        this._buf_ptr     = args[0];
                        this._buf_max     = args[1];
                        this._out_len_ptr = args[2];
                    }
                },
                onLeave: function (retval) {
                    if (!this._slot_evt) return;
                    this._slot_evt.ret = retval.toUInt32();
                    /* Re-read flags post-call so we can see what the
                     * call MUTATED on `this`. */
                    var post = read_this_block(this._slot_evt.this);
                    if (post) {
                        this._slot_evt.post_refcount = post.refcount;
                        this._slot_evt.post_init_flag = post.init_flag;
                        this._slot_evt.post_err_flag = post.err_flag;
                    }
                    var post_del = post ? read_state_delegates(post.state) : null;
                    if (post_del) {
                        this._slot_evt.post_delegate_A = post_del.delegate_A;
                        this._slot_evt.post_delegate_B = post_del.delegate_B;
                    }
                    /* For slot 42 / 44: read out_len + buffer contents. */
                    if (this._buf_ptr) {
                        var out_len = safeReadU32(ptr(this._out_len_ptr));
                        this._slot_evt.out_len = out_len;
                        /* The buffer is engine-allocated and presumably
                         * filled with text or binary data. Read up to
                         * min(out_len, buf_max) bytes; cap at 256 for
                         * volume. */
                        var n = Math.min(out_len || 0, this._buf_max || 0, 2048);
                        if (n > 0) {
                            try {
                                var bytes = ptr(this._buf_ptr).readByteArray(n);
                                this._slot_evt.buf_hex = bytes ? Array.prototype.map.call(
                                    new Uint8Array(bytes),
                                    function (b) { return ('00' + b.toString(16)).slice(-2); }
                                ).join('') : null;
                                /* Also try as ASCII for readability. */
                                var ascii = safeReadStr(this._buf_ptr, n);
                                this._slot_evt.buf_ascii = ascii;
                            } catch (e) { this._slot_evt.buf_read_err = '' + e; }
                        }
                    }
                    send(this._slot_evt);
                    stats.sent++;
                }
            });
        })(SLOTS[i]);
    }
}

/* getObject(int kind, IObject **out) — track FE session creation. */
function install_getobject_hook() {
    Interceptor.attach(GETOBJECT_VA, {
        onEnter: function () {
            var esp = this.context.esp;
            this._kind = safeReadU32(esp.add(0x4));
            this._out  = safeReadU32(esp.add(0x8));
        },
        onLeave: function (retval) {
            stats.getobject_hits++;
            var obj_ptr = (this._out !== null) ? safeReadU32(ptr(this._out)) : null;
            send({
                type: 'fe_getobject',
                t: Date.now(),
                kind: this._kind,
                out_addr: this._out,
                obj: obj_ptr,
                ret: retval.toUInt32(),
            });
        }
    });
}

/* FUN_0836c420(delegate, a, b, c) — the indirect dispatch through a
 * delegate's vtable. Hooks every fire to capture the (delegate, args)
 * tuple. arg0 reveals which delegate; reading *arg0 yields the
 * delegate's vtable pointer; the vtable contents tell us the method
 * shapes the engine implements. */
function install_delegate_hook() {
    Interceptor.attach(FUN_0836c420_VA, {
        onEnter: function () {
            if (stats.delegate_hits >= DELEGATE_CAP) return;
            stats.delegate_hits++;
            var esp = this.context.esp;
            var delegate = safeReadU32(esp.add(0x4));
            var arg1     = safeReadU32(esp.add(0x8));
            var arg2     = safeReadU32(esp.add(0xc));
            var arg3     = safeReadU32(esp.add(0x10));
            /* Read the delegate's vtable + first few entries to
             * fingerprint it. */
            var del_vtbl = (delegate && delegate >= 0x100000)
                ? safeReadU32(ptr(delegate)) : null;
            var del_methods = [];
            if (del_vtbl && del_vtbl >= 0x100000) {
                for (var i = 0; i < 16; i++) {
                    var m = safeReadU32(ptr(del_vtbl).add(i * 4));
                    if (m === null) break;
                    del_methods.push(m);
                }
            }
            var caller = null;
            try { caller = this.returnAddress.toString(); } catch (e) {}
            this._del_evt = {
                type: 'fe_delegate_dispatch',
                t: Date.now(),
                delegate: delegate,
                delegate_vtbl: del_vtbl,
                delegate_methods: del_methods,
                args: [arg1, arg2, arg3],
                caller: caller,
            };
        },
        onLeave: function (retval) {
            if (!this._del_evt) return;
            this._del_evt.ret = retval.toUInt32();
            send(this._del_evt);
            stats.sent++;
        }
    });
}

/* ============================================================
 * Bootstrap
 * ============================================================ */

(function main() {
    var fe_mod = null;
    try { fe_mod = Process.findModuleByName('SWIttsFe-en-US.dll'); }
    catch (e) {}

    if (fe_mod) {
        if (fe_mod.base.compare(IMAGE_BASE_EXPECTED) !== 0) {
            send({
                type: 'fe_vtable_trace_warn',
                msg: 'image base differs from expected — slot VAs will be wrong',
                expected: IMAGE_BASE_EXPECTED.toString(),
                actual: fe_mod.base.toString()
            });
            /* Rebase by adding the delta to every absolute VA. */
            var delta = fe_mod.base.sub(IMAGE_BASE_EXPECTED).toInt32();
            for (var i = 0; i < SLOTS.length; i++) {
                SLOTS[i].va = ptr(SLOTS[i].va).add(delta).toString();
            }
            GETOBJECT_VA   = GETOBJECT_VA.add(delta);
            FUN_0836c420_VA = FUN_0836c420_VA.add(delta);
        }
    } else {
        send({
            type: 'fe_vtable_trace_warn',
            msg: 'SWIttsFe-en-US.dll not currently loaded in target',
        });
    }

    install_vtable_hooks();
    install_getobject_hook();
    install_delegate_hook();

    send({
        type: 'ready',
        hook: 'fe_vtable_trace',
        image_base: fe_mod ? fe_mod.base.toString() : null,
        n_slots: SLOTS.length,
        cap: { per_slot: PER_SLOT_CAP, delegate: DELEGATE_CAP, total: TOTAL_CAP },
    });
})();

rpc.exports = {
    stats: function () { return stats; },
    flush: function () { return stats; },
    reset: function () {
        stats = {
            total_calls: 0, sent: 0, by_slot: {}, delegate_hits: 0,
            getobject_hits: 0, ptr_invalid: 0, read_errors: 0,
        };
        for (var i = 0; i < SLOTS.length; i++) stats.by_slot[SLOTS[i].slot] = 0;
    },
};
