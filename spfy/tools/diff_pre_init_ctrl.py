"""Side-by-side u32 diff of two pre-initStage1 ctrl[0..0x800] dumps.

Usage:
    python spfy/tools/diff_pre_init_ctrl.py <ours.bin> <theirs.bin>

Prints `ctrl+0x<off>: <ours> != <theirs>` for each 4-byte word that
differs. Also prints summary counts by region (0x000..0x100, 0x100..0x200,
..., 0x700..0x800).
"""
from __future__ import annotations

import sys
from pathlib import Path


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    ours = Path(argv[1]).read_bytes()
    theirs = Path(argv[2]).read_bytes()
    n = min(len(ours), len(theirs))
    if len(ours) != len(theirs):
        print(f"WARN: size mismatch ours={len(ours)} theirs={len(theirs)} "
              f"comparing first {n} bytes", file=sys.stderr)

    region_size = 0x100
    region_diffs = [0] * (n // region_size + 1)
    total_diffs = 0
    word_diffs: list[tuple[int, int, int]] = []  # (off, ours, theirs)

    for off in range(0, n, 4):
        if off + 4 > n:
            break
        a = int.from_bytes(ours[off:off + 4], "little")
        b = int.from_bytes(theirs[off:off + 4], "little")
        if a != b:
            total_diffs += 1
            region_diffs[off // region_size] += 1
            word_diffs.append((off, a, b))

    print(f"=== summary: {total_diffs} u32 diffs over 0..0x{n:x}")
    for i, c in enumerate(region_diffs):
        if c:
            print(f"  0x{i*region_size:03x}..0x{(i+1)*region_size:03x}: {c} diffs")
    print()
    print("=== per-word (ours != theirs):")
    for off, a, b in word_diffs:
        print(f"  ctrl+0x{off:03x}: ours=0x{a:08x}  theirs=0x{b:08x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
