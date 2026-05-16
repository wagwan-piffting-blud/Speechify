'use strict';
/*
 * accent_decision_hook.js — capture per-syllable accent class decisions.
 *
 * Hooks FUN_07e09337 in SWIttsFe-en-US.dll. Per static RE, this is the
 * per-syllable accent-emit orchestrator that:
 *   - reads several state-resident "feature flags" at offsets
 *     +0x86b, +0x86f, +0x873, +0x883, +0x887, +0xea7, +0xeab
 *   - tests them via cascaded condition primitives
 *   - eventually writes the chosen accent ID to state[+0x124f]
 *   - that ID is later serialized to a ToBI label by FUN_07e0675b.
 *
 * The 6 accent-class slots are at state[+0x751..+0x765]:
 *   +0x751: L*    +0x755: H*    +0x759: !H*
 *   +0x75d: L*+H  +0x761: L+H*  +0x765: H+L*
 * (Verified against the static label table at .rdata 0x08393394+.)
 *
 * Hook strategy:
 *   - onEnter: capture state pointer + initial state[+0x124f] (input)
 *     and the feature flags +0x86b..+0x887, +0xea7..+0xeab.
 *     Also capture the 6 accent-class IDs (+0x751..+0x765) — these
 *     should be ~constant per-language but useful to log once.
 *   - onLeave: capture state[+0x124f] (output) and resolve to label
 *     via the slot table.
 *
 * Each call yields one (features, class, label) tuple per syllable.
 * Across the corpus this gives us the empirical (features -> accent)
 * mapping needed to train a decision tree.
 */

var MODULE_NAME = 'SWIttsFe-en-US.dll';
var FUN_07E09337_RVA = 0x39337;     /* 0x07e09337 - 0x07dd0000 image base */

var hooked = false;
var batch = [];
var BATCH_SIZE = 64;

function safeReadShort(p) {
    try { return p.readShort(); } catch (e) { return null; }
}
function safeReadU16(p) {
    try { return p.readU16(); } catch (e) { return null; }
}
function safeReadU8(p) {
    try { return p.readU8(); } catch (e) { return null; }
}

function find_module_base(name) {
    try {
        if (typeof Process.findModuleByName === 'function') {
            var m = Process.findModuleByName(name);
            return m ? m.base : null;
        }
    } catch (e) {}
    return null;
}

function flush_batch() {
    if (batch.length > 0) {
        send({ type: 'accent_decision_batch', samples: batch });
        batch = [];
    }
}

function snapshot_state(state_ptr) {
    /* All field offsets are in bytes from state_ptr base. Pull u16 by
     * default (most look like short IDs). */
    return {
        s124f: safeReadU16(state_ptr.add(0x124f)),
        s86b:  safeReadU16(state_ptr.add(0x86b)),
        s86f:  safeReadU16(state_ptr.add(0x86f)),
        s873:  safeReadU16(state_ptr.add(0x873)),
        s883:  safeReadU16(state_ptr.add(0x883)),
        s887:  safeReadU16(state_ptr.add(0x887)),
        sea7:  safeReadU16(state_ptr.add(0xea7)),
        seab:  safeReadU16(state_ptr.add(0xeab)),
        /* Accent class -> ID slots (should be constant per-language). */
        slot_L_star:  safeReadU16(state_ptr.add(0x751)),
        slot_H_star:  safeReadU16(state_ptr.add(0x755)),
        slot_dH_star: safeReadU16(state_ptr.add(0x759)),  /* !H* */
        slot_L_H:     safeReadU16(state_ptr.add(0x75d)),
        slot_LH_star: safeReadU16(state_ptr.add(0x761)),
        slot_HL_star: safeReadU16(state_ptr.add(0x765)),
    };
}

function install_hook(base) {
    if (hooked) return;
    var fn_addr = base.add(FUN_07E09337_RVA);
    Interceptor.attach(fn_addr, {
        onEnter: function (args) {
            /* __cdecl/stdcall: args via stack. param_1 = state. The
             * function signature from RE: (state, p2, p3) */
            var esp = this.context.esp;
            try {
                this.state_ptr = ptr(esp.add(4).readU32());
                this.entry = snapshot_state(this.state_ptr);
            } catch (e) {
                this.state_ptr = null;
                this.entry = null;
            }
        },
        onLeave: function (_retval) {
            if (!this.state_ptr) return;
            var exit_state = snapshot_state(this.state_ptr);
            batch.push({
                state_ptr: this.state_ptr.toString(),
                entry: this.entry,
                exit_124f: exit_state.s124f,
                slots: {
                    L_star:  exit_state.slot_L_star,
                    H_star:  exit_state.slot_H_star,
                    dH_star: exit_state.slot_dH_star,
                    L_H:     exit_state.slot_L_H,
                    LH_star: exit_state.slot_LH_star,
                    HL_star: exit_state.slot_HL_star,
                },
            });
            if (batch.length >= BATCH_SIZE) flush_batch();
        },
    });
    hooked = true;
    send({ type: 'ready' });
}

rpc.exports = {
    flush: function () { flush_batch(); },
    reset: function () { batch = []; },
};

(function main() {
    var base = find_module_base(MODULE_NAME);
    if (base) {
        install_hook(base);
    } else {
        /* Wait for module load */
        var sub = Process.attachModuleLoadCallback ?
            Process.attachModuleLoadCallback(function (m) {
                if (m && m.name === MODULE_NAME && !hooked) {
                    install_hook(m.base);
                }
            }) : null;
        send({ type: 'waiting_for_module', module: MODULE_NAME });
    }
})();
