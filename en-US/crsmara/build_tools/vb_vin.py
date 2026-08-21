#!/usr/bin/env python3
"""VIN/VDB codec. Proven by byte-exact round trip before anything is built on it.

Step 1 of the build pipeline is being rewritten to emit our OWN `indx` and unit
table instead of re-skinning Tom's, so every chunk has to be written rather
than patched. A writer that is subtly wrong produces a voice that LOADS, prints
plausible statistics and renders garbage -- the exact failure already paid for
once when a Jill VDB was dropped beside Tom's VIN.

So the gate is not "does it load", it is **read Tom's VIN and write it back
byte for byte**. Anything less and the writer is not describing the format.

    python vb_vin.py                 # round-trip every voice we hold

Layout notes that cost real time to establish, kept here so they are not
re-derived:

  * Both VIN and VDB are XOR 0xCE over the WHOLE file.
  * RIFF chunks pad to even size; the pad byte is part of the file and must
    be preserved, not regenerated.
  * Unit records are fixed-stride, stride by version:
        100004:23  100005:24  100006:29  100007:25  100008:30
    Tom is 100006, Jill is 100008. Field offsets differ between them -- the
    v100007+ layout inserts `phone_in_syl` at +0x10 and shifts everything
    after it one byte later, WITHOUT changing any field's meaning.
"""
import argparse
import os
import struct
import sys
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "wayback"))

from paths import REPO  # noqa: E402

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

XOR_KEY = 0xCE

# Per-version record geometry, transcribed from spfy/src/voice/unit_table.c.
# `None` means the field is absent at that version.
UNIT_LAYOUT = {
    100004: dict(size=23, phone_in_syl=None, f0=0x10, pc=0x14, half=0x15,
                 ctx=None, flag_b=None, ccost=None),
    100005: dict(size=24, phone_in_syl=None, f0=0x10, pc=0x14, half=0x15,
                 ctx=None, flag_b=None, ccost=0x17),
    100006: dict(size=29, phone_in_syl=None, f0=0x10, pc=0x14, half=0x15,
                 ctx=0x17, flag_b=0x1B, ccost=0x1C),
    100007: dict(size=25, phone_in_syl=0x10, f0=0x11, pc=0x15, half=0x16,
                 ctx=None, flag_b=None, ccost=0x18),
    100008: dict(size=30, phone_in_syl=0x10, f0=0x11, pc=0x15, half=0x16,
                 ctx=0x18, flag_b=0x1C, ccost=0x1D),
}


def xor_bytes(b):
    return bytes(x ^ XOR_KEY for x in b)


def read_encoded(path):
    return xor_bytes(Path(path).read_bytes())


def write_encoded(path, data):
    Path(path).write_bytes(xor_bytes(data))


def split_chunks(data, start, end):
    """[(tag, payload, pad_byte)] over a RIFF chunk region.

    The pad byte is CARRIED, not assumed zero: it is part of the file and a
    regenerated zero would break byte equality on any voice that used
    something else.
    """
    out = []
    p = start
    while p + 8 <= end:
        tag = data[p:p + 4]
        size = struct.unpack_from("<I", data, p + 4)[0]
        body = data[p + 8:p + 8 + size]
        pad = b""
        if size & 1:
            pad = data[p + 8 + size:p + 9 + size]
        out.append((tag, body, pad))
        p += 8 + size + (size & 1)
    return out


def join_chunks(chunks):
    parts = []
    for tag, body, pad in chunks:
        parts.append(tag)
        parts.append(struct.pack("<I", len(body)))
        parts.append(body)
        if len(body) & 1:
            parts.append(pad if pad else b"\x00")
    return b"".join(parts)


