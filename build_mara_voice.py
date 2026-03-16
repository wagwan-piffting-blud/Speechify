#!/usr/bin/env python3
"""Build mara8.vdb and mara.vin from Mara's 24kHz recordings + MFA alignment.

Strategy
--------
mara8.vdb:
  For each recording slot in the original indx (in file_idx order):
    - If a Mara WAV exists: downsample 24kHz->8kHz in-memory (3:1 decimation).
    - Otherwise: copy tom8.vdb audio unchanged.
  Rebuild the RIFF/WAVE file with a fresh indx pointing to the new offsets.

mara.vin:
  Copy tom.vin byte-for-byte, replace only the unit/data sub-chunk.
  For each unit:
    - lp/dl: proportional to full MFA recording duration (TextGrid xmax).
      Uses xmax (whole recording) not speech-span only -- avoids compression.
    - phone_center: from MFA phone label at unit's proportional time position.
    - f0_start/f0_end/f0_mid: from PyWorld HARVEST on Mara's 8kHz u-law audio.
  Recordings without a Mara WAV keep all original Tom values.

State caching:
  Per-recording results are saved to build_state.pkl alongside the output files.
  On subsequent runs, a recording is reused from cache when:
    - tom.vin has not changed (mtime + size)
    - the WAV file has not changed (mtime + size)
    - the TextGrid file has not changed (mtime + size)
  Only changed or new recordings are recomputed.

Audio format note:
  VDB data is G.711 u-law, 8 kHz, mono (1 byte/sample).
  Engine byte offset formula: byte_offset = lp * 8.
  Cap formula: cap = mara_n // 8 - 1 (mara_n = u-law byte count).
  Time -> unit: 1 unit = 8 u-law bytes at 8000 Hz = 1 ms.
"""

import argparse
import glob as _glob
import io
import os
import pickle
import shutil
import struct
import tempfile
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

# -- Paths -----------------------------------------------------------------------
_HERE    = os.path.dirname(os.path.abspath(__file__))
TOM_VIN  = os.path.join(_HERE, 'en-US', 'tom', 'tom.vin')
TOM_VDB  = os.path.join(_HERE, 'en-US', 'tom', 'tom8.vdb')
MARA_DIR = os.path.join(_HERE, 'en-US', 'mara')
VIN_OUT  = os.path.join(MARA_DIR, 'mara.vin')
VDB_OUT  = os.path.join(MARA_DIR, 'mara8.vdb')
WAV_DIR  = os.path.join(MARA_DIR, 'output', 'mara_voice_wavs')
TG_DIR   = os.path.join(MARA_DIR, 'output', 'mfa_forced_output')
STATE_FILE = os.path.join(MARA_DIR, 'build_state.pkl')

# Bump this whenever the state schema or algorithm changes to force a full rebuild.
STATE_VERSION = 70  # bumped: SPEC_N_MELS 20->40, SPEC_MAX_DB 10->20

XOR_KEY   = 0xCE
UNIT_SIZE = 29
N_UNITS   = 169579

# ARPAbet phone inventory (indices 0..45) matching ccos/labl and unit.phone_center.
PHONE_LABELS = [
    'aa', 'ae', 'ah', 'ao', 'aw', 'ax', 'ay', 'b', 'ch', 'dx',
    'd',  'dh', 'eh', 'el', 'er', 'en', 'ey', 'f', 'g',  'hh',
    'ih', 'ix', 'iy', 'jh', 'k',  'l',  'm',  'n', 'ng', 'ow',
    'oy', 'p',  'pau','r',  's',  'sh', 't',  'th','uh', 'uw',
    'v',  'w',  'xx', 'y',  'z',  'zh',
]
PHONE_IDX = {ph: i for i, ph in enumerate(PHONE_LABELS)}

_SILENCE_LABELS = {'', 'sil', 'sp', 'spn', 'silence', '<sil>', '<unk>'}

# IPA -> ARPAbet mapping for english_mfa TextGrid output.
# The english_mfa dictionary produces a rich IPA set including palatalized,
# retroflex, dental variants, and length marks. Map them all to the closest
# ARPAbet phone in Tom's 46-phone inventory.
_IPA_TO_ARPA = {
    # --- Vowels ---
    '\u0251\u02d0': 'aa', '\u0251': 'aa',          # open back unrounded
    'a\u02d0': 'aa', 'a': 'aa',                     # open front (english_mfa)
    '\xe6': 'ae',                                    # near-open front
    '\u0250': 'ah', '\u028c': 'ah',                  # open-mid back unrounded
    '\u0259': 'ah',                                  # schwa -> ah
    '\u0254': 'ao', '\u0252': 'ao',                  # open-mid back rounded
    '\u0252\u02d0': 'ao',                            # long variant
    'aw': 'aw',
    'aj': 'ay',
    '\u025b': 'eh',                                  # open-mid front
    'e\u02d0': 'ey', 'e': 'eh',                     # close-mid front
    '\u025d': 'er', '\u025a': 'er',                  # r-colored
    '\u025c\u02d0': 'er',                            # long r-colored (english_mfa)
    'ej': 'ey',
    '\u026a': 'ih',                                  # near-close front
    'i\u02d0': 'iy', 'i': 'iy',                     # close front
    '\u028a': 'uh',                                  # near-close back rounded
    'u\u02d0': 'uw', 'u': 'uw',                     # close back rounded
    '\u0289\u02d0': 'uw', '\u0289': 'uw',            # barred u
    'ow': 'ow',
    '\u0259w': 'ow',                                 # schwa-w diphthong
    '\u0254j': 'oy',
    # --- Consonants (plain) ---
    'b': 'b',
    't\u0283': 'ch',
    'd': 'd',
    '\xf0': 'dh',                                    # eth
    'f': 'f',
    '\u0261': 'g',                                   # voiced velar stop
    'h': 'hh',
    'd\u0292': 'jh',
    'k': 'k',
    'l': 'l', '\u026b': 'l', '\u026b\u0329': 'l',
    'm': 'm', 'm\u0329': 'm',
    'n': 'n', 'n\u0329': 'n',
    '\u014b': 'ng',
    'p': 'p',
    '\u0279': 'r',                                   # alveolar approximant
    's': 's',
    '\u0283': 'sh',
    't': 't',
    '\u03b8': 'th',
    'v': 'v',
    'w': 'w',
    'j': 'y',
    'z': 'z',
    '\u0292': 'zh',
    '\u027e': 't',                                   # alveolar tap -> t (like dx)
    # --- Palatalized consonants (english_mfa: X + U+02B2) ---
    't\u02b2': 't',
    'd\u02b2': 'd',
    'b\u02b2': 'b',
    'f\u02b2': 'f',
    'm\u02b2': 'm',
    'n\u02b2': 'n',
    'k\u02b2': 'k',
    'p\u02b2': 'p',
    'v\u02b2': 'v',
    'l\u02b2': 'l',
    's\u02b2': 's',
    'z\u02b2': 'z',
    '\u0261\u02b2': 'g',
    'h\u02b2': 'hh',
    '\u0279\u02b2': 'r',
    # --- Dental variants (X + U+032A) ---
    't\u032a': 'th',                                 # dental t -> th
    'd\u032a': 'dh',                                 # dental d -> dh
    # --- Retroflex (english_mfa) ---
    '\u0288': 't',                                   # voiceless retroflex stop
    '\u0256': 'd',                                   # voiced retroflex stop
    # --- Palatal (english_mfa) ---
    '\u028e': 'l',                                   # palatal lateral -> l
    '\u0272': 'n',                                   # palatal nasal -> n
    '\u00e7': 'sh',                                  # voiceless palatal fricative -> sh
    'c': 'k',                                        # voiceless palatal stop -> k
    '\u025f': 'jh',                                  # voiced palatal stop -> jh
    # --- Labio-dental approximant ---
    '\u028b': 'v',                                   # labiodental approx -> v
}

# Map Tom's allophonic distinctions to MFA's broader phone set.
# MFA never produces ax, ix, dx, el, en -- it uses ah/ih, t/d, l, n instead.
# Used for phone label comparison in sequence alignment.
_PHONE_NORM = {
    'ax': 'ah',   # schwa -> open-mid back unrounded
    'ix': 'ih',   # reduced ih -> ih
    'dx': 't',    # alveolar flap -> voiceless alveolar stop (MFA's usual output)
    'el': 'l',    # syllabic l -> l
    'en': 'n',    # syllabic n -> n
}

