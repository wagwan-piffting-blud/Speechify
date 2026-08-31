"""Generate pmindex/pmdata from MEASURED pitch marks in the VDB audio.

Companion to gen_pitchmarks.py, which synthesises marks from the unit table's
per-unit F0 bytes. That gets the engine into mode 0 but leaves jitter flat,
because mode-0 overlap-add is a PHASE-ALIGNMENT mechanism: it needs marks that
sit on the actual glottal cycles of the recording, not merely a plausible
period sequence.

Two things this does that the synthetic generator cannot:

  1. Marks are detected on the CONTINUOUS recording, then sliced per unit. A
     mark train built per-unit restarts phase at every unit boundary; built
     per-recording it stays coherent across the joins the engine is trying to
     smooth.
  2. The FIRST period of a unit is the distance from the unit start to the
     first mark, i.e. the unit's phase offset. The synthetic generator always
     emitted a full period there, discarding exactly the information mode 0
     consumes.

Detection is F0-guided peak picking: pyworld dio+stonemask gives the period
track, then each mark is refined to the argmax of a ~900 Hz low-passed copy
within +-30% of a period. Absolute GCI accuracy matters less here than
CONSISTENCY of phase from cycle to cycle, which peak picking gives cheaply.

  python gen_pitchmarks_real.py <voice.vin> <voice8.vdb> <out_dir>
         [--stem tom8] [--workers 20] [--limit N]

Format is identical to gen_pitchmarks.py; see reveng/DLL_ANALYSIS.md
"pmindex / pmdata ON-DISK FORMAT".
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
import pyworld as pw
from scipy.signal import butter, filtfilt
from tqdm import tqdm

SR = 8000
MAX_PERIOD = 160        # engine's max_pitch_period at 8 kHz (state+0xd83*4)
MIN_PERIOD = 20
DEFAULT_F0 = 118.0      # Tom's median nonzero f0; used in unvoiced stretches
# ⚠ The docstring premise below -- "one dominant excursion per glottal cycle"
# -- is not true at 900 Hz. Measured (pmland.py) over 3,272 units: marks sit on
# roughly one maximum in every TWO (marks/peaks 0.47) even where the resulting
# F0 is correct, and on units the F0 check calls wrong the marks land a median
# 9 samples off the nearest maximum (a period is ~67) with the maxima 27% more
# irregular. Env-overridable so the anchor can be swept against pmcompare.py
# rather than argued about.
LPF_HZ = float(os.environ.get("PM_LPF_HZ", "900.0"))
SEARCH_FRAC = 0.30      # refine window, +-fraction of a period
# ⚠ TRIED AND REVERTED 2026-08-08. Damping the phase feedback (pos moved only
# 0.25 of the way to the refined position) plus a period-plausibility gate at
# [0.75, 1.25]*T was aimed at cumulative pulse slip. Measured over the same
# 5,858 units (pmcompare.py): >3 st rate 4.35% -> 4.24%, slip rate 0.55% ->
# 0.57%, >1 st rate 12.6% -> 14.0%. No improvement, so the accumulated-slip
# story is NOT established -- the marks changed but did not get better. Do not
# retry that shape without new evidence about where the marks actually land.

LAYOUTS = {100004: (23, 0x10), 100005: (24, 0x10), 100006: (29, 0x10),
           100007: (25, 0x11), 100008: (30, 0x11)}

# Set once per worker process by _init.
_PLAIN: np.ndarray | None = None
_ULAW: np.ndarray | None = None


# ---------------------------------------------------------------- containers

def _riff_walk(buf, start, end):
    pos = start
    while pos + 8 <= end:
        fcc = buf[pos:pos + 4]
        size = struct.unpack_from("<I", buf, pos + 4)[0]
        if pos + 8 + size > end:
            return
        yield fcc, pos + 8, size
        pos += 8 + size + (size & 1)


def load_vin_plain(path: Path) -> bytes:
    raw = path.read_bytes()
    if raw[:4] == b"RIFF":
        return raw
    if bytes(b ^ 0xCE for b in raw[:4]) == b"RIFF":
        return bytes(b ^ 0xCE for b in raw)
    raise SystemExit(f"{path}: not RIFF and not XOR-0xCE RIFF")


def vin_chunks(buf: bytes) -> dict:
    out = {}
    for fcc, off, size in _riff_walk(buf, 12, len(buf)):
        out[fcc.decode("latin-1")] = (off, size)
    return out


def parse_unit_table(buf: bytes, off: int, size: int):
    """-> (n_units, version, rec_size, data_off)"""
    for base in (off, off + 4):
        ver = data_off = data_n = None
        for f2, o2, s2 in _riff_walk(buf, base, off + size):
            if f2 == b"vers":
                ver = struct.unpack_from("<I", buf, o2)[0]
            elif f2 == b"data":
                data_off, data_n = o2, s2
        if ver is not None and data_off is not None:
            rec = LAYOUTS[ver][0]
            return data_n // rec, ver, rec, data_off, LAYOUTS[ver][1]
    raise SystemExit("unit chunk malformed")


def parse_feat_filenames(buf: bytes, off: int, size: int) -> list[str]:
    """feat is a dict of key -> [(name, stored_id)]; we want 'filename',
    ordered by stored_id (spfy/src/voice/feat_table.c)."""
    p, end = off, off + size
    while p < end:
        klen = struct.unpack_from("<H", buf, p)[0]; p += 2
        key = buf[p:p + klen]; p += klen
        count = struct.unpack_from("<I", buf, p)[0]; p += 4
        if key == b"filename":
            ents = []
            for _ in range(count):
                nlen = struct.unpack_from("<H", buf, p)[0]; p += 2
                name = buf[p:p + nlen].decode("latin-1"); p += nlen
                sid = struct.unpack_from("<I", buf, p)[0]; p += 4
                ents.append((sid, name))
            ents.sort()
            return [n for _, n in ents]
        for _ in range(count):
            nlen = struct.unpack_from("<H", buf, p)[0]; p += 2
            p += nlen + 4
    raise SystemExit("feat: 'filename' key not found")


def load_vdb(path: Path):
    """-> (plain_bytes, data_off, data_n, {name: (offset, size)})"""
    raw = bytearray(path.read_bytes())
    plain = np.frombuffer(bytes(raw), dtype=np.uint8) ^ np.uint8(0xCE)
    buf = plain.tobytes()
    if buf[:4] != b"RIFF" or buf[8:12] != b"WAVE":
        raise SystemExit(f"{path}: not a RIFF/WAVE VDB after XOR")
    riff_size = struct.unpack_from("<I", buf, 4)[0]
    indx = data = None
    for fcc, off, size in _riff_walk(buf, 12, riff_size + 8):
        if fcc == b"indx":
            indx = (off, size)
        elif fcc == b"data":
            data = (off, size)
        elif fcc == b"fmt ":
            sr = struct.unpack_from("<I", buf, off + 4)[0]
            if sr != SR:
                raise SystemExit(f"{path}: sample_rate={sr}, expected {SR} "
                                 f"(pass the *8.vdb, never the 16k one)")
    if indx is None or data is None:
        raise SystemExit(f"{path}: missing indx/data chunk")

    p = indx[0]
    count = struct.unpack_from("<I", buf, p)[0]; p += 4
    raw_ents = []
    for _ in range(count):
        off_ = struct.unpack_from("<I", buf, p)[0]; p += 4
        nlen = struct.unpack_from("<H", buf, p)[0]; p += 2
        name = buf[p:p + nlen].decode("latin-1"); p += nlen
        raw_ents.append((off_, name))
    # size = delta to the next entry's offset; final entry is a sentinel
    recs = {}
    for i in range(len(raw_ents) - 1):
        off_, name = raw_ents[i]
        if name:
            recs[name] = (off_, raw_ents[i + 1][0] - off_)
    return buf, data[0], data[1], recs


# ---------------------------------------------------------------- detection

def _ulaw_table() -> np.ndarray:
    """G.711 mu-law byte -> s16, as a 256-entry LUT."""
    out = np.empty(256, dtype=np.int16)
    for i in range(256):
        u = ~i & 0xFF
        sign = u & 0x80
        exp = (u >> 4) & 0x07
        mant = u & 0x0F
        mag = ((mant << 3) + 0x84) << exp
        mag -= 0x84
        out[i] = -mag if sign else mag
    return out


def _init(plain_path: str, n_bytes: int):
    global _PLAIN, _ULAW
    _PLAIN = np.memmap(plain_path, dtype=np.uint8, mode="r", shape=(n_bytes,))
    _ULAW = _ulaw_table()


def detect_marks(x: np.ndarray) -> np.ndarray:
    """Absolute mark positions (samples) spanning the whole recording."""
    n = x.size
    if n < 64:
        return np.arange(0, n, int(SR / DEFAULT_F0), dtype=np.int64)

    xd = x.astype(np.float64)
    f0, t = pw.dio(xd, SR, f0_floor=60.0, f0_ceil=350.0, frame_period=5.0)
    f0 = pw.stonemask(xd, f0, t, SR)

    # ~900 Hz low-pass gives one dominant excursion per glottal cycle at
    # Tom's F0, so argmax within a period window is a consistent phase anchor.
    wn = min(0.99, LPF_HZ / (SR / 2.0))
    b, a = butter(4, wn, btype="low")
    lp = filtfilt(b, a, xd) if n > 3 * max(len(a), len(b)) else xd

    def period_at(pos: int) -> float:
        fi = int(pos / (0.005 * SR))
        if 0 <= fi < f0.size and f0[fi] > 0:
            return SR / f0[fi]
        return SR / DEFAULT_F0

    marks: list[int] = []
    pos = 0.0
    while pos < n:
        T = period_at(int(pos))
        T = float(np.clip(T, MIN_PERIOD, MAX_PERIOD))
        fi = int(pos / (0.005 * SR))
        voiced = 0 <= fi < f0.size and f0[fi] > 0
        if voiced:
            w = max(1, int(T * SEARCH_FRAC))
            lo = max(0, int(pos) - w)
            hi = min(n, int(pos) + w + 1)
            if hi > lo:
                cand = lo + int(np.argmax(lp[lo:hi]))
                # Never let refinement collapse or reverse the train.
                if marks and cand - marks[-1] < MIN_PERIOD // 2:
                    cand = int(pos)
                pos = float(cand)
        m = int(pos)
        if not marks or m > marks[-1]:
            marks.append(m)
        pos += T
    return np.asarray(marks, dtype=np.int64)


def _job(task):
    """task = (rec_offset, rec_size, [(uid, local_pos, dur_like), ...])"""
    rec_off, rec_size, units = task
    raw = np.asarray(_PLAIN[rec_off:rec_off + rec_size])
    x = _ULAW[raw].astype(np.float64)
    marks = detect_marks(x)

    out = []
    for uid, lp, dl in units:
        start = lp * 8
        length = dl * 8
        if length <= 0 or start >= rec_size:
            out.append((uid, []))
            continue
        if start + length > rec_size:
            length = rec_size - start
        lo = np.searchsorted(marks, start, side="left")
        hi = np.searchsorted(marks, start + length, side="left")
        rel = marks[lo:hi] - start
        if rel.size == 0:
            out.append((uid, [min(length, MAX_PERIOD)]))
            continue
        periods = np.diff(np.concatenate(([0], rel)))
        periods = np.clip(periods, 1, MAX_PERIOD).astype(np.int64)
        out.append((uid, periods.tolist()))
    return out


# ---------------------------------------------------------------------- main

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("vin", type=Path)
    ap.add_argument("vdb", type=Path)
    ap.add_argument("out_dir", type=Path)
    ap.add_argument("--stem", default="tom8")
    ap.add_argument("--workers", type=int, default=20)
    ap.add_argument("--limit", type=int, default=0,
                    help="only process the first N recordings (smoke test)")
    ap.add_argument("--cache", type=Path, default=Path(r"C:\tmp\vdb_plain.bin"))
    ap.add_argument("--rate", type=int, default=SR)
    args = ap.parse_args()

    print("loading VIN ...")
    vbuf = load_vin_plain(args.vin)
    ch = vin_chunks(vbuf)
    n_units, ver, rec_size, uoff, f0_off = parse_unit_table(vbuf, *ch["unit"])
    names = parse_feat_filenames(vbuf, *ch["feat"])
    print(f"  unit vers={ver} n_units={n_units}; feat.filename={len(names)}")

    print("loading VDB ...")
    buf, data_off, data_n, recs = load_vdb(args.vdb)
    print(f"  data={data_n} bytes, {len(recs)} recordings")

    if not args.cache.exists() or args.cache.stat().st_size != len(buf):
        print(f"  writing plain VDB cache -> {args.cache}")
        args.cache.write_bytes(buf)

    # Group units by recording so each is pitch-marked exactly once.
    by_rec: dict[str, list] = {}
    missing = 0
    for uid in range(n_units):
        p = uoff + uid * rec_size
        file_idx = struct.unpack_from("<H", vbuf, p + 0x04)[0]
        lp = struct.unpack_from("<H", vbuf, p + 0x06)[0]
        dl = struct.unpack_from("<H", vbuf, p + 0x0A)[0]
        if file_idx >= len(names):
            missing += 1
            continue
        by_rec.setdefault(names[file_idx], []).append((uid, lp, dl))
    print(f"  units grouped into {len(by_rec)} recordings "
          f"({missing} with out-of-range file_idx)")

    tasks = []
    for name, units in by_rec.items():
        if name not in recs:
            missing += len(units)
            continue
        off, size = recs[name]
        tasks.append((data_off + off, size, units))
    if args.limit:
        tasks = tasks[:args.limit]
    print(f"  {len(tasks)} recordings to mark")

    per_uid: dict[int, list] = {}
    with ProcessPoolExecutor(max_workers=args.workers, initializer=_init,
                             initargs=(str(args.cache), len(buf))) as ex:
        for res in tqdm(ex.map(_job, tasks, chunksize=8), total=len(tasks),
                        desc="pitch-marking"):
            for uid, periods in res:
                per_uid[uid] = periods

    index = bytearray(struct.pack(">III", args.rate, 0, 0))
    data = bytearray()
    n_marks = 0
    n_empty = 0
    for uid in range(n_units):
        periods = per_uid.get(uid, [])
        if not periods:
            n_empty += 1
        index += struct.pack(">II", len(data) // 2, len(periods))
        if periods:
            data += np.asarray(periods, dtype=">i2").tobytes()
        n_marks += len(periods)

    args.out_dir.mkdir(parents=True, exist_ok=True)
    idx_p = args.out_dir / f"{args.stem}.pmindex"
    dat_p = args.out_dir / f"{args.stem}.pmdata"
    idx_p.write_bytes(bytes(index))
    dat_p.write_bytes(bytes(data))

    expect = 12 + 8 * n_units
    allp = np.concatenate([np.asarray(v) for v in per_uid.values() if v]) \
        if any(per_uid.values()) else np.zeros(1)
    print(f"\n  units      : {n_units} ({n_empty} with no marks)")
    print(f"  marks      : {n_marks} (mean {n_marks / max(1, n_units):.1f}/unit)")
    print(f"  period p5/p50/p95 : {np.percentile(allp, 5):.0f} / "
          f"{np.percentile(allp, 50):.0f} / {np.percentile(allp, 95):.0f} samples "
          f"(= {SR / max(1, np.percentile(allp, 50)):.0f} Hz median)")
    print(f"  {idx_p.name}: {len(index)} B (expect {expect}) "
          f"{'OK' if len(index) == expect else 'MISMATCH'}")
    print(f"  {dat_p.name}: {len(data)} B")
    return 0 if len(index) == expect else 1


if __name__ == "__main__":
    sys.exit(main())
