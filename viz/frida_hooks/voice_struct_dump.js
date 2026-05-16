'use strict';

// Dump voice struct fields relevant to the S cost lookup.
// Per decomp of FUN_08E88DE0 + README §2185-2220 Session 9 trace:
//   voice[0x600]: used as arg to FUN_08e87b50 (phone-to-context-class)
//   voice[0x604]: u32[] translation table (feature_idx -> row)
//   voice[0x610]: array of 0x30-byte per-phone context configs
//   voice[0xc0] / voice[0xc4]: per-candidate 4-byte context index arrays
//
// At scorer entry, dump these so we can reverse-engineer their layout.

var ADDR_SCORER = ptr('0x08e88de0');
var dumped = false;
var dump = {};

function readBytes(ptr, n) {
    try { return Array.from(new Uint8Array(ptr.readByteArray(n))); } catch(e) { return null; }
}
function readU32(ptr) { try { return ptr.readU32(); } catch(e) { return null; } }
function readFloatArr(ptr, n) {
    var out = [];
    try {
        for (var i = 0; i < n; i++) {
            out.push(ptr.add(i*4).readFloat());
        }
        return out;
    } catch(e) { return null; }
}

Interceptor.attach(ADDR_SCORER, {
    onEnter: function() {
        if (dumped) return;
        dumped = true;

        var esi = this.context.ecx;  // __thiscall: ECX is this
        var voice = readU32(esi.add(0x20));
        if (!voice) return;
        var vp = ptr(voice);
        dump.voice_base = voice.toString(16);

        // Dump voice struct region 0x600..0x700 as u32 array
        var region = [];
        for (var off = 0x600; off < 0x700; off += 4) {
            region.push({off: off.toString(16), val: readU32(vp.add(off))});
        }
        dump.voice_region_600_700 = region;

        // voice[0xc0], voice[0xc4] as pointers
        dump.ptr_c0 = readU32(vp.add(0xc0));
        dump.ptr_c4 = readU32(vp.add(0xc4));

        // voice[0x604] as pointer - try to read first N u32s
        var p604 = readU32(vp.add(0x604));
        if (p604) {
            dump.ptr_604 = p604.toString(16);
            var arr604 = [];
            for (var i = 0; i < 64; i++) {
                arr604.push(readU32(ptr(p604).add(i*4)));
            }
            dump.arr_604 = arr604;
        }

        // voice[0x610] as pointer to 0x30-byte entries
        var p610 = readU32(vp.add(0x610));
        if (p610) {
            dump.ptr_610 = p610.toString(16);
            // Read first 10 entries (0x30 bytes each = 480 bytes)
            var entries = [];
            for (var i = 0; i < 50; i++) {
                var e = {idx: i};
                for (var j = 0; j < 0x30; j += 4) {
                    e['off_' + j.toString(16)] = readU32(ptr(p610).add(i*0x30 + j));
                }
                entries.push(e);
            }
            dump.entries_610 = entries;
        }

        // Candidate array base: voice[0x20][0x20] per decomp
        // voice[0x20]? That's another pointer inside voice struct.
        var cand_base = readU32(vp.add(0x20));
        if (cand_base) dump.cand_base = cand_base.toString(16);
    }
});

rpc.exports = {
    getDump: function() { return dump; },
    reset: function() { dumped = false; dump = {}; },
};

send({type: 'ready'});