def _norm_phone(ph):
    """Normalize a phone label for cross-inventory comparison."""
    return _PHONE_NORM.get(ph, ph)

_ap = argparse.ArgumentParser(description='Build Mara voice files.')
_ap.add_argument('--workers', type=int, default=os.cpu_count() or 4,
                 help='Thread count for parallel F0 extraction (default: cpu_count)')
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


def load_state(tom_key):
    """Load build_state.pkl; return empty dict if missing, stale, or wrong version."""
    if not os.path.exists(STATE_FILE):
        return {}
    try:
        with open(STATE_FILE, 'rb') as f:
            state = pickle.load(f)
        if state.get('version') != STATE_VERSION:
            print("  State file version mismatch -- full rebuild.")
            return {}
        if state.get('tom_key') != tom_key:
            print("  tom.vin changed -- full rebuild.")
            return {}
        return state.get('recordings', {})
    except Exception as e:
        print(f"  Could not load state file ({e}) -- full rebuild.")
        return {}


def save_state(tom_key, recordings):
    """Persist recordings cache to build_state.pkl."""
    state = {
        'version':    STATE_VERSION,
        'tom_key':    tom_key,
        'recordings': recordings,
    }
    with open(STATE_FILE, 'wb') as f:
        pickle.dump(state, f, protocol=4)


# -- Audio / signal helpers ------------------------------------------------------
def xor_codec(data):
    return bytes(b ^ XOR_KEY for b in data)


def riff_chunks(data, start=12, end=None):
    end = end or len(data)
    pos = start
    while pos + 8 <= end:
        tag = data[pos:pos+4]
        sz  = struct.unpack_from('<I', data, pos+4)[0]
        yield tag, pos+8, sz
        pos += 8 + sz + (sz & 1)


def pack_chunk(tag, body):
    pad = b'\x00' if len(body) & 1 else b''
    return tag + struct.pack('<I', len(body)) + body + pad


def load_encoded(path):
    with open(path, 'rb') as f:
        return xor_codec(f.read())


def pcm16_to_ulaw(pcm_bytes):
    import audioop
    return audioop.lin2ulaw(pcm_bytes, 2)


def downsample_3x(pcm_bytes):
    from scipy.signal import resample_poly
    samples = np.frombuffer(pcm_bytes, dtype='<i2').copy().astype(np.float64)
    out = resample_poly(samples, 1, 3)
    return np.clip(out, -32768, 32767).astype('<i2').tobytes()


# Target RMS level for all Mara recordings.
# Normalizing every recording to the same RMS eliminates the amplitude-level jumps
# that occur when the engine concatenates units from different recordings.
# Tom's measured per-unit RMS averages ~6900. Setting Mara close to this level
# ensures comparable energy in the output and reduces mu-law quantization artifacts
# at low amplitudes (mu-law has better SNR at higher levels).
TARGET_RMS = 6500.0

def normalize_rms(pcm_bytes, target_rms=TARGET_RMS):
    """Scale PCM16 bytes so the recording RMS equals target_rms.
    No-op if the signal is near silence (rms < 200) to avoid amplifying
    pure-silence or near-empty recordings.
    """
    samples = np.frombuffer(pcm_bytes, dtype='<i2').astype(np.float64)
    rms = np.sqrt(np.mean(samples ** 2)) if len(samples) > 0 else 0.0
    if rms < 200.0:
        return pcm_bytes
    scale = target_rms / rms
    out = np.clip(samples * scale, -32768, 32767).astype('<i2')
    return out.tobytes()


# --------------------------------------------------------------------------- #
#  Spectral envelope normalization                                              #
# --------------------------------------------------------------------------- #
# Qwen3-TTS generates each recording independently, producing different
# spectral tilts / formant balances across recordings.  RMS normalization
# matches volume, but not frequency balance.  Spectral envelope normalization
# computes each recording's average mel spectrum and applies a gentle EQ
# correction so all recordings match a global average.  This makes cross-
# recording joins spectrally smoother without changing the voice identity.

SPEC_FRAME  = 256       # 32 ms at 8 kHz
SPEC_HOP    = 128       # 16 ms hop
SPEC_N_MELS = 40        # number of mel bands for EQ (was 20; more = finer spectral match)
SPEC_MAX_DB = 20.0      # max per-band correction in dB (was 10; aggressive to reduce stutter)

def _build_mel_filterbank(n_fft, sr, n_mels):
    """Build a mel-scale filterbank matrix (n_mels x n_fft//2+1)."""
    def hz2mel(h):
        return 2595.0 * np.log10(1.0 + h / 700.0)
    def mel2hz(m):
        return 700.0 * (10.0 ** (m / 2595.0) - 1.0)
    n_bins = n_fft // 2 + 1
    lo, hi = hz2mel(0.0), hz2mel(sr / 2.0)
    mel_pts = np.linspace(lo, hi, n_mels + 2)
    hz_pts = mel2hz(mel_pts)
    bins = np.floor((n_fft + 1) * hz_pts / sr).astype(int)
    fb = np.zeros((n_mels, n_bins), dtype=np.float64)
    for m in range(1, n_mels + 1):
        lo_b, mid_b, hi_b = int(bins[m - 1]), int(bins[m]), int(bins[m + 1])
        if mid_b > lo_b:
            r = np.arange(lo_b, mid_b)
            fb[m - 1, r] = (r - lo_b) / float(mid_b - lo_b)
        if hi_b > mid_b:
            r = np.arange(mid_b, hi_b)
            fb[m - 1, r] = (hi_b - r) / float(hi_b - mid_b)
    return fb

_SPEC_FB = _build_mel_filterbank(SPEC_FRAME, 8000, SPEC_N_MELS)
_SPEC_WIN = np.hamming(SPEC_FRAME).astype(np.float64)


def compute_avg_mel_spectrum(pcm_bytes):
    """Compute average mel power spectrum of a PCM16 recording.
    Returns (n_mels,) array of average mel-band energies, or None if too short.
    """
    samples = np.frombuffer(pcm_bytes, dtype='<i2').astype(np.float64)
    n = len(samples)
    if n < SPEC_FRAME:
        return None
    n_frames = (n - SPEC_FRAME) // SPEC_HOP + 1
    if n_frames < 1:
        return None
    avg_mel = np.zeros(SPEC_N_MELS, dtype=np.float64)
    for i in range(n_frames):
        frame = samples[i * SPEC_HOP : i * SPEC_HOP + SPEC_FRAME]
        windowed = frame * _SPEC_WIN
        power = np.abs(np.fft.rfft(windowed)) ** 2
        mel = _SPEC_FB @ power
        avg_mel += mel
    avg_mel /= n_frames
    return avg_mel


def spectral_eq(pcm_bytes, rec_mel, target_mel, max_db=SPEC_MAX_DB):
    """Apply mel-band EQ to match target spectral envelope.
    Computes per-mel-band gain, interpolates to full frequency resolution,
    and applies via overlap-add STFT.  Gains are clamped to +/- max_db.
    """
    samples = np.frombuffer(pcm_bytes, dtype='<i2').astype(np.float64)
    n = len(samples)
    if n < SPEC_FRAME:
        return pcm_bytes

    eps = 1e-10
    # Per-band linear gain
    gains_mel = np.sqrt((target_mel + eps) / (rec_mel + eps))
    # Clamp to +/- max_db
    max_lin = 10.0 ** (max_db / 20.0)
    min_lin = 10.0 ** (-max_db / 20.0)
    gains_mel = np.clip(gains_mel, min_lin, max_lin)

    # Interpolate mel gains to full frequency resolution using filterbank
    # Each frequency bin gets a weighted average of mel gains
    n_bins = SPEC_FRAME // 2 + 1
    fb_sum = _SPEC_FB.sum(axis=0) + eps  # (n_bins,)
    gains_freq = (_SPEC_FB.T @ gains_mel) / fb_sum  # (n_bins,)
    # Bins outside all mel bands get gain 1.0
    outside = fb_sum < eps * 2
    gains_freq[outside] = 1.0

    # Apply via overlap-add STFT
    output = np.zeros(n, dtype=np.float64)
    weight = np.zeros(n, dtype=np.float64)
    n_frames = (n - SPEC_FRAME) // SPEC_HOP + 1
    for i in range(n_frames):
        start = i * SPEC_HOP
        end = start + SPEC_FRAME
        frame = samples[start:end] * _SPEC_WIN
        spec = np.fft.rfft(frame)
        spec *= gains_freq
        frame_out = np.fft.irfft(spec, n=SPEC_FRAME)
        output[start:end] += frame_out * _SPEC_WIN
        weight[start:end] += _SPEC_WIN ** 2
    # Handle tail samples beyond last full frame
    weight[weight < eps] = 1.0
    output /= weight
    # Samples after the last frame: copy original
    last_end = (n_frames - 1) * SPEC_HOP + SPEC_FRAME if n_frames > 0 else 0
    if last_end < n:
        output[last_end:] = samples[last_end:]
    return np.clip(output, -32768, 32767).astype('<i2').tobytes()


