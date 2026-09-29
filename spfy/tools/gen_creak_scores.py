"""Per-unit CREAK score sidecar (`<stem>.creak`) for creak-aware selection.

2.1.5 Tom's sentence-final vowels are creakier / more pressed than what
vendor tom8 selection gives (H1-H2 -1.46 vs +1.93 dB, lower-f0, less voiced,
lower-HNR tail; SPFY2_MODE_HANDOFF.md). tom8 already holds creaky finals, so
the lever is a target cost that prefers them. This measures every unit.

Per unit, over the unit +-20 ms, on a pitch track of the WHOLE recording:
  low   fraction of voiced frames below 70 Hz          (40 Hz floor)
  sub   fraction of voiced frames below 0.6 x the recording's median f0
  unv   fraction of frames the tracker calls unvoiced
  hnr   mean harmonicity, dB (cc, 40 Hz min pitch)
  h1h2  H1-H2, dB (Hann FFT, harmonics picked within +-20%)
creak = sigmoid(weighted z), z fitted on VOWEL units only. Only H1-H2 carries
weight (WEIGHTS): the name is historical, the score measures PRESSED phonation. Non-vowel units get 128 (neutral): every candidate for a
consonant slot is the same phone, so a constant cannot reorder them.

  python gen_creak_scores.py <voice.vin> <voice8.vdb> <out_dir> [--stem tom8]

Writes <stem>.creak (`CRK1`, u32 n_units, n_units x u8), <stem>.creak.npz
(raw features) and <stem>.creak.json (the z parameters, so any other audio
can be scored on the same scale with score_segment()).
"""
from __future__ import annotations

import argparse
import json
import struct
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
import parselmouth
from tqdm import tqdm

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_pitchmarks_real import (_ulaw_table, load_vdb, load_vin_plain,  # noqa: E402
                                 parse_feat_filenames, parse_unit_table, vin_chunks)

SR = 8000
STEP = 0.005
F0_FLOOR = 40.0
F0_CEIL = 250.0
LOW_HZ = 70.0
SUB_RATIO = 0.6
CTX = int(0.020 * SR)
H1H2_MIN_PERIODS = 3.5
FEATS = ("low", "sub", "unv", "h1h2", "hnr")
# ⚠ Only H1-H2 is weighted. Measured 2026-09-28 (C:\tmp\spfy2\creak_control.py,
# 2,492 paired sentence-final vowels, 2.1.5 vs vendor tom, same word): 2.1.5 is
# MORE PRESSED (H1-H2 -3.7 dB, 75% of pairs, p=2e-224) but has LESS low-f0
# creak than vendor (<70 Hz 0.64 vs 0.69, subharmonic 0.64 vs 0.72). Weighting
# low/sub pulled toward what vendor already over-selects; the five-term mean
# separated the arms in only 59% of pairs. The other features stay in the npz.
WEIGHTS = {"low": 0.0, "sub": 0.0, "unv": 0.0, "h1h2": -1.0, "hnr": 0.0}
VOWELS = {"aa", "ae", "ah", "ao", "aw", "ax", "axr", "ay", "eh", "er", "ey",
          "ih", "ix", "iy", "ow", "oy", "uh", "uw", "ux"}
DUMP = Path(r"C:\tmp\spfy_build32\src\cli\spfy_dump_voice.exe")


def tracks(x: np.ndarray):
    """-> (frame_times, f0 (0 = unvoiced), hnr dB (nan = unvoiced)).
    Silence-padded so a recording shorter than Praat's 3 periods at 40 Hz
    still tracks; times are shifted back to the unpadded signal."""
    pad = int(0.080 * SR)
    xp = np.concatenate([np.zeros(pad), x.astype(np.float64), np.zeros(pad)])
    s = parselmouth.Sound(xp, SR)
    p = s.to_pitch_ac(time_step=STEP, pitch_floor=F0_FLOOR, pitch_ceiling=F0_CEIL)
    f0 = p.selected_array["frequency"]
    t = p.xs()
    h = s.to_harmonicity_cc(time_step=STEP, minimum_pitch=F0_FLOOR)
    hv = np.interp(t, h.xs(), h.values[0])
    hv[hv < -100] = np.nan
    return t - pad / SR, f0, hv


def h1h2(x: np.ndarray, f0: float) -> float:
    if not f0 or f0 <= 0 or len(x) < 64:
        return np.nan
    n = 1 << int(np.ceil(np.log2(len(x) * 4)))
    spec = np.abs(np.fft.rfft(x * np.hanning(len(x)), n))
    freqs = np.fft.rfftfreq(n, 1 / SR)

    def pk(c):
        m = (freqs > 0.8 * c) & (freqs < 1.2 * c)
        return spec[m].max() if m.any() else 0.0
    return float(20 * np.log10(pk(f0) + 1e-12) - 20 * np.log10(pk(2 * f0) + 1e-12))


