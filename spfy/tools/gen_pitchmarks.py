"""Generate Speechify pmindex/pmdata pitch-mark files for a voice.

Format (see reveng/DLL_ANALYSIS.md "pmindex / pmdata ON-DISK FORMAT"):

  pmindex : BIG-ENDIAN u32.
              [0]      sample rate the marks were measured at
              [1],[2]  reserved (read by nothing)
              [3+2k]   offset of unit k's marks into pmdata, in int16 ELEMENTS
              [4+2k]   number of marks for unit k
            size = 12 + 8 * n_units
  pmdata  : BIG-ENDIAN signed int16, flat. Values are PITCH PERIODS (deltas)
            in SAMPLES; the engine cumulative-sums them into positions.

The index is keyed by VIN unit id -- a WSOLA "sub-unit" is exactly a VIN
unit-table record (proven 44/44 against live Frida traces). Durations in the
unit table are 1 ms ticks, so a unit spans dur_like*8 samples at 8 kHz and its
marks must sum to about that.

Marks are synthesised from the unit table's own per-unit F0 bytes (raw Hz,
0 = unvoiced) by linearly interpolating f0_start -> f0_end across the unit.
Prior measurement (reveng/spfy4, n=30) found detected LPC/GCI marks and
uniform F0-driven marks statistically tied, so this does not need the audio.

  python gen_pitchmarks.py <voice.vin> <out_dir> [--stem tom8] [--rate 8000]
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

# Record geometry per spfy/src/voice/unit_table.c UNIT_LAYOUTS.
# (rec_size, f0_start offset); f0_end/f0_mid/f0_context follow at +1/+2/+3.
LAYOUTS = {
    100004: (23, 0x10),
    100005: (24, 0x10),
    100006: (29, 0x10),
    100007: (25, 0x11),
    100008: (30, 0x11),
}

# The engine's own ceiling, read live from WSOLA state+0x3614-adjacent
# max_pitch_period (= 160 at 8 kHz). Floor keeps a sane upper F0 bound.
MIN_PERIOD = 20
MAX_PERIOD = 160
# Fallback period for units the table marks unvoiced (f0 == 0). Tom's median
# nonzero f0 is 118 Hz -> 8000/118 ~= 68 samples.
DEFAULT_F0 = 118.0

TICKS_TO_SAMPLES = 8


def load_plain(path: Path) -> bytes:
    raw = path.read_bytes()
    if raw[:4] == b"RIFF":
        return raw
    if bytes(b ^ 0xCE for b in raw[:4]) == b"RIFF":
        return bytes(b ^ 0xCE for b in raw)
    raise SystemExit(f"{path}: not RIFF and not XOR-0xCE RIFF")


def riff_walk(buf: bytes, start: int, end: int):
    pos = start
    while pos + 8 <= end:
        fcc = buf[pos:pos + 4]
        size = struct.unpack_from("<I", buf, pos + 4)[0]
        if pos + 8 + size > end:
            return
        yield fcc, pos + 8, size
        pos += 8 + size + (size & 1)


def find_unit_data(buf: bytes):
    """Return (data_offset, data_size, version) for the unit chunk."""
    for fcc, off, size in riff_walk(buf, 12, len(buf)):
        if fcc != b"unit":
            continue
        for base in (off, off + 4):     # chunk may carry a 4-byte form id
            ver = data_off = data_n = None
            for f2, o2, s2 in riff_walk(buf, base, off + size):
                if f2 == b"vers":
                    ver = struct.unpack_from("<I", buf, o2)[0]
                elif f2 == b"data":
                    data_off, data_n = o2, s2
            if ver is not None and data_off is not None:
                return data_off, data_n, ver
    raise SystemExit("unit chunk not found in VIN")


def marks_for_unit(n_samples: int, f0_start: int, f0_end: int) -> list[int]:
    """Periods (samples) whose cumulative sum lands at <= n_samples."""
    if n_samples <= 0:
        return []
    a = float(f0_start)
    b = float(f0_end)
    if a <= 0.0 and b <= 0.0:
        a = b = DEFAULT_F0          # unvoiced: uniform pseudo-period
    elif a <= 0.0:
        a = b
    elif b <= 0.0:
        b = a

    periods: list[int] = []
    pos = 0
    while True:
        frac = pos / n_samples
        f0 = a + (b - a) * frac
        p = int(round(8000.0 / f0)) if f0 > 0 else int(round(8000.0 / DEFAULT_F0))
        if p < MIN_PERIOD:
            p = MIN_PERIOD
        elif p > MAX_PERIOD:
            p = MAX_PERIOD
        if pos + p > n_samples:
            break
        pos += p
        periods.append(p)
    if not periods:
        # Unit shorter than one period: one mark at the unit end so the
        # cumulative position still terminates inside the span.
        periods.append(max(1, n_samples))
    return periods


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("vin", type=Path)
    ap.add_argument("out_dir", type=Path)
    ap.add_argument("--stem", default=None,
                    help="output basename (default: <vin stem>8)")
    ap.add_argument("--rate", type=int, default=8000,
                    help="value written to pmindex[0] (default 8000)")
    args = ap.parse_args()

    buf = load_plain(args.vin)
    off, size, ver = find_unit_data(buf)
    if ver not in LAYOUTS:
        raise SystemExit(f"unsupported unit version {ver}")
    rec_size, f0_off = LAYOUTS[ver]
    if size % rec_size:
        raise SystemExit(f"unit/data {size} not divisible by {rec_size}")
    n_units = size // rec_size
    print(f"{args.vin.name}: unit vers={ver} rec_size={rec_size} "
          f"n_units={n_units}")

    index = bytearray()
    index += struct.pack(">III", args.rate, 0, 0)
    data = bytearray()

    n_marks_total = 0
    n_voiced = n_unvoiced = n_empty = 0
    total_samples = 0

    for uid in range(n_units):
        p = off + uid * rec_size
        stored = struct.unpack_from("<I", buf, p)[0]
        if stored != uid:
            raise SystemExit(f"unit[{uid}]: on-disk unit_id={stored} mismatch")
        dur_like = struct.unpack_from("<H", buf, p + 0x0A)[0]
        f0_start = buf[p + f0_off]
        f0_end = buf[p + f0_off + 1]

        n_samples = dur_like * TICKS_TO_SAMPLES
        total_samples += n_samples
        periods = marks_for_unit(n_samples, f0_start, f0_end)

        if n_samples == 0:
            n_empty += 1
        elif f0_start or f0_end:
            n_voiced += 1
        else:
            n_unvoiced += 1

        elem_off = len(data) // 2
        index += struct.pack(">II", elem_off, len(periods))
        for v in periods:
            data += struct.pack(">h", v)
        n_marks_total += len(periods)

    args.out_dir.mkdir(parents=True, exist_ok=True)
    stem = args.stem or (args.vin.stem + "8")
    idx_path = args.out_dir / f"{stem}.pmindex"
    dat_path = args.out_dir / f"{stem}.pmdata"
    idx_path.write_bytes(bytes(index))
    dat_path.write_bytes(bytes(data))

    expect_idx = 12 + 8 * n_units
    print(f"  units            : {n_units} "
          f"(voiced {n_voiced}, unvoiced {n_unvoiced}, zero-dur {n_empty})")
    print(f"  total span       : {total_samples} samples "
          f"({total_samples / 8000.0:.1f} s)")
    print(f"  marks            : {n_marks_total} "
          f"(mean {n_marks_total / max(1, n_units):.1f}/unit)")
    print(f"  {idx_path.name:16s}: {len(index)} B (expect {expect_idx}) "
          f"{'OK' if len(index) == expect_idx else 'MISMATCH'}")
    print(f"  {dat_path.name:16s}: {len(data)} B "
          f"({len(data) // 2} int16 elements)")
    if len(index) != expect_idx:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
