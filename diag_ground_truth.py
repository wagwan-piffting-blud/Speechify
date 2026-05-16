"""Ground-truth diagnostic: compare pre-prune best UIDs vs actual Viterbi path.

diag_stutter.py captures the pre-prune best candidate (lowest total_score at
candidate+0x04 BEFORE pruning). But experiment 10 proved the Viterbi forward
pass does NOT read total_score from +0x04 -- it recomputes costs from component
fields and join cost hash lookups. The reported 70% switch rate may describe
the WRONG units.

This script hooks BOTH:
  1. PRUNE_FN onEnter  -> pre-prune best
  2. WSOLA_CONCAT onEnter -> actual Viterbi-selected path (ground truth)

Then compares them side-by-side.

Usage:
    python diag_ground_truth.py "Please call us at five five five, zero one two three."
    python diag_ground_truth.py "Your text" --voice tom   (baseline comparison)
"""
import argparse
import audioop
import frida
import json
import os
import struct
import subprocess
import sys
import threading
import time

import numpy as np

# ---------------------------------------------------------------------------
# Paths and constants
# ---------------------------------------------------------------------------
TARGET = "Speechify.exe"


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


PROJ = _detect_proj_root()
SYNTH_EXE = os.path.join(PROJ, "bin", "spfy_dumpwav.exe")

XOR = 0xCE
UNIT_SIZE = 29

VOICE_CONFIGS = {
    'mara': {
        'vin': os.path.join(PROJ, "en-US", "aimara", "aimara.vin"),
        'vdb': os.path.join(PROJ, "en-US", "aimara", "aimara8.vdb"),
        'tom_vin': os.path.join(PROJ, "en-US", "tom", "tom.vin"),
        'n_units': 169579,
    },
    'tom': {
        'vin': os.path.join(PROJ, "en-US", "tom", "tom.vin"),
        'vdb': os.path.join(PROJ, "en-US", "tom", "tom8.vdb"),
        'tom_vin': os.path.join(PROJ, "en-US", "tom", "tom.vin"),
        'n_units': 169579,
    },
    'craig': {
        'vin': os.path.join(PROJ, "en-US", "aicraig", "aicraig.vin"),
        'vdb': os.path.join(PROJ, "en-US", "aicraig", "aicraig8.vdb"),
        'tom_vin': os.path.join(PROJ, "en-US", "tom", "tom.vin"),
        'n_units': 169579,
    }
}

PHONE_LABELS = [
    'aa', 'ae', 'ah', 'ao', 'aw', 'ax', 'ay', 'b', 'ch', 'dx',
    'd',  'dh', 'eh', 'el', 'er', 'en', 'ey', 'f', 'g',  'hh',
    'ih', 'ix', 'iy', 'jh', 'k',  'l',  'm',  'n', 'ng', 'ow',
    'oy', 'p',  'pau','r',  's',  'sh', 't',  'th','uh', 'uw',
    'v',  'w',  'xx', 'y',  'z',  'zh',
]
SILENCE_PCS = {32}  # pau


# ---------------------------------------------------------------------------
# Helpers (reused from diag_stutter.py)
# ---------------------------------------------------------------------------
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
    """Read actual unit count from VIN cnts chunk."""
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
    feat = tvin[feat_off:feat_off+feat_sz]
    fn_idx = feat.find(b'filename')
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
    """Extract phone, half, fidx, lp, dl, rec_name for a unit ID."""
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