def normalize_phone(label):
    lbl = label.strip().lower().rstrip('012')
    if lbl in _SILENCE_LABELS:
        return 'pau'
    # Try IPA -> ARPAbet conversion (for english_mfa TextGrids).
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
    # Word-level OOV labels leak into the phone tier from english_mfa
    # (e.g., "the", "to", "of"). Treat as silence/skip.
    if len(lbl) > 3 or (len(lbl) > 1 and lbl.isascii() and lbl.isalpha()):
        return 'pau'
    return lbl


def interval_at_time(all_intervals, t_sec):
    """Binary-search all_intervals; return (start, end, label) containing t_sec."""
    lo, hi = 0, len(all_intervals) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        s, e, lbl = all_intervals[mid]
        if t_sec < s:
            hi = mid - 1
        elif t_sec >= e:
            lo = mid + 1
        else:
            return s, e, lbl
    if all_intervals:
        if t_sec <= all_intervals[0][0]:
            return all_intervals[0]
        return all_intervals[-1]
    return 0.0, 0.0, ''


def phone_at_time(all_intervals, t_sec):
    s, e, lbl = interval_at_time(all_intervals, t_sec)
    return normalize_phone(lbl)


def f0_track_from_ulaw(ulaw_bytes):
    """Return PyWorld HARVEST F0 array (1 value/ms) or None on failure."""
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


# Scale Mara's F0 values into Tom's range so the f0tr CART tree predictions
# (trained on Tom, avg ~118 Hz) produce reasonable target costs.
# Mara HARVEST avg ~184 Hz; Tom avg ~118 Hz; ratio = 118/184 = 0.641.
#
# F0 encoding (CONFIRMED 2026-03-13): f0_start/f0_end/f0_mid are stored as
# direct Hz integers (u8). Tom's nonzero range: 99-150 Hz; median 118 Hz.
# Values 0 = unvoiced/silence. Values 1-98 do NOT appear in Tom (all nonzero
# values are >= 99). The engine's quadratic F0 cost penalizes values far from
# the f0tr tree prediction (~117 Hz). Tom's p5=106, p95=127.
#
# F0_SCALE maps Mara's higher average pitch (184 Hz) to Tom's median (118 Hz).
# After scaling: if result < TOM_F0_MIN, the frame is treated as unvoiced (0),
# mirroring Tom's convention where 1-98 never appear. If result > TOM_F0_MAX,
# clamp to 150 (Tom's confirmed maximum) to avoid outlier cost penalties.
F0_SCALE = 0.641

# Tom f0 nonzero range (confirmed from 169,579 units): strictly [99, 150] for
# f0_start and f0_mid; [99, 156] for f0_end. Use 150 as the cap for all three.
TOM_F0_MIN = 99   # below this -> treat as unvoiced (store 0)
TOM_F0_MAX = 150  # Tom's confirmed max; clamp above this

def lookup_f0(f0_arr, pos_ms):
    if f0_arr is None or len(f0_arr) == 0:
        return 0
    idx = max(0, min(len(f0_arr) - 1, int(pos_ms)))
    hz = f0_arr[idx]
    if hz < 50.0:
        return 0
    scaled = round(hz * F0_SCALE)
    # Values below Tom's minimum are treated as unvoiced (not stored as 1-98
    # which never appear in Tom). Values above Tom's max are clamped.
    if scaled < TOM_F0_MIN:
        return 0
    return min(TOM_F0_MAX, scaled)


# -- Per-recording worker --------------------------------------------------------
_REAL_PHONE_SET = {'', 'spn', 'sil', 'sp'}  # MFA labels that are NOT real phones
_SILENCE_PCS    = {32, 255}                  # phone_center values: pau=32, unknown=255


def _build_phone_groups(units):
    """Split lp-sorted units into consecutive runs of the same phone_center.

    Returns (speech_groups, silence_units):
      speech_groups:  list of (lp_min, tom_span, [units]) for each speech run
      silence_units:  units whose phone_center is silence/unknown (pc in _SILENCE_PCS)
    """
    runs = []          # [(pc, [units])]
    silence_units = []
    for unit in units:  # already sorted by lp
        uid, lp, dl, is_first, pc = unit
        if pc in _SILENCE_PCS:
            silence_units.append(unit)
            continue
        if runs and runs[-1][0] == pc:
            runs[-1][1].append(unit)
        else:
            runs.append((pc, [unit]))

    speech_groups = []
    for pc, grp in runs:
        lp_min    = grp[0][1]
        lp_end    = max(u[1] + u[2] for u in grp)   # max(lp + dl)
        tom_span  = max(1, lp_end - lp_min)
        speech_groups.append((lp_min, tom_span, grp))
    return speech_groups, silence_units


def _seq_align(speech_groups, mfa_speech, min_match_frac=0.15):
    """Align Tom speech groups to MFA intervals via Needleman-Wunsch sequence alignment.

    Handles arbitrary count differences between the two sequences.
    Uses phone label similarity (with allophone normalization) for scoring.

    Returns list of (tom_group_index, mfa_index) pairs for matched positions,
    or None if the alignment quality is too poor (< min_match_frac coverage).
    """
    n = len(speech_groups)
    m = len(mfa_speech)
    if n == 0 or m == 0:
        return None

    # Phone labels for scoring.
    tom_phs = [_norm_phone(PHONE_LABELS[g[2][0][4]])
               if g[2][0][4] < len(PHONE_LABELS) else ''
               for g in speech_groups]
    mfa_phs = [_norm_phone(normalize_phone(iv[2])) for iv in mfa_speech]

    # Scoring: +2 exact match, +1 vowel/vowel or consonant/consonant, -1 mismatch.
    _VOWELS = {'aa', 'ae', 'ah', 'ao', 'aw', 'ay', 'eh', 'er', 'ey',
               'ih', 'iy', 'ow', 'oy', 'uh', 'uw'}
    def _sim(a, b):
        if a == b:
            return 2
        if a in _VOWELS and b in _VOWELS:
            return 1
        if a not in _VOWELS and b not in _VOWELS and a and b:
            return 0
        return -1

    GAP = -0.5

    # DP with linear-space traceback (store only direction codes).
    # 0 = diagonal (match/mismatch), 1 = skip Tom (gap in MFA), 2 = skip MFA (gap in Tom).
    dp_prev = [GAP * j for j in range(m + 1)]
    dp_cur = [0.0] * (m + 1)
    bt = [[0] * (m + 1) for _ in range(n + 1)]
    for j in range(1, m + 1):
        bt[0][j] = 2

    for i in range(1, n + 1):
        dp_cur[0] = GAP * i
        bt[i][0] = 1
        for j in range(1, m + 1):
            s_diag = dp_prev[j - 1] + _sim(tom_phs[i - 1], mfa_phs[j - 1])
            s_skip_tom = dp_prev[j] + GAP
            s_skip_mfa = dp_cur[j - 1] + GAP
            if s_diag >= s_skip_tom and s_diag >= s_skip_mfa:
                dp_cur[j] = s_diag
                bt[i][j] = 0
            elif s_skip_tom >= s_skip_mfa:
                dp_cur[j] = s_skip_tom
                bt[i][j] = 1
            else:
                dp_cur[j] = s_skip_mfa
                bt[i][j] = 2
        dp_prev, dp_cur = dp_cur, dp_prev

    # Traceback.
    pairs = []
    i, j = n, m
    while i > 0 or j > 0:
        if i > 0 and j > 0 and bt[i][j] == 0:
            pairs.append((i - 1, j - 1))
            i -= 1
            j -= 1
        elif i > 0 and (j == 0 or bt[i][j] == 1):
            i -= 1
        else:
            j -= 1
    pairs.reverse()

    # Require minimum coverage to accept.
    if len(pairs) < min_match_frac * min(n, m):
        return None
    return pairs


