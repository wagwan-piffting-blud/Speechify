"""fe_carve_tables.py -- carve the 173 dict tables registered by
FUN_0836849e to individual files, with a manifest.

The FE engine state holds a 173-pointer array at offset +0x54, populated
by FUN_0836849e from compile-time DAT_* references in .data. Each table
spans from its registered pointer to the next pointer, so sizes are
exactly determined.

Usage:
    python reveng/fe_carve_tables.py --dll bin/SWIttsFe-en-US.dll \
        --out spfy/data/fe_tables/

Produces:
    spfy/data/fe_tables/t000.bin .. t172.bin
    spfy/data/fe_tables/manifest.csv (idx, va, data_off, size, first_8_hex, sha1_short)
"""

import argparse
import csv
import hashlib
import struct
import sys
from pathlib import Path

# Addresses extracted from FUN_0836849e decompile (173 entries).
TABLE_ADDRS = [
    0x083eb850, 0x083eba38, 0x083ec258, 0x083eccc0, 0x083ecd48, 0x083ed068,
    0x083ed4a0, 0x083ee500, 0x083ee9f0, 0x083eea20, 0x083eea48, 0x083eedc8,
    0x083efdd8, 0x083f0d98, 0x083f1c80, 0x083f26a0, 0x083f3700, 0x083f3f48,
    0x083f45a0, 0x083f4c40, 0x083f5f30, 0x083f6c00, 0x083f8680, 0x083f8cd0,
    0x083f92a8, 0x083f93c0, 0x083f9bd8, 0x083f9e40, 0x083fa958, 0x083fb598,
    0x083fc228, 0x083fcef8, 0x083fdc20, 0x083fe980, 0x083ff6c0, 0x08400468,
    0x084011b8, 0x08401ee8, 0x08402d60, 0x08403bb8, 0x08404aa0, 0x084058f8,
    0x084067e8, 0x08407668, 0x084084c0, 0x08409380, 0x0840a200, 0x0840b0b8,
    0x0840bed8, 0x0840cd60, 0x0840dbd0, 0x0840ea30, 0x0840f908, 0x084107a0,
    0x08411688, 0x08412510, 0x08413418, 0x084142e0, 0x08415150, 0x08416008,
    0x08416f30, 0x08417d60, 0x08418c48, 0x08419ad8, 0x0841aa10, 0x0841b850,
    0x0841c720, 0x0841d5c8, 0x0841e460, 0x0841f2d0, 0x08420168, 0x08421000,
    0x08421ed8, 0x08422db8, 0x08423c60, 0x08424ab8, 0x08425990, 0x084268e8,
    0x08427770, 0x08428680, 0x084294f8, 0x0842a368, 0x0842b220, 0x0842c100,
    0x0842cfe0, 0x0842de20, 0x0842ed30, 0x0842fb80, 0x08430a38, 0x08431960,
    0x08432798, 0x08433630, 0x084344f0, 0x08434d88, 0x08435b38, 0x084368d0,
    0x08437670, 0x08438410, 0x084391d0, 0x08439fc0, 0x0843ad88, 0x0843bb60,
    0x0843c920, 0x0843d710, 0x0843e4e0, 0x0843f308, 0x08440130, 0x08440f28,
    0x08441d48, 0x08442b40, 0x08443900, 0x08444708, 0x08445510, 0x08446358,
    0x08447158, 0x08447f90, 0x08448d88, 0x08449b68, 0x0844a978, 0x0844b770,
    0x0844c5b0, 0x0844d3d0, 0x0844e1e8, 0x0844efd8, 0x0844fe00, 0x08450c48,
    0x08451a90, 0x084528b8, 0x08453710, 0x08454520, 0x08455340, 0x08456158,
    0x08456f78, 0x08457dd8, 0x08458c10, 0x08459a78, 0x0845a8a0, 0x0845b6e0,
    0x0845c4d8, 0x0845d348, 0x0845e168, 0x0845eff8, 0x0845fe08, 0x08460c90,
    0x08461af0, 0x08462988, 0x08463790, 0x084645b0, 0x084653f8, 0x08466230,
    0x084670b8, 0x08467f30, 0x08468d80, 0x08469b98, 0x0846aa30, 0x0846b8a8,
    0x0846c6d8, 0x0846d510, 0x0846e398, 0x0846f190, 0x0846ff90, 0x08470df8,
    0x08471c38, 0x08472ac0, 0x08473940, 0x08474770, 0x084755e8, 0x08476440,
    0x084772d8, 0x084780f8, 0x08478fa0, 0x08479e38, 0x0847acc8,
]

IMG_BASE = 0x07dd0000
DATA_VADDR = 0x005c9000
DATA_RAW_OFF = 0x005c9000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dll", required=True)
    ap.add_argument("--out", required=True,
                    help="Output directory for tNNN.bin files")
    args = ap.parse_args()

    buf = Path(args.dll).read_bytes()
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    rows = []
    n = len(TABLE_ADDRS)
    for i, va in enumerate(TABLE_ADDRS):
        # End address: next table's va, or end of registered region.
        if i + 1 < n:
            end_va = TABLE_ADDRS[i + 1]
        else:
            # Estimate last table's size: median of others = ~3.6 KB.
            # The lookup code can't read past where it expects -- we use
            # 4096 as a safe upper bound for the tail.
            end_va = va + 4096
        size = end_va - va
        data_off = (va - IMG_BASE) - DATA_VADDR
        file_off = (va - IMG_BASE)
        chunk = buf[file_off : file_off + size]
        path = out_dir / f"t{i:03d}.bin"
        path.write_bytes(chunk)
        first_8 = chunk[:8].hex()
        sha1 = hashlib.sha1(chunk).hexdigest()[:12]
        rows.append({
            "idx": i,
            "va_hex": f"0x{va:08x}",
            "data_off_hex": f"0x{data_off:06x}",
            "size": size,
            "first_8_hex": first_8,
            "sha1_short": sha1,
        })

    manifest = out_dir / "manifest.csv"
    with manifest.open("w", newline="", encoding="utf-8") as fp:
        w = csv.DictWriter(fp,
            fieldnames=["idx", "va_hex", "data_off_hex", "size",
                        "first_8_hex", "sha1_short"])
        w.writeheader()
        for r in rows:
            w.writerow(r)
    print(f"wrote {n} tables + manifest.csv to {out_dir}")
    print(f"  total carved: {sum(r['size'] for r in rows):,} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
