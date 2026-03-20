#!/usr/bin/env python3
"""
patch_f0tr.py — Patch f0tr tree leaf means to Mara's pitch range.

The f0tr tree predicts target F0 for each phonetic context. Tom's leaves
are clustered at 107-127 Hz (3 semitones). Mara needs ~165-390 Hz (14+ semitones).

Usage:
    python patch_f0tr.py --vin mara.vin --output mara_f0patched.vin --scale 1.58
    python patch_f0tr.py --vin mara.vin --output mara_f0patched.vin --scale 1.58 --expand 3.0
    python patch_f0tr.py --vin mara.vin --diag-only

    --scale S      Multiply all leaf means by S (1.58 maps Tom 118Hz to Mara 187Hz)
    --expand E     Expand range around median by factor E (3.0 = 9 semitone range)
                   Applied AFTER scale.
    --diag-only    Just show current leaf values
"""

import argparse
import os
import sys
import struct
import numpy as np

XOR_KEY = 0xCE

def xor_decode(data):
    return bytes(b ^ XOR_KEY for b in data)


def parse_f0tr_tree(plain_vin):
    """Parse the f0tr tree from a decoded VIN, return leaf info and offsets."""
    # Find f0tr chunk
    pos = 12
    f0tr_offset = None
    f0tr_data = None
    while pos < len(plain_vin) - 8:
        cid = plain_vin[pos:pos+4].decode("ascii", errors="replace")
        csz = struct.unpack_from("<I", plain_vin, pos+4)[0]
        if cid == "f0tr":
            f0tr_offset = pos
            f0tr_data = plain_vin[pos+8:pos+8+csz]
            break
        pos += 8 + csz + (csz % 2)

    if f0tr_data is None:
        print("ERROR: no f0tr chunk found")
        return None

    # Find tree sub-chunk
    p = 0
    tree_data = None
    tree_off_in_f0tr = None
    while p < len(f0tr_data) - 8:
        sid = f0tr_data[p:p+4].decode("ascii", errors="replace")
        ssz = struct.unpack_from("<I", f0tr_data, p+4)[0]
        if sid == "tree":
            tree_data = f0tr_data[p+8:p+8+ssz]
            tree_off_in_f0tr = p
            break
        p += 8 + ssz + (ssz % 2)

    if tree_data is None:
        print("ERROR: no tree sub-chunk in f0tr")
        return None

    # Absolute base of tree data in VIN
    tree_abs_base = f0tr_offset + 8 + tree_off_in_f0tr + 8

    # Parse nodes
    n_nodes = struct.unpack_from("<I", tree_data, 0)[0]
    leaves = []
    pos = 4
    for i in range(n_nodes):
        node_index = struct.unpack_from("<I", tree_data, pos)[0]
        yes_child = struct.unpack_from("<i", tree_data, pos+4)[0]

        if yes_child < 0:  # Leaf
            mean = struct.unpack_from("<f", tree_data, pos+12)[0]
            variance = struct.unpack_from("<f", tree_data, pos+16)[0]
            # Absolute offset of mean f32 in the VIN
            mean_abs_offset = tree_abs_base + pos + 12
            var_abs_offset = tree_abs_base + pos + 16
            leaves.append({
                "node": node_index,
                "mean": mean,
                "variance": variance,
                "mean_offset": mean_abs_offset,
                "var_offset": var_abs_offset,
            })
            pos += 20
        else:
            pos += 16

    return leaves


