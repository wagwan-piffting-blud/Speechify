#!/usr/bin/env python
"""
analyze_cart_walker_args.py -- M3.4r Phase B4.3 ground-truth analysis.

Joins three captures per text:

  cart_walks/text_NNN.jsonl       (per-slot leaf hits + question/value sequence)
  cart_walker_args/text_NNN.jsonl (per-slot 8 q_type input values, one row per
                                   walker call)
  fe_tree/text_NNN.jsonl          (FE-emitted utterance tree with shared item
                                   features: name + stress per Word/Syl/Seg)

and produces two outputs:

  1. **Mapping validation** -- for every (utt, slot, tree, q_type, value)
     observed in cart_walks, look up the corresponding cart_walker_args
     row's qN field. Should match bit-for-bit if the dispatcher ABI we
     decoded is right. Reports per-q_type match counts.

  2. **Kernel hint dump** -- for each (slot, q_type) pair, emit a row
     {q_type, value, fe_state} where fe_state is the slot's surrounding
     FE-tree context (segment phoneme + neighbors, syllable stress, word
     position, etc). Save as parquet/csv so we can run empirical fits to
     decode each q_type's kernel.

Usage:

  python analyze_cart_walker_args.py validate \\
      --cart-walks      spfy/test/oracle/traces/cart_walks \\
      --cart-walker-args spfy/test/oracle/traces/cart_walker_args \\
      --filter text_001

  python analyze_cart_walker_args.py kernel-dump \\
      --cart-walks       spfy/test/oracle/traces/cart_walks \\
      --cart-walker-args spfy/test/oracle/traces/cart_walker_args \\
      --fe-tree          spfy/test/oracle/traces/fe_tree \\
      --out              c:/tmp/spfy_qtype_kernel_data.csv
"""

from __future__ import annotations

import argparse
import collections
import json
import sys
from pathlib import Path


# ---------------------------------------------------------------------- #
# Loaders                                                                #
# ---------------------------------------------------------------------- #

def load_cart_walks(path: Path) -> list[dict]:
    """Return list of {utt, slot, tree, questions:[...]} per leaf-hit walk.

    cart_walks events come as one record per CART tree walk. We assume
    sequential utterance ordering -- the runner emits a "next_utt" marker
    between corpus entries (or the trace itself is segmented per text)."""
    walks = []
    with path.open() as fh:
        for line in fh:
            try:
                ev = json.loads(line)
            except json.JSONDecodeError:
                continue
            if ev.get("type") != "cart_walk":
                continue
            walks.append(ev)
    return walks


def load_walker_args(path: Path) -> list[dict]:
    rows = []
    with path.open() as fh:
        for line in fh:
            try:
                ev = json.loads(line)
            except json.JSONDecodeError:
                continue
            if ev.get("type") != "cart_walker_args":
                continue
            rows.append(ev)
    return rows


# ---------------------------------------------------------------------- #
# Joining                                                                #
# ---------------------------------------------------------------------- #

def pair_walks_with_args(walks: list[dict],
                         args_rows: list[dict]) -> list[tuple[dict, dict]]:
    """Both streams are emitted in order from the same Frida session.

    Each walk corresponds to exactly one walker call, so pairing by
    sequence + (tree, slot) match should always succeed. We pair walk[i]
    with args[i] and verify (tree, slot) coincide.
    """
    pairs = []
    if len(walks) != len(args_rows):
        print(f"WARN: walk count {len(walks)} != args count "
              f"{len(args_rows)}; pairing by min length", file=sys.stderr)
    n = min(len(walks), len(args_rows))
    for i in range(n):
        w = walks[i]
        a = args_rows[i]
        if w.get("tree") != a.get("tree"):
            print(f"WARN: i={i} tree mismatch: walk={w.get('tree')} "
                  f"args={a.get('tree')}", file=sys.stderr)
            continue
        if w.get("slot") != a.get("slot"):
            print(f"WARN: i={i} slot mismatch (tree={w['tree']}): "
                  f"walk={w.get('slot')} args={a.get('slot')}",
                  file=sys.stderr)
            continue
        pairs.append((w, a))
    return pairs


# ---------------------------------------------------------------------- #
# Validate mapping                                                       #
# ---------------------------------------------------------------------- #

