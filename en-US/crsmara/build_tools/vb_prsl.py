#!/usr/bin/env python3
"""Decode `prsl` and read the hp_class mapping out of it, rather than guessing.

`context_key = left_hp*10000 + center_hp*100 + right_hp`, so every group in a
shipped voice states the CENTER half-phone class of its candidates. The
candidates are unit ids. Cross-referencing the two therefore answers, from
data alone, the question that stalled the `mean` work:

    what is a unit's hp_class, given its record?

Three hypotheses are scored against every group:

    A   pc*2 + is_first_half
    B   pc*2 + (1 - is_first_half)          (the engine's "_inv" comment)
    C   pc*2 + (uid & 1)                     (position, not a field)

A hypothesis that is right will agree on essentially 100% of candidates. This
cannot come out flat: the three disagree on most units, since `is_first_half`
is 1 for only ~20% of them.

    python vb_prsl.py --voice tom
"""
import argparse
import os
import struct
import sys
from collections import Counter

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


def decode_prsl(blob):
    """[(context_key, [uid, ...])] in file order."""
    n_groups = struct.unpack_from("<I", blob, 0)[0]
    p = 4
    out = []
    for _ in range(n_groups):
        n, key = struct.unpack_from("<II", blob, p)
        p += 8
        n_cand = n - 1
        cands = struct.unpack_from("<%dI" % n_cand, blob, p) if n_cand else ()
        p += n_cand * 4
        out.append((key, cands))
    return out


def with_fallbacks(buckets, hp_bound=92):
    """Add the WIDE preselection groups every vendor voice ships.

    ⚠ WITHOUT THESE A VOICE HAS NO GRACEFUL DEGRADATION AT ALL, and the real
    3.0.5 will not render it: preselection returns empty and the server raises
    7059 "no valid units found at index" and produces no audio. donnart had
    11,612 groups, ALL exact, against tom's 76,676 -- and 0 wide groups against
    tom's 3,594. The engine's chain (spfy_synth.c:3697) is

        exact       (l, c, r)
        one-sided   (l, c, 92) for a LEFT half, (92, c, r) for a RIGHT half
        centre only (92, c, 92)

    and `spfy_prsl_lookup` is a plain binary search for an exact key, so the
    wide groups are not synthesised at lookup time -- they must be IN the
    chunk.

    THE RULE IS DERIVED FROM tom AND jill, NOT GUESSED. On both voices every
    shipped wide group is exactly the UNION of the exact groups it
    generalises: 1750/1750 wide-right, 1754/1754 wide-left and 90/90 centre
    matched with nothing capped and nothing extra.

    ⚠ (92, c, 92) IS NOT "every unit of class c". It is the union of the units
    that appear in some EXACT group, which on tom is 89 of 90 classes short of
    the class total (class 64: 575 against 2,080 units). A unit no exact
    context ever admits stays unreachable, and the vendors keep it that way.

    Side parity follows from which key the engine asks for: wide-right keys
    carry an EVEN centre (left halves) and wide-left an ODD one, which is
    exactly what both vendors show.

    ⚠ ORDER IS NOT SORTED. A wide group lists its uids in the order the exact
    groups are visited by ASCENDING KEY, first occurrence winning -- dict as an
    ordered set. Sorting instead gets every key and every candidate SET right
    and still leaves 3,386 of tom's 3,594 groups in the wrong order, so the
    chunk comes out the right size and the wrong bytes.
    """
    wide = {}

    def add(k, uids):
        s = wide.get(k)
        if s is None:
            wide[k] = s = {}
        for u in uids:
            if u not in s:
                s[u] = None

    for key in sorted(buckets):
        uids = buckets[key]
        l, c, r = key // 10000, (key // 100) % 100, key % 100
        if l >= hp_bound or r >= hp_bound:
            continue            # already wide; folding it back in double-counts
        if c & 1:
            add(hp_bound * 10000 + c * 100 + r, uids)
        else:
            add(l * 10000 + c * 100 + hp_bound, uids)
        add(hp_bound * 10000 + c * 100 + hp_bound, uids)

    out = {k: list(v) for k, v in buckets.items()}
    for k, v in wide.items():
        # An exact group can legitimately collide with a wide key when a real
        # context uses hp_bound as a sentinel; the exact one wins.
        if k not in out:
            out[k] = list(v)
    return sorted(out.items())


