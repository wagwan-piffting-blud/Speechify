"""build_mara_extra.py -- Replace low-quality units with extra recording data.

REPLACE strategy: Instead of appending new UIDs (which the engine can't use
because the hash structure is immutable), we REPLACE existing low-quality
Tom units with extra recording data. Replaced units keep their original UID,
prosodic context fields, and prsl membership -- only lp, dl, file_idx, f0,
and f0_context are updated.

Run AFTER build_mara_voice.py, BEFORE build_mara_rest.py and build_mara_trees.py.

Pipeline order:
  1. build_mara_voice.py        (base voice from Tom's unit table)
  2. build_mara_extra.py        (THIS SCRIPT -- replace worst units with extras)
  3. build_mara_rest.py         (hash + prsl + mean over all units)
  4. build_mara_trees.py        (CART tree leaf scaling)

Usage:
  python build_mara_extra.py [--workers N]

State caching:
  Per-recording results are saved to build_extra_state.pkl alongside the
  output files.  On subsequent runs, a recording is reused from cache when:
    - the WAV file has not changed (mtime + size)
    - the TextGrid file has not changed (mtime + size)
  Only changed or new recordings are recomputed.

Inputs  (read-only): en-US/mara/output/extra_wavs/*.wav
                     en-US/mara/output/extra_tg/*.TextGrid
                     en-US/tom/tom.vin (for f0_context regression)
Inputs  (modified):  en-US/mara/mara.vin, en-US/mara/mara8.vdb
Output (overwritten): en-US/mara/mara.vin, en-US/mara/mara8.vdb
"""

import argparse
import io
import math
import os
import pickle
import struct
import wave
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed

import numpy as np
from tqdm import tqdm

import psutil
for proc in psutil.process_iter(['pid', 'name']):
    if proc.info['name'] and proc.info['name'].lower() in ("speechify.exe", "speechify"):
        proc.kill()
        proc.wait(timeout=5)
        print("Killed Speechify process (pid %d) to free file locks." % proc.info['pid'])

# --------------------------------------------------------------------------- #
#  Paths / constants                                                           #
# --------------------------------------------------------------------------- #
_HERE         = os.path.dirname(os.path.abspath(__file__))
MARA_DIR      = os.path.join(_HERE, 'en-US', 'mara')
VIN_PATH      = os.path.join(MARA_DIR, 'mara.vin')
VDB_PATH      = os.path.join(MARA_DIR, 'mara8.vdb')
TOM_VIN_PATH  = os.path.join(_HERE, 'en-US', 'tom', 'tom.vin')
EXTRA_WAV_DIR = os.path.join(MARA_DIR, 'output', 'extra_wavs')
EXTRA_TG_DIR  = os.path.join(MARA_DIR, 'output', 'extra_tg')
STATE_FILE    = os.path.join(MARA_DIR, 'build_extra_state.pkl')

# Bump this whenever the algorithm or record layout changes to force full rebuild.
STATE_VERSION = 4  # bumped: REPLACE strategy (was APPEND)

XOR_KEY       = 0xCE
UNIT_SIZE     = 29
WSOLA_PAD     = 8192   # silence padding appended to VDB data

# F0 scaling (must match build_mara_voice.py)
F0_SCALE      = 0.641
TOM_F0_MIN    = 99
TOM_F0_MAX    = 150

# ARPAbet phone inventory (indices 0..45)
PHONE_LABELS = [
    'aa', 'ae', 'ah', 'ao', 'aw', 'ax', 'ay', 'b', 'ch', 'dx',
    'd',  'dh', 'eh', 'el', 'er', 'en', 'ey', 'f', 'g',  'hh',
    'ih', 'ix', 'iy', 'jh', 'k',  'l',  'm',  'n', 'ng', 'ow',
    'oy', 'p',  'pau','r',  's',  'sh', 't',  'th','uh', 'uw',
    'v',  'w',  'xx', 'y',  'z',  'zh',
]
PHONE_IDX = {ph: i for i, ph in enumerate(PHONE_LABELS)}

_SILENCE_LABELS = {'', 'sil', 'sp', 'spn', 'silence', '<sil>', '<unk>'}

_ap = argparse.ArgumentParser(description='Replace low-quality units with extra recordings.')
_ap.add_argument('--workers', type=int, default=os.cpu_count() or 4,
                 help='Thread count for parallel processing (default: cpu_count)')
_ARGS = _ap.parse_args()
N_WORKERS = max(1, _ARGS.workers)


# -- State cache helpers ---------------------------------------------------------
def _file_key(path):
    """Return (mtime, size) for a file, or None if it does not exist."""
    try:
        st = os.stat(path)
        return (st.st_mtime, st.st_size)
    except OSError:
        return None


def load_state():
    """Load build_extra_state.pkl; return empty dict if missing or stale."""
    if not os.path.exists(STATE_FILE):
        return {}
    try:
        with open(STATE_FILE, 'rb') as f:
            state = pickle.load(f)
        if state.get('version') != STATE_VERSION:
            print("  State file version mismatch -- full rebuild.")
            return {}
        return state.get('recordings', {})
    except Exception as e:
        print("  Could not load state file (%s) -- full rebuild." % e)
        return {}


