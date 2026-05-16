'use strict';

// Capture stock's per-slot target SP feature values (the 5 inputs to the
// tables at voice+0xd8 .. +0x534). Phase 8.9 Session 17.
//
// Scorer signature: __thiscall FUN_08e88de0(this, param_1, slot, param_3)
//   this     = ECX (USelNetworkSlice)
//   param_1  = esp+0x4 (NetworkSlice feature arrays)
//   slot     = esp+0x8 (current slot index)
//
// Per-slot target features come from param_1 at offsets:
//   +0x2c -> int[]: sylInPhrase target  (table 0, voice+0xd8,  10 cols)
//   +0x28 -> int[]: sylType target       (table 1, voice+0x268, 9 cols)
//   +0x34 -> int[]: wordInPhrase target  (table 2, voice+0x3ac, 7 cols)
//   +0x38 -> int[]: sylInWord target     (table 3, voice+0x470, 7 cols)
//   +0x3c -> int[]: 5th-table target     (table 4, voice+0x534, 7 cols)
//
// Each is `*(int*)(param_1 + off) + slot*4` -> reads int.

var ADDR_SCORER = ptr('0x08e88de0');
var perSlot = [];
var sessionDone = false;

function tryU32(p) { try { return p.readU32(); } catch(e) { return null; } }

Interceptor.attach(ADDR_SCORER, {
    onEnter: function() {
        if (sessionDone) return;
        var esp = this.context.esp;
        var param1 = tryU32(esp.add(0x4));
        var slot   = tryU32(esp.add(0x8));
        if (param1 === null || slot === null) return;
        if (slot < 0 || slot > 4096) return;
        var p1 = ptr(param1);

        function readSlotArr(base_off) {
            var arr = tryU32(p1.add(base_off));
            if (!arr) return null;
            return tryU32(ptr(arr).add(slot * 4));
        }

        var entry = {
            slot:        slot,
            sylInPhrase: readSlotArr(0x2c),
            sylType:     readSlotArr(0x28),
            wordInPhrase: readSlotArr(0x34),
            sylInWord:   readSlotArr(0x38),
            table4:      readSlotArr(0x3c),
        };

        // Also capture the label being worked on (for correlation).
        // `this+0xc` contains the target label index (used in CART call).
        entry.label = tryU32(this.context.ecx.add(0xc));

        perSlot.push(entry);
    }
});

rpc.exports = {
    getSlots: function() { return perSlot; },
    reset: function() { perSlot = []; sessionDone = false; },
    done: function() { sessionDone = true; }
};

send({type: 'ready'});
