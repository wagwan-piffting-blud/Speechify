'use strict';
/*
 * fe_registry_init_hook.js -- capture FE registry-B base + per-table bases.
 *
 * The Eloquence FE (SWIttsFe-en-US.dll) constructs two pointer tables at
 * FE-state-init time:
 *   state[+0x30] = registry-A: 553 lexical-word-list bucket pointers
 *   state[+0x54] = registry-B: 173 word-list bucket pointers
 *
 * At rest the table data lives in .data at known addresses; what we want
 * is the runtime *pointer-to-pointer-array* address (the malloc'd block
 * inside engine_state) so subsequent hooks can:
 *   - Watch reads at registry-B[i] to learn WHEN and WHICH bucket is
 *     consulted during synthesis (i.e., what Frida "fired" while
 *     processing a given input word).
 *   - Correlate code addresses doing those reads with Ghidra functions.
 *
 * This hook fires only once per FE-init (typically synthesizer startup).
 *
 * Targets in SWIttsFe-en-US.dll (RVAs from analysis; we resolve at attach):
 *   0x0836849e  FUN_0836849e -- registry-B initializer. Returns; on
 *                                exit, *(state+0x54) points at the
 *                                malloc'd 0x2b4-byte ptr array.
 *   0x08391d20  FUN_08391d20 -- per-slot fetch (writes table base into
 *                                caller-supplied struct +4).
 */

var TARGET_MODULE = 'SWIttsFe-en-US.dll';
/* FUN_0836849e absolute = 0x0836849e; module base 0x07dd0000 → RVA 0x59849e. */
var RVA_REG_B_INIT = 0x0059849e;

var calls = 0;
var captured = { state_ptr: null, reg_b_base: null, hits: [] };

function moduleBase() {
    var m = Process.findModuleByName(TARGET_MODULE);
    return m ? m.base : null;
}

rpc.exports = {
    snapshot: function () {
        return captured;
    }
};

(function () {
    var base = moduleBase();
    if (!base) {
        send({ type: 'error',
               message: 'module ' + TARGET_MODULE + ' not loaded' });
        return;
    }
    var addrInit = base.add(RVA_REG_B_INIT);
    send({ type: 'ready', module_base: base.toString(),
           init_addr: addrInit.toString() });

    /* Hook FUN_0836849e: __cdecl, single arg = state pointer.
     *   void FUN_0836849e(int param_1):
     *     puVar1 = (undefined4 *)malloc(0x2b4);
     *     *(undefined4 **)(param_1 + 0x54) = puVar1;
     *     ... 173 store-pointer instructions ...
     * On exit, *(state + 0x54) points at the freshly malloc'd 173-entry
     * ptr array (+ 1 extra slot, 0x2b4/4 = 173). */
    Interceptor.attach(addrInit, {
        onEnter: function (args) {
            this.state_ptr = args[0];
        },
        onLeave: function () {
            calls++;
            try {
                var state = this.state_ptr;
                var reg_b_ptr_addr = state.add(0x54);
                var reg_b_base = reg_b_ptr_addr.readPointer();
                captured.state_ptr = state.toString();
                captured.reg_b_base = reg_b_base.toString();

                /* Read first 8 table base pointers as smoke-test. */
                var samples = [];
                for (var i = 0; i < 8; ++i) {
                    var p = reg_b_base.add(i * 4).readPointer();
                    samples.push({ slot: i, ptr: p.toString() });
                }
                captured.first_8 = samples;

                send({ type: 'reg_b_init',
                       call: calls,
                       state_ptr: state.toString(),
                       reg_b_base: reg_b_base.toString(),
                       first_8: samples });
            } catch (e) {
                send({ type: 'error', where: 'reg_b_init',
                       message: e.toString() });
            }
        }
    });
})();
