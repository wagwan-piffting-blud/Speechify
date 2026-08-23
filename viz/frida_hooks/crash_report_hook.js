/* crash_report_hook.js -- first-chance exception reporter.
 *
 * Installs no Interceptors at all, so it cannot perturb the x87 hot loops
 * (see the entry-only policy in run_frida_capture.py). It only sits on
 * Frida's exception path and prints where the fault happened, in what
 * module, at what offset, with what registers, which heap range the base
 * pointer lives in, and how far past that range's end the fault landed.
 */
'use strict';

function mod(addr) {
    const m = Process.findModuleByAddress(addr);
    if (!m) return String(addr) + " <unmapped>";
    return m.name + "+0x" + addr.sub(m.base).toString(16) +
           "  (base " + m.base + ")";
}

function rangeOf(addr) {
    try {
        const r = Process.findRangeByAddress(addr);
        if (!r) return "<no mapped range>";
        const end = r.base.add(r.size);
        return "base=" + r.base + " size=" + r.size + " (0x" +
               r.size.toString(16) + ") end=" + end + " prot=" + r.protection +
               " off=+0x" + addr.sub(r.base).toString(16);
    } catch (e) { return "<range lookup failed: " + e + ">"; }
}

function regs(ctx) {
    const names = ['eax', 'ebx', 'ecx', 'edx', 'esi', 'edi', 'ebp', 'esp',
                   'eip'];
    const out = [];
    for (const n of names) {
        if (ctx[n] !== undefined) {
            out.push(n + "=" + ctx[n] + " (" + ctx[n].toInt32() + ")");
        }
    }
    return out.join('\n            ');
}

function stackDump(sp, words) {
    const out = [];
    for (let i = 0; i < words; i++) {
        const a = sp.add(i * 4);
        let v;
        try { v = a.readPointer(); } catch (e) { v = null; }
        if (v === null) { out.push("  [esp+0x" + (i * 4).toString(16) +
                                   "] <unreadable>"); continue; }
        let note = "";
        const m = Process.findModuleByAddress(v);
        if (m) note = "  " + m.name + "+0x" + v.sub(m.base).toString(16);
        else {
            const r = Process.findRangeByAddress(v);
            if (r) note = "  heap[" + r.base + " +0x" + r.size.toString(16) +
                          "] off=+0x" + v.sub(r.base).toString(16);
        }
        out.push("  [esp+0x" + (i * 4).toString(16) + "] = " + v +
                 "  (" + v.toInt32() + ")" + note);
    }
    return out.join("\n");
}

let seen = 0;

Process.setExceptionHandler(function (details) {
    seen++;
    const c = details.context;
    const lines = [];
    lines.push("======== EXCEPTION #" + seen + " ========");
    lines.push("type      : " + details.type);
    lines.push("address   : " + details.address + "   " + mod(details.address));
    if (details.memory) {
        lines.push("memory    : " + details.memory.operation + " @ " +
                   details.memory.address);
        lines.push("  fault range : " + rangeOf(details.memory.address));
    }
    lines.push("regs      : " + regs(c));
    if (c.esi !== undefined) {
        lines.push("esi range : " + rangeOf(c.esi));
    }
    if (c.edi !== undefined) lines.push("edi range : " + rangeOf(c.edi));
    if (c.ecx !== undefined) lines.push("ecx range : " + rangeOf(c.ecx));
    if (c.edx !== undefined) lines.push("edx range : " + rangeOf(c.edx));

    lines.push("stack (esp .. esp+0x60):");
    lines.push(stackDump(c.esp, 25));

    try {
        const bt = Thread.backtrace(c, Backtracer.ACCURATE);
        lines.push("backtrace (accurate):");
        bt.forEach(function (a) { lines.push("    " + a + "  " + mod(a)); });
    } catch (e) { lines.push("  accurate backtrace failed: " + e); }

    send({ crash: lines.join("\n") });
    return false;          /* let the process take it -- do not resume */
});

send({ ready: true });