# ---------------------------------------------------------------------------
# Frida JS -- hooks BOTH prune (pre-prune best) AND WSOLA (actual path)
# ---------------------------------------------------------------------------
JS_CODE = r"""
'use strict';

var ADDR_PRUNE_FN   = ptr('0x08E88830');
var ADDR_USEL       = ptr('0x08E819E0');
var ADDR_WSOLA_CONCAT = ptr('0x08EE65E0');

var synthCount = 0;
var hpCount = 0;
var hpData = [];       // pre-prune best per HP
var wsolaUnits = [];   // actual Viterbi-selected units (WSOLA input)
var wsolaCallCount = 0;

function tryU32(addr) {
    try { return addr.readU32(); } catch(e) { return null; }
}
function tryF32(addr) {
    try { return addr.readFloat(); } catch(e) { return null; }
}

// --- Hook USEL orchestrator (tracks synthesis calls) ---
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

// --- Hook prune to capture pre-prune best (same as diag_stutter.py) ---
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
            var uid   = tryU32(base);
            var total = tryF32(base.add(0x04));
            if (total === null) continue;
            if (total < bestTotal) {
                bestTotal = total;
                bestUid = uid;
            }
        }

        hpData.push({
            hp: hpCount,
            uid: bestUid,
            total: bestTotal > 1e10 ? 99 : bestTotal,
            n_cand: n
        });
    }
});

// --- Hook WSOLA concat to capture the actual Viterbi-selected path ---
// Confirmed from probing run: arg4 (esp+16) holds the output struct.
//   [arg4+0x04] = unit count (matches HP count: 44 + 28 = 72)
//   [arg4+0x08] = pointer to unit array, stride 0x18, uid at +0x00
Interceptor.attach(ADDR_WSOLA_CONCAT, {
    onEnter: function(args) {
        wsolaCallCount++;
        var esp = this.context.esp;
        var arg4Val = tryU32(esp.add(16));
        if (arg4Val === null || arg4Val < 0x100000) {
            send({type: 'wsola', call: wsolaCallCount, units: [],
                  error: 'bad arg4=' + arg4Val});
            return;
        }
        var arg4 = ptr(arg4Val);
        var count = tryU32(arg4.add(0x04));
        var arrPtrVal = tryU32(arg4.add(0x08));

        send({type: 'wsola_probe', call: wsolaCallCount,
              count: count, arrPtr: arrPtrVal !== null ? '0x' + arrPtrVal.toString(16) : 'null'});

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
            units.push({
                uid: uid !== null ? uid : -1,
                f04: f04 !== null ? f04 : 0,
                f08: f08 !== null ? f08 : 0,
                f0c: f0c !== null ? f0c : 0
            });
        }
        send({type: 'wsola', call: wsolaCallCount, units: units, count: count,
              src: 'arg4+0x08/+0x04'});
    }
});

send({type: 'ready'});
"""


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(description='Ground truth diagnostic')
    parser.add_argument('text', help='Text to synthesize')
    parser.add_argument('--voice', default='mara',
                        choices=list(VOICE_CONFIGS.keys()),
                        help='Voice to diagnose (default: mara)')
    args = parser.parse_args()

    text = args.text
    voice = args.voice
    vcfg = VOICE_CONFIGS[voice]
    N_UNITS = vcfg['n_units']  # default; overridden below from VIN cnts

    tag = voice + '_gt'
    OUT_DIR = os.path.join(PROJ, "diagnostics", "diag_%s" % tag)
    os.makedirs(OUT_DIR, exist_ok=True)
    wav_path = os.path.join(OUT_DIR, "diag_%s.wav" % tag)
    summary_path = os.path.join(OUT_DIR, "diag_%s_summary.txt" % tag)
    json_path = os.path.join(OUT_DIR, "diag_%s.json" % tag)

    # -------------------------------------------------------------------
    # 1. Frida capture
    # -------------------------------------------------------------------
    print("Attaching to %s ..." % TARGET)
    try:
        session = frida.attach(TARGET)
    except frida.ProcessNotFoundError:
        print("ERROR: %s not running. Start it first." % TARGET)
        sys.exit(1)

    synth_results = {}
    wsola_results = []
    ready_event = threading.Event()

    def on_message(message, data):
        if message["type"] == "send":
            payload = message["payload"]
            t = payload.get("type")
            if t == "ready":
                ready_event.set()
            elif t == "synth_done":
                synth_results[payload["synth"]] = payload
            elif t == "wsola":
                wsola_results.append(payload)
            elif t == "wsola_probe":
                print("  [WSOLA] call=%d count=%s arr=%s" % (
                    payload.get("call", 0), payload.get("count", "?"),
                    payload.get("arrPtr", "?")))
            elif t in ("info", "warning"):
                print("  [Frida] %s" % payload.get("msg", ""))
        elif message["type"] == "error":
            print("  [Frida ERROR] %s" % message.get("stack", message))

    script = session.create_script(JS_CODE)
    script.on("message", on_message)
    script.load()
    ready_event.wait(timeout=5)
    print("Frida ready.")

    print("Synthesizing: %s" % text)
    result = subprocess.run(
        [SYNTH_EXE, text, wav_path],
        capture_output=True, text=True, timeout=30
    )
    if result.returncode != 0:
        print("Synth error: %s" % result.stderr.strip())
    time.sleep(1.0)
    session.detach()

    # Collect pre-prune HPs from all USEL calls
    pre_prune_hps = []
    for sid in sorted(synth_results.keys()):
        pre_prune_hps.extend(synth_results[sid].get("hps", []))
    print("Captured %d pre-prune halfphones across %d USEL calls" % (
        len(pre_prune_hps), len(synth_results)))

    # Collect WSOLA units from all WSOLA calls
    wsola_uids = []
    for wr in wsola_results:
        if wr.get("error"):
            print("  WSOLA error: %s" % wr["error"])
        for u in wr.get("units", []):
            wsola_uids.append(u)
    print("Captured %d WSOLA (Viterbi-selected) units across %d WSOLA calls" % (
        len(wsola_uids), len(wsola_results)))

    if not pre_prune_hps:
        print("ERROR: No pre-prune data captured!")
        sys.exit(1)
    if not wsola_uids:
        print("ERROR: No WSOLA data captured!")
        sys.exit(1)

    # -------------------------------------------------------------------
    # 2. Load voice data
    # -------------------------------------------------------------------
    print("Loading VIN/VDB for voice '%s'..." % voice)
    vin_data = load_xor(vcfg['vin'])
    # Read actual unit count from VIN (may differ from Tom if extras added)
    actual_n = read_n_units(vin_data)
    if actual_n > 0:
        N_UNITS = actual_n
    # Parse filenames from mara.vin (has extra filenames), fall back to tom
    filenames = parse_filenames(vin_data)
    if not filenames:
        tvin = load_xor(vcfg['tom_vin'])
        filenames = parse_filenames(tvin)
    voice_ud = find_unit_data(vin_data, N_UNITS)

    if voice_ud is None:
        print("ERROR: Could not find unit data in VIN!")
        sys.exit(1)

    # -------------------------------------------------------------------
    # 3. Side-by-side comparison
    # -------------------------------------------------------------------
    n_pp = len(pre_prune_hps)
    n_ws = len(wsola_uids)

    lines = []
    lines.append("=" * 140)
    lines.append("GROUND TRUTH DIAGNOSTIC: Pre-Prune Best vs Viterbi-Selected Path")
    lines.append("Text: %s" % text)
    lines.append("Voice: %s" % voice)
    lines.append("Pre-prune HPs: %d  |  WSOLA units: %d" % (n_pp, n_ws))
    lines.append("=" * 140)

    # The WSOLA unit count may differ from the HP count.
    # WSOLA receives the final selected path which has exactly one unit per HP.
    # But the counts might differ if USEL calls don't map 1:1 to WSOLA calls.
    # We'll align by position and compare as far as possible.

    n_compare = min(n_pp, n_ws)
    matches = 0
    mismatches = 0
    pp_switches = 0
    ws_switches = 0
    pp_prev_rec = None
    ws_prev_rec = None

    header = "%4s  %8s %8s %5s  %-6s %-6s  %-4s %-4s  %-20s %-20s  %s" % (
        "HP", "PP_UID", "WS_UID", "Match",
        "PP_Ph", "WS_Ph", "PP_H", "WS_H",
        "PP_Recording", "WS_Recording", "Flags")
    lines.append("")
    lines.append(header)
    lines.append("-" * 140)

    comparison_data = []

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

        # Track recording switches for both paths
        pp_rec = pp_info['rec_name']
        ws_rec = ws_info['rec_name']
        if pp_prev_rec is not None and pp_rec != pp_prev_rec:
            if not pp_info['is_silence']:
                pp_switches += 1
        if ws_prev_rec is not None and ws_rec != ws_prev_rec:
            if not ws_info['is_silence']:
                ws_switches += 1
                if 'MISMATCH' not in flags:
                    pass  # switch in both
                flags.append("WS-REC-SW")
        if pp_prev_rec is not None and pp_rec != pp_prev_rec:
            if not pp_info['is_silence']:
                flags.append("PP-REC-SW")

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
            'hp': i + 1,
            'pp_uid': pp_uid, 'ws_uid': ws_uid,
            'match': match,
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
    # 4. Summary statistics
    # -------------------------------------------------------------------
    lines.append("")
    lines.append("=" * 80)
    lines.append("SUMMARY")
    lines.append("=" * 80)
    lines.append("  Compared:     %d halfphones" % n_compare)
    lines.append("  UID matches:  %d (%.1f%%)" % (matches, 100.0 * matches / max(1, n_compare)))
    lines.append("  UID mismatches: %d (%.1f%%)" % (mismatches, 100.0 * mismatches / max(1, n_compare)))

    # Phone-level comparison
    phone_matches = sum(1 for c in comparison_data if c['pp_phone'] == c['ws_phone'])
    phone_mismatches = n_compare - phone_matches
    lines.append("")
    lines.append("  Phone matches:    %d (%.1f%%)" % (phone_matches, 100.0 * phone_matches / max(1, n_compare)))
    lines.append("  Phone mismatches: %d (%.1f%%)" % (phone_mismatches, 100.0 * phone_mismatches / max(1, n_compare)))

    lines.append("")
    lines.append("  Pre-prune recording switches (speech): %d" % pp_switches)
    lines.append("  WSOLA/Viterbi recording switches (speech): %d" % ws_switches)

    # Run lengths for WSOLA path
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

    # List all mismatched HPs with detail
    if mismatches > 0:
        lines.append("")
        lines.append("MISMATCHED HALFPHONES (Viterbi chose differently from pre-prune best):")
        lines.append("%4s  %8s %8s  %-6s %-6s  %-20s %-20s  PP_lp/dl  WS_lp/dl" % (
            "HP", "PP_UID", "WS_UID", "PP_Ph", "WS_Ph", "PP_Rec", "WS_Rec"))
        lines.append("-" * 120)
        for c in comparison_data:
            if not c['match']:
                lines.append("%4d  %8d %8d  %-6s %-6s  %-20s %-20s  %d/%d     %d/%d" % (
                    c['hp'], c['pp_uid'], c['ws_uid'],
                    c['pp_phone'], c['ws_phone'],
                    c['pp_rec'][:20], c['ws_rec'][:20],
                    c['pp_lp'], c['pp_dl'],
                    c['ws_lp'], c['ws_dl']))

    # If counts differ, note it
    if n_pp != n_ws:
        lines.append("")
        lines.append("WARNING: Count mismatch! Pre-prune HPs=%d, WSOLA units=%d" % (n_pp, n_ws))
        lines.append("This could mean:")
        lines.append("  - WSOLA receives a different number of units than HP count")
        lines.append("  - The unit list has padding/header entries")
        lines.append("  - Multiple USEL calls map differently to WSOLA calls")
        if n_ws > n_pp:
            lines.append("")
            lines.append("Extra WSOLA units beyond pre-prune range:")
            for i in range(n_pp, min(n_ws, n_pp + 20)):
                ws_uid = wsola_uids[i]["uid"]
                ws_info = uid_info(ws_uid, vin_data, voice_ud, filenames, N_UNITS)
                lines.append("  WS[%d] uid=%d phone=%s half=%s rec=%s lp=%d dl=%d" % (
                    i + 1, ws_uid, ws_info['phone'], ws_info['half'],
                    ws_info['rec_name'], ws_info['lp'], ws_info['dl']))

    # Raw WSOLA data dump (first 20 entries for debugging stride/format)
    lines.append("")
    lines.append("RAW WSOLA UNIT DATA (first 20 entries, for stride/format validation):")
    lines.append("%4s  %8s  %10s  %10s  %10s" % ("Idx", "UID", "+0x04", "+0x08", "+0x0C"))
    for i in range(min(20, n_ws)):
        u = wsola_uids[i]
        lines.append("%4d  %8d  0x%08X  0x%08X  0x%08X" % (
            i, u['uid'], u['f04'], u['f08'], u['f0c']))

    # -------------------------------------------------------------------
    # 5. Write output
    # -------------------------------------------------------------------
    summary_text = "\n".join(lines)
    print(summary_text)

    with open(summary_path, 'w') as f:
        f.write(summary_text)
    print("\nSummary written to: %s" % summary_path)

    with open(json_path, 'w') as f:
        json.dump({
            'text': text,
            'voice': voice,
            'pre_prune_hps': pre_prune_hps,
            'wsola_uids': wsola_uids,
            'comparison': comparison_data,
            'n_pp': n_pp, 'n_ws': n_ws,
            'matches': matches, 'mismatches': mismatches,
            'pp_switches': pp_switches, 'ws_switches': ws_switches,
        }, f, indent=2)
    print("JSON written to: %s" % json_path)

    if os.path.exists(wav_path):
        sz = os.path.getsize(wav_path)
        print("WAV written to: %s (%d bytes)" % (wav_path, sz))

    # -------------------------------------------------------------------
    # 6. Visualization
    # -------------------------------------------------------------------
    viz_path = os.path.join(OUT_DIR, f"diag_{voice}_gt_viz.png")
    print("Generating visualization -> %s" % viz_path)
    generate_timeline_viz(comparison_data, wsola_uids, text, voice, viz_path)
    print("Visualization saved to: %s" % viz_path)


