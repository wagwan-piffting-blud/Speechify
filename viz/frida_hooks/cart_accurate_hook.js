'use strict';

// Accurate CART feature dump v2 — hooks FUN_08e87c70 (the ESI-compare
// function) and uses return address to infer q_type. Also hooks dispatcher
// entry to catch types 1 and 2.

var ADDR_COMPARE   = ptr('0x08e87c70');  // compares ESI against value list
var ADDR_DISPATCH  = ptr('0x08e87c90');
var ADDR_DURT_WALK = ptr('0x08e87d90');
var ADDR_F0TR_WALK = ptr('0x08e87e10');
var ADDR_SCORER    = ptr('0x08e88de0');

// Return address (inside dispatcher) -> q_type
var RETADDR_TO_TYPE = {};
RETADDR_TO_TYPE[ptr('0x08e87d27').toString()] = 8;
RETADDR_TO_TYPE[ptr('0x08e87d3e').toString()] = 3;
RETADDR_TO_TYPE[ptr('0x08e87d55').toString()] = 4;
RETADDR_TO_TYPE[ptr('0x08e87d6c').toString()] = 5;
RETADDR_TO_TYPE[ptr('0x08e87d87').toString()] = 9;

var currentSlot = -1;
var currentTree = '';
var featuresBySlot = {};

function tryU32(p) { try { return p.readU32(); } catch(e) { return null; } }

Interceptor.attach(ADDR_SCORER, {
    onEnter: function() {
        var slotIdx = tryU32(this.context.esp.add(0x8));
        if (slotIdx !== null && slotIdx >= 0 && slotIdx < 2048) {
            currentSlot = slotIdx;
        }
    }
});

Interceptor.attach(ADDR_DURT_WALK, {
    onEnter: function() { currentTree = 'durt'; },
    onLeave: function(retval) {
        if (currentSlot < 0) return;
        var key = currentSlot + '.durt';
        if (!featuresBySlot[key]) featuresBySlot[key] = {questions: []};
        try {
            featuresBySlot[key].leaf_mean = retval.add(0x10).readFloat();
            featuresBySlot[key].leaf_stddev = retval.add(0x14).readFloat();
        } catch(e) {}
    }
});

Interceptor.attach(ADDR_F0TR_WALK, {
    onEnter: function() { currentTree = 'f0tr'; },
    onLeave: function(retval) {
        if (currentSlot < 0) return;
        var key = currentSlot + '.f0tr';
        if (!featuresBySlot[key]) featuresBySlot[key] = {questions: []};
        try {
            featuresBySlot[key].leaf_mean = retval.add(0x10).readFloat();
            featuresBySlot[key].leaf_stddev = retval.add(0x14).readFloat();
        } catch(e) {}
    }
});

// Hook dispatcher entry — capture types 1, 2, 7 (these don't call FUN_08e87c70)
Interceptor.attach(ADDR_DISPATCH, {
    onEnter: function() {
        if (currentSlot < 0 || currentTree === '') return;
        var eax = this.context.eax;
        var qType = tryU32(eax);
        if (qType === null) return;
        var key = currentSlot + '.' + currentTree;
        if (!featuresBySlot[key]) featuresBySlot[key] = {questions: []};
        var val = null;
        if (qType === 1) {
            val = tryU32(this.context.esp.add(0x4));  // arg_1 (syl_type)
        } else if (qType === 2) {
            val = this.context.ecx.toInt32() & 0xffffffff;
        } else if (qType === 7) {
            val = 0;  // walker XORs EBX before call
        } else {
            return;
        }
        featuresBySlot[key].questions.push({q_type: qType, value: val});
    }
});

// Hook FUN_08e87c70 for types 3, 4, 5, 8, 9 — ESI holds feature value
Interceptor.attach(ADDR_COMPARE, {
    onEnter: function() {
        if (currentSlot < 0 || currentTree === '') return;
        var retAddr = tryU32(this.context.esp);
        if (retAddr === null) return;
        var retStr = ptr(retAddr).toString();
        var qType = RETADDR_TO_TYPE[retStr];
        if (qType === undefined) return;
        var key = currentSlot + '.' + currentTree;
        if (!featuresBySlot[key]) featuresBySlot[key] = {questions: []};
        var val = this.context.esi.toInt32() & 0xffffffff;
        featuresBySlot[key].questions.push({q_type: qType, value: val});
    }
});

rpc.exports = {
    getFeatures: function() { return featuresBySlot; },
    reset: function() { featuresBySlot = {}; currentSlot = -1; currentTree = ''; },
};

send({type: 'ready'});