def save_state(recordings):
    """Persist recordings cache to build_extra_state.pkl."""
    state = {
        'version':    STATE_VERSION,
        'recordings': recordings,
    }
    with open(STATE_FILE, 'wb') as f:
        pickle.dump(state, f, protocol=4)


# IPA -> ARPAbet (for english_mfa TextGrid output)
# Must match build_mara_voice.py's expanded mapping.
_IPA_TO_ARPA = {
    # --- Vowels ---
    '\u0251\u02d0': 'aa', '\u0251': 'aa',
    'a\u02d0': 'aa', 'a': 'aa',
    '\xe6': 'ae',
    '\u0250': 'ah', '\u028c': 'ah',
    '\u0259': 'ah',
    '\u0254': 'ao', '\u0252': 'ao',
    '\u0252\u02d0': 'ao',
    'aw': 'aw',
    'aj': 'ay',
    '\u025b': 'eh',
    'e\u02d0': 'ey', 'e': 'eh',
    '\u025d': 'er', '\u025a': 'er',
    '\u025c\u02d0': 'er',
    'ej': 'ey',
    '\u026a': 'ih',
    'i\u02d0': 'iy', 'i': 'iy',
    '\u028a': 'uh',
    'u\u02d0': 'uw', 'u': 'uw', '\u0289\u02d0': 'uw', '\u0289': 'uw',
    'ow': 'ow',
    '\u0259w': 'ow',
    '\u0254j': 'oy',
    # --- Consonants (plain) ---
    'b': 'b',
    't\u0283': 'ch',
    'd': 'd',
    '\xf0': 'dh',
    'f': 'f',
    '\u0261': 'g',
    'h': 'hh',
    'd\u0292': 'jh',
    'k': 'k',
    'l': 'l', '\u026b': 'l', '\u026b\u0329': 'l',
    'm': 'm', 'm\u0329': 'm',
    'n': 'n', 'n\u0329': 'n',
    '\u014b': 'ng',
    'p': 'p',
    '\u0279': 'r',
    's': 's',
    '\u0283': 'sh',
    't': 't',
    '\u03b8': 'th',
    'v': 'v',
    'w': 'w',
    'j': 'y',
    'z': 'z',
    '\u0292': 'zh',
    '\u027e': 't',
    # --- Palatalized consonants (english_mfa: X + U+02B2) ---
    't\u02b2': 't', 'd\u02b2': 'd', 'b\u02b2': 'b', 'f\u02b2': 'f',
    'm\u02b2': 'm', 'n\u02b2': 'n', 'k\u02b2': 'k', 'p\u02b2': 'p',
    'v\u02b2': 'v', 'l\u02b2': 'l', 's\u02b2': 's', 'z\u02b2': 'z',
    '\u0261\u02b2': 'g', 'h\u02b2': 'hh', '\u0279\u02b2': 'r',
    # --- Dental variants (X + U+032A) ---
    't\u032a': 'th', 'd\u032a': 'dh',
    # --- Retroflex (english_mfa) ---
    '\u0288': 't', '\u0256': 'd',
    # --- Palatal (english_mfa) ---
    '\u028e': 'l', '\u0272': 'n', '\u00e7': 'sh',
    'c': 'k', '\u025f': 'jh',
    # --- Labio-dental approximant ---
    '\u028b': 'v',
}


# --------------------------------------------------------------------------- #
#  Shared utilities                                                            #
# --------------------------------------------------------------------------- #
def xor_decode(path):
    raw = np.fromfile(path, dtype=np.uint8)
    raw ^= XOR_KEY
    return raw.tobytes()


def xor_encode(data):
    arr = np.frombuffer(data, dtype=np.uint8).copy()
    arr ^= XOR_KEY
    return arr.tobytes()


def riff_chunks(data, start=0, end=None):
    if end is None:
        end = len(data)
    pos = start
    while pos + 8 <= end:
        tag = bytes(data[pos:pos+4])
        sz  = struct.unpack_from('<I', data, pos+4)[0]
        yield tag, pos+8, sz
        pos += 8 + sz + (sz & 1)


def pack_chunk(tag, body):
    if isinstance(tag, str):
        tag = tag.encode('latin1')
    pad = b'\x00' if len(body) & 1 else b''
    return tag + struct.pack('<I', len(body)) + body + pad


def normalize_phone(label):
    lbl = label.strip().lower().rstrip('012')
    if lbl in _SILENCE_LABELS:
        return 'pau'
    arpa = _IPA_TO_ARPA.get(lbl)
    if arpa:
        return arpa
    # Try stripping length mark (U+02D0) then look up again.
    if lbl.endswith('\u02d0'):
        arpa = _IPA_TO_ARPA.get(lbl[:-1])
        if arpa:
            return arpa
    # Try stripping palatalization mark (U+02B2) then look up again.
    if lbl.endswith('\u02b2'):
        arpa = _IPA_TO_ARPA.get(lbl[:-1])
        if arpa:
            return arpa
    # Word-level OOV labels from english_mfa -> treat as silence
    if len(lbl) > 3 or (len(lbl) > 1 and lbl.isascii() and lbl.isalpha()):
        return 'pau'
    return lbl


