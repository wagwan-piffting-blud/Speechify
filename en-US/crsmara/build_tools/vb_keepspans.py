#!/usr/bin/env python3
"""Emit the `--compress` keep-span list: the audio the inventory actually needs.

Pairs with `spfy_vb_build --compress`. Each line is

    stem<TAB>lo_ms<TAB>hi_ms

in RECORDING-relative milliseconds. Times, not uids: a uid is an artefact of the
build that is about to change, a time is the same in every build of the same
corpus, so one list survives a rebuild.

A unit is kept if the DP ever picked it (`vb_pickmap.py`) OR it is needed to hold
its preselection group at `--floor`. Picks decide WHICH candidates survive; the
floor decides that the CONTEXT survives at all, which is what leaves a pool for
text the sample never reached.

⚠ THE TRAILING MARGIN IS LOAD-BEARING. WSOLA reads past a unit's last sample;
`vb_compact --pad-ms 8` instead of 20 changed all three demo renders. The pad is
baked in here because the builder emits exactly what this file says.

⭐ `--keep-words` IS THE EXCEPTION LIST, and it exists because the sample is not
the vocabulary. `tornado` has 13 anchors in this corpus and 1,588 held-out Brown
lines touch NONE of them, so the cut left the word as loose half-phones and it
came out rough while everything the sample did reach was fine. Naming a token
keeps every unit of every anchor for it, picked or not -- 214 units, 0.09 MB for
`tornado`. Use it for the words the voice EXISTS to say.

⚠ CHUNK BASES COME FROM THE indx OFFSETS. A recording's chunks are written back
to back, so chunk_base_ms = (offs[chunk] - offs[first chunk of stem]) / 8. That
holds for an UNCOMPRESSED source voice; deriving spans from an already
compressed one would compound the rebasing.

    py vb_keepspans.py --voice C:\\tmp\\crsmara_pk1618 \\
        --picks C:\\tmp\\picks_heldout.json --floor 8 \\
        --out C:\\tmp\\keep8.tsv
"""
import argparse
import json
import os
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "wayback"))
sys.path.insert(0, HERE)

import vb_vin as V      # noqa: E402
import vb_listen as L   # noqa: E402
import vb_prsl as P     # noqa: E402
import vb_ckls as CK    # noqa: E402

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

BPMS = 8


