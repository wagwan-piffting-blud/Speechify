"""
Frida Viterbi inner loop recording-switch penalty + ground truth diagnostic.

Combines:
  - Penalty hook at 0x8E8B854 (ViterbiFwd_NoJoin comparison point)
  - Pre-prune best capture (0x8E88830)
  - WSOLA Viterbi-selected path capture (0x8EE65E0)
  - Full comparison report

Usage:
  python frida_viterbi_penalty.py "Text to synthesize" [penalty_value]
  python frida_viterbi_penalty.py "Text to synthesize" 0     # diagnostic only
  python frida_viterbi_penalty.py "Text to synthesize" 50.0  # penalty = 50.0
  python frida_viterbi_penalty.py "Text to synthesize" 50 --voice tom

Target: bin/Speechify.exe (the server process)
"""
import argparse
import struct
import sys
import os
import frida
import subprocess
import threading
import time
import tempfile

import numpy as np

# =====================================================================
# Constants
# =====================================================================
TARGET = "Speechify.exe"
XOR = 0xCE
UNIT_SIZE = 29

PHONE_LABELS = [
    'aa', 'ae', 'ah', 'ao', 'aw', 'ax', 'ay', 'b', 'ch', 'dx',
    'd',  'dh', 'eh', 'el', 'er', 'en', 'ey', 'f', 'g',  'hh',
    'ih', 'ix', 'iy', 'jh', 'k',  'l',  'm',  'n', 'ng', 'ow',
    'oy', 'p',  'pau','r',  's',  'sh', 't',  'th','uh', 'uw',
    'v',  'w',  'xx', 'y',  'z',  'zh',
]
SILENCE_PCS = {32}


def _detect_proj_root():
    """Find project root from cwd/script dir by looking for expected layout."""
    starts = (os.path.abspath(os.getcwd()), os.path.abspath(os.path.dirname(__file__)))
    seen = set()
    for start in starts:
        cur = start
        while cur and cur not in seen:
            seen.add(cur)
            if os.path.isfile(os.path.join(cur, "bin", "spfy_dumpwav.exe")):
                return cur
            parent = os.path.dirname(cur)
            if parent == cur:
                break
            cur = parent
    return os.path.abspath(os.path.dirname(__file__))


# =====================================================================
# VIN helpers
# =====================================================================
def load_xor(path):
    raw = bytearray(open(path, 'rb').read())
    for i in range(len(raw)):
        raw[i] ^= XOR
    return bytes(raw)


def riff_chunks(data, start=12, end=None):
    end = end or len(data)
    pos = start
    while pos + 8 <= end:
        tag = data[pos:pos+4]
        sz = struct.unpack_from('<I', data, pos+4)[0]
        yield tag, pos+8, sz
        pos += 8 + sz + (sz & 1)


def read_n_units(vin_data):
    for tag, ds, sz in riff_chunks(vin_data):
        if tag == b'cnts':
            return struct.unpack_from('<I', vin_data, ds + 8)[0]
    return 0


def find_unit_data(vin_data, n_units):
    for tag, ds, sz in riff_chunks(vin_data):
        if tag == b'unit':
            for t2, d2, s2 in riff_chunks(vin_data, ds):
                if t2 == b'data' and s2 == n_units * UNIT_SIZE:
                    return d2
    return None


def parse_filenames(tvin):
    feat_off = feat_sz = None
    for tag, ds, sz in riff_chunks(tvin):
        if tag == b'feat':
            feat_off, feat_sz = ds, sz
            break
    if feat_off is None:
        return {}
    feat = tvin[feat_off:feat_off+feat_sz]
    fn_idx = feat.find(b'filename')
    if fn_idx < 0:
        return {}
    fn_count = struct.unpack_from('<I', feat, fn_idx+8)[0]
    p = fn_idx + 12
    fnames = {}
    for _ in range(fn_count):
        nlen = struct.unpack_from('<H', feat, p)[0]
        name = feat[p+2:p+2+nlen].decode('latin-1', errors='replace')
        stored_id = struct.unpack_from('<I', feat, p+2+nlen)[0]
        fnames[stored_id] = name
        p += 2 + nlen + 4
    return fnames