def parse_textgrid_phones(path):
    """Return (intervals, xmax) for the phones tier.
    intervals: [(start_sec, end_sec, arpa_label)]
    """
    intervals = []
    xmax = 0.0
    xmax_found = False
    in_phones = False
    in_iv = False
    cur_xmin = cur_xmax = None

    with open(path, encoding='utf-8', errors='replace') as fh:
        for line in fh:
            line = line.strip()
            if not xmax_found and line.startswith('xmax ='):
                try:
                    xmax = float(line.split('=', 1)[1].strip())
                    xmax_found = True
                except ValueError:
                    pass
            elif '"phones"' in line.lower() or '"phone"' in line.lower():
                in_phones = True
            elif in_phones:
                if line.startswith('intervals ['):
                    in_iv = True
                    cur_xmin = cur_xmax = None
                elif in_iv:
                    if line.startswith('xmin ='):
                        try:
                            cur_xmin = float(line.split('=', 1)[1].strip())
                        except ValueError:
                            pass
                    elif line.startswith('xmax ='):
                        try:
                            cur_xmax = float(line.split('=', 1)[1].strip())
                        except ValueError:
                            pass
                    elif line.startswith('text ='):
                        label = line.split('=', 1)[1].strip().strip('"')
                        if cur_xmin is not None and cur_xmax is not None:
                            arpa = normalize_phone(label)
                            intervals.append((cur_xmin, cur_xmax, arpa))
                        in_iv = False
                elif line.startswith('item ['):
                    break

    return intervals, xmax


def downsample_3x(pcm_bytes):
    from scipy.signal import resample_poly
    samples = np.frombuffer(pcm_bytes, dtype='<i2').copy().astype(np.float64)
    out = resample_poly(samples, 1, 3)
    return np.clip(out, -32768, 32767).astype('<i2').tobytes()


TARGET_RMS = 6500.0  # must match build_mara_voice.py

def normalize_rms(pcm_bytes, target_rms=TARGET_RMS):
    samples = np.frombuffer(pcm_bytes, dtype='<i2').astype(np.float64)
    rms = np.sqrt(np.mean(samples ** 2)) if len(samples) > 0 else 0.0
    if rms < 200.0:
        return pcm_bytes
    scale = target_rms / rms
    return np.clip(samples * scale, -32768, 32767).astype('<i2').tobytes()


def pcm16_to_ulaw(pcm_bytes):
    import audioop
    return audioop.lin2ulaw(pcm_bytes, 2)


def f0_track_from_ulaw(ulaw_bytes):
    import audioop
    import pyworld as pw
    if len(ulaw_bytes) < 800:
        return None
    try:
        pcm = audioop.ulaw2lin(ulaw_bytes, 2)
        x = np.frombuffer(pcm, dtype='<i2').astype(np.float64) / 32768.0
        f0, _ = pw.harvest(x, 8000, frame_period=1.0)
        if np.all(f0 == 0.0):
            return None
        return f0
    except Exception:
        return None


def lookup_f0(f0_arr, pos_ms):
    if f0_arr is None or len(f0_arr) == 0:
        return 0
    idx = max(0, min(len(f0_arr) - 1, int(pos_ms)))
    hz = f0_arr[idx]
    if hz < 50.0:
        return 0
    scaled = round(hz * F0_SCALE)
    if scaled < TOM_F0_MIN:
        return 0
    return min(TOM_F0_MAX, scaled)


# --------------------------------------------------------------------------- #
#  f0_context regression from Tom                                              #
# --------------------------------------------------------------------------- #
def fit_f0_context(tom_vin_data):
    """Fit log(dur_like) -> f0_context from Tom's units.
    Returns a function(dur_like) -> u8 f0_context value.
    """
    # Find unit/data in Tom's VIN
    for tag, ds, sz in riff_chunks(tom_vin_data, 12):
        if tag == b'unit':
            for stag, sds, ssz in riff_chunks(tom_vin_data, ds, ds + sz):
                if stag == b'data':
                    n = ssz // UNIT_SIZE
                    dls = []
                    ctxs = []
                    for i in range(n):
                        base = sds + i * UNIT_SIZE
                        dl = struct.unpack_from('<H', tom_vin_data, base + 10)[0]
                        fc = tom_vin_data[base + 19]
                        if dl > 0 and fc > 0:
                            dls.append(math.log(dl + 1))
                            ctxs.append(fc)
                    break
            break

    dls = np.array(dls)
    ctxs = np.array(ctxs, dtype=np.float64)
    # Linear regression: f0_context = a * log(dur_like+1) + b
    A = np.column_stack([dls, np.ones_like(dls)])
    result = np.linalg.lstsq(A, ctxs, rcond=None)
    a, b = result[0]
    r2 = 1.0 - np.sum((ctxs - (a * dls + b))**2) / np.sum((ctxs - ctxs.mean())**2)
    print("  f0_context regression: f0ctx = %.3f * log(dl+1) + %.3f  (R^2=%.4f)" % (a, b, r2))

    def predict(dur_like):
        if dur_like <= 0:
            return 1  # never 0
        val = round(a * math.log(dur_like + 1) + b)
        return max(1, min(255, val))

    return predict


# --------------------------------------------------------------------------- #
#  Unit record creation from MFA phoneme intervals                             #
# --------------------------------------------------------------------------- #
MIN_PHONE_DUR_MS = 5   # skip intervals shorter than this
MIN_UNIT_DUR     = 25  # minimum lp spacing between halfphones (must match build_mara_voice.py)