def resolve(spec):
    if not L.VOICES:
        L.VOICES = L.discover()
    d = L.VOICES.get(spec)
    if d is not None:
        return Path(d), spec
    p = Path(spec)
    vins = sorted(p.glob("*.vin"))
    if not vins:
        raise SystemExit(f"{spec}: not a registry voice and no .vin inside")
    return p, vins[0].stem


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--voice", required=True)
    ap.add_argument("--picks", required=True)
    ap.add_argument("--floor", type=int, default=8,
                    help="candidates kept for a context the demand sample "
                         "actually visits")
    ap.add_argument("--demand", default=None,
                    help="JSON {prsl key: request count} from vb_demandmap.py. "
                         "Without it every context gets --floor, which spends "
                         "the budget on contexts speech never reaches")
    ap.add_argument("--cold-floor", type=int, default=3,
                    help="floor for a context the demand sample never visited. "
                         "⚠ unseen is not never -- this is a risk dial")
    ap.add_argument("--pad-ms", type=int, default=20,
                    help="trailing margin for WSOLA; 8 was measured to be too "
                         "little")
    ap.add_argument("--lead-ms", type=int, default=5)
    ap.add_argument("--merge-gap", type=int, default=40,
                    help="join two kept spans closer than this; the bytes "
                         "saved by splitting are not worth the extra seam")
    ap.add_argument("--no-whole-words", action="store_true",
                    help="do not round kept spans out to whole ckls _WORD_ / "
                         "_SYL_ anchors. ⚠ the builder drops any anchor "
                         "containing a withheld unit, so without this _WORD_ "
                         "collapses (39,951 -> 8,624 measured) and words stop "
                         "being taken whole")
    ap.add_argument("--keep-words", default=None,
                    help="comma list, or a path to a file of one token per "
                         "line. Every unit of every _WORD_/_SYL_ anchor for "
                         "these tokens is kept whether or not the demand "
                         "sample picked it, so the word stays available WHOLE "
                         "instead of being spliced out of half-phones. "
                         "Matched case-insensitively against the ckls token")
    ap.add_argument("--rvc-prefix", default="st2")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    keep_words = set()
    if a.keep_words:
        src = Path(a.keep_words)
        text = src.read_text(encoding="utf-8") if src.exists() else a.keep_words
        # ⚠ Comments go BEFORE the comma split, not after: a `#` line that
        # contains a comma otherwise breaks into fragments that no longer
        # start with `#` and get taken for tokens.
        body = "\n".join(l for l in text.splitlines()
                         if not l.lstrip().startswith("#"))
        keep_words = {w.strip().lower()
                      for w in body.replace(",", "\n").splitlines() if w.strip()}

    d, stem = resolve(a.voice)
    vin = V.Riff(V.read_encoded(d / f"{stem}.vin"))
    ver, ud = V.unit_version(vin)
    stride = V.UNIT_LAYOUT[ver]["size"]
    n = len(ud) // stride
    raw = np.frombuffer(ud, dtype=np.uint8, count=n * stride).reshape(n, stride)
    off = dict((f[0], f[1]) for f in V.UNIT_FIELDS[ver])

    def u16(nm):
        o = off[nm]
        return (raw[:, o].astype(np.int64) | (raw[:, o + 1].astype(np.int64) << 8))

    fi, lp, dur = u16("file_idx"), u16("local_pos"), u16("dur_like")

    vdb = V.Riff(V.read_encoded(d / f"{stem}8.vdb"))
    ix = vdb.get(b"indx")
    cnt = struct.unpack_from("<I", ix, 0)[0]
    p, offs, names = 4, [], []
    for _ in range(cnt):
        offs.append(struct.unpack_from("<I", ix, p)[0])
        p += 4
        nl = struct.unpack_from("<H", ix, p)[0]
        p += 2
        names.append(ix[p:p + nl].decode("latin-1", "replace"))
        p += nl
    data_n = len(vdb.get(b"data"))

    # chunk -> (stem, base_ms within that stem), and the stem's total length
    first_off, rec_len = {}, {}
    for i, nm in enumerate(names):
        if not nm:
            continue
        s = nm.split("~")[0]
        if s not in first_off:
            first_off[s] = offs[i]
        end = offs[i + 1] if i + 1 < len(offs) else data_n
        rec_len[s] = max(rec_len.get(s, 0), (end - first_off[s]) // BPMS)
    base_ms = np.zeros(len(names), dtype=np.int64)
    stem_of = []
    for i, nm in enumerate(names):
        s = nm.split("~")[0] if nm else ""
        stem_of.append(s)
        if s:
            base_ms[i] = (offs[i] - first_off[s]) // BPMS

    picks = json.load(open(a.picks, encoding="utf-8"))
    keep = np.zeros(n, dtype=bool)
    for u in picks:
        u = int(u)
        if 0 <= u < n:
            keep[u] = True
    n_pick = int(keep.sum())

    cb = vin.get(b"ckls")
    ckls = CK.decode_ckls(cb) if cb else []

    # ⭐ NAMED WORDS SURVIVE WHOLE, PICKED OR NOT. Rounding below only protects
    # a word the demand sample happened to ask for, and the sample is general
    # prose: `tornado` has 13 anchors in this corpus and the Brown lines touch
    # NONE of them, so the cut left it as loose half-phones to splice. A word
    # the voice exists to say is not something to leave to a sample -- this
    # keeps every unit of every anchor for the listed tokens, which is what
    # keeps the _WORD_ record alive for anchor_score.c to offer whole.
    if keep_words:
        kept_tok, kw_units = Counter(), 0
        for name, recs in ckls:
            if name not in ("_WORD_", "_SYL_"):
                continue
            for tok, ss, se, _fn in recs:
                t = str(tok).strip().lower()
                if t not in keep_words:
                    continue
                ss, se = int(ss), min(int(se), n - 1)
                if se < ss or ss >= n:
                    continue
                kw_units += int((~keep[ss:se + 1]).sum())
                keep[ss:se + 1] = True
                kept_tok[t] += 1
        miss = sorted(keep_words - set(kept_tok))
        print(f"  --keep-words: {sum(kept_tok.values())} anchor(s) over "
              f"{len(kept_tok)} token(s), +{kw_units:,} units"
              + (f"   ⚠ NO ANCHOR: {', '.join(miss)}" if miss else ""))

    # ⭐ ROUND THE PICKS OUT TO WHOLE WORDS, NOT THE FLOOR TOP-UP. The builder
    # drops any ckls anchor containing a withheld unit, so a word cut in half
    # stops being available whole -- _WORD_ 39,951 -> 8,624 without this. But
    # rounding the FLOOR units too costs 146,657 extra units and 139.0 MiB: a
    # top-up unit is there to be a half-phone candidate, not a word.
    if not a.no_whole_words:
        grown = 0
        for name, recs in ckls:
            if name not in ("_WORD_", "_SYL_"):
                continue
            for _tok, ss, se, _fn in recs:
                ss, se = int(ss), int(se)
                if se < ss or ss >= n:
                    continue
                se = min(se, n - 1)
                if keep[ss:se + 1].any() and not keep[ss:se + 1].all():
                    grown += int((~keep[ss:se + 1]).sum())
                    keep[ss:se + 1] = True
        print(f"  whole-word rounding added {grown:,} units "
              f"-> KEEP {int(keep.sum()):,} ({100.0 * keep.mean():.1f}%)")

    synth = np.array([stem_of[int(f)].startswith((a.rvc_prefix, "rvc_"))
                      if int(f) < len(stem_of) else False for f in fi])
    prsl = P.decode_prsl(vin.get(b"prsl")) if vin.get(b"prsl") else []
    demand = None
    if a.demand:
        demand = {int(k): int(v) for k, v in
                  json.load(open(a.demand, encoding="utf-8")).items()}
        vis = sum(1 for k, _c in prsl if demand.get(int(k), 0))
        print(f"  demand: {vis:,} of {len(prsl):,} stored groups visited "
              f"({100.0 * vis / max(len(prsl), 1):.1f}%) -> floor {a.floor}; "
              f"the rest -> {a.cold_floor}")
    added = 0
    for _key, cands in prsl:
        cs = [int(u) for u in cands if 0 <= int(u) < n]
        fl = a.floor
        if demand is not None and demand.get(int(_key), 0) == 0:
            fl = a.cold_floor
        have = sum(1 for u in cs if keep[u])
        if have >= fl:
            continue
        for u in sorted((u for u in cs if not keep[u]),
                        key=lambda z: (1 if synth[z] else 0, z)):
            keep[u] = True
            added += 1
            have += 1
            if have >= fl:
                break
    print(f"{stem}: {n:,} units   picked {n_pick:,}   +floor {a.floor} "
          f"{added:,}   KEEP {int(keep.sum()):,} "
          f"({100.0 * keep.mean():.1f}%)")

    spans = defaultdict(list)
    for u in np.flatnonzero(keep):
        c = int(fi[u])
        if c >= len(stem_of) or not stem_of[c]:
            continue
        s = stem_of[c]
        t0 = int(base_ms[c]) + int(lp[u]) - a.lead_ms
        t1 = int(base_ms[c]) + int(lp[u]) + int(dur[u]) + a.pad_ms
        spans[s].append((max(0, t0), min(t1, rec_len.get(s, t1))))

    total_ms, out_n = 0, 0
    with open(a.out, "w", encoding="utf-8") as f:
        for s in sorted(spans):
            v = sorted(spans[s])
            merged = [list(v[0])]
            for lo, hi in v[1:]:
                if lo - merged[-1][1] <= a.merge_gap:
                    merged[-1][1] = max(merged[-1][1], hi)
                else:
                    merged.append([lo, hi])
            for lo, hi in merged:
                if hi > lo:
                    f.write(f"{s}\t{lo}\t{hi}\n")
                    total_ms += hi - lo
                    out_n += 1
    print(f"  {out_n:,} span(s) over {len(spans):,} recordings, "
          f"{total_ms / 1000.0:,.0f}s = {total_ms * BPMS / 1e6:.1f} MB "
          f"({total_ms * BPMS / 1048576:.1f} MiB)")
    print(f"  wrote {a.out}")
    print(f"  build with: spfy_vb_build ... --compress {a.out}")


if __name__ == "__main__":
    main()