# q_type -> walker args field name.
# Decoded from disasm trace at FUN_08e87d90 / 08e87e10:
QTYPE_TO_FIELD = {
    1: "q1",
    2: "q2",
    3: "q3",
    4: "q4",
    5: "q5",
    7: "q7",
    8: "q8",
    9: "q9",
}


def validate(walk_dir: Path, args_dir: Path,
             text_filter: str | None = None) -> None:
    files = sorted(walk_dir.glob("text_*.jsonl"))
    if text_filter:
        files = [p for p in files if text_filter in p.name]
    if not files:
        print(f"no cart_walks files in {walk_dir}", file=sys.stderr)
        sys.exit(1)

    total = collections.Counter()
    match = collections.Counter()
    by_qtype_mismatches: dict[int, list] = {}

    for wpath in files:
        apath = args_dir / wpath.name
        if not apath.exists():
            print(f"missing cart_walker_args/{wpath.name}; skip",
                  file=sys.stderr)
            continue

        walks = load_cart_walks(wpath)
        args  = load_walker_args(apath)
        pairs = pair_walks_with_args(walks, args)

        for w, a in pairs:
            for q in w.get("questions", []):
                qt = q.get("q_type")
                expected = q.get("value")
                field = QTYPE_TO_FIELD.get(qt)
                if field is None:
                    total[("unmapped", qt)] += 1
                    continue
                actual = a.get(field)
                total[qt] += 1
                if actual == expected:
                    match[qt] += 1
                else:
                    by_qtype_mismatches.setdefault(qt, []).append(
                        (wpath.name, w.get("tree"), w.get("slot"),
                         w.get("phone_idx"), expected, actual))

    print(f"=== Mapping validation ({len(files)} text files) ===")
    print(f"{'q_type':>6}  {'matched':>8}  {'total':>8}  {'rate':>7}")
    for qt in sorted(total):
        if isinstance(qt, tuple):
            continue
        m = match.get(qt, 0)
        t = total[qt]
        print(f"{qt:>6}  {m:>8}  {t:>8}  {m/t*100:>6.2f}%")

    for qt, mm in sorted(by_qtype_mismatches.items()):
        print(f"\nq_type {qt} mismatches (first 10):")
        for x in mm[:10]:
            print(f"  {x}")


# ---------------------------------------------------------------------- #
# Kernel dump                                                            #
# ---------------------------------------------------------------------- #

def kernel_dump(walk_dir: Path, args_dir: Path, fe_dir: Path,
                out_path: Path,
                text_filter: str | None = None) -> None:
    """Emit a CSV joining each per-(slot, q_type) value with surrounding
    FE-tree state, so we can run empirical fits to decode kernels.

    Columns:
      text, utt_idx, tree, slot, phone_idx,
      q_type, value,
      seg_name, seg_left2, seg_left1, seg_right1, seg_right2,
      syl_stress, word_idx_in_phrase, syl_idx_in_word, ...
    """
    # Stub for now -- structure to extend once we have actual capture
    # data. The FE tree parser is in spfy_build_graph_replay.c; we'll
    # mirror the relevant subset here when needed.
    print("kernel_dump: not yet implemented (waiting on capture)",
          file=sys.stderr)
    sys.exit(2)


# ---------------------------------------------------------------------- #
# Main                                                                   #
# ---------------------------------------------------------------------- #

def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    v = sub.add_parser("validate")
    v.add_argument("--cart-walks", type=Path, required=True)
    v.add_argument("--cart-walker-args", type=Path, required=True)
    v.add_argument("--filter", default=None)

    k = sub.add_parser("kernel-dump")
    k.add_argument("--cart-walks", type=Path, required=True)
    k.add_argument("--cart-walker-args", type=Path, required=True)
    k.add_argument("--fe-tree", type=Path, required=True)
    k.add_argument("--out", type=Path, required=True)
    k.add_argument("--filter", default=None)

    args = ap.parse_args()

    if args.cmd == "validate":
        validate(args.cart_walks, args.cart_walker_args, args.filter)
    elif args.cmd == "kernel-dump":
        kernel_dump(args.cart_walks, args.cart_walker_args,
                    args.fe_tree, args.out, args.filter)


if __name__ == "__main__":
    main()