def uid_info(uid, vin_data, voice_ud, filenames, n_units):
    if uid < 0 or uid >= n_units:
        return {'phone': '???', 'half': '?', 'fidx': -1,
                'lp': 0, 'dl': 0, 'rec_name': '???', 'is_silence': True}
    base = voice_ud + uid * UNIT_SIZE
    pc = vin_data[base + 20]
    fidx = struct.unpack_from('<H', vin_data, base + 4)[0]
    lp = struct.unpack_from('<H', vin_data, base + 6)[0]
    dl = struct.unpack_from('<H', vin_data, base + 10)[0]
    is_first = vin_data[base + 21]
    phone = PHONE_LABELS[pc] if pc < len(PHONE_LABELS) else '#%d' % pc
    half = '1st' if is_first else '2nd'
    rec_name = filenames.get(fidx, '???')
    return {'phone': phone, 'half': half, 'fidx': int(fidx),
            'lp': int(lp), 'dl': int(dl), 'rec_name': rec_name,
            'is_silence': pc in SILENCE_PCS}


def build_fidx_table(vin_data, n_units):
    """Build file_idx lookup table from decoded VIN data."""
    voice_ud = find_unit_data(vin_data, n_units)
    if voice_ud is None:
        return None, 0
    table = []
    for i in range(n_units):
        base = voice_ud + i * UNIT_SIZE
        fidx = struct.unpack_from('<H', vin_data, base + 4)[0]
        table.append(fidx)
    return table, n_units


