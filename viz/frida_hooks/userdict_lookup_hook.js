'use strict';
/*
 * userdict_lookup_hook.js
 *
 * Captures every (dict_name, query_key, returned_value) triple from
 * SWIttsFe-en-US.dll!UserDict::lookup. The disambigDict is the engine's
 * POS disambiguation lookup table; hooking at this layer reveals
 *   (a) the EXACT key string the Delta VM constructs for each word, and
 *   (b) the dict's resolved POS-class value.
 *
 * Why hook here rather than at output emission:
 *   The Delta bytecode does the key construction. We don't want to
 *   reverse-engineer that bytecode; we just intercept at the entry to
 *   the underlying native UserDict::lookup so the key it built is
 *   already a normal NUL-terminated C string we can read.
 *
 * Function signature (from Ghidra decompilation @ 0x0836fe00):
 *   undefined4 __thiscall UserDict::lookup(
 *       int *self, char *key, int *out_value);
 *
 *   self        = ecx       (thiscall this)
 *   key         = [esp+4]   (NUL-terminated C string)
 *   out_value   = [esp+8]   (int* — filled with translation cstring ptr on success)
 *
 *   Returns 0 on success, negative err on miss.
 *
 *   self+0x1e = name (cstring ptr) — the dict name we filter by.
 *   self[6]   = type code.
 *
 * Output: send() events with type='userdict_lookup' for every entry+leave.
 * Hot path: hash lookup runs thousands of times per phrase, so we batch.
 */

/* UserDict::lookup absolute VA assuming the historical SWIttsFe-en-US.dll
 * image base of 0x07dd0000 (same convention as fe_vtable_trace.js).
 * Rebased at script load to whatever base ASLR actually picked. */
var IMAGE_BASE_EXPECTED = ptr('0x07dd0000');
var ADDR_LOOKUP = ptr('0x0836fe00');     // UserDict::lookup

(function rebase() {
    var fe_mod = null;
    try { fe_mod = Process.findModuleByName('SWIttsFe-en-US.dll'); }
    catch (e) {}
    if (fe_mod && fe_mod.base.compare(IMAGE_BASE_EXPECTED) !== 0) {
        var delta = fe_mod.base.sub(IMAGE_BASE_EXPECTED).toInt32();
        ADDR_LOOKUP = ADDR_LOOKUP.add(delta);
        send({ type: 'userdict_lookup_rebase',
               expected: IMAGE_BASE_EXPECTED.toString(),
               actual: fe_mod.base.toString(),
               adjusted: ADDR_LOOKUP.toString() });
    } else if (!fe_mod) {
        send({ type: 'userdict_lookup_warn',
               msg: 'SWIttsFe-en-US.dll not loaded yet' });
    }
})();

var batch = [];
var BATCH_MAX = 256;

function flush() {
    if (batch.length === 0) return;
    send({ type: 'userdict_lookup_batch', samples: batch });
    batch = [];
}

function readCString(addr_u32) {
    if (addr_u32 === 0 || addr_u32 === null) return null;
    var p = ptr(addr_u32);
    try {
        var r = Process.findRangeByAddress(p);
        if (!r || r.protection.charAt(0) !== 'r') return null;
        return p.readCString(/*length=*/ -1, /*encoding=*/ 'utf8');
    } catch (e) { return null; }
}

function readU32(addr_u32) {
    if (addr_u32 === 0 || addr_u32 === null) return null;
    var p = ptr(addr_u32);
    try {
        var r = Process.findRangeByAddress(p);
        if (!r || r.protection.charAt(0) !== 'r') return null;
        if (p.add(4).compare(r.base.add(r.size)) > 0) return null;
        return p.readU32();
    } catch (e) { return null; }
}

Interceptor.attach(ADDR_LOOKUP, {
    onEnter: function (args) {
        // thiscall: this in ecx; key + out_value on stack.
        var self    = this.context.ecx.toUInt32();
        var esp     = this.context.esp.toUInt32();
        var key_p   = readU32(esp + 4);
        var outv_p  = readU32(esp + 8);

        // Capture name + type + key text at entry.
        var name_ptr = readU32(self + 0x1e);
        this._dict_name = readCString(name_ptr);
        this._dict_type = readU32(self + 0x18);   // param_1_00[6] = +0x18
        this._key       = readCString(key_p);
        this._outv_p    = outv_p;
    },
    onLeave: function (retval) {
        // Skip silently if key wasn't readable.
        if (this._key === null) return;
        // (debug pass: no name filter — see every dict name)
        var rc = retval.toInt32();
        var value = null;
        if (rc === 0 && this._outv_p !== null) {
            var v_str_ptr = readU32(this._outv_p);
            if (v_str_ptr !== null) value = readCString(v_str_ptr);
        }
        batch.push({
            dict: this._dict_name,
            type: this._dict_type,
            key:  this._key,
            value: value,
            rc:   rc
        });
        if (batch.length >= BATCH_MAX) flush();
    }
});

rpc.exports = {
    reset:  function () { batch = []; },
    flush:  function () { flush(); }
};

send({ type: 'ready', hook: 'userdict_lookup' });