class Riff:
    """A RIFF container preserving declared size and form type verbatim."""

    def __init__(self, data):
        assert data[:4] == b"RIFF", "not RIFF"
        self.declared = struct.unpack_from("<I", data, 4)[0]
        self.form = data[8:12]
        # The declared size may disagree with the real file length (tom.vin
        # is 8 bytes longer than it claims); trust the FILE for parsing and
        # keep the declared value for writing.
        self.chunks = split_chunks(data, 12, len(data))
        self.tail = b""

    def to_bytes(self):
        body = join_chunks(self.chunks)
        return b"RIFF" + struct.pack("<I", self.declared) + self.form + body

    def get(self, tag):
        for t, body, _ in self.chunks:
            if t == tag:
                return body
        return None

    def set(self, tag, body):
        """Replace a chunk body, fixing the RIFF header if the size changed.

        ⚠ `declared` is written back VERBATIM by to_bytes() because tom.vin
        ships a header 8 bytes short of its own file and parsing must not
        "correct" that. But a body of a DIFFERENT length makes the stale value
        an outright lie: splicing a ccos 190 bytes smaller produced
        `RIFF size 91446496 overruns file (91446314 bytes)` and the engine
        loaders refused the voice. Only recompute when the length moves, so a
        same-size edit still round-trips byte for byte.
        """
        for i, (t, old, pad) in enumerate(self.chunks):
            if t == tag:
                self.chunks[i] = (t, body, pad)
                if len(body) != len(old):
                    self.declared = 4 + len(join_chunks(self.chunks))
                return True
        return False


# Full byte accounting for a unit record. Every byte of the stride is named,
# including the two the engine never reads, so pack(unpack(x)) == x is a real
# test of the map rather than of the bytes we happened to look at. A gap here
# would silently become zeros in a written VIN.
UNIT_FIELDS = {
    100006: [("uid", 0x00, 4), ("file_idx", 0x04, 2), ("local_pos", 0x06, 2),
             ("u08", 0x08, 2), ("dur_like", 0x0A, 2),
             ("sp_syl_in_phrase", 0x0C, 1), ("sp_syl_type", 0x0D, 1),
             ("sp_word_in_phrase", 0x0E, 1), ("sp_syl_in_word", 0x0F, 1),
             ("f0_start", 0x10, 1), ("f0_end", 0x11, 1),
             ("f0_mid", 0x12, 1), ("f0_context", 0x13, 1),
             ("phone_center", 0x14, 1), ("is_first_half", 0x15, 1),
             ("voice_const", 0x16, 1), ("phone_ctx", 0x17, 4),
             ("flag_b", 0x1B, 1), ("context_cost", 0x1C, 1)],
    100008: [("uid", 0x00, 4), ("file_idx", 0x04, 2), ("local_pos", 0x06, 2),
             ("u08", 0x08, 2), ("dur_like", 0x0A, 2),
             ("sp_syl_in_phrase", 0x0C, 1), ("sp_syl_type", 0x0D, 1),
             ("sp_word_in_phrase", 0x0E, 1), ("sp_syl_in_word", 0x0F, 1),
             ("phone_in_syl", 0x10, 1),
             ("f0_start", 0x11, 1), ("f0_end", 0x12, 1),
             ("f0_mid", 0x13, 1), ("f0_context", 0x14, 1),
             ("phone_center", 0x15, 1), ("is_first_half", 0x16, 1),
             ("voice_const", 0x17, 1), ("phone_ctx", 0x18, 4),
             ("flag_b", 0x1C, 1), ("context_cost", 0x1D, 1)],
}


def check_field_map(version):
    """Refuse to use a map that does not cover the stride exactly."""
    stride = UNIT_LAYOUT[version]["size"]
    seen = bytearray(stride)
    for name, off, size in UNIT_FIELDS[version]:
        for i in range(off, off + size):
            if i >= stride:
                raise AssertionError(f"{name} runs past stride {stride}")
            if seen[i]:
                raise AssertionError(f"{name} overlaps at byte {i}")
            seen[i] = 1
    missing = [i for i, v in enumerate(seen) if not v]
    if missing:
        raise AssertionError(f"v{version}: bytes unaccounted for: {missing}")
    return True