# =====================================================================
# Frida JS -- penalty cave + diagnostic hooks
# =====================================================================
def build_frida_js(penalty, n_units, fidx_bin_path):
    """Build the Frida JS code with penalty cave + USEL/Prune/WSOLA hooks."""
    return r"""
'use strict';

var PENALTY = %f;
var N_UNITS = %d;
var FIDX_PATH = '%s';
var APPLY_PENALTY = (PENALTY > 0.001);

// ================================================================
// DLL addresses (SWIttsUSel.dll, no ASLR)
// ================================================================
var HOOK_ADDR     = ptr('0x8E8B854');   // fcom [esp+0x18]; fnstsw ax (6 bytes)
var RETURN_ADDR   = ptr('0x8E8B85A');
var ADDR_PRUNE_FN = ptr('0x08E88830');
var ADDR_USEL     = ptr('0x08E819E0');
var ADDR_WSOLA    = ptr('0x08EE65E0');

// ================================================================
// Load file_idx table
// ================================================================
var fidxBuf = null;
if (APPLY_PENALTY) {
    var f = new File(FIDX_PATH, 'rb');
    var hdr = f.readBytes(4);
    var nFile = new DataView(hdr).getUint32(0, true);
    var fidxBytes = f.readBytes(nFile * 4);
    f.close();
    fidxBuf = Memory.alloc(nFile * 4);
    fidxBuf.writeByteArray(fidxBytes);
    send({type:'info', msg:'Loaded ' + nFile + ' file_idx entries'});
}

// ================================================================
// Penalty cave (only if PENALTY > 0)
// ================================================================
var statsBuf = Memory.alloc(16);
statsBuf.writeU32(0);
statsBuf.add(4).writeU32(0);

if (APPLY_PENALTY) {
    var penaltyBuf = Memory.alloc(4);
    penaltyBuf.writeFloat(PENALTY);

    var cave = Memory.alloc(Process.pageSize);
    Memory.protect(cave, Process.pageSize, 'rwx');

    var off = 0;
    function emit(bytes) { for (var i = 0; i < bytes.length; i++) cave.add(off++).writeU8(bytes[i]); }
    function emitU32(v) { cave.add(off).writeU32(v); off += 4; }

    // push eax; push esi
    emit([0x50, 0x56]);

    // inc total counter
    emit([0xBE]); emitU32(statsBuf.toUInt32());
    emit([0xFF, 0x06]);

    // mov eax, [edx+0x10]  (predecessor uid)
    emit([0x8B, 0x42, 0x10]);

    // bounds check: eax < N_UNITS
    emit([0x3D]); emitU32(N_UNITS);
    var jae1 = off;
    emit([0x0F, 0x83, 0, 0, 0, 0]);

    // bounds check: ebx < N_UNITS
    emit([0x81, 0xFB]); emitU32(N_UNITS);
    var jae2 = off;
    emit([0x0F, 0x83, 0, 0, 0, 0]);

    // mov esi, fidx_table
    emit([0xBE]); emitU32(fidxBuf.toUInt32());

    // mov eax, [esi + eax*4]  (pred fidx)
    emit([0x8B, 0x04, 0x86]);

    // push edi; mov edi, [esi + ebx*4]; cmp eax, edi; pop edi
    emit([0x57]);
    emit([0x8B, 0x3C, 0x9E]);
    emit([0x39, 0xF8]);
    emit([0x5F]);

    // je .skip
    var je1 = off;
    emit([0x0F, 0x84, 0, 0, 0, 0]);

    // fadd dword [penalty]
    emit([0xD8, 0x05]); emitU32(penaltyBuf.toUInt32());

    // inc penalty counter
    emit([0xBE]); emitU32(statsBuf.toUInt32() + 4);
    emit([0xFF, 0x06]);

    // .skip:
    var skipOff = off;
    cave.add(jae1 + 2).writeS32(skipOff - (jae1 + 6));
    cave.add(jae2 + 2).writeS32(skipOff - (jae2 + 6));
    cave.add(je1 + 2).writeS32(skipOff - (je1 + 6));

    // pop esi; pop eax
    emit([0x5E, 0x58]);

    // original: fcom [esp+0x18]; fnstsw ax
    emit([0xD8, 0x54, 0x24, 0x18]);
    emit([0xDF, 0xE0]);

    // jmp RETURN_ADDR (push+ret)
    emit([0x68]); emitU32(RETURN_ADDR.toUInt32());
    emit([0xC3]);

    // Patch hook site
    Memory.patchCode(HOOK_ADDR, 6, function(code) {
        var rel = cave.toUInt32() - (HOOK_ADDR.toUInt32() + 5);
        code.writeU8(0xE9);
        code.add(1).writeS32(rel);
        code.add(5).writeU8(0x90);
    });

    send({type:'info', msg:'PENALTY HOOK at 0x8E8B854 -> cave (' + off + ' bytes), penalty=' + PENALTY});
} else {
    send({type:'info', msg:'NO PENALTY (diagnostic only mode)'});
}

// ================================================================
// Diagnostic hooks
// ================================================================
function tryU32(addr) { try { return addr.readU32(); } catch(e) { return null; } }
function tryF32(addr) { try { return addr.readFloat(); } catch(e) { return null; } }

var synthCount = 0;
var hpCount = 0;
var hpData = [];
var wsolaUnits = [];
var wsolaCallCount = 0;

// USEL hook
Interceptor.attach(ADDR_USEL, {
    onEnter: function(args) {
        synthCount++;
        hpCount = 0;
        hpData = [];
    },
    onLeave: function(retval) {
        send({type: 'synth_done', synth: synthCount, hps: hpData});
    }
});

// Prune hook (pre-prune best)
Interceptor.attach(ADDR_PRUNE_FN, {
    onEnter: function(args) {
        hpCount++;
        var thisPtr = this.context.ecx;
        var n = tryU32(thisPtr.add(0x14));
        var arrVal = tryU32(thisPtr.add(0x18));
        if (n === null || arrVal === null || n < 1 || n > 500 || arrVal < 0x100000) {
            hpData.push({hp: hpCount, uid: -1, total: 99, n_cand: 0});
            return;
        }
        var arr = ptr(arrVal);
        var bestTotal = 1e30;
        var bestUid = -1;
        for (var i = 0; i < n; i++) {
            var base = arr.add(i * 0x18);
            var uid = tryU32(base);
            var total = tryF32(base.add(0x04));
            if (total === null) continue;
            if (total < bestTotal) { bestTotal = total; bestUid = uid; }
        }
        hpData.push({hp: hpCount, uid: bestUid,
                      total: bestTotal > 1e10 ? 99 : bestTotal, n_cand: n});
    }
});

// WSOLA hook (Viterbi-selected path)
Interceptor.attach(ADDR_WSOLA, {
    onEnter: function(args) {
        wsolaCallCount++;
        var esp = this.context.esp;
        var arg4Val = tryU32(esp.add(16));
        if (arg4Val === null || arg4Val < 0x100000) {
            send({type: 'wsola', call: wsolaCallCount, units: [], error: 'bad arg4'});
            return;
        }
        var arg4 = ptr(arg4Val);
        var count = tryU32(arg4.add(0x04));
        var arrPtrVal = tryU32(arg4.add(0x08));
        send({type: 'wsola_probe', call: wsolaCallCount, count: count,
              arrPtr: arrPtrVal !== null ? '0x' + arrPtrVal.toString(16) : 'null'});
        if (count === null || arrPtrVal === null ||
            count < 1 || count > 500 || arrPtrVal < 0x100000) {
            send({type: 'wsola', call: wsolaCallCount, units: [],
                  error: 'count=' + count + ' arr=' + arrPtrVal});
            return;
        }
        var arrPtr = ptr(arrPtrVal);
        var units = [];
        for (var i = 0; i < count; i++) {
            var base = arrPtr.add(i * 0x18);
            var uid = tryU32(base);
            var f04 = tryU32(base.add(0x04));
            var f08 = tryU32(base.add(0x08));
            var f0c = tryU32(base.add(0x0c));
            units.push({uid: uid !== null ? uid : -1,
                         f04: f04 !== null ? f04 : 0,
                         f08: f08 !== null ? f08 : 0,
                         f0c: f0c !== null ? f0c : 0});
        }
        send({type: 'wsola', call: wsolaCallCount, units: units, count: count});
    }
});

// Penalty stats reporter
setInterval(function() {
    var total = statsBuf.readU32();
    var pens = statsBuf.add(4).readU32();
    if (total > 0) {
        send({type:'stats', total:total, penalties:pens,
              same_rec: total-pens,
              same_pct: (100*(total-pens)/total).toFixed(1)});
    }
}, 3000);

rpc.exports = {
    getStats: function() {
        return { total: statsBuf.readU32(), penalties: statsBuf.add(4).readU32() };
    },
    resetStats: function() {
        statsBuf.writeU32(0);
        statsBuf.add(4).writeU32(0);
    }
};

send({type: 'ready'});
""" % (penalty, n_units, fidx_bin_path.replace('\\', '\\\\'))