def segment_features(x: np.ndarray, t: np.ndarray, f0: np.ndarray, hnr: np.ndarray,
                     rec_med: float, lo: int, hi: int) -> dict:
    """Features of samples [lo, hi) of a recording whose tracks are (t, f0, hnr).
    The caller has already widened [lo, hi) by the +-20 ms context."""
    lo, hi = max(0, lo), min(len(x), hi)
    m = (t >= lo / SR) & (t < hi / SR)
    fr, hr = f0[m], hnr[m]
    out = dict.fromkeys(FEATS, np.nan)
    if fr.size == 0:
        return out
    v = fr[fr > 0]
    out["unv"] = 1.0 - v.size / fr.size
    if v.size >= 2:
        out["low"] = float(np.mean(v < LOW_HZ))
        if rec_med > 0:
            out["sub"] = float(np.mean(v < SUB_RATIO * rec_med))
        f = float(np.median(v))
        need = int(H1H2_MIN_PERIODS * SR / f)
        if hi - lo < need:
            c = (lo + hi) // 2
            lo2, hi2 = max(0, c - need // 2), min(len(x), c + need // 2)
        else:
            lo2, hi2 = lo, hi
        out["h1h2"] = h1h2(x[lo2:hi2], f)
    if np.isfinite(hr).any():
        out["hnr"] = float(np.nanmean(hr))
    return out


def score(feats: dict, params: dict) -> float:
    """0..1 score on the scale fitted by main(); higher = more pressed."""
    acc = wsum = 0.0
    for k in FEATS:
        w = WEIGHTS[k]
        if not w:
            continue
        wsum += abs(w)
        v = feats.get(k, np.nan)
        mu, sd = params[k]
        if np.isfinite(v) and sd > 0:
            acc += w * (v - mu) / sd
    return float(1.0 / (1.0 + np.exp(-acc / max(wsum, 1e-9))))


def score_segment(x: np.ndarray, lo: int, hi: int, params: dict) -> tuple[float, dict]:
    """Score samples [lo, hi) of an arbitrary 8 kHz recording `x` exactly as a
    unit is scored: tracks on the whole of `x`, +-20 ms context."""
    t, f0, hv = tracks(x)
    v = f0[f0 > 0]
    rec_med = float(np.median(v)) if v.size else 0.0
    ft = segment_features(x, t, f0, hv, rec_med, lo - CTX, hi + CTX)
    return score(ft, params), ft


def _lower_priority():
    import psutil
    psutil.Process().nice(psutil.BELOW_NORMAL_PRIORITY_CLASS)


def _job(task):
    raw, units = task
    x = _ulaw_table()[np.frombuffer(raw, dtype=np.uint8)].astype(np.float64) / 32768.0
    t, f0, hv = tracks(x)
    v = f0[f0 > 0]
    rec_med = float(np.median(v)) if v.size else 0.0
    out = []
    for uid, start, length in units:
        ft = segment_features(x, t, f0, hv, rec_med, start - CTX, start + length + CTX)
        out.append((uid, [ft[k] for k in FEATS]))
    return out


def unit_phones(vin: Path) -> dict[int, str]:
    r = subprocess.run([str(DUMP), "--index", str(vin)], capture_output=True, text=True, check=True)
    out = {}
    for line in r.stdout.splitlines()[1:]:
        c = line.split("\t")
        out[int(c[0])] = c[4]
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("vin", type=Path)
    ap.add_argument("vdb", type=Path)
    ap.add_argument("out_dir", type=Path)
    ap.add_argument("--stem", default="tom8")
    # 16 of 24 threads at below-normal priority: 22 pegged the desktop so hard
    # a console could not take keystrokes.
    ap.add_argument("--workers", type=int, default=16)
    args = ap.parse_args()

    vbuf = load_vin_plain(args.vin)
    ch = vin_chunks(vbuf)
    n_units, ver, rec_size, uoff, _ = parse_unit_table(vbuf, *ch["unit"])
    names = parse_feat_filenames(vbuf, *ch["feat"])
    buf, data_off, _, recs = load_vdb(args.vdb)
    phones = unit_phones(args.vin)
    print(f"units {n_units} (vers {ver}), recordings {len(recs)}")

    by_rec: dict[str, list] = {}
    for uid in range(n_units):
        p = uoff + uid * rec_size
        fi, lp = struct.unpack_from("<HH", vbuf, p + 0x04)
        dl = struct.unpack_from("<H", vbuf, p + 0x0A)[0]
        if fi < len(names) and names[fi] in recs and dl > 0:
            by_rec.setdefault(names[fi], []).append((uid, lp * 8, dl * 8))
    tasks = []
    for name, units in by_rec.items():
        off, size = recs[name]
        tasks.append((buf[data_off + off:data_off + off + size], units))

    feats = np.full((n_units, len(FEATS)), np.nan)
    with ProcessPoolExecutor(max_workers=args.workers, initializer=_lower_priority) as ex:
        for res in tqdm(ex.map(_job, tasks, chunksize=4), total=len(tasks), desc="creak"):
            for uid, fv in res:
                feats[uid] = fv

    vowel = np.array([phones.get(u, "") in VOWELS for u in range(n_units)])
    params = {}
    for j, k in enumerate(FEATS):
        col = feats[vowel, j]
        col = col[np.isfinite(col)]
        params[k] = (float(col.mean()), float(col.std())) if col.size else (0.0, 0.0)

    sc = np.full(n_units, 128, dtype=np.uint8)
    for u in np.flatnonzero(vowel):
        s = score(dict(zip(FEATS, feats[u])), params)
        sc[u] = int(round(s * 255))

    args.out_dir.mkdir(parents=True, exist_ok=True)
    (args.out_dir / f"{args.stem}.creak").write_bytes(b"CRK1" + struct.pack("<I", n_units) + sc.tobytes())
    np.savez_compressed(args.out_dir / f"{args.stem}.creak.npz", feats=feats, vowel=vowel,
                        score=sc, names=np.array(FEATS))
    (args.out_dir / f"{args.stem}.creak.json").write_text(json.dumps(
        {"feats": FEATS, "weights": WEIGHTS, "params": params}, indent=1))

    vs = sc[vowel]
    print(f"vowel units {vowel.sum()}: score p5/p50/p95 = "
          f"{np.percentile(vs, 5):.0f} / {np.percentile(vs, 50):.0f} / {np.percentile(vs, 95):.0f}")
    for j, k in enumerate(FEATS):
        col = feats[vowel, j]
        print(f"  {k:5s} finite {np.isfinite(col).mean():6.1%}  mean {params[k][0]:+8.3f}  sd {params[k][1]:7.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
