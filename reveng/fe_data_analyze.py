"""fe_data_analyze.py -- structural inference on the FE DLL's .data section.

The Speechify FE is a customized port of IBM Eloquence ECI (per F0
catalog). Its proprietary linguistic resources (dictionaries, LTS rules,
prosody tables) are embedded in the .data segment of SWIttsFe-en-US.dll.

The PE .reloc section tells us EXACTLY which 4-byte slots inside .data
hold runtime-fixed-up pointers (vtables, function pointer tables, RTTI,
inter-table indices). Slots that are NOT relocated hold pure data --
that's where the dictionaries live.

This script produces:

  1. A relocation map for the .data section (which offsets are pointers).
  2. An entropy profile (sliding-window byte-entropy histogram) -- packed
     dictionaries / hash tables show as distinct entropy plateaus.
  3. Zero-run boundaries -- table-of-tables structures are usually
     separated by aligned zero padding.
  4. ASCII-text spans -- string tables (used by Eloquence's TPL programs).
  5. Detected aligned-pointer clusters (consecutive relocated slots) --
     these are vtables / dispatch tables / function pointer arrays.
  6. Likely-data regions = ranges with no relocations + nontrivial
     entropy -- the dictionary content.

Usage:
    python reveng/fe_data_analyze.py \\
        --dll bin/SWIttsFe-en-US.dll \\
        --report build/fe_data_layout.json
"""

import argparse
import json
import math
import struct
import sys
from collections import Counter
from pathlib import Path


# -------------------------------------------------------------------- #
# PE parsing                                                           #
# -------------------------------------------------------------------- #

def parse_pe(buf):
    if buf[:2] != b"MZ":
        raise ValueError("not PE")
    pe_off = struct.unpack_from("<I", buf, 0x3C)[0]
    if buf[pe_off:pe_off+4] != b"PE\x00\x00":
        raise ValueError("no PE sig")
    coff = pe_off + 4
    machine, n_sec, _, _, _, opt_size, _ = struct.unpack_from(
        "<HHIIIHH", buf, coff)
    opt_off  = coff + 20
    image_base = struct.unpack_from("<I", buf, opt_off + 28)[0]
    sect_off = opt_off + opt_size
    sects = []
    for i in range(n_sec):
        s = sect_off + i * 40
        name = buf[s:s+8].rstrip(b"\x00").decode("ascii", errors="replace")
        vsize, vaddr, raw_size, raw_off = struct.unpack_from(
            "<IIII", buf, s + 8)
        sects.append({"name": name, "vaddr": vaddr, "vsize": vsize,
                      "raw_off": raw_off, "raw_size": raw_size})
    return {"image_base": image_base, "sections": sects, "machine": machine}


def parse_relocs(buf, reloc_sect):
    """Yield virtual addresses of all relocated 4-byte slots."""
    p = reloc_sect["raw_off"]
    end = p + reloc_sect["raw_size"]
    while p + 8 <= end:
        page_va = struct.unpack_from("<I", buf, p)[0]
        block_size = struct.unpack_from("<I", buf, p + 4)[0]
        if block_size < 8 or page_va == 0:
            break
        n_entries = (block_size - 8) // 2
        ents_off = p + 8
        for i in range(n_entries):
            e = struct.unpack_from("<H", buf, ents_off + i * 2)[0]
            typ = e >> 12
            off = e & 0xFFF
            if typ == 0:
                continue        # IMAGE_REL_BASED_ABSOLUTE = padding
            if typ == 3:        # IMAGE_REL_BASED_HIGHLOW (32-bit)
                yield page_va + off
        p += block_size


# -------------------------------------------------------------------- #
# Analysis                                                             #
# -------------------------------------------------------------------- #

def shannon_entropy(byts):
    if not byts:
        return 0.0
    c = Counter(byts)
    n = float(len(byts))
    return -sum((v / n) * math.log2(v / n) for v in c.values())


def find_zero_runs(buf, min_len=64):
    """Yield (offset, length) of consecutive zero-byte runs >= min_len."""
    n = len(buf)
    i = 0
    while i < n:
        if buf[i] != 0:
            i += 1
            continue
        j = i
        while j < n and buf[j] == 0:
            j += 1
        if j - i >= min_len:
            yield i, j - i
        i = j


def find_ascii_runs(buf, min_len=12):
    n = len(buf)
    i = 0
    while i < n:
        if not (0x20 <= buf[i] < 0x7f):
            i += 1
            continue
        j = i
        while j < n and 0x20 <= buf[j] < 0x7f:
            j += 1
        if j - i >= min_len:
            yield i, j - i, buf[i:j].decode("ascii", errors="replace")
        i = j


def cluster_runs(positions, max_gap=8):
    """Group sorted positions into runs whose successive deltas <= max_gap."""
    if not positions:
        return []
    runs = []
    start = prev = positions[0]
    for p in positions[1:]:
        if p - prev <= max_gap:
            prev = p
        else:
            runs.append((start, prev))
            start = prev = p
    runs.append((start, prev))
    return runs