# =====================================================================
# Visualization
# =====================================================================
def rec_color(rec_name):
    import colorsys
    h = hash(rec_name) & 0xFFFFFFFF
    hue = (h * 0.6180339887) % 1.0
    sat = 0.45 + 0.25 * ((h >> 8) & 0xFF) / 255.0
    val = 0.70 + 0.20 * ((h >> 16) & 0xFF) / 255.0
    r, g, b = colorsys.hsv_to_rgb(hue, sat, val)
    return (r, g, b)


def generate_timeline_viz(comparison, text, voice, penalty, out_path):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt

    n = len(comparison)
    if n == 0:
        return

    recs = [c['ws_rec'] for c in comparison]
    phones = [c['ws_phone'] for c in comparison]
    halves = [c['ws_half'] for c in comparison]
    f0cs = [c['ws_f0c'] for c in comparison]
    hps = [c['hp'] for c in comparison]
    max_f0c = max(f0cs) if max(f0cs) > 0 else 1

    switches = [i for i in range(1, n) if recs[i] != recs[i-1]]

    fig_w = max(22, n * 0.3)
    fig, ax = plt.subplots(figsize=(fig_w, 6))

    for i in range(n):
        height = f0cs[i] / max_f0c if max_f0c > 0 else 0
        ax.bar(i, height, width=0.85, color=rec_color(recs[i]),
               edgecolor='black', linewidth=0.3)

    for si in switches:
        ax.axvline(x=si - 0.5, color='red', linewidth=1.5, zorder=5)

    for i in range(n):
        h_tag = '1' if halves[i] == '1st' else '2'
        ax.text(i, -0.08, '%s.%s' % (phones[i], h_tag),
                ha='center', va='top', fontsize=5.5, rotation=90, family='monospace')
        ax.text(i, -0.28, str(hps[i]),
                ha='center', va='top', fontsize=4.5, color='gray', family='monospace')

    run_starts = [0] + switches
    for rs_idx, rs in enumerate(run_starts):
        re_end = switches[rs_idx] if rs_idx < len(switches) else n
        mid = (rs + re_end - 1) / 2.0
        lbl = recs[rs][:25]
        ax.text(mid, 1.05, lbl, ha='center', va='bottom',
                fontsize=5, rotation=90, family='monospace', clip_on=False)

    ax.set_xlim(-0.5, n - 0.5)
    ax.set_ylim(-0.35, 1.6)
    ax.set_ylabel('Relative f0c', fontsize=9)
    ax.set_title('Viterbi Path -- %s -- penalty=%.1f\n"%s"' % (voice, penalty, text),
                 fontsize=11, pad=40)
    ax.set_xticks([])
    for s in ('top', 'right', 'bottom'):
        ax.spines[s].set_visible(False)

    plt.tight_layout()
    plt.savefig(out_path, dpi=150, bbox_inches='tight')
    plt.close(fig)


