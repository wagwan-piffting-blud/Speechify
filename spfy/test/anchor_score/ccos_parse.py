"""Parse Tom's ccos chunk into in-memory matrices.

Mirror of `spfy/src/voice/ccos.c`. Returns a numpy/list of shape
(n_hp_classes, 4_slots, n_labels, n_labels) of floats.
"""
import struct
import sys
import os

sys.path.insert(0, "c:/tmp")
import cklx_ckls_parse as parser  # noqa: E402

SLOT_SCALE_LEFT  = (0.2, 1.0, 0.5, 0.1)
SLOT_SCALE_RIGHT = (0.1, 0.5, 1.0, 0.2)


def parse_ccos(ccos_chunk: bytes):
    """Returns (tables, n_labels) where tables is a flat float list of
    shape n_hp_classes * 4 * n_labels * n_labels.
    """
    p = 0
    n_labels = None
    data = None
    while p + 8 <= len(ccos_chunk):
        cc = ccos_chunk[p:p+4]
        sz = struct.unpack_from("<I", ccos_chunk, p+4)[0]
        if cc == b"labl":
            sub = ccos_chunk[p+8:p+8+sz]
            n_labels = struct.unpack_from("<I", sub, 0)[0]
        elif cc == b"data":
            data = ccos_chunk[p+8:p+8+sz]
        p += 8 + sz + (sz & 1)
    assert n_labels and data is not None

    n_hp = 2 * n_labels
    triangle_n = n_labels * (n_labels - 1) // 2
    matrix_floats = n_labels * n_labels
    total = n_hp * 4 * matrix_floats
    tables = [0.0] * total

    p = 0
    for hp in range(n_hp):
        scale_row = SLOT_SCALE_LEFT if hp < n_labels else SLOT_SCALE_RIGHT
        for slot in range(4):
            v_hp, v_slot = struct.unpack_from("<II", data, p)
            p += 8
            assert v_hp == hp and v_slot == slot
            scale = scale_row[slot]
            mat_off = (hp * 4 + slot) * matrix_floats
            for i in range(1, n_labels):
                for j in range(i):
                    raw = struct.unpack_from("<f", data, p)[0]
                    p += 4
                    scaled = (raw + 0.1) * scale
                    tables[mat_off + i * n_labels + j] = scaled
                    tables[mat_off + j * n_labels + i] = scaled
            # diag = 0
    assert p == len(data), f"ccos data: p={p} expected {len(data)}"
    return tables, n_labels


def ccos_cell(tables, n_labels, hp, slot, row, col):
    matrix_floats = n_labels * n_labels
    return tables[(hp * 4 + slot) * matrix_floats + row * n_labels + col]


if __name__ == "__main__":
    with open(os.path.expanduser("~/Documents/Speechify/en-US/tom/tom.vin"), "rb") as f:
        raw = parser.deobfuscate(f.read())
    ccos_chunk = parser.find_chunk(raw, b"ccos")
    tables, n_labels = parse_ccos(ccos_chunk)
    print(f"loaded ccos: n_labels={n_labels}, "
          f"n_floats={len(tables)}")
    # Sample cells
    print(f"  cell(hp=32, slot=0, row=0, col=0) = "
          f"{ccos_cell(tables, n_labels, 32, 0, 0, 0):.4f}  (diag=0)")
    print(f"  cell(hp=32, slot=0, row=0, col=10) = "
          f"{ccos_cell(tables, n_labels, 32, 0, 0, 10):.4f}")
