'use strict';
/*
 * fe_callback_probe_hook.js — capture what the FE actually asks of the
 * args[1] callback object during a synth.
 *
 * Background (2026-05-11):
 *   - SWIttsEngine.dll's setPair_E/F/G are tiny adapters whose helpers
 *     do: ecx = *(self+0x20); call ecx[+0xa8]/[+0xb0]/[+0x24].
 *   - That means the real per-phoneme work happens in a virtual method
 *     on an object the engine creates and hands over via setPair_X(args[1]).
 *   - If we capture (a) which methods are called, (b) what args, and
 *     (c) what they return per phoneme, we can decide whether a stub
 *     in our hosted process can replace the whole subsystem.
 *
 * Strategy:
 *   1. Hook setPair_E (slot 41 wrapper) at onEnter; from args[1]:
 *        inner = *(args[1] + 0x20)
 *        vtbl  = *inner
 *        m0xa8 = *(vtbl + 0xa8)   // setPair_E's method
 *        m0xb0 = *(vtbl + 0xb0)   // setPair_F's method
 *        m0x24 = *(vtbl + 0x24)   // setPair_G's method
 *   2. Install Interceptor.attach on m0xa8/m0xb0/m0x24.
 *   3. Each invocation logs args[0..3] (best-effort: 4 stack u32s) + ret.
 *   4. Cap to first N invocations per method to keep volume manageable.
 *
 * Output volume: per-phrase we expect O(phonemes * 3) calls. With cap=40
 * per method we get a representative sample.
 */

var ADDR_SLOT41 = ptr('0x0836d150');   /* setPair_E wrapper */

var PER_METHOD_CAP = 40;
var SET_VTABLE_OFFSETS = [
    { off: 0x024, label: 'setPair_G_method' },
    { off: 0x0a8, label: 'setPair_E_method' },
    { off: 0x0b0, label: 'setPair_F_method' },
];

var hooked_method_addrs = {};   // dedupe across multiple setPair_E calls
var stats = { setpair_E_calls: 0, methods_hooked: 0, method_calls: {} };

function rangeOK(addr) {
    try { var r = Process.findRangeByAddress(addr);
          return r !== null && r.protection.indexOf('r') !== -1; }
    catch (e) { return false; }
}
function safeReadU32(addr) {
    if (!rangeOK(addr)) return null;
    try { return addr.readU32(); } catch (e) { return null; }
}
function safeReadHex(addr, n) {
    if (!rangeOK(addr)) return null;
    try {
        var bytes = new Uint8Array(addr.readByteArray(n));
        var arr = [];
        for (var i = 0; i + 4 <= bytes.length; i += 4) {
            arr.push((bytes[i] | (bytes[i+1]<<8) | (bytes[i+2]<<16) | (bytes[i+3]<<24)) >>> 0);
        }
        return arr;
    } catch (e) { return null; }
}
function probeAddr(a) {
    if (!a || a.isNull()) return null;
    var mod = null, rng = null;
    try { mod = Process.findModuleByAddress(a); } catch (e) {}
    try { rng = Process.findRangeByAddress(a); } catch (e) {}
    return {
        addr: a.toString(),
        module: mod ? mod.name : null,
        offset: mod ? '+0x' + a.sub(mod.base).toString(16) : null,
        prot: rng ? rng.protection : null,
    };
}