# =====================================================================
# Main
# =====================================================================
def main():
    PROJ = _detect_proj_root()
    SYNTH_EXE = os.path.join(PROJ, "bin", "spfy_dumpwav.exe")

    VOICE_CONFIGS = {
        'mara': {
            'vin': os.path.join(PROJ, "en-US", "mara", "mara.vin"),
            'vdb': os.path.join(PROJ, "en-US", "mara", "mara8.vdb"),
            'tom_vin': os.path.join(PROJ, "en-US", "tom", "tom.vin"),
            'n_units': 169579,
        },
        'tom': {
            'vin': os.path.join(PROJ, "en-US", "tom", "tom.vin"),
            'vdb': os.path.join(PROJ, "en-US", "tom", "tom8.vdb"),
            'tom_vin': os.path.join(PROJ, "en-US", "tom", "tom.vin"),
            'n_units': 169579,
        },
    }

    parser = argparse.ArgumentParser(
        description='Viterbi penalty hook + ground truth diagnostic')
    parser.add_argument('text', help='Text to synthesize')
    parser.add_argument('penalty', nargs='?', type=float, default=50.0,
                        help='Recording-switch penalty (0 = diagnostic only)')
    parser.add_argument('--voice', default='mara', choices=list(VOICE_CONFIGS.keys()))
    args = parser.parse_args()

    text = args.text
    penalty = args.penalty
    voice = args.voice
    vcfg = VOICE_CONFIGS[voice]

    tag = "%s_p%.0f" % (voice, penalty)
    OUT_DIR = os.path.join(PROJ, "diagnostics", "diag_%s" % tag)
    os.makedirs(OUT_DIR, exist_ok=True)
    wav_path = os.path.join(OUT_DIR, "output.wav")

    # Use temp directory for fidx table (avoid hardcoded paths)
    FIDX_BIN = os.path.join(tempfile.gettempdir(), "_fidx_table.bin")

    # -------------------------------------------------------------------
    # 1. Load VIN, build file_idx table
    # -------------------------------------------------------------------
    print("[*] Loading VIN for voice '%s'..." % voice)
    vin_data = load_xor(vcfg['vin'])
    actual_n = read_n_units(vin_data)
    N_UNITS = actual_n if actual_n > 0 else vcfg['n_units']
    filenames = parse_filenames(vin_data)
    if not filenames:
        tvin = load_xor(vcfg['tom_vin'])
        filenames = parse_filenames(tvin)
    voice_ud = find_unit_data(vin_data, N_UNITS)
    if voice_ud is None:
        print("[!] Could not find unit data in VIN!")
        sys.exit(1)

    fidx_table, n_units = build_fidx_table(vin_data, N_UNITS)
    if fidx_table is None:
        print("[!] Failed to build file_idx table!")
        sys.exit(1)

    # Write binary file for Frida to read
    fidx_packed = struct.pack('<%dI' % n_units, *fidx_table)
    with open(FIDX_BIN, 'wb') as f:
        f.write(struct.pack('<I', n_units))
        f.write(fidx_packed)
    print("[*] %d units, %d recordings, penalty=%.1f" % (
        n_units, len(set(fidx_table)), penalty))

    # -------------------------------------------------------------------
    # 2. Attach Frida
    # -------------------------------------------------------------------
    print("[*] Attaching to %s..." % TARGET)
    try:
        session = frida.attach(TARGET)
    except frida.ProcessNotFoundError:
        print("[!] %s not running. Start it first." % TARGET)
        sys.exit(1)

    synth_results = {}
    wsola_results = []
    ready_event = threading.Event()

    def on_message(message, data):
        if message["type"] == "send":
            p = message["payload"]
            t = p.get("type", "")
            if t == "ready":
                ready_event.set()
            elif t == "synth_done":
                synth_results[p["synth"]] = p
            elif t == "wsola":
                wsola_results.append(p)
            elif t == "wsola_probe":
                print("  [WSOLA] call=%d count=%s arr=%s" % (
                    p.get("call", 0), p.get("count", "?"), p.get("arrPtr", "?")))
            elif t == "stats":
                print("  [stats] transitions=%d penalties=%d same_rec=%d (%.1s%%)" % (
                    p['total'], p['penalties'], p['same_rec'], p['same_pct']))
            elif t == "info":
                print("  [*] %s" % p.get("msg", ""))
        elif message["type"] == "error":
            print("  [!] %s" % message.get("stack", message.get("description", "")))

    js = build_frida_js(penalty, n_units, FIDX_BIN)
    script = session.create_script(js)
    script.on("message", on_message)
    script.load()
    ready_event.wait(timeout=10)
    print("[*] Hooks ready. Synthesizing...")

    # -------------------------------------------------------------------
    # 3. Synthesize
    # -------------------------------------------------------------------
    print("[*] Text: %s" % text)
    result = subprocess.run(
        [SYNTH_EXE, text, wav_path],
        capture_output=True, text=True, timeout=30
    )
    if result.returncode != 0:
        print("[!] Synth error: %s" % result.stderr.strip())
    time.sleep(1.5)

    # Read penalty stats before detaching
    pen_stats = None
    try:
        pen_stats = script.exports_sync.get_stats()
    except Exception:
        pass

    session.detach()

    # -------------------------------------------------------------------
    # 4. Collect captured data
    # -------------------------------------------------------------------
    pre_prune_hps = []
    for sid in sorted(synth_results.keys()):
        pre_prune_hps.extend(synth_results[sid].get("hps", []))
    print("[*] Captured %d pre-prune HPs, %d WSOLA calls" % (
        len(pre_prune_hps), len(wsola_results)))

    wsola_uids = []
    for wr in wsola_results:
        if wr.get("error"):
            print("  WSOLA error: %s" % wr["error"])
        for u in wr.get("units", []):
            wsola_uids.append(u)

    if not pre_prune_hps or not wsola_uids:
        print("[!] Missing data! PP=%d WS=%d" % (len(pre_prune_hps), len(wsola_uids)))
        sys.exit(1)

    # -------------------------------------------------------------------
    # 5. Comparison report
    # -------------------------------------------------------------------
    n_pp = len(pre_prune_hps)
    n_ws = len(wsola_uids)
    n_compare = min(n_pp, n_ws)

    lines = []
    lines.append("=" * 140)
    lines.append("VITERBI PENALTY DIAGNOSTIC -- penalty=%.1f" % penalty)
    lines.append("Text: %s" % text)
    lines.append("Voice: %s  |  Pre-prune HPs: %d  |  WSOLA units: %d" % (voice, n_pp, n_ws))
    if pen_stats:
        lines.append("Penalty stats: transitions=%d penalties=%d same_rec=%d (%.1f%%)" % (
            pen_stats['total'], pen_stats['penalties'],
            pen_stats['total'] - pen_stats['penalties'],
            100 * (pen_stats['total'] - pen_stats['penalties']) / max(1, pen_stats['total'])))
    lines.append("=" * 140)

    header = "%4s  %8s %8s %5s  %-6s %-6s  %-4s %-4s  %-20s %-20s  %s" % (
        "HP", "PP_UID", "WS_UID", "Match",
        "PP_Ph", "WS_Ph", "PP_H", "WS_H",
        "PP_Recording", "WS_Recording", "Flags")
    lines.append("")
    lines.append(header)
    lines.append("-" * 140)

    comparison_data = []
    matches = mismatches = pp_switches = ws_switches = 0
    pp_prev_rec = ws_prev_rec = None

    for i in range(n_compare):
        pp_uid = pre_prune_hps[i]["uid"]
        ws_uid = wsola_uids[i]["uid"]

        pp_info = uid_info(pp_uid, vin_data, voice_ud, filenames, N_UNITS)
        ws_info = uid_info(ws_uid, vin_data, voice_ud, filenames, N_UNITS)

        match = (pp_uid == ws_uid)
        if match:
            matches += 1
        else:
            mismatches += 1

        flags = []
        if not match:
            flags.append("MISMATCH")
        if pp_info['phone'] != ws_info['phone']:
            flags.append("PHONE-DIFF")
        if pp_info['half'] != ws_info['half']:
            flags.append("HALF-DIFF")

        pp_rec = pp_info['rec_name']
        ws_rec = ws_info['rec_name']
        if pp_prev_rec is not None and pp_rec != pp_prev_rec and not pp_info['is_silence']:
            pp_switches += 1
            flags.append("PP-REC-SW")
        if ws_prev_rec is not None and ws_rec != ws_prev_rec and not ws_info['is_silence']:
            ws_switches += 1
            flags.append("WS-REC-SW")

        pp_prev_rec = pp_rec if not pp_info['is_silence'] else pp_prev_rec
        ws_prev_rec = ws_rec if not ws_info['is_silence'] else ws_prev_rec

        line = "%4d  %8d %8d %5s  %-6s %-6s  %-4s %-4s  %-20s %-20s  %s" % (
            i + 1, pp_uid, ws_uid,
            "Y" if match else "***N",
            pp_info['phone'], ws_info['phone'],
            pp_info['half'], ws_info['half'],
            pp_rec[:20], ws_rec[:20],
            " ".join(flags))
        lines.append(line)

        comparison_data.append({
            'hp': i + 1, 'pp_uid': pp_uid, 'ws_uid': ws_uid, 'match': match,
            'pp_phone': pp_info['phone'], 'ws_phone': ws_info['phone'],
            'pp_half': pp_info['half'], 'ws_half': ws_info['half'],
            'pp_rec': pp_rec, 'ws_rec': ws_rec,
            'pp_lp': pp_info['lp'], 'pp_dl': pp_info['dl'],
            'ws_lp': ws_info['lp'], 'ws_dl': ws_info['dl'],
            'ws_f04': wsola_uids[i].get('f04', 0),
            'ws_f08': wsola_uids[i].get('f08', 0),
            'ws_f0c': wsola_uids[i].get('f0c', 0),
        })

    # -------------------------------------------------------------------
    # 6. Summary
    # -------------------------------------------------------------------
    lines.append("")
    lines.append("=" * 80)
    lines.append("SUMMARY (penalty=%.1f)" % penalty)
    lines.append("=" * 80)
    lines.append("  Compared:     %d halfphones" % n_compare)
    lines.append("  UID matches:  %d (%.1f%%)" % (matches, 100.0 * matches / max(1, n_compare)))
    lines.append("  UID mismatches: %d (%.1f%%)" % (mismatches, 100.0 * mismatches / max(1, n_compare)))

    phone_matches = sum(1 for c in comparison_data if c['pp_phone'] == c['ws_phone'])
    lines.append("  Phone matches:    %d (%.1f%%)" % (phone_matches, 100.0 * phone_matches / max(1, n_compare)))

    lines.append("")
    lines.append("  Pre-prune recording switches (speech): %d" % pp_switches)
    lines.append("  WSOLA/Viterbi recording switches (speech): %d" % ws_switches)

    if pen_stats:
        lines.append("")
        lines.append("  Penalty hook: %d total transitions, %d penalties applied" % (
            pen_stats['total'], pen_stats['penalties']))
        lines.append("  Same-rec transitions: %d (%.1f%%)" % (
            pen_stats['total'] - pen_stats['penalties'],
            100 * (pen_stats['total'] - pen_stats['penalties']) / max(1, pen_stats['total'])))

    # Run lengths
    ws_runs = []
    cur_run = 1
    for i in range(1, n_compare):
        ws_cur = comparison_data[i]
        ws_prev = comparison_data[i-1]
        if ws_cur['ws_phone'] == 'pau' or ws_prev['ws_phone'] == 'pau':
            if cur_run > 0:
                ws_runs.append(cur_run)
            cur_run = 1 if ws_cur['ws_phone'] != 'pau' else 0
            continue
        if ws_cur['ws_rec'] == ws_prev['ws_rec']:
            cur_run += 1
        else:
            ws_runs.append(cur_run)
            cur_run = 1
    if cur_run > 0:
        ws_runs.append(cur_run)

    if ws_runs:
        lines.append("")
        lines.append("  WSOLA path run lengths (same-recording consecutive):")
        lines.append("    mean=%.1f  median=%.1f  min=%d  max=%d" % (
            np.mean(ws_runs), np.median(ws_runs), min(ws_runs), max(ws_runs)))

    # -------------------------------------------------------------------
    # 7. Output
    # -------------------------------------------------------------------
    summary_text = "\n".join(lines)
    print(summary_text)

    summary_path = os.path.join(OUT_DIR, "summary.txt")
    with open(summary_path, 'w') as f:
        f.write(summary_text)
    print("\nSummary: %s" % summary_path)

    import json
    json_path = os.path.join(OUT_DIR, "data.json")
    with open(json_path, 'w') as f:
        json.dump({
            'text': text, 'voice': voice, 'penalty': penalty,
            'pen_stats': pen_stats,
            'pre_prune_hps': pre_prune_hps,
            'wsola_uids': wsola_uids,
            'comparison': comparison_data,
            'n_pp': n_pp, 'n_ws': n_ws,
            'matches': matches, 'mismatches': mismatches,
            'pp_switches': pp_switches, 'ws_switches': ws_switches,
        }, f, indent=2)
    print("JSON: %s" % json_path)

    if os.path.exists(wav_path):
        print("WAV: %s (%d bytes)" % (wav_path, os.path.getsize(wav_path)))

    viz_path = os.path.join(OUT_DIR, "timeline.png")
    try:
        generate_timeline_viz(comparison_data, text, voice, penalty, viz_path)
        print("Viz: %s" % viz_path)
    except Exception as e:
        print("[!] Viz failed: %s" % e)


if __name__ == '__main__':
    main()
