#!/usr/bin/env python3
"""Pad a VIN's `hash` cell array so the vendor engine's unbounded probe stays
inside it.

⛔ WHY THIS TOOL EXISTS. SWIttsUSel.dll+0xb7e6 is

    cmp [cells + (rows[uid_right] + uid_left)*8], uid_right

and there is no comparison against n_cells in front of it -- the key check IS
the bounds check, and it happens after the read. Our own `spfy_hash_lookup`
guards the index (`if (idx >= n_cells) return SPFY_E_OOB`), so a table sized to
its last POPULATED cell renders perfectly in spfy_synth and access-violates in
Speechify on the first phrase that pairs the widest row with a high uid_left.

Measured 2026-08-22: crstom died on "attention signal." --
rows[222144] = 4,449,427 plus uid_left 278,391 = 4,727,818 against n_cells
4,724,617, reading 25,616 bytes past a 37,797,888-byte allocation. crsmara
carried the same defect (6,506 cells short) and had simply not been asked yet.

The correct width is not a guess: every vendor voice satisfies
n_cells == max(rows[]) + n_rows to the cell -- tom 1,724,291+692,190,
jill 2,059,585+560,534, javier 1,638,488+668,348, paulina 1,367,589+663,410,
felix 2,906,700+737,394, all delta +0.

`spfy_hash_build` now pads at build time and `spfy_vb_verify` gates it, so this
tool is only for repairing a voice already built -- it appends empty cells,
which cannot change any lookup result, so a padded VIN is what a rebuild would
have produced.

    py vb_hashpad.py --vin en-US/crstom/crstom.vin            # report only
    py vb_hashpad.py --vin en-US/crstom/crstom.vin --write    # repair in place

Layout (spfy/src/usel/hash.c):
    head : u32 n_rows, u32 n_cells
    rows : n_rows * u32
    cell : n_cells * u32 validators  THEN  n_cells * f32 costs
"""
from __future__ import annotations

import argparse
import os
import shutil
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vb_vin as V  # noqa: E402

EMPTY_KEY = 0xFFFFFFFF
EMPTY_COST = -1.0


def pad_hash(hash_blob):
    """-> (new_hash_blob, n_rows, n_cells, max_row, added). added == 0 when
    the table is already wide enough, and the blob comes back unchanged.

    The target is n_cells == max(rows[]) + n_rows, which is not a chosen
    margin: every vendor voice hits it exactly (tom, jill, javier, paulina,
    felix, delta +0). uid_left is a unit id and n_rows >= n_units always, so
    it covers every probe the engine can make."""
    subs = V.split_chunks(hash_blob, 0, len(hash_blob))
    head = rows = cell = None
    for i, (tag, body, _) in enumerate(subs):
        if tag == b"head":
            head = i
        elif tag == b"rows":
            rows = i
        elif tag == b"cell":
            cell = i
    if head is None or rows is None or cell is None:
        raise SystemExit("hash chunk is missing head/rows/cell")

    n_rows, n_cells = struct.unpack_from("<II", subs[head][1], 0)
    row = np.frombuffer(subs[rows][1], dtype="<u4", count=n_rows)
    max_row = int(row.max())

    want = max_row + n_rows
    if want <= n_cells:
        return hash_blob, n_rows, n_cells, max_row, 0

    add = want - n_cells
    cells = subs[cell][1]
    if len(cells) != n_cells * 8:
        raise SystemExit(f"cell sub-chunk is {len(cells)} B, "
                         f"expected {n_cells * 8}")
    a = cells[:n_cells * 4]
    b = cells[n_cells * 4:]
    pad_a = np.full(add, EMPTY_KEY, dtype="<u4").tobytes()
    pad_b = np.full(add, EMPTY_COST, dtype="<f4").tobytes()

    subs[head] = (b"head",
                  struct.pack("<II", n_rows, n_cells + add) +
                  subs[head][1][8:],
                  subs[head][2])
    subs[cell] = (b"cell", a + pad_a + b + pad_b, subs[cell][2])
    return V.join_chunks(subs), n_rows, n_cells, max_row, add


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vin", required=True)
    ap.add_argument("--write", action="store_true",
                    help="repair in place (a .bak copy is made first)")
    a = ap.parse_args()

    raw = V.read_encoded(a.vin)
    r = V.Riff(raw)
    cnts = r.get(b"cnts")
    n_units = struct.unpack_from("<III", cnts, 0)[2] if cnts else 0
    hash_blob = r.get(b"hash")
    if hash_blob is None:
        raise SystemExit("no hash chunk")

    new, n_rows, n_cells, max_row, add = pad_hash(hash_blob)
    worst = max_row + n_rows
    print(f"{os.path.basename(a.vin)}")
    print(f"  units        {n_units}")
    print(f"  hash rows    {n_rows}   cells {n_cells}")
    print(f"  max(rows)    {max_row}")
    print(f"  worst probe  {worst}   headroom {n_cells - worst:+d}")
    if not add:
        print("  already wide enough -- nothing to do")
        return 0
    print(f"  ⛔ short by {add} cells ({add * 8} bytes)")
    if not a.write:
        print("  (report only; pass --write to repair)")
        return 1

    shutil.copy2(a.vin, a.vin + ".bak")
    r.set(b"hash", new)
    V.write_encoded(a.vin, r.to_bytes())
    print(f"  padded to {n_cells + add} cells; "
          f"{os.path.getsize(a.vin)} B on disk (.bak kept)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