function install_method_hook(method_addr, label) {
    var key = method_addr.toString();
    if (hooked_method_addrs[key]) return;
    hooked_method_addrs[key] = label;
    stats.methods_hooked++;
    stats.method_calls[label] = 0;

    try {
        Interceptor.attach(method_addr, {
            onEnter: function (args) {
                if (stats.method_calls[label] >= PER_METHOD_CAP) return;
                this.label = label;
                this.cap_idx = stats.method_calls[label];
                stats.method_calls[label]++;

                var esp = this.context.esp;
                /* __stdcall thiscall: ecx = self, args on stack at esp+4..
                 * We capture ecx + first 4 stack args. */
                var self_ptr = ptr(this.context.ecx >>> 0);
                var stack_args = [];
                for (var i = 0; i < 4; i++) {
                    var v = safeReadU32(esp.add(4 + i*4));
                    stack_args.push(v);
                }
                /* Try to read 16 bytes at each pointer-looking arg for
                 * structure inspection. */
                var arg_dumps = stack_args.map(function (v) {
                    if (v === null || v < 0x100000 || v > 0x80000000) return null;
                    return safeReadHex(ptr(v >>> 0), 32);
                });

                this.snap = {
                    type: 'cb_call',
                    method: label,
                    idx: this.cap_idx,
                    self: self_ptr.toString(),
                    self_head: safeReadHex(self_ptr, 32),
                    args: stack_args,
                    arg_dumps: arg_dumps,
                };
            },
            onLeave: function (retval) {
                if (!this.snap) return;
                this.snap.ret = retval ? retval.toUInt32() : 0;
                /* If any of our pointer-looking args got *written* by the
                 * function (output param), capture again to diff. */
                var arg_post = [];
                for (var i = 0; i < this.snap.args.length; i++) {
                    var v = this.snap.args[i];
                    if (v === null || v < 0x100000 || v > 0x80000000) {
                        arg_post.push(null);
                    } else {
                        arg_post.push(safeReadHex(ptr(v >>> 0), 32));
                    }
                }
                this.snap.arg_dumps_post = arg_post;
                send(this.snap);
            },
        });
        send({ type: 'method_hooked', label: label,
               addr: method_addr.toString() });
    } catch (e) {
        send({ type: 'hook_err', label: label,
               addr: method_addr.toString(), err: e.toString() });
    }
}

Interceptor.attach(ADDR_SLOT41, {
    onEnter: function (args) {
        stats.setpair_E_calls++;
        var esp = this.context.esp;
        try {
            /* FE vtable slot 41 wrapper, called by Speechify as:
             *   setPair_E(this=fe_iobj, arg0=eng_func_ptr, arg1=eng_data_ptr)
             * After the return-address push:
             *   [esp+0x4] = this
             *   [esp+0x8] = arg0 (the function pointer in SWIttsEngine.dll)
             *   [esp+0xc] = arg1 (the data object whose vtable we want) */
            var self_arg = ptr(safeReadU32(esp.add(0x4)) || 0);
            var func_arg = ptr(safeReadU32(esp.add(0x8)) || 0);
            var data_arg = ptr(safeReadU32(esp.add(0xc)) || 0);
            if (!rangeOK(data_arg)) {
                send({ type: 'setE_call_skip', reason: 'data_arg unreadable' });
                return;
            }
            /* inner = *(data_arg + 0x20) */
            var inner_u32 = safeReadU32(data_arg.add(0x20));
            if (inner_u32 === null || inner_u32 < 0x100000) {
                send({ type: 'setE_call_skip', reason: 'inner_ptr invalid',
                       data_arg: data_arg.toString(),
                       at_0x20: inner_u32 });
                return;
            }
            var inner = ptr(inner_u32 >>> 0);
            var vtbl_u32 = safeReadU32(inner);
            if (vtbl_u32 === null || vtbl_u32 < 0x100000) {
                send({ type: 'setE_call_skip', reason: 'vtbl_ptr invalid',
                       inner: inner.toString() });
                return;
            }
            var vtbl = ptr(vtbl_u32 >>> 0);
            send({
                type: 'setpair_E_resolve',
                self: self_arg.toString(),
                func: func_arg.toString(),
                func_probe: probeAddr(func_arg),
                data: data_arg.toString(),
                data_probe: probeAddr(data_arg),
                inner: inner.toString(),
                vtbl: vtbl.toString(),
                vtbl_probe: probeAddr(vtbl),
            });

            for (var i = 0; i < SET_VTABLE_OFFSETS.length; i++) {
                var s = SET_VTABLE_OFFSETS[i];
                var m_u32 = safeReadU32(vtbl.add(s.off));
                if (m_u32 === null || m_u32 < 0x100000) continue;
                var m = ptr(m_u32 >>> 0);
                send({
                    type: 'method_resolved',
                    label: s.label,
                    vtbl_off: '+0x' + s.off.toString(16),
                    addr: m.toString(),
                    probe: probeAddr(m),
                });
                install_method_hook(m, s.label);
            }
        } catch (e) {
            send({ type: 'setE_err', err: e.toString() });
        }
    },
});

rpc.exports = {
    stats: function () { return stats; },
    flush: function () { return stats; },
    reset: function () {
        stats = { setpair_E_calls: 0, methods_hooked: 0, method_calls: {} };
        hooked_method_addrs = {};
    },
};

send({ type: 'ready', hook: 'fe_callback_probe',
       slot41: ADDR_SLOT41.toString(),
       per_method_cap: PER_METHOD_CAP });
