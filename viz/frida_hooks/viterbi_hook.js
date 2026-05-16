'use strict';

var ADDR_PRUNE_FN     = ptr('0x08E88830');
var ADDR_USEL         = ptr('0x08E819E0');
var ADDR_WSOLA_CONCAT = ptr('0x08EE65E0');

var synthCount = 0;
var hpCount = 0;
var hpData = [];
var wsolaUnits = [];

function tryU32(addr) {
    try { return addr.readU32(); } catch(e) { return null; }
}
function tryF32(addr) {
    try { return addr.readFloat(); } catch(e) { return null; }
}

// --- USEL orchestrator: tracks synthesis start/end ---
Interceptor.attach(ADDR_USEL, {
    onEnter: function(args) {
        synthCount++;
        hpCount = 0;
        hpData = [];
        wsolaUnits = [];
    },
    onLeave: function(retval) {
        send({type: 'synth_done', synth: synthCount, hps: hpData});
    }
});

// --- Prune: capture pre-prune best + top candidates per halfphone ---
// FUN_08e88830 decompile (Ghidra 2026-05-09 plan-zero audit):
//   iVar10 = *this;                       // n_cands at this+0x00
//   pfVar11 = (float *)(*(this + 0x18) + 4);  // cand array, 0x18 stride, cost@+4
// Earlier this hook read n from this+0x14 which was a stale field-offset
// drift from a prior struct-shape hypothesis; corrected to this+0x00.
Interceptor.attach(ADDR_PRUNE_FN, {
    onEnter: function(args) {
        hpCount++;
        var thisPtr = this.context.ecx;
        var n = tryU32(thisPtr);              // this+0x00 per FUN_08e88830 disasm
        var arrVal = tryU32(thisPtr.add(0x18));

        if (n === null || arrVal === null || n < 1 || n > 500 || arrVal < 0x100000) {
            hpData.push({hp: hpCount, uid: -1, total: 99, n_cand: 0, top: []});
            return;
        }

        var arr = ptr(arrVal);
        var candidates = [];

        for (var i = 0; i < n; i++) {
            var base = arr.add(i * 0x18);
            var uid   = tryU32(base);
            var total = tryF32(base.add(0x04));
            if (uid === null || total === null) continue;
            candidates.push({uid: uid, total: total > 1e10 ? 99 : total});
        }

        // Sort by cost and keep top 5
        candidates.sort(function(a, b) { return a.total - b.total; });
        var top = candidates.slice(0, 5);
        var best = candidates.length > 0 ? candidates[0] : {uid: -1, total: 99};

        hpData.push({
            hp: hpCount,
            uid: best.uid,
            total: best.total,
            n_cand: n,
            top: top
        });
    }
});

// --- WSOLA concat: capture final Viterbi-selected unit path ---
Interceptor.attach(ADDR_WSOLA_CONCAT, {
    onEnter: function(args) {
        var esp = this.context.esp;
        var arg4Val = tryU32(esp.add(16));
        if (arg4Val === null || arg4Val < 0x100000) return;

        var arg4 = ptr(arg4Val);
        var count = tryU32(arg4.add(0x04));
        var arrPtrVal = tryU32(arg4.add(0x08));

        if (count === null || arrPtrVal === null ||
            count < 1 || count > 500 || arrPtrVal < 0x100000) return;

        var arrPtr = ptr(arrPtrVal);
        var units = [];
        for (var i = 0; i < count; i++) {
            var base = arrPtr.add(i * 0x18);
            var uid = tryU32(base);
            units.push(uid !== null ? uid : -1);
        }
        send({type: 'wsola', synth: synthCount, units: units, count: count});
    }
});

rpc.exports = {
    getSynthCount: function() { return synthCount; }
};

send({type: 'ready'});
