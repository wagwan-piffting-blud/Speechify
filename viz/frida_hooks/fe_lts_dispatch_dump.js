'use strict';
/*
 * fe_lts_dispatch_dump.js
 *
 * Capture the actual (sVar1, sVar2) input stream consumed by the LTS
 * dispatcher FUN_08372c20 during a single synthesis. On exit also capture
 * the (phoneme_id, flag) output the dispatcher wrote.
 *
 * Goal: determine what (sVar1, sVar2) pairs are produced by a known input
 * phrase. Combined with the static decode of the 287-record dispatch table
 * (c:/tmp/fe_lts_dispatch.json) this gives ground-truth for every step of
 * the upstream input-stream encoding.
 *
 * Dispatcher signature (decompiled):
 *   void FUN_08372c20(void *state, struct out_t **out, short *in)
 *     where in[0]=sVar1 (token kind, signed; <0 = sentinel),
 *           in[1]=sVar2 (sub-action; -1 = terminator),
 *           in[2..] = payload (advanced by handler).
 *
 * RVA: 0x5a2c20 (DLL preferred base 0x07dd0000 -> abs 0x08372c20)
 */

var TARGET_MODULE = 'SWIttsFe-en-US.dll';
var DISPATCHER_RVA = 0x5a2c20;

var events = [];
var MAX_EVENTS = 200000;
var call_count = 0;

function moduleBase() {
    var m = Process.findModuleByName(TARGET_MODULE);
    return m ? m.base : null;
}

rpc.exports = {
    snapshot: function () {
        return { call_count: call_count, n_events: events.length, events: events };
    },
    reset: function () {
        events = [];
        call_count = 0;
        return 'reset';
    },
};

(function () {
    var base = moduleBase();
    if (!base) {
        send({ type: 'error', message: 'no module loaded' });
        return;
    }
    var addr = base.add(DISPATCHER_RVA);
    send({ type: 'ready', module_base: base.toString(),
           dispatcher: addr.toString() });

    Interceptor.attach(addr, {
        onEnter: function (args) {
            call_count++;
            // args[0] = state, args[1] = out**, args[2] = in (short*)
            var inPtr = this.context.esp.add(12);  // [esp+12] = arg3 = in
            var sVar1 = -32768, sVar2 = -32768, payload0 = 0, payload1 = 0;
            try {
                var inAddr = inPtr.readPointer();
                sVar1 = inAddr.readS16();
                sVar2 = inAddr.add(2).readS16();
                payload0 = inAddr.add(4).readU16();
                payload1 = inAddr.add(6).readU16();
                this._inAddr = inAddr;
                this._outPtr = this.context.esp.add(8).readPointer();
                this._statePtr = this.context.esp.add(4).readPointer();
            } catch (e) { /* swallow */ }
            if (events.length < MAX_EVENTS) {
                events.push({
                    n: call_count,
                    s1: sVar1,
                    s2: sVar2,
                    p0: payload0,
                    p1: payload1,
                });
            }
        },
        onLeave: function (retval) {
            if (events.length === 0) return;
            var ev = events[events.length - 1];
            try {
                if (this._outPtr) {
                    var outRec = this._outPtr;
                    ev.out_phon = outRec.add(2).readS16();
                    ev.out_flag = outRec.add(6).readU8();
                    var newIn = outRec.readPointer();
                    if (this._inAddr && newIn) {
                        ev.advance = newIn.toUInt32() - this._inAddr.toUInt32();
                    }
                }
            } catch (e) { /* swallow */ }
        },
    });

    send({ type: 'hook_installed', addr: addr.toString() });
})();
