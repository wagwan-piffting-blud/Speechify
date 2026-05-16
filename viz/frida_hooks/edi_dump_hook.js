'use strict';

// EDI/AX prosody-table dump — hooks the USel scorer (FUN_08e88de0) and
// dumps the per-halfphone prosody table (EDI) and the voice LUT (AX)
// that supply CART question values.
//
// Discovered in Phase 8.9 Session 2 RE:
//   ESI = scorer's `this` pointer
//   EDI = ESI[0x24] dereferenced at +0x4c = per-halfphone prosody table
//   AX  = ESI[0x20] deref at +0xd0, +0x8 = voice-file LUT
//
// At scorer entry, captures:
//   - slot index from [ESP+0x8]
//   - EDI pointer from ECX->[0x24]->[0x4c]
//   - EDI[0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c, 0x40] for the current slot
//     (each is an array of u32 indexed by slot)
//   - AX pointer + ESI[0x8], ESI[0xc], ESI[0x10] indices into AX
//   - AX[ESI[0x8]], AX[ESI[0x10]] values

var ADDR_SCORER = ptr('0x08e88de0');

var perSlot = {};  // slot -> {edi: [...], ax_idx: {...}, ax_lookup: {...}}
var axBase = null;
var esiStaticIndices = null;  // captured ESI[0x8, 0xc, 0x10] (should be const)

function safeU32(p, off) {
    try { return p.add(off).readU32(); } catch(e) { return null; }
}

var callCount = 0;
Interceptor.attach(ADDR_SCORER, {
    onEnter: function(args) {
        var esp = this.context.esp;
        var esi = this.context.ecx;  // __thiscall: ECX is this; scorer uses ESI=ECX
        callCount++;

        // Slot index from stack
        var slotIdx = safeU32(esp, 0x8);
        if (slotIdx === null || slotIdx < 0 || slotIdx > 2048) return;

        var edi_vals = {
            'this_ptr': esi.toString(16),
            'esi_0x20': safeU32(esi, 0x20),
            'esi_0x24': safeU32(esi, 0x24),
            'call_num': callCount,
        };

        // ESI[0x24] -> struct; that struct's [0x4c] = EDI base
        var ediHost = safeU32(esi, 0x24);
        var edi = null;
        if (ediHost) edi = safeU32(ptr(ediHost), 0x4c);
        edi_vals['edi_base'] = edi ? edi.toString(16) : 'NULL';

        var ediPtr = edi ? ptr(edi) : null;

        // For each feature offset, read the array and index by slot
        [0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c, 0x40].forEach(function(off) {
            var arrPtr = ediPtr ? safeU32(ediPtr, off) : null;
            if (arrPtr) {
                var v = safeU32(ptr(arrPtr), slotIdx * 4);
                edi_vals['edi_' + off.toString(16)] = v;
            } else {
                edi_vals['edi_' + off.toString(16)] = null;
            }
        });

        // AX = ESI[0x20][0xd0][0x8] (+0x8 is the "data" base pointer inside
        // the sub-struct)
        var voiceData = safeU32(esi, 0x20);
        if (voiceData) {
            var subStruct = safeU32(ptr(voiceData), 0xd0);
            if (subStruct) {
                var ax = safeU32(ptr(subStruct), 0x8);
                if (ax && !axBase) {
                    axBase = ax.toString(16);
                }
                // Feature indices into AX — should be constant per scorer
                var i8  = safeU32(esi, 0x8);
                var ic  = safeU32(esi, 0xc);
                var i10 = safeU32(esi, 0x10);
                if (esiStaticIndices === null && i8 !== null) {
                    esiStaticIndices = {i8: i8, ic: ic, i10: i10};
                }
                // Actually the AX lookups are per-CANDIDATE not per-target.
                // Let's still capture target-level: EAX base pointer
                // derivations. These are less critical — what we care about
                // is the phone_left/phone_right TARGET value which should
                // also come from EDI or a related target-side array.
                edi_vals['ax_base'] = ax ? ax.toString(16) : null;
                edi_vals['idx8']  = i8;
                edi_vals['idxc']  = ic;
                edi_vals['idx10'] = i10;
                if (ax && i8 !== null) {
                    edi_vals['ax_at_i8']  = safeU32(ptr(ax), i8 * 4);
                    edi_vals['ax_at_i10'] = safeU32(ptr(ax), i10 * 4);
                }
            }
        }

        if (!perSlot[slotIdx]) perSlot[slotIdx] = edi_vals;
    }
});

rpc.exports = {
    getSlots: function() { return perSlot; },
    getAxBase: function() { return axBase; },
    getEsiIndices: function() { return esiStaticIndices; },
    reset: function() { perSlot = {}; axBase = null; esiStaticIndices = null; },
};

send({type: 'ready'});
