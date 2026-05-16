'use strict';

// CART feature dump — hooks SWIttsUSel.dll's CART question dispatcher
// (FUN_08e87c90) and logs every feature value passed in per question.
//
// Registers/stack layout (verified from disassembly at 0x08e87c90):
//   type 1 (syl_type)       -> ESI = [ESP+0x8]  (first stack arg)
//   type 2 (syl_in_phrase)  -> EDI = ECX        (this / reg arg)
//   type 3 (phone_left)     -> [ESP+0x14]
//   type 4 (phone_right)    -> [ESP+0x18]
//   type 5 (word_in_phrase) -> [ESP+0x1C]
//   type 7 (unknown)        -> EBX
//   type 8 (phone_in_syl)   -> [ESP+0x10]
//   type 9 (unknown)        -> [ESP+0x20]
//
// Also hooks the two CART walkers (durt @ 0x08e87d90, f0tr @ 0x08e87e10)
// to tag features with which tree they belonged to, and the scorer
// (0x08e88de0) to track slot index.

var ADDR_CART_Q    = ptr('0x08e87c90');
var ADDR_DURT_WALK = ptr('0x08e87d90');
var ADDR_F0TR_WALK = ptr('0x08e87e10');
var ADDR_SCORER    = ptr('0x08e88de0');

var currentSlot = -1;
var currentTree = '';
var featuresBySlot = {};  // slot -> {tree: {features: [...], leaf: uid}}

function tryU32(p) { try { return p.readU32(); } catch(e) { return null; } }

Interceptor.attach(ADDR_SCORER, {
    onEnter: function(args) {
        // param_2 is the 2nd arg (EDX in __fastcall). Scorer sig:
        // FUN_08e88de0(param_1_ptr, slot_idx, flag_ptr)
        // ECX = this (slice obj), EDX = param_1, stack0 = param_2, stack1 = param_3
        // But __thiscall in Ghidra view: ECX = this, then 3 stack args.
        // Let's pull [ESP+4] = param_1, [ESP+8] = slot_idx (param_2).
        var esp = this.context.esp;
        var slotIdx = tryU32(esp.add(8));
        currentSlot = slotIdx;
    }
});

Interceptor.attach(ADDR_DURT_WALK, {
    onEnter: function(args) { currentTree = 'durt'; },
    onLeave: function(retval) {
        if (currentSlot < 0) return;
        var key = currentSlot + '.durt';
        if (!featuresBySlot[key]) featuresBySlot[key] = {questions: []};
        // retval is a pointer to the leaf CartNode. node+0x10 = mean, +0x14 = stddev.
        try {
            var leafPtr = retval;
            var mean = leafPtr.add(0x10).readFloat();
            var stddev = leafPtr.add(0x14).readFloat();
            featuresBySlot[key].leaf_mean = mean;
            featuresBySlot[key].leaf_stddev = stddev;
        } catch(e) {}
    }
});

Interceptor.attach(ADDR_F0TR_WALK, {
    onEnter: function(args) { currentTree = 'f0tr'; },
    onLeave: function(retval) {
        if (currentSlot < 0) return;
        var key = currentSlot + '.f0tr';
        if (!featuresBySlot[key]) featuresBySlot[key] = {questions: []};
        try {
            var leafPtr = retval;
            var mean = leafPtr.add(0x10).readFloat();
            var stddev = leafPtr.add(0x14).readFloat();
            featuresBySlot[key].leaf_mean = mean;
            featuresBySlot[key].leaf_stddev = stddev;
        } catch(e) {}
    }
});

Interceptor.attach(ADDR_CART_Q, {
    onEnter: function(args) {
        if (currentSlot < 0) return;
        var key = currentSlot + '.' + currentTree;
        if (!featuresBySlot[key]) featuresBySlot[key] = {questions: []};

        var ctx = this.context;
        var esp = ctx.esp;

        // EAX points at the question struct (per __thiscall implicit contract
        // for this dispatcher). Question layout: {type:u32, values:ptr, count:u32}
        var qType = tryU32(ctx.eax);

        // Feature values at the various register/stack slots.
        var feats = {
            t1_syl_type:      tryU32(esp.add(0x8)),
            t2_syl_in_phr:    ctx.ecx.toInt32() & 0xffffffff,
            t8_phone_in_syl:  tryU32(esp.add(0x10)),
            t3_phone_left:    tryU32(esp.add(0x14)),
            t4_phone_right:   tryU32(esp.add(0x18)),
            t5_word_in_phr:   tryU32(esp.add(0x1C)),
            t9_unk:           tryU32(esp.add(0x20)),
            t7_ebx:           ctx.ebx.toInt32() & 0xffffffff,
        };

        featuresBySlot[key].questions.push({q_type: qType, feats: feats});
    }
});

rpc.exports = {
    getFeatures: function() { return featuresBySlot; },
    reset: function() { featuresBySlot = {}; currentSlot = -1; },
};

send({type: 'ready'});
