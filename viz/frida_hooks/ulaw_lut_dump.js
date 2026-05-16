'use strict';
/*
 * ulaw_lut_dump.js -- verify u-law decode is bit-exact ITU G.711.
 *
 * Closes plan gap #7.
 *
 * KNOWN ISSUE (2026-05-05): live capture against the running engine
 * produces zero (in_byte, out_s16) pairs for typical text synthesis.
 * SWIttsAudioCvtUlawToL16 is exported by SWIttsEngineUtil.dll (DLL_ANALYSIS
 * confirms), but the synthesis hot path appears to bypass it -- WSOLA
 * likely streams u-law bytes through its own decode path (consistent with
 * WSOLA owning audio output per DLL_ANALYSIS pipeline). Need to verify
 * whether: (a) the export is mangled differently in this build,
 * (b) WSOLA has its own inline decode, or (c) the function is only
 * called for the 16kHz/test paths. Likely we'll need to hook inside
 * SWIttsWsola.dll directly. Logging the LUT statically from the DLL's
 * .rdata is the fallback if the function is truly never called.
 *
 * Output: type='ulaw_pair' { in: u8, out: s16 } events, plus a one-shot
 * type='ulaw_table' { table: s16[256] } if we can locate the LUT directly.
 */

var MOD = 'SWIttsEngineUtil.dll';
var SYM = 'SWIttsAudioCvtUlawToL16';

/* Frida 17 dropped the static Module.findExportByName. Resolve via the
 * per-module instance API, which is stable across 14..17. */
var fn = null;
try {
    var mod = Process.findModuleByName(MOD) || Process.getModuleByName(MOD);
    if (mod) {
        fn = mod.findExportByName(SYM);
        if (fn === null) {
            var exports = mod.enumerateExports();
            for (var i = 0; i < exports.length; ++i) {
                if (exports[i].name.indexOf('UlawToL16') !== -1) {
                    fn = exports[i].address;
                    SYM = exports[i].name;
                    break;
                }
            }
        }
    }
} catch (e) {
    send({ type: 'error', hook: 'ulaw_lut', msg: String(e) });
}

if (fn === null) {
    send({ type: 'ready', hook: 'ulaw_lut', error: 'export not found in ' + MOD });
} else {
    /* Standard signature is plausibly:
     *     void SWIttsAudioCvtUlawToL16(const u8 *src, s16 *dst, size_t n)
     * with __cdecl. We capture src/dst/n on enter and read the dst buffer
     * byte-by-byte to match each input ulaw byte to its produced s16. */
    var samples_taken = 0;
    var SAMPLES_MAX = 4096;

    Interceptor.attach(fn, {
        onEnter: function (args) {
            this.src = args[0];
            this.dst = args[1];
            this.n   = args[2].toUInt32();
        },
        onLeave: function () {
            if (samples_taken >= SAMPLES_MAX) return;
            var n = this.n;
            if (n === 0 || n > 4000) return;
            try {
                var pairs = [];
                var src_bytes = this.src.readByteArray(n);
                var dst_bytes = this.dst.readByteArray(n * 2);
                var sb = new Uint8Array(src_bytes);
                var db = new Uint8Array(dst_bytes);
                /* sample at most 256 distinct in_bytes per call */
                var seen = {};
                for (var i = 0; i < n && samples_taken < SAMPLES_MAX; ++i) {
                    var k = sb[i];
                    if (seen[k]) continue;
                    seen[k] = 1;
                    var s16 = (db[2 * i] | (db[2 * i + 1] << 8));
                    if (s16 & 0x8000) s16 -= 0x10000;
                    pairs.push({ in_byte: k, out_s16: s16 });
                    samples_taken++;
                }
                if (pairs.length > 0) {
                    send({ type: 'ulaw_pair', sym: SYM, pairs: pairs });
                }
            } catch (e) { /* ignore unmapped reads */ }
        }
    });

    send({ type: 'ready', hook: 'ulaw_lut', sym: SYM, addr: fn.toString() });
}
