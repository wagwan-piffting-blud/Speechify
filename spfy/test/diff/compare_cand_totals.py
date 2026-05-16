"""Compare engine per-cand totals (from inner_scorer trace) against
spfy_viterbi_replay's computed totals (from SPFY_DUMP_CAND_TOTALS).

Both sources have the same per-(utt, slot, cand) shape; we compute the
delta `our_total - engine_total` per cand and aggregate stats. A
systematic non-zero offset implies a missing constant; varying signs
imply a per-component formula bug.

Usage:
  python compare_cand_totals.py <our_dump.txt> <inner_scorer_dir>

  <our_dump.txt> is stdout from
    SPFY_DUMP_CAND_TOTALS=1 spfy_viterbi_replay ... > our.txt

  <inner_scorer_dir> is spfy/test/oracle/traces/inner_scorer/
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from collections import defaultdict


CAND_RE = re.compile(
    r"^CAND_TOTAL utt=(\d+) slot=(\d+) uid=(\d+) tc=(\S+)\s*$"
)


def parse_our_dump(path: Path) -> dict:
    """Returns {(utt, slot, uid): our_tc}."""
    out: dict = {}
    with path.open("r", encoding="utf-8") as fp:
        for line in fp:
            m = CAND_RE.match(line)
            if not m:
                continue
            utt, slot, uid, tc = m.groups()
            out[(int(utt), int(slot), int(uid))] = float(tc)
    return out


def parse_engine_dump(dirp: Path, utt_filter: int | None = None) -> dict:
    """Walk inner_scorer trace files. Returns {(utt, slot, uid): engine_total}.

    Files are per-corpus-entry; each file's slots reset (slot index 0
    after seeing >0 means new utterance within that file). The 'utt'
    in our output is per-corpus-entry; we emit utt = 1, 2, ... per
    entry, BUT the spfy_viterbi_replay CLI numbers utts within ONE
    file. So this script processes one corpus entry at a time and
    re-numbers utts per file."""
    out: dict = {}
    files = sorted(dirp.glob("*.jsonl"))
    for fp in files:
        with fp.open("r", encoding="utf-8") as f:
            entry_utt = 0
            prev_slot = -1
            for raw in f:
                raw = raw.strip()
                if not raw:
                    continue
                try:
                    obj = json.loads(raw)
                except json.JSONDecodeError:
                    continue
                if obj.get("type") != "inner_scorer":
                    continue
                slot = obj.get("slot")
                if slot is None:
                    continue
                if slot == 0 and prev_slot > 0:
                    entry_utt += 1
                if prev_slot < 0:
                    entry_utt = 1
                prev_slot = slot
                cand_totals = obj.get("cand_totals") or []
                for uid, total in cand_totals:
                    out.setdefault((fp.stem, entry_utt, slot), {})[uid] = total
    # Flatten: (entry_stem, utt, slot, uid) -> total
    flat = {}
    for (stem, utt, slot), uid_map in out.items():
        for uid, total in uid_map.items():
            flat[(stem, utt, slot, uid)] = total
    return flat


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2

    our_path  = Path(sys.argv[1])
    eng_dir   = Path(sys.argv[2])

    if not our_path.exists():
        print(f"missing: {our_path}", file=sys.stderr)
        return 1
    if not eng_dir.is_dir():
        print(f"not a dir: {eng_dir}", file=sys.stderr)
        return 1

    # Our dump is keyed by (utt, slot, uid). The driver shell loop ran
    # spfy_viterbi_replay per corpus entry, so utt is per-entry and the
    # caller annotates with the entry id. But we don't have that
    # annotation in CAND_TOTAL lines -- so we need the caller to dump
    # per-entry. For this script we assume our_path is the dump for
    # ONE corpus entry, and the entry id is passed via filename stem
    # (e.g., text_001.txt).
    entry_stem = our_path.stem
    eng_file   = eng_dir / f"{entry_stem}.jsonl"
    if not eng_file.exists():
        print(f"missing engine file: {eng_file}", file=sys.stderr)
        return 1

    our = parse_our_dump(our_path)

    # Parse just this entry's engine file.
    engine: dict = {}
    with eng_file.open("r", encoding="utf-8") as f:
        entry_utt = 0
        prev_slot = -1
        for raw in f:
            raw = raw.strip()
            if not raw:
                continue
            try:
                obj = json.loads(raw)
            except json.JSONDecodeError:
                continue
            if obj.get("type") != "inner_scorer":
                continue
            slot = obj.get("slot")
            if slot is None:
                continue
            if slot == 0 and prev_slot > 0:
                entry_utt += 1
            if prev_slot < 0:
                entry_utt = 1
            prev_slot = slot
            cand_totals = obj.get("cand_totals") or []
            for uid, total in cand_totals:
                engine[(entry_utt, slot, uid)] = total

    # Diff.
    matched_keys = set(our.keys()) & set(engine.keys())
    only_ours    = set(our.keys()) - set(engine.keys())
    only_engine  = set(engine.keys()) - set(our.keys())

    deltas = []
    big_deltas = []   # |delta| > 1
    for k in matched_keys:
        d = our[k] - engine[k]
        deltas.append(d)
        if abs(d) > 1.0:
            big_deltas.append((k, our[k], engine[k], d))

    print(f"entry: {entry_stem}")
    print(f"  matched cands : {len(matched_keys)}")
    print(f"  only ours     : {len(only_ours)} (cands in our pool but not engine's)")
    print(f"  only engine   : {len(only_engine)} (cands in engine pool but not ours)")
    if not deltas:
        print("  no overlapping cands")
        return 0
    deltas.sort()
    n = len(deltas)
    print(f"  delta (ours-engine):")
    print(f"    min   : {deltas[0]:.3f}")
    print(f"    max   : {deltas[-1]:.3f}")
    print(f"    median: {deltas[n//2]:.3f}")
    print(f"    mean  : {sum(deltas)/n:.3f}")
    abs_d = sorted(abs(d) for d in deltas)
    print(f"    |d| p50: {abs_d[n//2]:.3f}, p90: {abs_d[int(n*0.9)]:.3f}, p99: {abs_d[int(n*0.99)]:.3f}")

    # Show top 10 mismatches.
    big_deltas.sort(key=lambda t: abs(t[3]), reverse=True)
    print(f"  top |delta| > 1.0 (showing first 10 of {len(big_deltas)}):")
    for k, our_v, eng_v, d in big_deltas[:10]:
        utt, slot, uid = k
        print(f"    utt={utt} slot={slot} uid={uid:6d}  "
              f"ours={our_v:.3f}  engine={eng_v:.3f}  delta={d:+.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
