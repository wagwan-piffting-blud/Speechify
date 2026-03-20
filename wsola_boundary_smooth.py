#!/usr/bin/env python3
"""
wsola_boundary_smooth.py - Smooth VDB audio at WSOLA boundary positions.

The Speechify engine uses WSOLA overlap-add with a Hanning window centered at
each unit's lp (local_pos) position. The window size is 80 samples (10ms at 8kHz).
When the audio has a spectral discontinuity at that exact sample position, the
overlap creates interference artifacts.

This script:
  1. Reads the unit table to find all lp positions per recording
  2. Reads the VDB audio
  3. At each lp boundary, applies a short crossfade/smoothing to reduce
     spectral discontinuities that WSOLA's Hanning window will amplify
  4. Writes a new VDB with smoothed audio

The key insight: we're not changing WHAT the engine selects (unit selection is
unchanged). We're making the audio smoother at the exact positions where WSOLA
will cut and overlap, so the concatenation sounds better regardless of which
units get selected.

Usage:
    python wsola_boundary_smooth.py ^
        --vin "en-US\\mara\\mara.vin" ^
        --vdb "en-US\\mara\\mara8.vdb" ^
        --output "en-US\\mara\\mara8_smooth.vdb" ^
        --window 40 ^
        --strength 0.5

    --window N      Smoothing window half-size in samples (default: 40 = 5ms at 8kHz)
                    Should be <= WSOLA window_size/2 (40 samples)
    --strength S    Blend factor 0.0-1.0 (0=no change, 1=full smoothing, default: 0.5)
    --diag-only     Print boundary statistics without modifying VDB
"""

import argparse
import os
import sys
import struct
import numpy as np

try:
    from tqdm import tqdm
except ImportError:
    print("Install: pip install numpy tqdm")
    sys.exit(1)

XOR_KEY = 0xCE


def xor_decode(data):
    return bytes(b ^ XOR_KEY for b in data)


def parse_riff_sub_chunks(data):
    result = []
    pos = 0
    while pos < len(data) - 8:
        sub_id = data[pos:pos+4].decode("ascii", errors="replace")
        sub_size = struct.unpack_from("<I", data, pos+4)[0]
        sub_data = data[pos+8:pos+8+sub_size]
        result.append((sub_id, pos, sub_data))
        pos += 8 + sub_size
        if sub_size % 2 == 1:
            pos += 1
    return result


def read_vin_units(vin_path):
    """Read VIN, extract unit table (uid, file_idx, local_pos, dur_like)."""
    with open(vin_path, "rb") as f:
        raw = f.read()
    plain = xor_decode(raw)
    assert plain[:4] == b"RIFF"

    # Find unit chunk
    pos = 12
    unit_data = None
    while pos < len(plain) - 8:
        chunk_id = plain[pos:pos+4].decode("ascii", errors="replace")
        chunk_size = struct.unpack_from("<I", plain, pos+4)[0]
        if chunk_id == "unit":
            unit_data = plain[pos+8:pos+8+chunk_size]
            break
        pos += 8 + chunk_size
        if chunk_size % 2 == 1:
            pos += 1

    if unit_data is None:
        print("ERROR: no unit chunk in VIN")
        sys.exit(1)

    # Parse sub-chunks
    sub_chunks = parse_riff_sub_chunks(unit_data)
    data = None
    for name, off, d in sub_chunks:
        if name == "data":
            data = d
            break

    record_size = 29
    n_units = len(data) // record_size
    print(f"  Unit table: {n_units:,} units")

    units = []
    for i in range(n_units):
        off = i * record_size
        uid = struct.unpack_from("<I", data, off)[0]
        file_idx = struct.unpack_from("<H", data, off+4)[0]
        local_pos = struct.unpack_from("<H", data, off+6)[0]
        dur_like = struct.unpack_from("<H", data, off+10)[0]
        units.append((uid, file_idx, local_pos, dur_like))

    return units