def main():
    parser = argparse.ArgumentParser(description="Patch f0tr tree for Mara pitch range")
    parser.add_argument("--vin", required=True)
    parser.add_argument("--output", default=None)
    parser.add_argument("--scale", type=float, default=1.58,
                        help="Scale factor for leaf means (1.58 = Tom->Mara median)")
    parser.add_argument("--expand", type=float, default=1.0,
                        help="Range expansion factor around median (1.0=no change, 3.0=3x wider)")
    parser.add_argument("--diag-only", action="store_true")

    args = parser.parse_args()

    if args.output is None:
        base, ext = os.path.splitext(args.vin)
        args.output = f"{base}_f0patched{ext}"

    # Read and decode VIN
    with open(args.vin, "rb") as f:
        raw = f.read()
    plain = bytearray(xor_decode(raw))

    assert plain[:4] == b"RIFF"
    assert plain[8:12] == b"svin"

    # Parse tree
    leaves = parse_f0tr_tree(plain)
    if leaves is None:
        sys.exit(1)

    means = np.array([l["mean"] for l in leaves])
    variances = np.array([l["variance"] for l in leaves])

    print(f"{'='*60}")
    print(f"  f0tr Tree Patcher")
    print(f"{'='*60}")
    print(f"  VIN: {args.vin}")
    print(f"  Leaves: {len(leaves)}")
    print(f"\n  Current leaf means:")
    print(f"    Min:    {np.min(means):.2f} Hz")
    print(f"    Median: {np.median(means):.2f} Hz")
    print(f"    Max:    {np.max(means):.2f} Hz")
    print(f"    Range:  {12*np.log2(np.max(means)/np.min(means)):.1f} semitones")
    print(f"\n  Current variances:")
    print(f"    Min:  {np.min(variances):.4f}  Mean: {np.mean(variances):.4f}  Max: {np.max(variances):.4f}")

    if args.diag_only:
        print(f"\n  All leaves:")
        for l in leaves:
            print(f"    Node {l['node']:3d}: mean={l['mean']:7.2f} Hz  var={l['variance']:.4f}")
        return

    # Compute new means
    median_old = np.median(means)
    new_means = means * args.scale

    if args.expand != 1.0:
        # Expand range around the new median
        new_median = np.median(new_means)
        # Convert to semitones, expand, convert back
        semitones = 12 * np.log2(new_means / new_median)
        semitones_expanded = semitones * args.expand
        new_means = new_median * (2 ** (semitones_expanded / 12))

    print(f"\n  Patching with scale={args.scale}, expand={args.expand}")
    print(f"\n  New leaf means:")
    print(f"    Min:    {np.min(new_means):.2f} Hz")
    print(f"    Median: {np.median(new_means):.2f} Hz")
    print(f"    Max:    {np.max(new_means):.2f} Hz")
    print(f"    Range:  {12*np.log2(np.max(new_means)/np.min(new_means)):.1f} semitones")

    # Show per-leaf changes
    print(f"\n  Leaf changes:")
    for i, l in enumerate(leaves):
        old = l["mean"]
        new = new_means[i]
        print(f"    Node {l['node']:3d}: {old:7.2f} -> {new:7.2f} Hz ({new/old:.2f}x)")

    # Patch the VIN
    for i, l in enumerate(leaves):
        struct.pack_into("<f", plain, l["mean_offset"], float(new_means[i]))

    # Verify
    print(f"\n  Verifying patch...")
    for i, l in enumerate(leaves[:3]):
        val = struct.unpack_from("<f", plain, l["mean_offset"])[0]
        print(f"    Node {l['node']}: read back {val:.2f} Hz (expected {new_means[i]:.2f})")

    # XOR encode and write
    encoded = xor_decode(bytes(plain))
    with open(args.output, "wb") as f:
        f.write(encoded)

    print(f"\n  Written: {args.output}")
    print(f"  Replace your VIN and test synthesis.")
    print(f"\n  If pitch sounds wrong, try different values:")
    print(f"    --scale 1.3  (conservative, ~154 Hz median)")
    print(f"    --scale 1.58 (target 187 Hz, our measured GT median)")
    print(f"    --scale 2.0  (aggressive, ~237 Hz median)")
    print(f"    --expand 2.0 (double the pitch range)")
    print(f"    --expand 3.0 (triple, closer to Mara's 14.7 semitone GT range)")


if __name__ == "__main__":
    main()

# TEST SENTENCE: And now a look at weather conditions at 9 P.M. Wichita was cloudy, the temperature was 63, dew point 54 and the relative humidity was 72 percent.

"""
USAGE:

python patch_f0tr.py ^
    --vin "en-US\aimara\aimara_orig.vin" ^
    --output "en-US\aimara\aimara.vin" ^
    --scale 1.0 ^
    --expand 6.0

"""