def create_unit_fields(intervals, cap, f0_arr, f0ctx_fn):
    """Create halfphone unit field tuples from MFA phoneme intervals.

    Each non-trivial phoneme produces 2 units (first half + second half).
    Returns list of tuples:
      (lp, dl, pc, is_first, f0s, f0e, f0m, f0ctx)
    unit_id and file_idx are assigned later during assembly.
    """
    # Filter to real phoneme intervals with sufficient duration
    phones = []
    for start, end, label in intervals:
        dur_ms = round((end - start) * 1000)
        if dur_ms < MIN_PHONE_DUR_MS:
            continue
        pc = PHONE_IDX.get(label, -1)
        if pc < 0:
            continue  # unknown phone label
        lp = round(start * 1000)
        phones.append((lp, dur_ms, pc, label))

    fields = []

    for pi, (lp, dur_ms, pc, label) in enumerate(phones):
        # Split into two halfphones with minimum spacing
        dl_first = max(MIN_UNIT_DUR, dur_ms // 2)
        dl_second = max(MIN_UNIT_DUR, dur_ms - dl_first)

        lp_first = lp
        lp_second = lp + dl_first

        # Clamp to cap
        if cap > 0:
            lp_first = min(lp_first, cap)
            dl_first = min(dl_first, max(0, cap - lp_first))
            lp_second = min(lp_second, cap)
            dl_second = min(dl_second, max(0, cap - lp_second))

        # f0 values
        f0s_1 = lookup_f0(f0_arr, lp_first)
        f0e_1 = lookup_f0(f0_arr, lp_first + dl_first)
        f0m_1 = lookup_f0(f0_arr, lp_first + dl_first // 2)
        f0s_2 = lookup_f0(f0_arr, lp_second)
        f0e_2 = lookup_f0(f0_arr, lp_second + dl_second)
        f0m_2 = lookup_f0(f0_arr, lp_second + dl_second // 2)

        # f0_context: same for both halves, based on total phoneme duration
        f0ctx = f0ctx_fn(dur_ms)

        fields.append((lp_first, dl_first, pc, 1, f0s_1, f0e_1, f0m_1, f0ctx))
        fields.append((lp_second, dl_second, pc, 0, f0s_2, f0e_2, f0m_2, f0ctx))

    return fields


def process_extra_recording(name, wav_path, tg_path, f0ctx_fn):
    """Process one extra recording: WAV load, downsample, u-law, F0, MFA parse.

    Returns (ulaw, unit_fields, f0_status) or None on skip.
      ulaw: bytes -- u-law encoded 8kHz audio
      unit_fields: list of field tuples (see create_unit_fields)
      f0_status: 1=ok, -1=failed
    Called from thread pool; all inputs are read-only.
    """
    # Load and process WAV
    with wave.open(wav_path) as w:
        n_ch = w.getnchannels()
        sampwidth = w.getsampwidth()
        framerate = w.getframerate()
        raw_pcm = w.readframes(w.getnframes())

    if n_ch != 1 or sampwidth != 2:
        return None

    if framerate == 24000:
        pcm_8k = downsample_3x(raw_pcm)
    elif framerate == 8000:
        pcm_8k = raw_pcm
    else:
        from scipy.signal import resample_poly
        g = math.gcd(framerate, 8000)
        up, down = 8000 // g, framerate // g
        samples = np.frombuffer(raw_pcm, dtype='<i2').copy().astype(np.float64)
        out = resample_poly(samples, up, down)
        pcm_8k = np.clip(out, -32768, 32767).astype('<i2').tobytes()

    ulaw = pcm16_to_ulaw(normalize_rms(pcm_8k))
    mara_n = len(ulaw)
    cap = mara_n // 8 - 1 if mara_n >= 16 else 0
    if cap <= 0:
        return None

    # Parse TextGrid
    intervals, xmax = parse_textgrid_phones(tg_path)
    real_phones = [(s, e, l) for s, e, l in intervals
                   if l not in _SILENCE_LABELS and l != 'pau']
    if not real_phones and not intervals:
        return None

    # F0 extraction (the expensive part)
    f0_arr = f0_track_from_ulaw(ulaw)
    f0_status = 1 if f0_arr is not None else -1

    # Create unit field tuples (cheap struct work)
    unit_fields = create_unit_fields(intervals, cap, f0_arr, f0ctx_fn)
    if not unit_fields:
        return None

    return (ulaw, unit_fields, f0_status)


# --------------------------------------------------------------------------- #
#  Run-potential scoring                                                        #
# --------------------------------------------------------------------------- #
def compute_run_potential(vin_data, unit_data_ds, n_units):
    """Compute run_potential for every unit.

    run_potential = number of consecutive same-recording active neighbors.
    Units with dl=0 get run_potential = -1 (inactive, not replaceable).
    Lower run_potential = worse unit = better replacement target.
    """
    # Read file_idx and dl for all units
    file_idxs = np.zeros(n_units, dtype=np.uint16)
    dls = np.zeros(n_units, dtype=np.uint16)

    for i in range(n_units):
        base = unit_data_ds + i * UNIT_SIZE
        file_idxs[i] = struct.unpack_from('<H', vin_data, base + 4)[0]
        dls[i] = struct.unpack_from('<H', vin_data, base + 10)[0]

    # Group UIDs by file_idx
    file_groups = defaultdict(list)
    for uid in range(n_units):
        if dls[uid] > 0:
            file_groups[file_idxs[uid]].append(uid)

    # For each unit, count how many active same-recording neighbors exist
    # (units with the same file_idx and dl > 0)
    run_potential = np.full(n_units, -1, dtype=np.int32)
    for fid, uids in file_groups.items():
        group_size = len(uids)
        for uid in uids:
            run_potential[uid] = group_size

    return run_potential


# --------------------------------------------------------------------------- #
#  Main                                                                        #
# --------------------------------------------------------------------------- #
def main():
    print("=" * 70)
    print("build_mara_extra.py -- REPLACE strategy")
    print("  Replace low-quality units with extra recording data")
    print("=" * 70)

    # -- 1. Find extra recordings -----------------------------------------------
    if not os.path.isdir(EXTRA_WAV_DIR):
        print("\nNo extra_wavs directory found at: %s" % EXTRA_WAV_DIR)
        print("Create this directory and add WAV files to add extra recordings.")
        return
    wav_names = sorted(
        f[:-4] for f in os.listdir(EXTRA_WAV_DIR)
        if f.lower().endswith('.wav')
    )
    if not wav_names:
        print("\nNo WAV files found in %s. Nothing to do." % EXTRA_WAV_DIR)
        return

    tg_dir_exists = os.path.isdir(EXTRA_TG_DIR)
    tg_available = set()
    if tg_dir_exists:
        tg_available = set(
            f[:-9] for f in os.listdir(EXTRA_TG_DIR)
            if f.endswith('.TextGrid')
        )

    matched = [n for n in wav_names if n in tg_available]
    unmatched = [n for n in wav_names if n not in tg_available]
    print("\n  WAVs found:      %d" % len(wav_names))
    print("  TextGrids found: %d" % len(tg_available))
    print("  Matched pairs:   %d" % len(matched))
    if unmatched:
        print("  Unmatched (skipped): %d" % len(unmatched))

    if not matched:
        print("\nNo matched WAV+TextGrid pairs. Run MFA on extra_wavs first.")
        return

    # -- 2. Fit f0_context regression from Tom -----------------------------------
    print("\nFitting f0_context regression from Tom ...")
    tom_vin = xor_decode(TOM_VIN_PATH)
    f0ctx_fn = fit_f0_context(tom_vin)

    # -- 3. Load existing mara.vin -----------------------------------------------
    print("\nLoading mara.vin ...")
    vin_data = bytearray(xor_decode(VIN_PATH))

    # Parse top-level chunks (preserve order for reassembly)
    vin_chunk_list = []  # [(tag, ds, sz)]
    for tag, ds, sz in riff_chunks(vin_data, 12):
        vin_chunk_list.append((tag, ds, sz))

    # Read cnts
    cnts_ds = cnts_sz = None
    for tag, ds, sz in vin_chunk_list:
        if tag == b'cnts':
            cnts_ds, cnts_sz = ds, sz
            break
    assert cnts_ds is not None, "cnts chunk not found"
    current_n_units = struct.unpack_from('<I', vin_data, cnts_ds + 8)[0]
    print("  Current unit count: %d" % current_n_units)

    # Read feat.filename to find max stored_id and existing names
    feat_ds = feat_sz = None
    for tag, ds, sz in vin_chunk_list:
        if tag == b'feat':
            feat_ds, feat_sz = ds, sz
            break
    assert feat_ds is not None, "feat chunk not found"
    feat_body = bytes(vin_data[feat_ds:feat_ds + feat_sz])

    fn_idx = feat_body.find(b'filename')
    assert fn_idx >= 0, "filename key not found in feat"
    fn_count = struct.unpack_from('<I', feat_body, fn_idx + 8)[0]
    p = fn_idx + 12
    existing_names = set()
    max_stored_id = -1
    for _ in range(fn_count):
        nlen = struct.unpack_from('<H', feat_body, p)[0]
        name = feat_body[p+2:p+2+nlen].decode('latin-1', errors='replace')
        sid = struct.unpack_from('<I', feat_body, p+2+nlen)[0]
        existing_names.add(name)
        if sid > max_stored_id:
            max_stored_id = sid
        p += 2 + nlen + 4
    fn_section_end = p  # byte offset within feat_body after last filename entry
    print("  Existing filenames: %d (max stored_id=%d)" % (fn_count, max_stored_id))

    # Find unit/data sub-chunk
    unit_ds = unit_sz = None
    for tag, ds, sz in vin_chunk_list:
        if tag == b'unit':
            unit_ds, unit_sz = ds, sz
            break
    assert unit_ds is not None, "unit chunk not found"
    unit_data_ds = unit_data_sz = None
    for tag, ds, sz in riff_chunks(vin_data, unit_ds, unit_ds + unit_sz):
        if tag == b'data':
            unit_data_ds, unit_data_sz = ds, sz
            break
    assert unit_data_ds is not None, "unit data sub-chunk not found"
    assert unit_data_sz == current_n_units * UNIT_SIZE, \
        "unit data size mismatch: %d != %d" % (unit_data_sz, current_n_units * UNIT_SIZE)

    # -- 4. Load existing mara8.vdb -----------------------------------------------
    print("\nLoading mara8.vdb ...")
    vdb_data = xor_decode(VDB_PATH)

    vdb_chunks = {}
    for tag, ds, sz in riff_chunks(vdb_data, 12):
        vdb_chunks[tag] = (ds, sz)

    vdb_data_ds, vdb_data_sz = vdb_chunks[b'data']
    vdb_indx_ds, vdb_indx_sz = vdb_chunks[b'indx']

    # Parse VDB indx
    vdb_indx_count = struct.unpack_from('<I', vdb_data, vdb_indx_ds)[0]
    vp = vdb_indx_ds + 4
    vdb_entries = []  # [(byte_offset, name)]
    for _ in range(vdb_indx_count):
        off = struct.unpack_from('<I', vdb_data, vp)[0]
        nlen = struct.unpack_from('<H', vdb_data, vp + 4)[0]
        name = vdb_data[vp+6:vp+6+nlen].decode('latin-1', errors='replace')
        vdb_entries.append((off, name))
        vp += 6 + nlen
    vdb_existing_names = set(name for _, name in vdb_entries if name)
    # Sentinel is last entry (empty name); its offset = total real data size
    sentinel_off = vdb_entries[-1][0]
    print("  VDB entries: %d (data up to byte %d)" % (len(vdb_entries) - 1, sentinel_off))

    # Extract VDB sub-chunks for reassembly (LIST, fmt)
    vdb_list_ds, vdb_list_sz = vdb_chunks[b'LIST']
    vdb_fmt_ds, vdb_fmt_sz = vdb_chunks[b'fmt ']
    vdb_list_chunk = vdb_data[vdb_list_ds-8 : vdb_list_ds + vdb_list_sz + (vdb_list_sz & 1)]
    vdb_fmt_chunk = vdb_data[vdb_fmt_ds-8 : vdb_fmt_ds + vdb_fmt_sz + (vdb_fmt_sz & 1)]

    # -- 5. Compute run_potential for all existing units -------------------------
    print("\nComputing run_potential for %d units ..." % current_n_units)
    run_potential = compute_run_potential(vin_data, unit_data_ds, current_n_units)
    active_count = int(np.sum(run_potential >= 0))
    print("  Active units (dl>0): %d" % active_count)
    print("  Run potential stats (active): min=%d, median=%d, mean=%.1f, max=%d" % (
        int(np.min(run_potential[run_potential >= 0])),
        int(np.median(run_potential[run_potential >= 0])),
        float(np.mean(run_potential[run_potential >= 0])),
        int(np.max(run_potential[run_potential >= 0])),
    ))

    # -- 6. Build per-(pc, is_first) replacement pools ---------------------------
    print("\nBuilding replacement pools by (phone_center, is_first_half) ...")

    # Index: (pc, is_first) -> list of (run_potential, uid), sorted worst-first
    replacement_pools = defaultdict(list)
    for uid in range(current_n_units):
        rp = run_potential[uid]
        if rp < 0:
            continue  # inactive unit (dl=0), skip
        base = unit_data_ds + uid * UNIT_SIZE
        pc = vin_data[base + 20]
        is_first = vin_data[base + 21]
        replacement_pools[(pc, is_first)].append((rp, uid))

    # Sort each pool: lowest run_potential first (worst units first)
    for key in replacement_pools:
        replacement_pools[key].sort(key=lambda x: x[0])

    pool_sizes = {k: len(v) for k, v in replacement_pools.items()}
    total_pool = sum(pool_sizes.values())
    print("  %d distinct (pc, is_first) groups, %d total replaceable units" % (
        len(pool_sizes), total_pool))

    # -- 7. Process extra recordings (cached + threaded) -------------------------
    # Filter: skip recordings whose names are already in VDB
    to_process = []
    skip_existing = 0
    for name in matched:
        if name in vdb_existing_names:
            skip_existing += 1
        else:
            to_process.append(name)
    if skip_existing:
        print("  Skipped %d recordings already in VDB" % skip_existing)
    if not to_process:
        print("\nAll extra recordings already present in VDB. Nothing to do.")
        return
    print("\n  Processing %d new recordings ..." % len(to_process))

    # Load cache from previous run
    cached_recs = load_state()
    new_cached_recs = {}

    # Collect file keys for cache invalidation
    wav_keys = {}
    tg_keys = {}
    for name in to_process:
        wav_keys[name] = _file_key(os.path.join(EXTRA_WAV_DIR, name + '.wav'))
        tg_keys[name] = _file_key(os.path.join(EXTRA_TG_DIR, name + '.TextGrid'))

    # Phase A: check cache, collect misses
    cache_hits = 0
    cache_misses = 0
    miss_names = []
    hit_results = {}

    for name in to_process:
        rec_cache_key = (wav_keys[name], tg_keys[name])
        cached = cached_recs.get(name)
        if cached and cached.get('key') == rec_cache_key:
            hit_results[name] = cached
            new_cached_recs[name] = cached
            cache_hits += 1
        else:
            miss_names.append(name)
            cache_misses += 1

    print("  Cache: %d hits, %d misses" % (cache_hits, cache_misses))

    # Phase B: process cache misses in parallel
    miss_results = {}

    if miss_names:
        print("  Spawning %d worker thread(s) for %d recordings ..." % (
            N_WORKERS, cache_misses))

        def _submit(name):
            wav_path = os.path.join(EXTRA_WAV_DIR, name + '.wav')
            tg_path = os.path.join(EXTRA_TG_DIR, name + '.TextGrid')
            return process_extra_recording(name, wav_path, tg_path, f0ctx_fn)

        with ThreadPoolExecutor(max_workers=N_WORKERS) as pool:
            future_to_name = {pool.submit(_submit, n): n for n in miss_names}
            for future in tqdm(as_completed(future_to_name), total=len(miss_names),
                               desc="Processing", unit="rec"):
                name = future_to_name[future]
                result = future.result()
                if result is None:
                    tqdm.write("  SKIP %s" % name)
                    continue
                ulaw, unit_fields, f0_status = result
                rec_cache_key = (wav_keys[name], tg_keys[name])
                entry = {
                    'key': rec_cache_key,
                    'ulaw': ulaw,
                    'fields': unit_fields,
                    'f0': f0_status,
                }
                miss_results[name] = entry
                new_cached_recs[name] = entry

    # -- 8. Assign replacements ---------------------------------------------------
    print("\nAssigning replacement targets ...")

    # Track replacement pool consumption (use mutable lists as queues)
    pool_cursors = {k: 0 for k in replacement_pools}

    def pop_replacement(pc, is_first):
        """Pop the next worst unit for (pc, is_first). Returns uid or None."""
        key = (pc, is_first)
        pool = replacement_pools.get(key)
        if pool is None:
            return None
        cursor = pool_cursors[key]
        if cursor >= len(pool):
            return None
        _, uid = pool[cursor]
        pool_cursors[key] = cursor + 1
        return uid

    # Collect all replacement assignments in deterministic order
    # Each entry: (recording_name, extra_ulaw, list of (target_uid, fields_tuple))
    replacement_groups = []
    total_replaced = 0
    total_skipped_no_target = 0
    f0_ok = 0
    f0_fail = 0

    new_vdb_audio = io.BytesIO()
    new_vdb_entries = []
    new_feat_entries = []
    next_stored_id = max_stored_id + 1

    for name in to_process:
        entry = hit_results.get(name) or miss_results.get(name)
        if entry is None:
            continue  # was skipped during processing

        ulaw = entry['ulaw']
        unit_fields = entry['fields']
        f0_status = entry['f0']

        if f0_status == 1:
            f0_ok += 1
        else:
            f0_fail += 1

        # Find replacement targets for each unit in this recording
        replacements = []
        for fields_tuple in unit_fields:
            # fields_tuple: (lp, dl, pc, is_first, f0s, f0e, f0m, f0ctx)
            pc = fields_tuple[2]
            is_first = fields_tuple[3]
            target_uid = pop_replacement(pc, is_first)
            if target_uid is not None:
                replacements.append((target_uid, fields_tuple))
            else:
                total_skipped_no_target += 1

        if not replacements:
            continue

        # Assign a new file_idx for this extra recording
        file_idx = next_stored_id

        # VDB: append audio at end
        vdb_offset = sentinel_off + new_vdb_audio.tell()
        new_vdb_audio.write(ulaw)
        new_vdb_entries.append((vdb_offset, name))

        # feat: new filename entry
        new_feat_entries.append((name, next_stored_id))
        next_stored_id += 1

        replacement_groups.append((name, file_idx, replacements))
        total_replaced += len(replacements)

    new_vdb_bytes = new_vdb_audio.getvalue()
    n_new_recs = len(new_vdb_entries)

    print("  Recordings with replacements: %d" % n_new_recs)
    print("  Units replaced:               %d" % total_replaced)
    print("  Units skipped (no target):    %d" % total_skipped_no_target)
    print("  F0 extraction: %d ok, %d failed" % (f0_ok, f0_fail))

    if n_new_recs == 0:
        print("\nNo replacements possible. Nothing to write.")
        return

    # -- 9. Apply replacements to unit records in mara.vin ----------------------
    print("\nApplying %d unit replacements to mara.vin ..." % total_replaced)

    for name, file_idx, replacements in replacement_groups:
        for target_uid, fields_tuple in replacements:
            lp, dl, pc, is_first, f0s, f0e, f0m, f0ctx = fields_tuple
            base = unit_data_ds + target_uid * UNIT_SIZE

            # Update ONLY: file_idx (bytes 4-5), lp (bytes 6-7), dl (bytes 10-11),
            #              f0_start (byte 16), f0_end (byte 17), f0_mid (byte 18),
            #              f0_context (byte 19)
            # Keep: uid (bytes 0-3), reserved (bytes 8-9),
            #       syl_type (byte 12), syl_in_phrase (byte 13),
            #       word_in_phrase (byte 14), phone_pos (byte 15),
            #       pc (byte 20), is_first (byte 21), const (byte 22),
            #       pctx[0..3] (bytes 23-26), flag_b (byte 27), ctx_cost (byte 28)

            struct.pack_into('<H', vin_data, base + 4, file_idx)         # file_idx
            struct.pack_into('<H', vin_data, base + 6, min(lp, 0xFFFF)) # local_pos
            struct.pack_into('<H', vin_data, base + 10, min(dl, 0xFFFF))# dur_like
            vin_data[base + 16] = f0s      # f0_start
            vin_data[base + 17] = f0e      # f0_end
            vin_data[base + 18] = f0m      # f0_mid
            vin_data[base + 19] = f0ctx    # f0_context

    # -- 10. Rebuild mara8.vdb ---------------------------------------------------
    print("\nRebuilding mara8.vdb ...")

    # Existing data (up to sentinel, excluding WSOLA padding)
    existing_data = vdb_data[vdb_data_ds:vdb_data_ds + sentinel_off]

    # Combined data + padding
    combined_data = existing_data + new_vdb_bytes + (b'\xff' * WSOLA_PAD)

    # Combined indx (existing entries + new entries + updated sentinel)
    all_entries = list(vdb_entries[:-1])  # existing (without old sentinel)
    all_entries.extend(new_vdb_entries)   # new recordings
    new_sentinel_off = sentinel_off + len(new_vdb_bytes)
    all_entries.append((new_sentinel_off, ''))  # new sentinel

    indx_buf = io.BytesIO()
    indx_buf.write(struct.pack('<I', len(all_entries)))
    for off, nm in all_entries:
        nm_enc = nm.encode('latin-1')
        indx_buf.write(struct.pack('<I', off))
        indx_buf.write(struct.pack('<H', len(nm_enc)))
        indx_buf.write(nm_enc)

    vdb_body = (
        b'WAVE' +
        vdb_list_chunk +
        vdb_fmt_chunk +
        pack_chunk(b'indx', indx_buf.getvalue()) +
        pack_chunk(b'data', combined_data)
    )
    new_vdb = b'RIFF' + struct.pack('<I', len(vdb_body)) + vdb_body
    with open(VDB_PATH, 'wb') as f:
        f.write(xor_encode(new_vdb))
    print("  Wrote %s (%d bytes, %d total recordings)" % (
        VDB_PATH, len(new_vdb), len(all_entries) - 1))

    # -- 11. Rebuild mara.vin (with modified unit data + new feat entries) -------
    print("\nRebuilding mara.vin ...")

    # 11a. Patch feat: insert new filename entries
    new_fn_entries_buf = bytearray()
    for name, sid in new_feat_entries:
        nm_enc = name.encode('latin-1')
        new_fn_entries_buf.extend(struct.pack('<H', len(nm_enc)))
        new_fn_entries_buf.extend(nm_enc)
        new_fn_entries_buf.extend(struct.pack('<I', sid))

    new_fn_count = fn_count + len(new_feat_entries)
    new_feat_body = bytearray(feat_body)
    # Update count
    struct.pack_into('<I', new_feat_body, fn_idx + 8, new_fn_count)
    # Insert new entries after existing filename entries
    new_feat_body = (bytes(new_feat_body[:fn_section_end]) +
                     bytes(new_fn_entries_buf) +
                     bytes(new_feat_body[fn_section_end:]))

    # 11b. Extract modified unit data from vin_data (already patched in-place)
    modified_unit_data = bytes(vin_data[unit_data_ds:unit_data_ds + unit_data_sz])

    # 11c. Rebuild VIN from chunks (unit count stays the same -- we replaced, not added)
    vin_body = bytearray()
    vin_body.extend(b'svin')

    for tag, ds, sz in vin_chunk_list:
        if tag == b'feat':
            vin_body.extend(pack_chunk(b'feat', bytes(new_feat_body)))
        elif tag == b'unit':
            # Preserve all sub-chunks before 'data' (e.g. 'vers'), then
            # replace 'data' with modified version
            prefix = vin_data[ds:unit_data_ds - 8]  # bytes before data header
            unit_inner = prefix + pack_chunk(b'data', modified_unit_data)
            vin_body.extend(pack_chunk(b'unit', unit_inner))
        else:
            # Copy original chunk verbatim (including cnts -- unit count unchanged)
            chunk_start = ds - 8
            chunk_len = 8 + sz + (sz & 1)
            vin_body.extend(vin_data[chunk_start:chunk_start + chunk_len])

    new_vin = b'RIFF' + struct.pack('<I', len(vin_body)) + bytes(vin_body)
    with open(VIN_PATH, 'wb') as f:
        f.write(xor_encode(new_vin))
    print("  Wrote %s (%d bytes, %d units unchanged, %d filenames)" % (
        VIN_PATH, len(new_vin), current_n_units, new_fn_count))

    # -- 12. Save build cache -----------------------------------------------------
    save_state(new_cached_recs)
    print("  %d recordings cached to %s" % (len(new_cached_recs), STATE_FILE))

    # -- Summary ------------------------------------------------------------------
    print("Done. REPLACE strategy results:")
    print("  Recordings added to VDB: %d" % n_new_recs)
    print("  Filenames added to feat: %d" % len(new_feat_entries))
    print("  Units REPLACED:          %d (of %d total)" % (total_replaced, current_n_units))
    print("  Units skipped (no pool): %d" % total_skipped_no_target)
    print("  Unit count:              %d (unchanged)" % current_n_units)
    print("\nReplaced units keep their original UID, prosodic context, and prsl membership.")
    print("Only lp, dl, file_idx, f0 values, and f0_context were updated.")
    print("\nNext steps:")
    print("  1. python build_mara_rest.py    (rebuild hash/prsl/mean)")
    print("  2. python build_mara_trees.py   (rebuild CART trees)")
    print("  3. Restart Speechify.exe")
    print("="*252)


if __name__ == '__main__':
    main()