def read_vdb(vdb_path):
    """Read and decode VDB, return plain bytes and chunk locations."""
    with open(vdb_path, "rb") as f:
        raw = f.read()
    plain = bytearray(xor_decode(raw))

    assert plain[:4] == b"RIFF"

    chunks = {}
    pos = 12
    while pos < len(plain) - 8:
        chunk_id = plain[pos:pos+4].decode("ascii", errors="replace")
        chunk_size = struct.unpack_from("<I", plain, pos+4)[0]
        chunks[chunk_id] = (pos+8, chunk_size)
        pos += 8 + chunk_size
        if chunk_size % 2 == 1:
            pos += 1

    # Parse indx
    indx_offset, indx_size = chunks["indx"]
    n_recordings = indx_size // 8
    indx = []
    for i in range(n_recordings):
        ro = struct.unpack_from("<I", plain, indx_offset + i*8)[0]
        rl = struct.unpack_from("<I", plain, indx_offset + i*8 + 4)[0]
        indx.append((ro, rl))

    data_offset, data_size = chunks["data"]

    return plain, indx, data_offset, data_size


# u-law decode/encode tables
ULAW_DECODE = np.array([
    -32124,-31100,-30076,-29052,-28028,-27004,-25980,-24956,
    -23932,-22908,-21884,-20860,-19836,-18812,-17788,-16764,
    -15996,-15484,-14972,-14460,-13948,-13436,-12924,-12412,
    -11900,-11388,-10876,-10364, -9852, -9340, -8828, -8316,
     -7932, -7676, -7420, -7164, -6908, -6652, -6396, -6140,
     -5884, -5628, -5372, -5116, -4860, -4604, -4348, -4092,
     -3900, -3772, -3644, -3516, -3388, -3260, -3132, -3004,
     -2876, -2748, -2620, -2492, -2364, -2236, -2108, -1980,
     -1884, -1820, -1756, -1692, -1628, -1564, -1500, -1436,
     -1372, -1308, -1244, -1180, -1116, -1052,  -988,  -924,
      -876,  -844,  -812,  -780,  -748,  -716,  -684,  -652,
      -620,  -588,  -556,  -524,  -492,  -460,  -428,  -396,
      -372,  -356,  -340,  -324,  -308,  -292,  -276,  -260,
      -244,  -228,  -212,  -196,  -180,  -164,  -148,  -132,
      -120,  -112,  -104,   -96,   -88,   -80,   -72,   -64,
       -56,   -48,   -40,   -32,   -24,   -16,    -8,     0,
     32124, 31100, 30076, 29052, 28028, 27004, 25980, 24956,
     23932, 22908, 21884, 20860, 19836, 18812, 17788, 16764,
     15996, 15484, 14972, 14460, 13948, 13436, 12924, 12412,
     11900, 11388, 10876, 10364,  9852,  9340,  8828,  8316,
      7932,  7676,  7420,  7164,  6908,  6652,  6396,  6140,
      5884,  5628,  5372,  5116,  4860,  4604,  4348,  4092,
      3900,  3772,  3644,  3516,  3388,  3260,  3132,  3004,
      2876,  2748,  2620,  2492,  2364,  2236,  2108,  1980,
      1884,  1820,  1756,  1692,  1628,  1564,  1500,  1436,
      1372,  1308,  1244,  1180,  1116,  1052,   988,   924,
       876,   844,   812,   780,   748,   716,   684,   652,
       620,   588,   556,   524,   492,   460,   428,   396,
       372,   356,   340,   324,   308,   292,   276,   260,
       244,   228,   212,   196,   180,   164,   148,   132,
       120,   112,   104,    96,    88,    80,    72,    64,
        56,    48,    40,    32,    24,    16,     8,     0,
], dtype=np.int16)


def pcm16_array_to_ulaw(pcm_array):
    """Encode PCM16 array to u-law bytes — fully vectorized via lookup table."""
    return bytes(PCM16_TO_ULAW_TABLE[np.clip(pcm_array.astype(np.int32) + 32768, 0, 65535)])


