"""fe_dump_data_section.py -- extract the .data section from
SWIttsFe-en-US.dll for offline format analysis (FE F1, 2026-05-06).

Per Ghidra MCP recon, the FE DLL packs its proprietary linguistic
resources (worddict, hugedict, abbrdict, rootdict, maindict, disambigDict,
LTS rules, prosody tables) into the .data segment. No external resource
files exist. This script dumps that segment to disk for hex/format work.

Pure stdlib -- no pefile, no third-party deps. Parses the PE header
manually (DOS stub, COFF header, optional header, section table) and
slices out the section by name.

Usage:
    python reveng/fe_dump_data_section.py \
        --dll bin/SWIttsFe-en-US.dll \
        --section .data \
        --out spfy/data/fe_data_section.bin

Default output: c:/tmp/fe_data_section.bin (1.1 MB for Tom's en-US FE).
"""

import argparse
import struct
import sys
from pathlib import Path


def parse_pe_sections(buf):
    """Return a list of {name, vaddr, vsize, raw_off, raw_size} dicts."""
    if buf[:2] != b"MZ":
        raise ValueError("not a DOS/PE file (no MZ header)")
    pe_off = struct.unpack_from("<I", buf, 0x3C)[0]
    if buf[pe_off:pe_off+4] != b"PE\x00\x00":
        raise ValueError(f"no PE signature at 0x{pe_off:08x}")
    coff = pe_off + 4
    machine, n_sections, _, _, _, opt_size, _ = struct.unpack_from(
        "<HHIIIHH", buf, coff)
    opt_off  = coff + 20
    sect_off = opt_off + opt_size
    sects = []
    for i in range(n_sections):
        s = sect_off + i * 40
        name = buf[s:s+8].rstrip(b"\x00").decode("ascii", errors="replace")
        vsize, vaddr, raw_size, raw_off = struct.unpack_from(
            "<IIII", buf, s + 8)
        sects.append({
            "name": name,
            "vaddr": vaddr, "vsize": vsize,
            "raw_off": raw_off, "raw_size": raw_size,
        })
    return sects, machine


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dll", required=True,
                    help="Path to SWIttsFe-en-US.dll (or any PE)")
    ap.add_argument("--section", default=".data",
                    help="Section name to dump (default: .data)")
    ap.add_argument("--out", default="c:/tmp/fe_data_section.bin",
                    help="Output path (default: c:/tmp/fe_data_section.bin)")
    ap.add_argument("--list", action="store_true",
                    help="List sections and exit (no dump)")
    args = ap.parse_args()

    buf = Path(args.dll).read_bytes()
    sects, machine = parse_pe_sections(buf)

    print(f"PE machine: 0x{machine:04x}  ({len(sects)} sections)")
    for s in sects:
        print(f"  {s['name']:<8s}  vaddr=0x{s['vaddr']:08x}  "
              f"vsize=0x{s['vsize']:08x}  raw_off=0x{s['raw_off']:08x}  "
              f"raw_size=0x{s['raw_size']:08x}")
    if args.list:
        return 0

    target = next((s for s in sects if s["name"] == args.section), None)
    if target is None:
        print(f"section {args.section!r} not found", file=sys.stderr)
        return 2

    raw = buf[target["raw_off"] : target["raw_off"] + target["raw_size"]]
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(raw)
    print(f"\nwrote {len(raw):,} bytes to {out}")
    print(f"section: {target['name']}  vaddr=0x{target['vaddr']:08x}  "
          f"vsize={target['vsize']:,}  raw_size={target['raw_size']:,}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
