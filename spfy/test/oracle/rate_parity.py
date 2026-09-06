"""rate_parity.py -- the audio gate for the SPEAKING RATE path.

master_spfy_parity.py reads 221/221 byte-identical and that number says
NOTHING about \\!rp: every entry in corpus.jsonl is untagged, and the engine
plays units at their natural length until a rate event arms target matching,
so none of those 221 renders ever entered the time-scaled path at all.

This runs the same comparison over corpus_rate.jsonl, which does.

    python rate_parity.py --exe <spfy_synth.exe> [--voice tom] [--filter RE]

Reports, per entry and in aggregate:

    BYTE-IDENTICAL  the goal, same test master_spfy_parity applies
    LENGTH          engine samples vs ours, and the ratio
    NCC             best normalised cross-correlation, so a near-miss is
                    distinguishable from noise

and separately checks the three structural properties that hold regardless
of how close the samples are, because each is falsifiable on its own:

    ARM       \\!rp100 / \\!rpr / \\!rdr must NOT match their untagged
              baseline.  (They arm target matching; the Guide calls them
              no-ops, and the engine disagrees.)
    CONTROL   \\!vp100 MUST match its untagged baseline. If this fails the
              arm is firing on volume and every ARM result is meaningless.
    CURVE     realised duration vs the ideal 100/N, ours beside the engine's.

Exit: 0 when every structural check passes, 1 otherwise. Byte-identity is
REPORTED, not required -- it is the target, not yet the contract.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import audio_compare as ac                                    # noqa: E402

HERE = Path(__file__).resolve().parent
DEFAULT_CORPUS = HERE / "corpus_rate.jsonl"


def load(corpus, filt):
    items = []
    with open(corpus, encoding="utf-8") as f:
        for ln in f:
            ln = ln.strip()
            if not ln:
                continue
            r = json.loads(ln)
            if filt and not re.search(filt, r["id"]):
                continue
            items.append(r)
    return items


def baseline_for(tid):
    """The untagged entry a tagged one should be measured against."""
    m = re.match(r"^rate_([a-z]+)_", tid)
    if not m:
        return None
    return f"rate_{m.group(1)}_base"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.environ.get("SPFY_SYNTH_EXE"))
    ap.add_argument("--corpus", default=str(DEFAULT_CORPUS))
    ap.add_argument("--voice", default="tom")
    ap.add_argument("--filter", default=None)
    ap.add_argument("--workers", type=int, default=8)
    a = ap.parse_args()
    if not a.exe:
        print("ERROR: --exe required (or set SPFY_SYNTH_EXE)")
        return 2

    items = load(a.corpus, a.filter)
    if not items:
        print("ERROR: no corpus entries")
        return 2
    pairs = [(it["id"], it["text"]) for it in items]

    vp = ac.resolve_voice(a.voice)
    eng_dir = ac.engine_ref_dir(a.voice) / "rate"
    our_dir = ac.SCRATCH / "ours_rate" / a.voice
    ac.render_engine(pairs, eng_dir, a.voice)
    ac.render_ours(pairs, our_dir, a.exe, a.workers, vp)

    eng, ours = {}, {}
    for tid, _ in pairs:
        eng[tid] = ac.read_wav(eng_dir / f"{tid}.wav")
        ours[tid] = ac.read_wav(our_dir / f"{tid}.wav")

    print("=" * 78)
    print(f"{'id':<20} {'eng':>7} {'ours':>7} {'ratio':>6} {'ncc':>6}  verdict")
    print("-" * 78)
    ident = total = 0
    for tid, _ in pairs:
        e, o = eng[tid], ours[tid]
        if e is None or o is None:
            print(f"{tid:<20} {'-':>7} {'-':>7} {'-':>6} {'-':>6}  MISSING")
            total += 1
            continue
        total += 1
        same = len(e) == len(o) and bool((e == o).all())
        if same:
            ident += 1
        ncc = 1.0 if same else ac.best_ncc(e, o)
        print(f"{tid:<20} {len(e):>7} {len(o):>7} "
              f"{len(o) / max(len(e), 1):>6.3f} {ncc:>6.3f}  "
              f"{'IDENTICAL' if same else 'differs'}")

    print("-" * 78)
    print(f"BYTE-IDENTICAL: {ident}/{total} "
          f"({100.0 * ident / max(total, 1):.1f}%)")
    print(f"  REFERENCE:    {ac.ENGINE_PROVENANCE}")

    # ---- structural checks -------------------------------------------
    fails = []

    def render_pair(tid):
        return eng.get(tid), ours.get(tid)

    def identical(x, y):
        return (x is not None and y is not None
                and len(x) == len(y) and bool((x == y).all()))

    print("\nSTRUCTURAL CHECKS")
    print("-" * 78)

    for tid in ("rate_wx_rp100", "rate_wx_rpr", "rate_wx_rdr"):
        if tid not in eng:
            continue
        be, bo = render_pair("rate_wx_base")
        te, to = render_pair(tid)
        e_armed = not identical(be, te)
        o_armed = not identical(bo, to)
        ok = e_armed and o_armed
        print(f"  ARM      {tid:<18} engine={'armed' if e_armed else 'NO-OP'}"
              f"  ours={'armed' if o_armed else 'NO-OP'}"
              f"   {'PASS' if ok else 'FAIL'}")
        if not ok:
            fails.append(f"{tid}: engine armed={e_armed} ours armed={o_armed}")

    if "rate_wx_vp100" in eng:
        be, bo = render_pair("rate_wx_base")
        te, to = render_pair("rate_wx_vp100")
        e_ok = identical(be, te)
        o_ok = identical(bo, to)
        ok = e_ok and o_ok
        print(f"  CONTROL  rate_wx_vp100      engine="
              f"{'inert' if e_ok else 'CHANGED'}"
              f"  ours={'inert' if o_ok else 'CHANGED'}"
              f"   {'PASS' if ok else 'FAIL'}")
        if not ok:
            fails.append("vp100 is not inert -- the arm fires on volume")

    print("\nRATE CURVE (duration relative to each side's own baseline)")
    print("-" * 78)
    print(f"  {'tag':<10} {'ideal':>7} {'engine':>7} {'ours':>7} {'delta':>7}")
    be, bo = render_pair("rate_wx_base")
    for tid, _ in pairs:
        m = re.match(r"^rate_wx_rp(\d+)$", tid)
        if not m:
            continue
        n = int(m.group(1))
        te, to = render_pair(tid)
        # ⚠ `None in (arr, ...)` does elementwise == on numpy arrays and then
        # asks for its truth value. Test identity explicitly.
        if any(x is None for x in (be, bo, te, to)):
            continue
        ideal = 100.0 / n
        re_ = len(te) / len(be)
        ro_ = len(to) / len(bo)
        print(f"  rp{n:<8} {ideal:>7.3f} {re_:>7.3f} {ro_:>7.3f} "
              f"{ro_ - re_:>+7.3f}")

    print("=" * 78)
    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        return 1
    print("structural checks: all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
