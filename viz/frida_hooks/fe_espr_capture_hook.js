'use strict';
/*
 * fe_espr_capture_hook.js -- capture the FULL ESPR input string
 * passed to ESPRparser entry (FUN_06b0c460) in SWIttsEngine.dll.
 *
 * The parser signature is __thiscall:
 *   FUN_06b0c460(this, char *input_str, int input_len, ...)
 * On x86-32 thiscall, `this` is in ECX, remaining args on stack.
 *
 * input_str (first stack arg) is the ESPR text; input_len is its length.
 */

var MODULE_NAME = 'SWIttsEngine.dll';
var ESPR_PARSE_RVA = 0xc460;
var hooked = false;

function find_module_base(name) {
    try {
        if (typeof Process.findModuleByName === 'function') {
            var m = Process.findModuleByName(name);
            return m ? m.base : null;
        }
    } catch (e) {}
    return null;
}

function install_hook(base) {
    if (hooked) return;
    Interceptor.attach(base.add(ESPR_PARSE_RVA), {
        onEnter: function (args) {
            try {
                /* thiscall: this in ECX; args on stack starting at [esp+4]. */
                var esp  = this.context.esp;
                var p_str = ptr(esp.add(4).readU32());
                var p_len = esp.add(8).readU32();
                if (p_len > 0 && p_len < 0x10000) {
                    var s = p_str.readUtf8String(p_len);
                    send({ type: 'espr_input', len: p_len, text: s });
                }
            } catch (e) {
                send({ type: 'espr_error', err: e.toString() });
            }
        }
    });
    hooked = true;
    send({ type: 'ready', hook: 'espr_capture' });
}

var b = find_module_base(MODULE_NAME);
if (b !== null) {
    install_hook(b);
} else {
    var iv = setInterval(function () {
        var bb = find_module_base(MODULE_NAME);
        if (bb !== null) { clearInterval(iv); install_hook(bb); }
    }, 50);
}

rpc.exports = {
    flush: function () { return { hooked: hooked }; }
};