def unpack_unit(buf, off, version):
    rec = {}
    for name, o, size in UNIT_FIELDS[version]:
        b = buf[off + o:off + o + size]
        if size == 1:
            rec[name] = b[0]
        elif size == 2:
            rec[name] = struct.unpack("<H", b)[0]
        elif size == 4 and name == "uid":
            rec[name] = struct.unpack("<I", b)[0]
        else:
            rec[name] = bytes(b)
    return rec


def pack_unit(rec, version):
    out = bytearray(UNIT_LAYOUT[version]["size"])
    for name, o, size in UNIT_FIELDS[version]:
        v = rec[name]
        if size == 1:
            out[o] = v & 0xFF
        elif size == 2:
            struct.pack_into("<H", out, o, v & 0xFFFF)
        elif size == 4 and name == "uid":
            struct.pack_into("<I", out, o, v & 0xFFFFFFFF)
        else:
            out[o:o + size] = v
    return bytes(out)


def unit_version(vin_riff):
    """The `unit` chunk wraps {vers, data}; the version selects the stride."""
    unit = vin_riff.get(b"unit")
    if unit is None:
        return None, None
    ver, data = None, None
    for tag, body, _ in split_chunks(unit, 0, len(unit)):
        if tag == b"vers":
            ver = struct.unpack_from("<I", body, 0)[0]
        elif tag == b"data":
            data = body
    return ver, data


def roundtrip(path):
    raw = read_encoded(path)
    r = Riff(raw)
    back = r.to_bytes()
    ok = back == raw
    return ok, r, raw, back


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--voice", action="append", default=None)
    a = ap.parse_args()
    voices = a.voice or ["tom", "jill", "felix", "javier", "paulina"]

    bad = 0
    for v in voices:
        vin = REPO / "en-US" / v / f"{v}.vin"
        vdb = REPO / "en-US" / v / f"{v}8.vdb"
        if not vin.is_file():
            print(f"  skip {v}: no vin")
            continue
        ok, r, raw, back = roundtrip(vin)
        ver, data = unit_version(r)
        L = UNIT_LAYOUT.get(ver)
        n_units = (len(data) // L["size"]) if (L and data) else 0
        tag = "OK " if ok else "FAIL"
        print(f"  {tag} {v+'.vin':16s} {len(raw):>10,} B  "
              f"{len(r.chunks):2d} chunks  v{ver}  stride {L['size'] if L else '?'}  "
              f"{n_units:,} units")
        if not ok:
            bad += 1
            k = next((i for i in range(min(len(raw), len(back)))
                      if raw[i] != back[i]), min(len(raw), len(back)))
            print(f"       first difference at byte {k}  "
                  f"(len {len(raw)} vs {len(back)})")
        if L and data and ver in UNIT_FIELDS:
            check_field_map(ver)
            stride = L["size"]
            miss = 0
            first_bad = None
            u08 = {}
            vconst = {}
            for uid in range(n_units):
                o = uid * stride
                rec = unpack_unit(data, o, ver)
                if pack_unit(rec, ver) != data[o:o + stride]:
                    miss += 1
                    if first_bad is None:
                        first_bad = uid
                u08[rec["u08"]] = u08.get(rec["u08"], 0) + 1
                vconst[rec["voice_const"]] = vconst.get(rec["voice_const"], 0) + 1
            print(f"       units repack {'OK' if not miss else str(miss)+' MISMATCH'}"
                  + (f" first@{first_bad}" if first_bad is not None else ""))
            top = sorted(u08.items(), key=lambda kv: -kv[1])[:3]
            print(f"       +0x08 values: "
                  + ", ".join(f"{k}x{c:,}" for k, c in top)
                  + f"   voice_const: {sorted(vconst)}")
            if miss:
                bad += 1
        if vdb.is_file():
            vraw = read_encoded(vdb)
            vr = Riff(vraw)
            vok = vr.to_bytes() == vraw
            print(f"  {'OK ' if vok else 'FAIL'} {v+'8.vdb':16s} "
                  f"{len(vraw):>10,} B  {len(vr.chunks):2d} chunks")
            if not vok:
                bad += 1
    print(f"\n{'round trip byte-exact for every voice' if not bad else str(bad)+' FAILED'}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
