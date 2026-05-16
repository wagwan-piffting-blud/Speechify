'use strict';
/*
 * fe_lts_hot_hook.js — Targeted argument-capture on the FE functions
 * identified by the Stalker calltree as runtime-frequent.
 *
 * Hooks each "hot" candidate, logs its first 3 args and (where available)
 * the EAX/ECX/EDX register values at entry. Compare across inputs to
 * identify which functions iterate per-letter / per-phoneme.
 */

var TARGET_MODULE = 'SWIttsFe-en-US.dll';

/* Hot candidates from Stalker trace: rva, expected hit count for "A." */
var TARGETS = [
    { rva: 0x59e960, name: 'FUN_0836e960', expect: 28 },
    { rva: 0x5a2140, name: 'FUN_08372140', expect: 28 },
    { rva: 0x59f090, name: 'FUN_0836f090', expect: 28 },
    { rva: 0x59eea0, name: 'FUN_0836eea0', expect: 28 },
    { rva: 0x5a2200, name: 'FUN_08372200', expect: 28 },
    { rva: 0x5aa2c0, name: 'FUN_0837a2c0', expect: 26 },
    { rva: 0x5bebc0, name: 'FUN_0838ebc0', expect: 17 },
    { rva: 0x5a7f00, name: 'FUN_08377f00', expect: 10 },
    { rva: 0x5b8b60, name: 'FUN_08388b60', expect: 11 },
    { rva: 0x5b4730, name: 'FUN_08384730', expect: 11 },
    { rva: 0x5a2c20, name: 'FUN_08372c20', expect: 11 },
    { rva: 0x5aa360, name: 'FUN_0837a360', expect: 11 },
];

var counts = {};

function moduleBase() {
    var m = Process.findModuleByName(TARGET_MODULE);
    return m ? m.base : null;
}

rpc.exports = {
    snapshot: function () { return counts; },
    reset: function () {
        for (var k in counts) counts[k] = { calls: 0, samples: [] };
        return 'reset';
    },
    drain: function () {
        var c = counts;
        counts = {};
        for (var i = 0; i < TARGETS.length; ++i) {
            counts[TARGETS[i].name] = { calls: 0, samples: [] };
        }
        return c;
    },
};

(function () {
    var base = moduleBase();
    if (!base) {
        send({ type: 'error', message: 'no module' });
        return;
    }
    send({ type: 'ready', module_base: base.toString() });

    for (var i = 0; i < TARGETS.length; ++i) {
        counts[TARGETS[i].name] = { calls: 0, samples: [], rva: TARGETS[i].rva };
        (function (t) {
            try {
                Interceptor.attach(base.add(t.rva), {
                    onEnter: function () {
                        var rec = counts[t.name];
                        rec.calls++;
                        if (rec.samples.length < 8) {
                            var ctx = this.context;
                            var esp = ctx.esp;
                            var sample = {
                                eax: ctx.eax.toUInt32(),
                                ecx: ctx.ecx.toUInt32(),
                                edx: ctx.edx.toUInt32(),
                                'esp+4':  null,
                                'esp+8':  null,
                                'esp+12': null,
                            };
                            try { sample['esp+4']  = esp.add(4).readU32(); } catch (e) {}
                            try { sample['esp+8']  = esp.add(8).readU32(); } catch (e) {}
                            try { sample['esp+12'] = esp.add(12).readU32(); } catch (e) {}
                            rec.samples.push(sample);
                        }
                    },
                });
            } catch (e) {
                send({ type: 'error', name: t.name,
                       message: e.toString() });
            }
        })(TARGETS[i]);
    }
    send({ type: 'hooks_installed', n: TARGETS.length });
})();