# -------------------------------------------------------------------- #
# Main                                                                 #
# -------------------------------------------------------------------- #

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dll", required=True)
    ap.add_argument("--report", required=True,
                    help="JSON output path with layout summary")
    ap.add_argument("--window", type=int, default=4096,
                    help="Entropy window size in bytes (default 4096)")
    args = ap.parse_args()

    buf = Path(args.dll).read_bytes()
    pe = parse_pe(buf)
    image_base = pe["image_base"]
    data_sect  = next(s for s in pe["sections"] if s["name"] == ".data")
    reloc_sect = next(s for s in pe["sections"] if s["name"] == ".reloc")
    data_va_lo = data_sect["vaddr"]
    data_va_hi = data_va_lo + data_sect["vsize"]

    # 1. relocations restricted to .data
    relocs = sorted(va for va in parse_relocs(buf, reloc_sect)
                    if data_va_lo <= va < data_va_hi)
    reloc_offsets = [va - data_va_lo for va in relocs]

    # 2. extract .data
    data = buf[data_sect["raw_off"]
               : data_sect["raw_off"] + data_sect["vsize"]]

    # 3. entropy profile
    win = args.window
    profile = []
    for off in range(0, len(data), win):
        block = data[off:off + win]
        e = shannon_entropy(block)
        profile.append({"off": off, "len": len(block), "entropy": e})

    # 4. zero-runs
    zero_runs = list(find_zero_runs(data, min_len=128))

    # 5. ASCII spans
    ascii_runs = list(find_ascii_runs(data, min_len=12))

    # 6. relocation clusters (vtables / fn-ptr tables)
    reloc_clusters = cluster_runs(reloc_offsets, max_gap=8)
    big_clusters = [(s, e, (e - s) // 4 + 1)
                    for s, e in reloc_clusters
                    if (e - s) // 4 + 1 >= 8]

    # 7. likely-data ranges -- between zero runs AND no relocs nearby
    reloc_set = set(reloc_offsets)
    def has_reloc_in(lo, hi):
        for o in reloc_offsets:
            if lo <= o < hi:
                return True
        return False

    boundaries = sorted(set(
        [0] + [o for o, _ in zero_runs] +
        [o + L for o, L in zero_runs] + [len(data)]
    ))
    data_ranges = []
    for i in range(len(boundaries) - 1):
        lo = boundaries[i]
        hi = boundaries[i + 1]
        if hi - lo < 1024:
            continue
        if has_reloc_in(lo, hi):
            kind = "code/ptr"
        else:
            kind = "data"
        data_ranges.append({"lo": lo, "hi": hi, "len": hi - lo, "kind": kind})

    # Sort large data-ranges by size
    data_ranges_sorted = sorted(data_ranges, key=lambda r: -r["len"])

    summary = {
        "image_base": image_base,
        "data_section": {
            "vaddr": data_va_lo,
            "vsize": data_sect["vsize"],
            "raw_size": data_sect["raw_size"],
        },
        "reloc_count_in_data": len(relocs),
        "reloc_clusters_total": len(reloc_clusters),
        "reloc_clusters_big_8plus": [
            {"off": s, "end": e, "n_ptrs": n}
            for s, e, n in sorted(big_clusters, key=lambda t: -t[2])[:20]
        ],
        "zero_runs_total": len(zero_runs),
        "zero_runs_big_4kplus": [
            {"off": o, "len": L}
            for o, L in zero_runs if L >= 4096
        ][:30],
        "ascii_runs_total": len(ascii_runs),
        "ascii_top": [
            {"off": o, "len": L,
             "text": s.replace("\n", "\\n")[:80]}
            for o, L, s in sorted(ascii_runs, key=lambda t: -t[1])[:25]
        ],
        "data_ranges_top10": data_ranges_sorted[:10],
        "entropy_profile": profile,
    }

    out = Path(args.report)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"wrote {out}")
    print(f"  image_base=0x{image_base:08x}  .data vaddr=0x{data_va_lo:08x}"
          f" vsize={data_sect['vsize']:,}")
    print(f"  reloc-in-data: {len(relocs):,} ptrs across "
          f"{len(reloc_clusters)} clusters; big (>=8): {len(big_clusters)}")
    print(f"  zero-runs: {len(zero_runs)} (>=4KB: "
          f"{sum(1 for _, L in zero_runs if L >= 4096)})")
    print(f"  ascii spans: {len(ascii_runs)}")
    print(f"  data ranges (top by size):")
    for r in data_ranges_sorted[:6]:
        print(f"    [{r['lo']:>8x} .. {r['hi']:>8x}]  "
              f"{r['len']:>10,d} bytes  kind={r['kind']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
