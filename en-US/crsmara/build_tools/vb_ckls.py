#!/usr/bin/env python3
"""Decode `ckls` / `cklx` -- the whole-word and whole-syllable anchor index.

These are not decoration. `anchor_score.c` reads `span_start`/`span_end` per
posting to offer WHOLE recorded words and syllables as candidates, so a corpus
that says "cloudy" a thousand times can use a real recorded "cloudy" instead of
concatenating eight half-phones. For a weather corpus that is most of the
quality argument.

⚠ Their mere PRESENCE also changes selection: `voice[0x94]` is the ckls entry
count, and with ckls the ccos cost is applied to every cross-recording
transition and never to same-recording ones. Shipping an empty one is not a
neutral choice.

Layout (reveng/README_TECHNICAL.md, confirmed there against Tom):

    cklx: u32 n_groups(2), then per group
          u16 name_len, name, u32 n_entries,
          n_entries * { u16 key_len, key, u32 n_post, u32 postings[] }

    ckls: u32 n_groups(2), then per group
          u16 name_len, name, u32 n_records,
          then n_records * { u32 seq_index, u16 len, token,
                             u32 span_start, u32 span_end, u16 len, filename }

⛔ The record STARTS with its sequence index. Earlier notes here framed that
leading word as a per-group `unk0` and the trailing word as the record's own
`file_id`; both are the same field seen off by one, because record i's index is
written before it and record i+1's after it. The difference only shows on an
EMPTY group, which has no record 0 and therefore no leading word at all -- felix
ships an empty `_WORD_` group, and the old framing consumed 4 bytes that were
not there and then raised on the whole chunk. Every voice now parses to the
exact byte.

`span_start` / `span_end` are GLOBAL unit indices; the range is inclusive and
holds exactly 2*n_phones units from one recording.

    python vb_ckls.py --voice tom --dump 8
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "wayback"))
sys.path.insert(0, HERE)

from paths import REPO  # noqa: E402
import vb_vin as V  # noqa: E402

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass


def _str(b, p):
    n = struct.unpack_from("<H", b, p)[0]
    return b[p + 2:p + 2 + n].decode("latin-1"), p + 2 + n


def decode_cklx(b):
    """[(group_name, {key: [posting_ids]})]"""
    n_groups = struct.unpack_from("<I", b, 0)[0]
    p = 4
    out = []
    for _ in range(n_groups):
        name, p = _str(b, p)
        n_entries = struct.unpack_from("<I", b, p)[0]
        p += 4
        d = {}
        for _ in range(n_entries):
            key, p = _str(b, p)
            n_post = struct.unpack_from("<I", b, p)[0]
            p += 4
            d[key] = list(struct.unpack_from("<%dI" % n_post, b, p))
            p += 4 * n_post
        out.append((name, d))
    return out


def decode_ckls(b):
    """[(group_name, [(token, span_start, span_end, filename)])]"""
    n_groups = struct.unpack_from("<I", b, 0)[0]
    p = 4
    out = []
    for _ in range(n_groups):
        name, p = _str(b, p)
        count = struct.unpack_from("<I", b, p)[0]
        p += 4
        # Record 0's sequence index. An EMPTY group has no record 0 and so no
        # such word -- consuming it unconditionally is what broke felix.
        if count:
            p += 4
        recs = []
        for i in range(count):
            tok, p = _str(b, p)
            ss, se = struct.unpack_from("<II", b, p)
            p += 8
            fn, p = _str(b, p)
            # ...and the NEXT record's index, so the last record has none.
            if i < count - 1:
                p += 4
            recs.append((tok, ss, se, fn))
        out.append((name, recs))
    return out


def _pstr(s):
    b = s.encode("latin-1", "replace")
    return struct.pack("<H", len(b)) + b


def encode_cklx(groups):
    """[(name, {key: [posting_ids]})] -> chunk bytes. Keys sorted."""
    out = [struct.pack("<I", len(groups))]
    for name, d in groups:
        out.append(_pstr(name) + struct.pack("<I", len(d)))
        for key in sorted(d):
            ids = d[key]
            out.append(_pstr(key) + struct.pack("<I", len(ids)))
            out.append(struct.pack("<%dI" % len(ids), *ids))
    return b"".join(out)


def encode_ckls(groups):
    """[(name, [(token, span_start, span_end, filename)])] -> chunk bytes.

    Each record carries its own sequence index, written just before it; the
    last record has no trailing word because there is no record after it. An
    EMPTY group therefore emits the count and nothing else -- felix ships one,
    and writing a leading zero anyway desynchronises every reader.
    """
    out = [struct.pack("<I", len(groups))]
    for name, recs in groups:
        out.append(_pstr(name) + struct.pack("<I", len(recs)))
        if recs:
            out.append(struct.pack("<I", 0))
        for i, (tok, ss, se, fn) in enumerate(recs):
            out.append(_pstr(tok) + struct.pack("<II", ss, se))
            out.append(_pstr(fn))
            if i < len(recs) - 1:
                out.append(struct.pack("<I", i + 1))
    return b"".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--voice", default="tom")
    ap.add_argument("--dump", type=int, default=6)
    a = ap.parse_args()

    # felix is fr-CA and javier/paulina es-MX, and felix is the ONLY voice that
    # exercises the empty-group path -- so this must not be hardwired to en-US.
    vin = None
    for lang in ("en-US", "fr-CA", "es-MX", "en-GB", "en-AU"):
        cand = REPO / lang / a.voice / f"{a.voice}.vin"
        if cand.exists():
            vin = cand
            break
    if vin is None:
        print(f"no .vin for {a.voice!r} under {REPO}")
        return 1
    r = V.Riff(V.read_encoded(vin))
    ver, udata = V.unit_version(r)
    lay = V.UNIT_LAYOUT[ver]
    n_units = len(udata) // lay["size"]

    cx = decode_cklx(r.get(b"cklx"))
    cs = decode_ckls(r.get(b"ckls"))
    print(f"{a.voice}: {n_units:,} units")
    for (gn, d), (gn2, recs) in zip(cx, cs):
        tot = sum(len(v) for v in d.values())
        print(f"\n  cklx {gn}: {len(d):,} keys, {tot:,} postings"
              f"   ckls {gn2}: {len(recs):,} records")
        for tok, ss, se, fn in recs[:a.dump]:
            delta = se - ss
            fh_s = udata[ss * lay["size"] + lay["half"]] if ss < n_units else -1
            fh_e = udata[se * lay["size"] + lay["half"]] if se < n_units else -1
            print(f"    {tok!r:22s} span {ss:>7,}..{se:<7,} "
                  f"(delta {delta:2d}, {(delta+1)//2} phones)  "
                  f"first_half {fh_s}/{fh_e}  file {fn!r}")
        # The invariant the README claims: starts on an L unit, ends on an R.
        bad_s = sum(1 for _t, ss, _se, _f in recs
                    if ss < n_units and
                    udata[ss * lay["size"] + lay["half"]] != 1)
        bad_e = sum(1 for _t, _ss, se, _f in recs
                    if se < n_units and
                    udata[se * lay["size"] + lay["half"]] != 0)
        odd = sum(1 for _t, ss, se, _f in recs if (se - ss) % 2 == 0)
        print(f"    invariants: span_start not first_half {bad_s}, "
              f"span_end not second_half {bad_e}, even delta {odd}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