def _refine_mfa_interval(ulaw_bytes, start_ms, end_ms, cap):
    """Refine an MFA phone interval so it points to audible speech.

    Checks audio energy at the MFA boundaries.  If silent, searches
    outward (forward first, then backward) across the ENTIRE recording
    to find the nearest speech.  Returns (refined_start, refined_end).
    """
    import audioop as _ao_ref
    if ulaw_bytes is None or len(ulaw_bytes) < 160:
        return start_ms, end_ms
    RMS_THRESH = 800
    CHECK_WIN = 10  # 10ms = 80 samples check window
    check_nb = CHECK_WIN * 8
    phone_dur = max(1, end_ms - start_ms)

    def _rms_at(pos_ms):
        bo = pos_ms * 8
        if bo < 0 or bo + check_nb > len(ulaw_bytes):
            return 0.0
        try:
            p = _ao_ref.ulaw2lin(ulaw_bytes[bo:bo + check_nb], 2)
            s = np.frombuffer(p, dtype='<i2').astype(np.float64)
            return np.sqrt(np.mean(s ** 2)) if len(s) >= 4 else 0.0
        except Exception:
            return 0.0

    # Check if start is already in speech
    if _rms_at(start_ms) >= RMS_THRESH:
        # Start is fine; just refine end if needed
        refined_end = end_ms
        if _rms_at(max(0, end_ms - CHECK_WIN)) < RMS_THRESH:
            for off in range(1, min(end_ms - start_ms, cap)):
                cand = end_ms - off
                if cand <= start_ms:
                    break
                if _rms_at(max(0, cand - CHECK_WIN)) >= RMS_THRESH:
                    refined_end = cand
                    break
        return start_ms, refined_end

    # Start is silent -- search entire recording for nearest speech.
    # Interleave forward and backward to find the CLOSEST speech.
    max_search = max(cap, start_ms)  # search entire recording
    best_start = None
    for off in range(1, max_search):
        # Try forward first
        fwd = start_ms + off
        if fwd + check_nb // 8 <= cap and _rms_at(fwd) >= RMS_THRESH:
            best_start = fwd
            break
        # Then backward
        bwd = start_ms - off
        if bwd >= 0 and _rms_at(bwd) >= RMS_THRESH:
            best_start = bwd
            break

    if best_start is not None:
        # Found speech; check if it's actually audible (not just barely above threshold)
        refined_start = max(0, best_start)
        refined_end = min(cap, refined_start + phone_dur)
        # Verify the found region has real speech
        if _rms_at(refined_start) >= RMS_THRESH:
            return refined_start, refined_end

    # No speech found anywhere -- return (-1, -1) to signal "disable this unit"
    return -1, -1
    return refined_start, refined_end