def rec_color(rec_name):
    """Generate a consistent RGB color from a recording name."""
    h = hash(rec_name) & 0xFFFFFFFF
    # Use golden-ratio-based hue spacing for good distribution
    hue = (h * 0.6180339887) % 1.0
    sat = 0.45 + 0.25 * ((h >> 8) & 0xFF) / 255.0
    val = 0.70 + 0.20 * ((h >> 16) & 0xFF) / 255.0
    # HSV to RGB
    import colorsys
    r, g, b = colorsys.hsv_to_rgb(hue, sat, val)
    return (r, g, b)


def generate_timeline_viz(comparison, wsola_uids, text, voice, out_path):
    """Create a wide horizontal timeline PNG of the WSOLA unit sequence."""
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import matplotlib.patches as mpatches

    n = len(comparison)
    if n == 0:
        print("  No comparison data to visualize.")
        return

    # Gather data
    recs = [c['ws_rec'] for c in comparison]
    phones = [c['ws_phone'] for c in comparison]
    halves = [c['ws_half'] for c in comparison]
    f0cs = [c['ws_f0c'] for c in comparison]
    hps = [c['hp'] for c in comparison]

    max_f0c = max(f0cs) if max(f0cs) > 0 else 1

    # Detect recording switches
    switches = []
    for i in range(1, n):
        if recs[i] != recs[i - 1]:
            switches.append(i)

    # Figure sizing: ~0.3 inches per unit, min 22 inches wide
    fig_w = max(22, n * 0.3)
    fig_h = 6
    fig, ax = plt.subplots(figsize=(fig_w, fig_h))

    bar_width = 0.85

    # Draw bars
    for i in range(n):
        height = f0cs[i] / max_f0c if max_f0c > 0 else 0
        color = rec_color(recs[i])
        ax.bar(i, height, width=bar_width, color=color, edgecolor='black',
               linewidth=0.3, align='center')

    # Draw red vertical lines at recording switches
    for si in switches:
        ax.axvline(x=si - 0.5, color='red', linewidth=1.5, linestyle='-', zorder=5)

    # Phone labels below each bar
    for i in range(n):
        label = phones[i]
        h_tag = '1' if halves[i] == '1st' else '2'
        ax.text(i, -0.08, '%s.%s' % (label, h_tag), ha='center', va='top',
                fontsize=5.5, rotation=90, family='monospace')

    # HP index labels at very bottom
    for i in range(n):
        ax.text(i, -0.28, str(hps[i]), ha='center', va='top',
                fontsize=4.5, color='gray', family='monospace')

    # Recording names at top of each run
    # Identify runs
    run_starts = [0]
    for si in switches:
        run_starts.append(si)

    for rs_idx, rs in enumerate(run_starts):
        re_end = switches[rs_idx] if rs_idx < len(switches) else n
        mid = (rs + re_end - 1) / 2.0
        rec_label = recs[rs]
        # Truncate long names
        if len(rec_label) > 25:
            rec_label = rec_label[:22] + '...'
        ax.text(mid, 1.05, rec_label, ha='center', va='bottom',
                fontsize=5, rotation=90, color='black', family='monospace',
                clip_on=False)

    # Axes formatting
    ax.set_xlim(-0.5, n - 0.5)
    ax.set_ylim(-0.35, 1.6)
    ax.set_ylabel('Relative f0c (output duration)', fontsize=9)
    ax.set_title('WSOLA Unit Timeline -- Voice: %s\n"%s"' % (voice, text),
                 fontsize=11, pad=40)
    ax.set_xticks([])
    ax.spines['top'].set_visible(False)
    ax.spines['right'].set_visible(False)
    ax.spines['bottom'].set_visible(False)

    # Legend note
    ax.text(0.01, -0.12,
            'Red lines = recording switch | Bar height = f0c / max(f0c) | '
            'Phone.half below each bar | HP index in gray',
            transform=ax.transAxes, fontsize=7, color='#555555',
            va='top')

    plt.tight_layout()
    plt.savefig(out_path, dpi=150, bbox_inches='tight')
    plt.close(fig)


if __name__ == '__main__':
    main()