# Build PCM16 -> u-law lookup table (65536 entries, one per possible int16 value)
def _build_ulaw_encode_table():
    BIAS = 0x84
    CLIP = 32635
    table = np.zeros(65536, dtype=np.uint8)
    for i in range(65536):
        pcm_val = i - 32768  # map [0, 65535] -> [-32768, 32767]
        sign = 0
        if pcm_val < 0:
            sign = 0x80
            pcm_val = -pcm_val
        if pcm_val > CLIP:
            pcm_val = CLIP
        pcm_val += BIAS
        exponent = 7
        mask = 0x4000
        while exponent > 0:
            if pcm_val & mask:
                break
            exponent -= 1
            mask >>= 1
        mantissa = (pcm_val >> (exponent + 3)) & 0x0F
        table[i] = ~(sign | (exponent << 4) | mantissa) & 0xFF
    return table

print("Building u-law encode table...", end=" ", flush=True)
PCM16_TO_ULAW_TABLE = _build_ulaw_encode_table()
print("done.")


def smooth_at_boundary(audio_pcm, center, half_window, strength):
    """
    Apply smoothing at a boundary position.

    Uses a weighted moving average centered at the boundary point,
    blended with the original signal by strength factor.

    This reduces spectral discontinuities that WSOLA's Hanning window
    would otherwise amplify during overlap-add.
    """
    n = len(audio_pcm)
    start = max(0, center - half_window)
    end = min(n, center + half_window)

    if end - start < 4:
        return  # too small to smooth

    segment = audio_pcm[start:end].astype(np.float64)

    # Gaussian-weighted moving average (preserves overall energy better than box)
    kernel_size = min(7, len(segment) // 2 * 2 + 1)  # odd, at most 7
    if kernel_size < 3:
        return

    sigma = kernel_size / 4
    x = np.arange(kernel_size) - kernel_size // 2
    kernel = np.exp(-x**2 / (2 * sigma**2))
    kernel /= kernel.sum()

    smoothed = np.convolve(segment, kernel, mode='same')

    # Blend: result = original * (1 - strength) + smoothed * strength
    blended = segment * (1 - strength) + smoothed * strength

    audio_pcm[start:end] = blended.astype(np.int16)


def process_single_recording(args):
    """Process one recording — worker function for multiprocessing."""
    fidx, rec_offset, rec_length, lp_positions, half_window, strength, ulaw_bytes = args

    if not lp_positions:
        return fidx, None, 0

    pcm = ULAW_DECODE[np.frombuffer(ulaw_bytes, dtype=np.uint8)].copy()

    count = 0
    for pos in sorted(lp_positions):
        if pos < half_window or pos >= len(pcm) - half_window:
            continue
        smooth_at_boundary(pcm, pos, half_window, strength)
        count += 1

    new_ulaw = pcm16_array_to_ulaw(pcm)
    return fidx, new_ulaw, count


def process_vdb(plain_vdb, indx, data_offset, units, half_window, strength, workers=0):
    """
    Apply boundary smoothing to all unit lp positions in the VDB.
    """
    from collections import defaultdict
    if workers <= 0:
        workers = os.cpu_count() or 4

    fidx_units = defaultdict(list)
    for uid, fidx, lp, dl in units:
        fidx_units[fidx].append((uid, lp, dl))

    # Build job list
    jobs = []
    skipped = 0
    for fidx in range(len(indx)):
        rec_offset, rec_length = indx[fidx]
        if rec_length < 16:
            skipped += 1
            continue

        unit_list = fidx_units.get(fidx, [])
        if not unit_list:
            skipped += 1
            continue

        lp_positions = set()
        for uid, lp, dl in unit_list:
            sample_pos = lp * 8
            if 0 < sample_pos < rec_length:
                lp_positions.add(sample_pos)

        if not lp_positions:
            skipped += 1
            continue

        abs_offset = data_offset + rec_offset
        ulaw_bytes = bytes(plain_vdb[abs_offset:abs_offset+rec_length])
        jobs.append((fidx, rec_offset, rec_length, lp_positions, half_window, strength, ulaw_bytes))

    print(f"  Jobs: {len(jobs):,} recordings to process, {skipped:,} skipped, {workers} workers")

    total_boundaries = 0
    processed = 0

    from concurrent.futures import ProcessPoolExecutor, as_completed

    with ProcessPoolExecutor(max_workers=workers) as pool:
        futures = {pool.submit(process_single_recording, job): job[0] for job in jobs}
        for future in tqdm(as_completed(futures), total=len(jobs), desc=f"Smoothing ({workers} workers)"):
            fidx, new_ulaw, count = future.result()
            if new_ulaw is not None:
                rec_offset, rec_length = indx[fidx]
                abs_offset = data_offset + rec_offset
                plain_vdb[abs_offset:abs_offset+rec_length] = new_ulaw
                processed += 1
                total_boundaries += count

    return processed, skipped, total_boundaries


def main():
    parser = argparse.ArgumentParser(
        description="Smooth VDB audio at WSOLA boundary positions",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--vin", required=True, help="Input VIN file")
    parser.add_argument("--vdb", required=True, help="Input VDB file")
    parser.add_argument("--output", default=None, help="Output VDB file")
    parser.add_argument("--window", type=int, default=40,
                        help="Smoothing half-window in samples (default: 40 = 5ms)")
    parser.add_argument("--strength", type=float, default=0.5,
                        help="Smoothing strength 0.0-1.0 (default: 0.5)")
    parser.add_argument("--diag-only", action="store_true",
                        help="Print boundary stats without modifying")
    parser.add_argument("--workers", type=int, default=0,
                        help="Parallel workers (0=auto CPU count)")

    args = parser.parse_args()

    if args.output is None:
        base, ext = os.path.splitext(args.vdb)
        args.output = f"{base}_smooth{ext}"

    print(f"{'='*60}")
    print(f"  WSOLA Boundary Smoothing")
    print(f"{'='*60}")
    print(f"  VIN:      {args.vin}")
    print(f"  VDB:      {args.vdb}")
    print(f"  Output:   {args.output}")
    print(f"  Window:   {args.window} samples ({args.window/8:.1f}ms)")
    print(f"  Strength: {args.strength}")

    # Read unit table
    print(f"\nReading VIN...")
    units = read_vin_units(args.vin)

    # Read VDB
    print(f"Reading VDB...")
    plain_vdb, indx, data_offset, data_size = read_vdb(args.vdb)
    print(f"  {len(indx):,} recordings, {data_size:,} bytes audio data")

    # Count boundaries
    from collections import defaultdict
    fidx_lps = defaultdict(set)
    for uid, fidx, lp, dl in units:
        if lp > 0:
            fidx_lps[fidx].add(lp * 8)

    total_boundaries = sum(len(v) for v in fidx_lps.values())
    recs_with_boundaries = sum(1 for v in fidx_lps.values() if len(v) > 0)
    print(f"\n  Boundary positions: {total_boundaries:,}")
    print(f"  Recordings with boundaries: {recs_with_boundaries:,}")
    print(f"  Mean boundaries/recording: {total_boundaries/max(recs_with_boundaries,1):.1f}")

    if args.diag_only:
        print(f"\n--diag-only: done")
        return

    # Process
    print(f"\nSmoothing...")
    processed, skipped, smoothed = process_vdb(
        plain_vdb, indx, data_offset, units, args.window, args.strength, args.workers
    )

    print(f"\n  Recordings processed: {processed:,}")
    print(f"  Recordings skipped:  {skipped:,}")
    print(f"  Boundaries smoothed: {smoothed:,}")

    # Write output
    print(f"\nWriting output...")
    encoded = xor_decode(bytes(plain_vdb))
    with open(args.output, "wb") as f:
        f.write(encoded)
    print(f"  Written: {args.output} ({len(encoded):,} bytes)")
    print(f"\nDone. Replace your VDB with this file and test.")


if __name__ == "__main__":
    main()