def process_recording(rec_name, units, tom_max_end, mara_n, cap, ulaw_bytes,
                      all_intervals=None, xmax=0.0):
    """Compute new (lp, dl, pc, f0s, f0e, f0m) for every unit in one recording.

    Strategy: build lp/dl directly FROM MFA phone boundaries, the same way
    SpeechWorks built Tom's unit table from his recordings.  Each MFA phone
    interval is the ground truth for where that phone exists in the audio.
    Each phone is split into two halfphones at the midpoint.

    No post-processing (inflation, gap closure, relocation, monotonicity
    enforcement).  The MFA boundaries ARE the ground truth.  The only
    refinement: if an MFA boundary lands on silence, shift it to where
    speech audio actually begins (per-interval energy correction).

    Returns (rec_units_out, f0_status, mfa_mode).
    """
    f0_arr = None
    f0_status = 0
    if ulaw_bytes is not None:
        f0_arr = f0_track_from_ulaw(ulaw_bytes)
        f0_status = 1 if f0_arr is not None else -1

    rec_units_out = []
    if mara_n == 0:
        # No audio for this recording -- use Tom's original positions.
        # Never dl=0: engine can select any unit via same-rec continuation.
        for uid, lp, dl, is_first, pc in units:
            rec_units_out.append((uid, lp, max(1, dl), 255, -1, -1, -1))
        return rec_units_out, f0_status, 0

    # -- MFA path: build lp/dl directly from phone boundaries --------------------
    use_mfa = (all_intervals is not None and
               any(lbl not in _REAL_PHONE_SET for _, _, lbl in all_intervals))

    if use_mfa:
        mfa_speech = [(s, e, l) for s, e, l in all_intervals
                      if l not in _REAL_PHONE_SET]
        speech_groups, silence_units = _build_phone_groups(units)
        pairs = _seq_align(speech_groups, mfa_speech)

        if pairs is not None and len(pairs) >= 1:
            # Build lookup: tom_group_index -> (mfa_start_ms, mfa_end_ms)
            # Only keep exact phone matches.
            matched_groups = {}
            for ti, mi in pairs:
                tom_pc = speech_groups[ti][2][0][4]
                tom_ph = _norm_phone(PHONE_LABELS[tom_pc]) if tom_pc < len(PHONE_LABELS) else ''
                mfa_s, mfa_e, mfa_lbl = mfa_speech[mi]
                mfa_ph = _norm_phone(normalize_phone(mfa_lbl))
                if tom_ph != mfa_ph:
                    continue
                # Convert to ms and refine boundaries against actual audio.
                raw_start = max(0, min(cap, round(mfa_s * 1000)))
                raw_end = max(raw_start, min(cap, round(mfa_e * 1000)))
                ref_start, ref_end = _refine_mfa_interval(
                    ulaw_bytes, raw_start, raw_end, cap)
                if ref_start < 0:
                    continue  # no speech found in recording; treat as unmatched
                matched_groups[ti] = (ref_start, ref_end)

            n_matched = 0
            n_unmatched = 0
            # Minimum lp spacing per halfphone. The engine's WSOLA output
            # duration = next.lp - this.lp for same-rec units. Tiny spacing
            # (1-5ms from tight MFA intervals) causes the output to be
            # severely compressed. Tom's typical halfphone spacing is 15-50ms.
            MIN_UNIT_DUR = 25  # ms -- minimum output duration per halfphone
            for gi, (lp_min, tom_span, grp) in enumerate(speech_groups):
                if gi in matched_groups:
                    ms, me = matched_groups[gi]
                    mfa_span = max(1, me - ms)
                    n = len(grp)
                    # Use Tom's relative spacing if it gives better results.
                    # Tom's spacing preserves prosodic timing; MFA might crush
                    # multiple halfphones into a few ms.
                    tom_lps = [u[1] for u in grp]
                    tom_grp_span = tom_lps[-1] - tom_lps[0] if n > 1 else 0
                    # unit_dur: at least MIN_UNIT_DUR, at least mfa_span/n,
                    # ideally matching Tom's spacing.
                    if n > 1 and tom_grp_span > 0:
                        tom_unit_dur = tom_grp_span / (n - 1)
                        unit_dur = max(MIN_UNIT_DUR, round(tom_unit_dur))
                    else:
                        unit_dur = max(MIN_UNIT_DUR, mfa_span // max(1, n))
                    for idx, (uid, lp, dl, is_first, pc) in enumerate(grp):
                        new_lp = max(0, min(cap, ms + idx * unit_dur))
                        # dl: extend to cover at least unit_dur of source audio,
                        # capped by recording length.
                        new_dl = max(unit_dur, min(cap - new_lp,
                                                   max(1, me - new_lp)))
                        f0s = lookup_f0(f0_arr, new_lp)
                        f0e = lookup_f0(f0_arr, new_lp + new_dl)
                        f0m = lookup_f0(f0_arr, new_lp + new_dl // 2)
                        rec_units_out.append((uid, new_lp, new_dl, 255,
                                              f0s, f0e, f0m))
                        n_matched += 1
                else:
                    # Fallback: proportional scaling for unmatched phones.
                    # NEVER use lp=0,dl=0 -- the engine can select ANY unit
                    # from a recording via same-rec continuation (bypassing
                    # prsl), so every unit MUST have a valid lp/dl.
                    denom = max(1, tom_max_end * 8)
                    for uid, lp, dl, is_first, pc in grp:
                        new_lp = max(0, min(cap, round(lp * mara_n / denom)))
                        new_dl = max(1, min(cap - new_lp, max(1, round(dl * mara_n / denom))))
                        f0s = lookup_f0(f0_arr, new_lp)
                        f0e = lookup_f0(f0_arr, new_lp + new_dl)
                        f0m = lookup_f0(f0_arr, new_lp + new_dl // 2)
                        rec_units_out.append((uid, new_lp, new_dl, 255,
                                              f0s, f0e, f0m))
                        n_unmatched += 1

            # Silence units: place at MFA silence intervals or recording start.
            # Must have dl >= 1 -- engine can select ANY unit via same-rec
            # continuation, so dl=0 is never safe.
            for uid, lp, dl, is_first, pc in silence_units:
                sil_ivs = [(s, e) for s, e, l in all_intervals
                           if l in _REAL_PHONE_SET and l != '']
                if sil_ivs:
                    tom_total = max(1, max(u[1] + u[2] for u in units))
                    rel = lp / tom_total if tom_total > 0 else 0.0
                    target = rel * (mara_n / 8000.0)
                    best = min(sil_ivs, key=lambda iv: abs((iv[0]+iv[1])/2 - target))
                    new_lp = max(0, min(cap, round(best[0] * 1000)))
                    scale = mara_n / max(1, tom_total * 8)
                    new_dl = max(1, min(cap - new_lp, max(1, round(dl * scale))))
                else:
                    new_lp = 0
                    tom_total = max(1, max(u[1] + u[2] for u in units))
                    scale = mara_n / max(1, tom_total * 8)
                    new_dl = max(1, min(cap - new_lp, max(1, round(dl * scale))))
                f0s = lookup_f0(f0_arr, new_lp)
                f0e = lookup_f0(f0_arr, new_lp + new_dl)
                f0m = lookup_f0(f0_arr, new_lp + new_dl // 2)
                rec_units_out.append((uid, new_lp, new_dl, 255,
                                      f0s, f0e, f0m))

            # Monotonicity + minimum spacing: sort by Tom's lp order
            # and ensure MIN_UNIT_DUR gap between consecutive active units.
            _uid_tom_lp = {u[0]: u[1] for u in units}
            rec_units_out.sort(key=lambda e: _uid_tom_lp.get(e[0], 0))
            _prev = -(MIN_UNIT_DUR + 1)
            _fixed = []
            for _uid, _lp, _dl, _pc, _f0s, _f0e, _f0m in rec_units_out:
                _min_lp = _prev + MIN_UNIT_DUR
                if _dl > 0 and _lp < _min_lp:
                    _lp = min(_min_lp, cap)  # never exceed cap
                    _dl = max(1, min(cap - _lp, _dl))  # dl >= 1 always
                if _dl > 0:
                    _prev = _lp
                _fixed.append((_uid, _lp, _dl, _pc, _f0s, _f0e, _f0m))
            rec_units_out = _fixed

            mfa_mode = 2 if len(speech_groups) != len(mfa_speech) else 1
            return rec_units_out, f0_status, mfa_mode

    # -- Fallback: proportional scaling (no MFA) ---------------------------------
    if tom_max_end > 0:
        denom = tom_max_end * 8
        _MIN_FB_SPACING = 15  # minimum spacing for fallback path too
        _prev_fb = -(_MIN_FB_SPACING + 1)
        for uid, lp, dl, is_first, pc in units:
            new_lp = max(0, min(cap, round(lp * mara_n / denom)))
            new_dl = max(1, min(cap - new_lp, max(1, round(dl * mara_n / denom))))
            # Minimum spacing for active units
            _min_fb_lp = _prev_fb + _MIN_FB_SPACING
            if new_dl > 0 and new_lp < _min_fb_lp:
                new_lp = min(_min_fb_lp, cap)  # never exceed cap
                new_dl = max(1, min(cap - new_lp, new_dl))  # dl >= 1 always
            if new_dl > 0:
                _prev_fb = new_lp
            f0s = lookup_f0(f0_arr, new_lp)
            f0e = lookup_f0(f0_arr, new_lp + new_dl)
            f0m = lookup_f0(f0_arr, new_lp + new_dl // 2)
            rec_units_out.append((uid, new_lp, new_dl, 255, f0s, f0e, f0m))
    else:
        for uid, lp, dl, is_first, pc in units:
            rec_units_out.append((uid, lp, dl, 255, -1, -1, -1))
    return rec_units_out, f0_status, 0


# -- TextGrid parser -------------------------------------------------------------
def parse_textgrid_all(path):
    """Return (all_intervals, xmax) for the phones tier.

    all_intervals: [(start_sec, end_sec, label)] including silence.
    xmax: full recording duration (global header xmax).
    """
    all_intervals = []
    xmax = 0.0
    xmax_found = False
    in_phones_tier = False
    in_interval = False
    cur_xmin = cur_xmax_iv = None

    with open(path, encoding='utf-8', errors='replace') as fh:
        for line in fh:
            line = line.strip()
            if not xmax_found and line.startswith('xmax ='):
                try:
                    xmax = float(line.split('=', 1)[1].strip())
                    xmax_found = True
                except ValueError:
                    pass
            if 'name = "phones"' in line:
                in_phones_tier = True
            elif in_phones_tier:
                if line.startswith('intervals ['):
                    in_interval = True
                    cur_xmin = cur_xmax_iv = None
                elif in_interval:
                    if line.startswith('xmin ='):
                        try:
                            cur_xmin = float(line.split('=', 1)[1].strip())
                        except ValueError:
                            pass
                    elif line.startswith('xmax ='):
                        try:
                            cur_xmax_iv = float(line.split('=', 1)[1].strip())
                        except ValueError:
                            pass
                    elif line.startswith('text ='):
                        label = line.split('=', 1)[1].strip().strip('"')
                        if cur_xmin is not None and cur_xmax_iv is not None:
                            all_intervals.append((cur_xmin, cur_xmax_iv, label))
                        in_interval = False
                elif line.startswith('item ['):
                    break

    return all_intervals, xmax


# -- 1. Parse tom.vin ------------------------------------------------------------
print("Parsing tom.vin ...")
vin = load_encoded(TOM_VIN)
assert vin[:4] == b'RIFF' and vin[8:12] == b'svin', "Not a valid tom.vin"

vin_chunks = {tag: (ds, sz) for tag, ds, sz in riff_chunks(vin)}

feat_off, feat_sz = vin_chunks[b'feat']
feat = vin[feat_off : feat_off + feat_sz]
fn_idx = feat.find(b'filename')
fn_count = struct.unpack_from('<I', feat, fn_idx + 8)[0]
p = fn_idx + 12
filenames = {}
for _ in range(fn_count):
    nlen = struct.unpack_from('<H', feat, p)[0]
    name = feat[p+2 : p+2+nlen].decode('latin-1', errors='replace')
    stored_id = struct.unpack_from('<I', feat, p+2+nlen)[0]
    filenames[stored_id] = name
    p += 2 + nlen + 4
print(f"  {fn_count} filenames in feat ({len(filenames)} distinct stored_ids)")

unit_off, unit_sz = vin_chunks[b'unit']
unit_data_ds = unit_data_sz = None
for tag, ds, sz in riff_chunks(vin, unit_off):
    if tag == b'data':
        unit_data_ds, unit_data_sz = ds, sz
        break
assert unit_data_ds is not None
assert unit_data_sz == N_UNITS * UNIT_SIZE

fidx_max_end = {}
for i in range(N_UNITS):
    base = unit_data_ds + i * UNIT_SIZE
    fidx = struct.unpack_from('<H', vin, base +  4)[0]
    lp   = struct.unpack_from('<H', vin, base +  6)[0]
    dl   = struct.unpack_from('<H', vin, base + 10)[0]
    end  = lp + dl
    if end > fidx_max_end.get(fidx, 0):
        fidx_max_end[fidx] = end
print(f"  {N_UNITS} units, {len(fidx_max_end)} distinct file_idx values")

tom_key = _file_key(TOM_VIN)


# -- 2. Parse tom8.vdb indx ------------------------------------------------------
print("Parsing tom8.vdb ...")
vdb = load_encoded(TOM_VDB)
assert vdb[:4] == b'RIFF' and vdb[8:12] == b'WAVE'

vdb_chunks = {tag: (ds, sz) for tag, ds, sz in riff_chunks(vdb)}
data_ds, data_sz = vdb_chunks[b'data']
indx_ds, indx_sz = vdb_chunks[b'indx']
indx_end = indx_ds + indx_sz
p = indx_ds + 4
indx_entries = []
while p <= indx_end - 6:
    off  = struct.unpack_from('<I', vdb, p)[0]
    nlen = struct.unpack_from('<H', vdb, p+4)[0]
    if p + 6 + nlen > indx_end:
        break
    name = vdb[p+6 : p+6+nlen].decode('latin-1', errors='replace')
    indx_entries.append((off, name))
    p += 6 + nlen
assert indx_entries[-1][1] == ''

tom_pcm = {}
for i, (off, name) in enumerate(indx_entries[:-1]):
    next_off = indx_entries[i+1][0]
    sz = next_off - off
    if sz > 0 and name:
        tom_pcm[name] = vdb[data_ds + off : data_ds + off + sz]
print(f"  {len(indx_entries)-1} indx entries, {len(tom_pcm)} non-empty recordings")


# -- 3. Load and downsample Mara's WAVs ------------------------------------------
# Two-pass pipeline:
#   Pass 1: load WAVs, downsample to 8kHz PCM16, compute per-recording mel spectrum
#   Pass 2: apply spectral EQ (match global average), RMS normalize, mu-law encode
print("Loading Mara's WAVs (pass 1: downsample + spectral analysis) ...")
wav_files = sorted(f for f in os.listdir(WAV_DIR) if f.lower().endswith('.wav'))
mara_pcm16 = {}   # name -> 8kHz PCM16 bytes (temporary, pre-normalization)
wav_mtimes = {}    # name -> (mtime, size)

for fname in tqdm(wav_files, desc="WAVs", unit="file"):
    name = fname[:-4]
    path = os.path.join(WAV_DIR, fname)
    wav_mtimes[name] = _file_key(path)
    with wave.open(path) as w:
        n_ch      = w.getnchannels()
        sampwidth = w.getsampwidth()
        framerate = w.getframerate()
        raw_pcm   = w.readframes(w.getnframes())
    if n_ch != 1 or sampwidth != 2:
        tqdm.write(f"  SKIP {fname}: not mono 16-bit")
        continue
    if framerate == 24000:
        pcm_8k = downsample_3x(raw_pcm)
    elif framerate == 8000:
        pcm_8k = raw_pcm
    else:
        import math
        from scipy.signal import resample_poly
        g = math.gcd(framerate, 8000)
        up, down = 8000 // g, framerate // g
        samples = np.frombuffer(raw_pcm, dtype='<i2').copy().astype(np.float64)
        out = resample_poly(samples, up, down)
        pcm_8k = np.clip(out, -32768, 32767).astype('<i2').tobytes()
    mara_pcm16[name] = pcm_8k
print(f"  {len(mara_pcm16)} recordings loaded")

# -- 3.1 Spectral envelope normalization -----------------------------------------
# Compute per-recording average mel spectrum, then a global target, and apply
# gentle EQ correction so all recordings share the same spectral balance.
print("Computing spectral envelopes ...")
rec_mel_spectra = {}  # name -> (n_mels,) array
for name, pcm in mara_pcm16.items():
    mel = compute_avg_mel_spectrum(pcm)
    if mel is not None:
        rec_mel_spectra[name] = mel

if rec_mel_spectra:
    # Global target = geometric mean of all recording spectra (in log domain)
    all_mels = np.array(list(rec_mel_spectra.values()))  # (N, n_mels)
    log_mels = np.log(all_mels + 1e-10)
    global_target_mel = np.exp(np.mean(log_mels, axis=0))
    # Show statistics
    gains_db_all = []
    for name, rm in rec_mel_spectra.items():
        g = np.sqrt((global_target_mel + 1e-10) / (rm + 1e-10))
        g = np.clip(g, 10.0 ** (-SPEC_MAX_DB / 20.0), 10.0 ** (SPEC_MAX_DB / 20.0))
        gains_db_all.append(20.0 * np.log10(g))
    gains_db_arr = np.array(gains_db_all)
    print(f"  {len(rec_mel_spectra)} recordings analyzed ({SPEC_N_MELS} mel bands)")
    print(f"  Per-band correction range: [{gains_db_arr.min():.1f}, {gains_db_arr.max():.1f}] dB")
    print(f"  Mean abs correction: {np.mean(np.abs(gains_db_arr)):.1f} dB")
else:
    global_target_mel = None
    print("  No recordings long enough for spectral analysis -- skipping EQ")

print("Applying spectral EQ + RMS normalization + mu-law encoding (pass 2) ...")
mara_pcm = {}   # name -> 8kHz mu-law bytes (final)
eq_applied = 0
eq_skipped = 0
for name, pcm in tqdm(mara_pcm16.items(), desc="EQ+encode", unit="rec"):
    if global_target_mel is not None and name in rec_mel_spectra:
        pcm = spectral_eq(pcm, rec_mel_spectra[name], global_target_mel)
        eq_applied += 1
    else:
        eq_skipped += 1
    mara_pcm[name] = pcm16_to_ulaw(normalize_rms(pcm))
del mara_pcm16  # free memory
print(f"  Spectral EQ: {eq_applied} applied, {eq_skipped} skipped (too short)")
print(f"  {len(mara_pcm)} recordings ready")


# -- 3.5. Load MFA TextGrid alignments ------------------------------------------
print("Loading MFA TextGrid alignments ...")
tg_files = sorted(f for f in os.listdir(TG_DIR) if f.endswith('.TextGrid')) \
           if os.path.isdir(TG_DIR) else []
tg_data   = {}   # name -> (all_intervals, xmax)
tg_mtimes = {}   # name -> (mtime, size)

for fname in tqdm(tg_files, desc="TextGrids", unit="file"):
    name = fname[:-9]
    path = os.path.join(TG_DIR, fname)
    tg_mtimes[name] = _file_key(path)
    ivs, xmax = parse_textgrid_all(path)
    if ivs and xmax > 0.0:
        tg_data[name] = (ivs, xmax)
print(f"  {len(tg_data)} TextGrids loaded")


# -- 4. Build mara8.vdb ----------------------------------------------------------
print("Building mara8.vdb ...")
regular = indx_entries[:-1]

new_entry_pcm  = []
name_n_samples = {}

for _, name in regular:
    if name in mara_pcm:
        pcm = mara_pcm[name]
    elif name in tom_pcm:
        pcm = tom_pcm[name]
    else:
        pcm = b''
    new_entry_pcm.append(pcm)
    if name:
        name_n_samples[name] = len(pcm)

data_buf = io.BytesIO()
new_indx = []
for i, (_, name) in enumerate(regular):
    new_indx.append((data_buf.tell(), name))
    data_buf.write(new_entry_pcm[i])
new_indx.append((data_buf.tell(), ''))
new_data_bytes = data_buf.getvalue()
# Pad VDB data with silence so WSOLA overlap-add lookahead does not read
# past the buffer end when a unit lands near the last byte.
# mu-law silence = 0xFF; 8192 bytes = 1024 "units" of lookahead room.
WSOLA_PAD = 8192
new_data_bytes = new_data_bytes + b'\xff' * WSOLA_PAD

indx_buf = io.BytesIO()
indx_buf.write(struct.pack('<I', len(new_indx)))
for off, name in new_indx:
    name_enc = name.encode('latin-1')
    indx_buf.write(struct.pack('<I', off))
    indx_buf.write(struct.pack('<H', len(name_enc)))
    indx_buf.write(name_enc)
new_indx_bytes = indx_buf.getvalue()

def extract_chunk(data, tag_ds, tag_sz):
    return data[tag_ds-8 : tag_ds-8 + 8 + tag_sz + (tag_sz & 1)]

list_ds, list_sz = vdb_chunks[b'LIST']
fmt_ds,  fmt_sz  = vdb_chunks[b'fmt ']
riff_body = (
    b'WAVE' +
    extract_chunk(vdb, list_ds, list_sz) +
    extract_chunk(vdb, fmt_ds,  fmt_sz)  +
    pack_chunk(b'indx', new_indx_bytes) +
    pack_chunk(b'data', new_data_bytes)
)
out_vdb = b'RIFF' + struct.pack('<I', len(riff_body)) + riff_body
with open(VDB_OUT, 'wb') as f:
    f.write(xor_codec(out_vdb))
print(f"  Wrote {VDB_OUT}  ({len(out_vdb):,} bytes)")


# -- 5. Build new unit/data bytes ------------------------------------------------
print("Building unit table ...")

# Load previously computed results.  Key per recording = (wav_key, tg_key).
cached_recs = load_state(tom_key)   # name -> {'key': ..., 'units': [...]}

units_by_fidx = defaultdict(list)
for i in range(N_UNITS):
    base     = unit_data_ds + i * UNIT_SIZE
    fidx     = struct.unpack_from('<H', vin, base +  4)[0]
    lp       = struct.unpack_from('<H', vin, base +  6)[0]
    dl       = struct.unpack_from('<H', vin, base + 10)[0]
    is_first = vin[base + 21]   # unit.is_first_half
    pc       = vin[base + 20]   # unit.phone_center (Tom's authoritative label)
    units_by_fidx[fidx].append((i, lp, dl, is_first, pc))
for fidx in units_by_fidx:
    units_by_fidx[fidx].sort(key=lambda x: x[1])  # sort by lp

# new_unit_data[uid] = (new_lp, new_dl, new_pc, new_f0s, new_f0e, new_f0m)
# Sentinels: new_pc=255 -> keep Tom's; new_f0*=-1 -> keep Tom's.
new_unit_data = {}
new_cached_recs = {}   # will replace cached_recs at the end

cache_hits = cache_misses = 0
tg_count = fuzzy_count = spn_count = prop_count = energy_count = zero_count = 0
mfa_aligned_uids = set()
f0_ok = f0_fail = f0_tom_fallback = 0

# -- Phase A: apply cache hits, collect cache misses for parallel processing -----
# task tuple: (fidx, units, rec_name, tom_max_end, mara_n, cap,
#              all_intervals, xmax, cache_key)
miss_tasks = []

for fidx, units in units_by_fidx.items():
    rec_name    = filenames.get(fidx, '')
    tom_max_end = fidx_max_end.get(fidx, 0)
    mara_n      = name_n_samples.get(rec_name, 0)
    cap         = mara_n // 8 - 1 if mara_n >= 16 else 0
    rec_cache_key = (wav_mtimes.get(rec_name), tg_mtimes.get(rec_name))

    ivs, xmax_rec = tg_data.get(rec_name, (None, 0.0))

    cached = cached_recs.get(rec_name)
    if cached and cached.get('key') == rec_cache_key:
        for entry in cached['units']:
            new_unit_data[entry[0]] = tuple(entry[1:])
        new_cached_recs[rec_name] = cached
        cache_hits += 1
        if mara_n == 0:
            zero_count += len(units)
        elif ivs and any(lbl not in _REAL_PHONE_SET for _, _, lbl in ivs):
            # Has real phone labels -> seq aligned (exact or fuzzy).
            tg_count += len(units)
            for entry in cached['units']:
                mfa_aligned_uids.add(entry[0])
        elif ivs and any(l.strip().lower() == 'spn' for _, _, l in ivs):
            spn_count += len(units)
        else:
            prop_count += len(units)
    else:
        cache_misses += 1
        miss_tasks.append((fidx, units, rec_name, tom_max_end, mara_n, cap,
                           ivs, xmax_rec, rec_cache_key))

# -- Phase B: process cache misses in parallel -----------------------------------
print(f"Building unit table (cache: {cache_hits} hits, {cache_misses} misses) ...")
if miss_tasks:
    print(f"  Spawning {N_WORKERS} worker thread(s) for {cache_misses} recordings ...")

    def _submit(task):
        _, units, rec_name, tom_max_end, mara_n, cap, ivs, xmax_rec, _ = task
        ulaw_bytes = mara_pcm.get(rec_name)
        return process_recording(rec_name, units, tom_max_end, mara_n, cap,
                                 ulaw_bytes, ivs, xmax_rec)

    with ThreadPoolExecutor(max_workers=N_WORKERS) as pool:
        future_to_task = {pool.submit(_submit, t): t for t in miss_tasks}
        for future in tqdm(as_completed(future_to_task), total=len(miss_tasks),
                           desc="Processing", unit="rec"):
            task = future_to_task[future]
            _, units, rec_name, tom_max_end, mara_n, cap, ivs, xmax_rec, rec_cache_key = task
            rec_units_out, f0_status, mfa_mode = future.result()

            for entry in rec_units_out:
                new_unit_data[entry[0]] = entry[1:]
            new_cached_recs[rec_name] = {'key': rec_cache_key, 'units': rec_units_out}

            if mara_n == 0:
                zero_count += len(rec_units_out)
            elif mfa_mode == 1:
                tg_count += len(rec_units_out)
                for entry in rec_units_out:
                    mfa_aligned_uids.add(entry[0])
            elif mfa_mode == 2:
                fuzzy_count += len(rec_units_out)
                for entry in rec_units_out:
                    mfa_aligned_uids.add(entry[0])
            elif mfa_mode == 3:
                spn_count += len(rec_units_out)
            elif mfa_mode == 4:
                energy_count += len(rec_units_out)
            else:
                prop_count += len(rec_units_out)
            if f0_status == 1:
                f0_ok += 1
            elif f0_status == -1:
                f0_fail += 1

print(f"  MFA seq exact:     {tg_count:,} units  ({100*tg_count//N_UNITS}%)")
print(f"  MFA seq aligned:   {fuzzy_count:,} units  ({100*fuzzy_count//N_UNITS}%)")
print(f"  SPN-region prop:   {spn_count:,} units  ({100*spn_count//N_UNITS}%)")
print(f"  Energy-region:     {energy_count:,} units  ({100*energy_count//N_UNITS}%)")
print(f"  Proportional:      {prop_count:,} units  ({100*prop_count//N_UNITS}%)")
print(f"  Zeroed (no audio): {zero_count:,} units")
# Count units with dl=0 from direct phone-boundary mapping (unmatched phone groups).
n_disabled = sum(1 for uid in new_unit_data if new_unit_data[uid][1] == 0)
print(f"  Disabled (unmatched phone): {n_disabled:,} units")
if cache_misses:
    print(f"  F0: {f0_ok} computed, {f0_fail} failed/skipped")

# -- Post-processing: inflate dl toward Tom's originals --------------------------
# dl determines how much source audio WSOLA gets to work with -- it does NOT
# determine the output duration (the prosody model does that).  More source
# material = better WSOLA quality.  Mara's recordings are shorter than Tom's,
# so MFA-scaled dl values are often 0.3-0.5x Tom's, forcing heavy WSOLA
# expansion that creates audible artifacts.
#
# Fix: inflate EVERY speech unit's dl toward Tom's dl, capped only by the
# available audio in the VDB (cap - new_lp).  This gives WSOLA the same amount
# of source material Tom would have had.  Units whose audio extends slightly
# past phoneme boundaries are fine -- WSOLA uses the interior audio for
# overlap-add, and the hash gets recomputed from the new boundaries afterward.
MIN_DL_FLOOR = 10  # absolute minimum dl for speech units
print("Inflating dl values toward Tom's originals ...")
dl_inflated = 0
dl_already_ok = 0
dl_total_gain = 0
for fidx, units in units_by_fidx.items():
    rec_name = filenames.get(fidx, '')
    mara_n = name_n_samples.get(rec_name, 0)
    cap = mara_n // 8 - 1 if mara_n >= 16 else 0
    for uid, tom_lp, tom_dl, is_first, pc in units:
        if uid not in new_unit_data:
            continue
        if pc in _SILENCE_PCS:
            continue
        new_lp, new_dl, new_pc, new_f0s, new_f0e, new_f0m = new_unit_data[uid]
        # Skip units with dl=0 (shouldn't exist anymore but be safe).
        if new_dl == 0:
            continue
        # MFA units ARE included -- dl inflation gives WSOLA more source
        # material without changing lp (read position). The last unit in
        # each same-rec group gets output_dur = dl, so bigger dl = longer
        # output for that unit.
        # If lp is near the recording end, pull it back to leave room.
        if cap > 0 and tom_dl > 0 and new_lp >= cap - MIN_DL_FLOOR:
            new_lp = max(0, cap - tom_dl)
        # Target: Tom's original dl, capped by available audio after new_lp.
        target_dl = min(tom_dl, max(0, cap - new_lp))
        target_dl = max(target_dl, MIN_DL_FLOOR)  # at least MIN_DL_FLOOR
        target_dl = min(target_dl, max(0, cap - new_lp))  # re-cap after floor
        if target_dl > new_dl:
            dl_total_gain += target_dl - new_dl
            new_unit_data[uid] = (new_lp, target_dl, new_pc, new_f0s, new_f0e, new_f0m)
            dl_inflated += 1
        else:
            dl_already_ok += 1
print(f"  {dl_inflated:,} units inflated (avg gain: "
      f"{dl_total_gain / max(1, dl_inflated):.1f} dl units = "
      f"{dl_total_gain * 0.5 / max(1, dl_inflated):.1f} ms)")
print(f"  {dl_already_ok:,} units already at or above Tom's dl")

# -- Post-processing: close LP gaps within same-recording unit sequences -------
# When consecutive units (in Tom's lp order) share a recording, gaps between
# lp+dl of one unit and lp of the next create spectral seams even though the
# engine selected units from the same source recording.  Close large gaps by
# pulling units' lp values forward to maintain continuity.
MAX_LP_GAP = 5   # allow small gaps (rounding etc), close anything larger
print("Closing LP gaps within same-recording unit groups ...")
lp_gaps_closed = 0
lp_gap_total_reduction = 0
for fidx, units in units_by_fidx.items():
    # Build list of (uid, tom_lp) for this recording, sorted by tom_lp.
    rec_uids = []
    for uid, tom_lp, tom_dl, is_first, pc in units:
        if uid not in new_unit_data:
            continue
        rec_uids.append((uid, tom_lp))
    if len(rec_uids) < 2:
        continue
    rec_uids.sort(key=lambda x: x[1])

    # Walk in tom_lp order, track prev_end, close positive gaps.
    # Negative gaps (overlaps from dl-inflate) are LEFT ALONE -- overlapping
    # source regions within the same recording are fine; each unit reads
    # independently from the VDB, and shared boundary audio actually improves
    # join quality.  Trimming overlaps would undo dl-inflate and starve WSOLA.
    prev_end = -1
    for uid, _ in rec_uids:
        new_lp, new_dl, new_pc, new_f0s, new_f0e, new_f0m = new_unit_data[uid]
        if new_dl == 0:
            continue  # skip intentionally zeroed units
        if uid in mfa_aligned_uids:
            prev_end = new_lp + new_dl  # track but don't modify
            continue
        if prev_end < 0:
            prev_end = new_lp + new_dl
            continue
        gap = new_lp - prev_end
        if gap > MAX_LP_GAP:
            # Pull this unit's lp back to close the gap.
            new_lp = prev_end
            # Don't extend dl -- keep the same dl, just shift lp.
            new_unit_data[uid] = (new_lp, new_dl, new_pc, new_f0s, new_f0e, new_f0m)
            lp_gaps_closed += 1
            lp_gap_total_reduction += gap
        prev_end = new_unit_data[uid][0] + new_unit_data[uid][1]
print(f"  {lp_gaps_closed:,} gaps closed (avg reduction: "
      f"{lp_gap_total_reduction / max(1, lp_gaps_closed):.1f} lp units)")

# RMS audit REMOVED -- was disabling ~13k units (8.5% of pool), gutting
# candidate coverage and forcing massive prsl back-fill with phonetically
# worse substitutes.  The MFA boundaries are the ground truth; if MFA says
# a phone is at a position, trust it.  A quiet unit is better than no unit.

# -- Final monotonicity + minimum spacing safety net --------------------------
# Enforce strictly increasing lp with minimum gap of MIN_LP_SPACING between
# consecutive active units. The engine's WSOLA output duration = lp gap
# between consecutive same-rec units. Gaps < 15ms create inaudible
# compressed output. This catches cases the MFA even-spacing formula missed
# (cap clamping, proportional fallback, gap closure side effects).
MIN_LP_SPACING = 15  # minimum ms between consecutive units in same recording
print("Final monotonicity + minimum spacing enforcement ...")
_mono_fixes = 0
for fidx, units in units_by_fidx.items():
    rec_name = filenames.get(fidx, '')
    mara_n_loc = name_n_samples.get(rec_name, 0)
    cap = mara_n_loc // 8 - 1 if mara_n_loc >= 16 else 0
    # Collect active units for this recording, sorted by Tom's lp
    active = []
    for uid, tom_lp, tom_dl, is_first, pc in units:
        if uid not in new_unit_data:
            continue
        new_lp, new_dl, new_pc, new_f0s, new_f0e, new_f0m = new_unit_data[uid]
        if new_dl > 0:
            active.append((tom_lp, uid))
    if len(active) < 2:
        continue
    active.sort()
    prev_lp = -(MIN_LP_SPACING + 1)
    for _, uid in active:
        new_lp, new_dl, new_pc, new_f0s, new_f0e, new_f0m = new_unit_data[uid]
        min_lp = prev_lp + MIN_LP_SPACING
        if new_lp < min_lp:
            new_lp = min(min_lp, cap)  # clamp to cap, never exceed
            new_dl = max(1, min(cap - new_lp, new_dl))  # dl >= 1 always
            new_unit_data[uid] = (new_lp, new_dl, new_pc, new_f0s, new_f0e, new_f0m)
            _mono_fixes += 1
        prev_lp = new_lp
print(f"  {_mono_fixes:,} units adjusted for monotonicity/spacing")

# Pack unit buffer.
new_unit_buf = bytearray(N_UNITS * UNIT_SIZE)
for i in tqdm(range(N_UNITS), desc="Packing units", unit="unit", miniters=10000):
    src_base = unit_data_ds + i * UNIT_SIZE
    dst_base = i * UNIT_SIZE
    rec = bytearray(vin[src_base : src_base + UNIT_SIZE])
    if i in new_unit_data:
        new_lp, new_dl, new_pc, new_f0s, new_f0e, new_f0m = new_unit_data[i]
        struct.pack_into('<H', rec,  6, min(new_lp, 0xFFFF))
        struct.pack_into('<H', rec, 10, min(new_dl, 0xFFFF))
        if new_f0s >= 0:
            # For each f0 field independently: if HARVEST returned 0 but Tom
            # had a nonzero value, keep Tom's value to avoid MISSING_F0_COST
            # and preserve correct concat cost computation.
            fell_back = False
            if new_f0s == 0 and rec[16] > 0:
                fell_back = True  # keep Tom's f0_start
            else:
                rec[16] = new_f0s
            if new_f0e == 0 and rec[17] > 0:
                fell_back = True  # keep Tom's f0_end
            else:
                rec[17] = new_f0e
            if new_f0m == 0 and rec[18] > 0:
                fell_back = True  # keep Tom's f0_mid
            else:
                rec[18] = new_f0m
            if fell_back:
                f0_tom_fallback += 1
        if new_pc != 255:
            rec[20] = new_pc
    new_unit_buf[dst_base : dst_base + UNIT_SIZE] = rec

if f0_tom_fallback:
    print(f"  F0 Tom fallback: {f0_tom_fallback:,} units kept Tom's f0 (HARVEST unvoiced, Tom voiced)")

# -- 6. Build mara.vin -----------------------------------------------------------
print("Building mara.vin ...")
new_vin = (vin[:unit_data_ds]
           + bytes(new_unit_buf)
           + vin[unit_data_ds + N_UNITS * UNIT_SIZE:])
with open(VIN_OUT, 'wb') as f:
    f.write(xor_codec(new_vin))
print(f"  Wrote {VIN_OUT}")


# -- 7. Persist state ------------------------------------------------------------
print("Saving build state ...")
save_state(tom_key, new_cached_recs)
print(f"  {len(new_cached_recs)} recordings cached to {STATE_FILE}")


# -- 8. Clear cache --------------------------------------------------------------
print("Clearing Mara voice cache ...")
tmpdir = os.environ.get('TMPDIR', os.environ.get('TEMP', tempfile.gettempdir()))
cleared = 0
for path in _glob.glob(os.path.join(tmpdir, 'cache_mara_8_*')):
    try:
        shutil.rmtree(path)
        cleared += 1
    except Exception as e:
        print(f"  Warning: could not remove {path}: {e}")
if cleared == 0:
    print("  (no cache entries found)")


# -- Done ------------------------------------------------------------------------
print("\nDone.")
print(f"  VDB: {VDB_OUT}")
print(f"  VIN: {VIN_OUT}")
print("="*252)