def encode_prsl(groups):
    """Inverse of decode_prsl. Keys MUST be sorted strictly ascending."""
    parts = [struct.pack("<I", len(groups))]
    for key, cands in groups:
        parts.append(struct.pack("<II", len(cands) + 1, key))
        if cands:
            parts.append(struct.pack("<%dI" % len(cands), *cands))
    return b"".join(parts)


def labl_to_feat(vin_riff):
    """ccos labl index -> feat phone id, matched by NAME.

    `ccos` carries its own label list and the unit record's `phone_center`
    indexes THAT, while every hp_class in prsl/mean is in feat order. The two
    are permutations of the same 46 names.
    """
    ccos = vin_riff.get(b"ccos")
    labl = None
    for tag, body, _ in V.split_chunks(ccos, 0, len(ccos)):
        if tag == b"labl":
            labl = body
            break
    if labl is None:
        return {}
    n = struct.unpack_from("<I", labl, 0)[0]
    p = 4
    names = []
    for _ in range(n):
        ln = struct.unpack_from("<H", labl, p)[0]
        p += 2
        names.append(labl[p:p + ln].decode("latin-1"))
        p += ln

    feat = vin_riff.get(b"feat")
    q = 0
    feat_names = []
    while q < len(feat):
        klen = struct.unpack_from("<H", feat, q)[0]
        q += 2
        key = feat[q:q + klen].decode("latin-1")
        q += klen
        cnt = struct.unpack_from("<I", feat, q)[0]
        q += 4
        vals = []
        for _ in range(cnt):
            nl = struct.unpack_from("<H", feat, q)[0]
            q += 2
            vals.append(feat[q:q + nl].decode("latin-1"))
            q += nl + 4
        if key == "name":
            feat_names = vals
    # feat['name'] is 92 half-phone names (aa1, aa2, ...); phone id is i//2.
    fid = {}
    for i, nm in enumerate(feat_names):
        if nm.endswith("1"):
            fid[nm[:-1]] = i // 2
    return {i: fid[nm] for i, nm in enumerate(names) if nm in fid}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--voice", action="append", default=None)
    ap.add_argument("--sample", type=int, default=4000)
    a = ap.parse_args()

    for v in a.voice or ["tom", "jill"]:
        vin_p = REPO / "en-US" / v / f"{v}.vin"
        if not vin_p.is_file():
            continue
        r = V.Riff(V.read_encoded(vin_p))
        ver, udata = V.unit_version(r)
        stride = V.UNIT_LAYOUT[ver]["size"]
        n_units = len(udata) // stride
        groups = decode_prsl(r.get(b"prsl"))

        keys = [k for k, _ in groups]
        print(f"\n{v}: {len(groups):,} prsl groups, "
              f"{sum(len(c) for _k, c in groups):,} candidate slots")
        print(f"  keys {'ascend' if all(b > a_ for a_, b in zip(keys, keys[1:])) else 'NOT sorted'}"
              f"   first {keys[0]}  last {keys[-1]}")
        # Round trip the chunk to prove the writer matches the reader.
        print(f"  re-encode byte-identical: "
              f"{encode_prsl(groups) == r.get(b'prsl')}")

        centers = Counter((k // 100) % 100 for k in keys)
        lefts = Counter(k // 10000 for k in keys)
        rights = Counter(k % 100 for k in keys)
        print(f"  center values seen: {len(centers)} distinct, "
              f"range {min(centers)}..{max(centers)}")
        print(f"  left  values seen: {len(lefts)} range {min(lefts)}..{max(lefts)}"
              f"   right: {len(rights)} range {min(rights)}..{max(rights)}")

        # Hypothesis E: units are emitted lp-ascending within a recording, two
        # per phone, so the half bit is the unit's INDEX PARITY INSIDE ITS
        # RECORDING -- which is not global uid parity, because recordings have
        # odd unit counts.
        # ⚠ `phone_center` is a LABL index (ccos order); prsl's centre is in
        # FEAT order. They coincide for most phones, which is why a naive
        # comparison scores a misleading 91.5% rather than failing outright.
        # Tom's labl list is non-alphabetical: a d/dh/dx 3-cycle and an en/er
        # swap.
        l2f = labl_to_feat(r)
        rank = [0] * n_units
        seen_file = {}
        for uid in range(n_units):
            fi = struct.unpack_from("<H", udata, uid * stride + 4)[0]
            rank[uid] = seen_file.get(fi, 0)
            seen_file[fi] = rank[uid] + 1

        rec_cache = {}

        def rec(uid):
            if uid not in rec_cache:
                rec_cache[uid] = (V.unpack_unit(udata, uid * stride, ver)
                                  if uid < n_units else None)
            return rec_cache[uid]

        hit = Counter()
        tot = 0
        for key, cands in groups[:a.sample]:
            centre = (key // 100) % 100
            for uid in cands:
                u = rec(uid)
                if u is None:
                    hit["uid out of range"] += 1
                    continue
                tot += 1
                pc, fh = l2f.get(u["phone_center"], u["phone_center"]), \
                    u["is_first_half"]
                if pc * 2 + fh == centre:
                    hit["A pc*2+is_first_half"] += 1
                if pc * 2 + (1 - fh) == centre:
                    hit["B pc*2+(1-is_first_half)"] += 1
                if pc * 2 + (uid & 1) == centre:
                    hit["C pc*2+(uid&1)"] += 1
                if pc == centre:
                    hit["D pc alone"] += 1
                if pc * 2 + (rank[uid] & 1) == centre:
                    hit["E pc*2+rank_in_rec&1"] += 1
                if centre // 2 == pc:
                    hit["centre//2 == pc"] += 1
        # Is a unit half-TYPED at all? If a uid appears under both pc*2 and
        # pc*2+1 then prsl candidacy is not a property of the unit's half, and
        # "which half is this unit" is simply the wrong question -- which
        # would explain every hypothesis above landing on a coin flip.
        where = {}
        for key, cands in groups:
            centre = (key // 100) % 100
            for uid in cands:
                where.setdefault(uid, set()).add(centre & 1)
        both = sum(1 for s in where.values() if len(s) > 1)
        print(f"  units appearing under BOTH halves: {both:,} of "
              f"{len(where):,} ({100.0*both/max(len(where),1):.1f}%)")

        # prsl states each unit's TRUE half. Rather than guess which field
        # carries it, score every field in the record for agreement with that
        # truth. A field that IS the half bit scores ~100% or ~0% (inverted);
        # anything near 50% is unrelated.
        truth = {uid: (list(s)[0]) for uid, s in where.items() if len(s) == 1}
        cand_fields = ["is_first_half", "flag_b", "context_cost", "u08",
                       "sp_syl_in_phrase", "sp_syl_type",
                       "sp_word_in_phrase", "sp_syl_in_word",
                       "f0_start", "f0_end", "f0_mid", "f0_context",
                       "local_pos", "dur_like", "file_idx"]
        score = {f: 0 for f in cand_fields}
        score["uid&1"] = 0
        score["ctx0&1"] = 0
        n_t = 0
        for uid, half in truth.items():
            u = rec(uid)
            if u is None:
                continue
            n_t += 1
            for f in cand_fields:
                if (u[f] & 1) == half:
                    score[f] += 1
            if (uid & 1) == half:
                score["uid&1"] += 1
            if (u["phone_ctx"][0] & 1) == half:
                score["ctx0&1"] += 1
        print(f"  field-vs-half agreement over {n_t:,} units "
              f"(100% or 0% = that IS the bit):")
        for f, c in sorted(score.items(), key=lambda kv: -abs(kv[1] - n_t / 2)):
            print(f"    {f:20s} {100.0*c/max(n_t,1):6.2f}%")

        print(f"  over {tot:,} candidates from {min(a.sample,len(groups)):,} groups:")
        for k in ("centre//2 == pc", "A pc*2+is_first_half",
                  "B pc*2+(1-is_first_half)", "C pc*2+(uid&1)",
                  "E pc*2+rank_in_rec&1", "D pc alone", "uid out of range"):
            c = hit.get(k, 0)
            if tot:
                print(f"    {k:26s} {c:8,}  {100.0*c/tot:6.2f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
