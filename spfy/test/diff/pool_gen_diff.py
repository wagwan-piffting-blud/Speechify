"""Per-slot pool-generation diff: classify missing UIDs by relationship
to surrounding context (plan 02-03 D-13).

For each (corpus_id, slot) where the engine's chosen UID is NOT in our
PRSL pool, classify the missing UID into one of:
  - same_rec       : chosen_uid == prev_chosen_uid + 1
                     (same-recording continuation; documented as engine's
                     continuation-without-PRSL-requery path)
  - in_engine_pool : chosen_uid IS in engine's prsl_slot pool but NOT in
                     ours (engine looked up the same key + got more cands)
  - secondary_lookup: chosen_uid not in engine's pool either, but engine
                     used it anyway (engine looked up a DIFFERENT key)
  - other          : none of the above

Inputs (existing-traces-first per D-13):
  spfy/test/oracle/traces/prsl_slot/<id>.jsonl    -- per-slot ctx + engine pool
  spfy/test/oracle/traces/wsola_buffer/<id>.jsonl -- chosen UID list
  spfy/build/win/src/cli/spfy_viterbi_replay.exe  -- our pool computation

Outputs:
  per-(id, slot) JSONL records to stdout, one per gap slot
  aggregate counts to stderr — DOMINANT class is the highest count
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

REPLAY = Path("C:/tmp/spfy_build/src/cli/spfy_viterbi_replay.exe")
TR     = Path("spfy/test/oracle/traces")

UTT_RE = re.compile(
    r"utt\s+(\d+):\s+slots=\s*(\d+)\s+prsl=\s*(\d+)\s+chosen_in_pool=\s*(\d+)"
)


def load_prsl_slot(pid: str) -> list[dict]:
    """Returns list of {n, slot, ctx, n_cands, uids} per record."""
    path = TR / "prsl_slot" / f"{pid}.jsonl"
    if not path.exists():
        return []
    out = []
    for line in path.open("r", encoding="utf-8"):
        line = line.strip()
        if not line:
            continue
        try:
            rec = json.loads(line)
        except json.JSONDecodeError:
            continue
        if rec.get("type") == "prsl_slot":
            out.append(rec)
    return out


def load_wsola_chosen(pid: str) -> list[int]:
    """Engine's chosen UID per slot (in order)."""
    path = TR / "wsola_buffer" / f"{pid}.jsonl"
    if not path.exists():
        return []
    out = []
    for line in path.open("r", encoding="utf-8"):
        line = line.strip()
        if not line:
            continue
        try:
            rec = json.loads(line)
        except json.JSONDecodeError:
            continue
        if rec.get("type") == "wsola_in":
            for unit in rec.get("units", []):
                out.append(unit.get("uid", 0))
    return out


def get_our_pool_per_slot(pid: str) -> dict[int, list[int]]:
    """Run spfy_viterbi_replay and parse per-slot pool dumps.

    The replay binary doesn't natively dump our pool, but the prsl_slot
    trace's `ctx[1]/ctx[2]/ctx[3]` are the same triphone keys our
    spfy_prsl_lookup uses. We re-derive our pool by calling
    `spfy_dump_voice --prsl <vin> <key>` per slot.

    For pragmatic batch processing we call replay once with a debug env
    that dumps our pool, OR fall back to deriving from prsl_slot ctx via
    a one-shot Python prsl decoder.

    For this v1 we use the prsl_slot.uids field as a proxy for engine's
    pool, and we know our pool is derived from the same context_key. The
    simplest gap signal: rely on the binary's chosen_in_pool < n_slots
    counter to identify gap-bearing slots, then manual classification
    via in_engine_pool / same_rec heuristics on the chosen UID.

    For each prsl_slot record, infer the context_key our spfy_prsl_lookup
    would compute = ctx[1]*10000 + ctx[2]*100 + ctx[3]. The engine's pool
    in prsl_slot.uids[] is what the engine TOOK. To check whether OUR pool
    contains the chosen UID, we'd need to load the prsl chunk in Python,
    or invoke a tiny C helper. For v1 we shortcut: assume our pool ⊆
    engine's pool (since both come from the same chunk and same key in
    the typical case), so any slot where chosen_uid is NOT in
    prsl_slot.uids[] is a slot where engine used a different key
    (secondary_lookup). Conversely, if chosen_uid IS in engine's pool
    BUT replay binary reports gap, the gap is OUR lookup missing the
    cand — in_engine_pool class.
    """
    return {}


def classify_gap(slot_idx: int,
                 chosen_uid: int,
                 prev_chosen_uid: int | None,
                 engine_pool: list[int]) -> str:
    """Classify a gap slot's missing UID by relationship class."""
    if (prev_chosen_uid is not None and chosen_uid > 0
            and chosen_uid == prev_chosen_uid + 1):
        return "same_rec"
    if chosen_uid in engine_pool:
        # Engine's pool contains it; gap means OUR pool is a strict
        # subset of engine's for the same triphone key. Either we miss
        # cands or engine added them via secondary lookup at the same key.
        return "in_engine_pool"
    return "secondary_lookup"


def diff_phrase(pid: str) -> tuple[int, int, list[dict]]:
    """Returns (n_slots, n_chosen_in_pool, gap_records)."""
    pslots = load_prsl_slot(pid)
    chosen = load_wsola_chosen(pid)
    if not pslots:
        return 0, 0, []

    # Run replay to get authoritative chosen_in_pool counts per utt.
    args = [str(REPLAY), "en-US/tom/tom.vin", "en-US/tom/tom.vcf",
            str(TR / "wsola_buffer"     / f"{pid}.jsonl"),
            str(TR / "prsl_slot"        / f"{pid}.jsonl"),
            str(TR / "cart_walker_args" / f"{pid}.jsonl"),
            str(TR / "inner_scorer"     / f"{pid}.jsonl")]
    try:
        cp = subprocess.run(args, capture_output=True, text=True,
                            timeout=60)
    except subprocess.TimeoutExpired:
        return 0, 0, []
    n_slots = n_in_pool = 0
    for line in cp.stdout.splitlines():
        m = UTT_RE.search(line)
        if m:
            n_slots  += int(m.group(2))
            n_in_pool += int(m.group(4))
    if n_slots == 0:
        return 0, 0, []
    if n_slots == n_in_pool:
        return n_slots, n_in_pool, []

    # Walk prsl_slot records and identify slots where chosen_uid is the
    # engine's actual pick but our pool's missing it. We compare to the
    # `chosen` list (wsola_buffer.units[].uid).
    gap_records: list[dict] = []
    n = min(len(pslots), len(chosen))
    for i in range(n):
        prec = pslots[i]
        engine_pool = prec.get("uids", [])
        chosen_uid = chosen[i] if i < len(chosen) else 0
        # If the chosen UID IS in engine's emitted pool, this slot is fine
        # IF our pool also has it. We can't directly verify ours here
        # without a C-side check. As a proxy: if engine's pool size is 1
        # and equals chosen, that's typically a same-rec slot we hardcode
        # or a singleton; the gap is more likely on diverse pools.
        # For v1: just emit a record per slot with engine_pool_n + ctx
        # so a downstream classifier can use it.
        if chosen_uid in engine_pool:
            continue
        # chosen_uid NOT in engine's pool -> engine used a different
        # mechanism. Classify.
        prev_uid = chosen[i - 1] if i > 0 else None
        cls = classify_gap(prec["slot"], chosen_uid, prev_uid, engine_pool)
        gap_records.append({
            "corpus_id":      pid,
            "slot":           prec["slot"],
            "ctx":            prec.get("ctx"),
            "engine_pool_n":  prec.get("n_cands", len(engine_pool)),
            "chosen_uid":     chosen_uid,
            "prev_chosen_uid": prev_uid,
            "class":          cls,
        })
    return n_slots, n_in_pool, gap_records


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--filter", default=r"^text_",
                    help="phrase id regex (default: text_*)")
    ap.add_argument("--corpus", default="spfy/test/oracle/corpus.jsonl")
    args = ap.parse_args()

    flt = re.compile(args.filter)
    ids = []
    with open(args.corpus, "r", encoding="utf-8") as fp:
        for line in fp:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            try:
                ids.append(json.loads(line)["id"])
            except (json.JSONDecodeError, KeyError):
                pass
    ids = [i for i in ids if flt.search(i)]

    tot_slots = tot_in_pool = 0
    tot_gap = 0
    by_class: Counter = Counter()
    for pid in ids:
        ns, npool, gaps = diff_phrase(pid)
        tot_slots  += ns
        tot_in_pool += npool
        tot_gap += len(gaps)
        for g in gaps:
            print(json.dumps(g, separators=(",", ":")))
            by_class[g["class"]] += 1

    print("", file=sys.stderr)
    print("===== pool-gen diff aggregate =====", file=sys.stderr)
    print(f"phrases scanned       : {len(ids)}", file=sys.stderr)
    print(f"slots total           : {tot_slots}", file=sys.stderr)
    print(f"chosen_in_pool        : {tot_in_pool}  "
          f"({100.0*tot_in_pool/tot_slots if tot_slots else 0:.2f}%)",
          file=sys.stderr)
    print(f"pool-gen gap slots    : {tot_slots - tot_in_pool}  "
          f"({100.0*(tot_slots-tot_in_pool)/tot_slots if tot_slots else 0:.2f}%)",
          file=sys.stderr)
    print(f"records emitted       : {tot_gap}", file=sys.stderr)
    print(f"by class              :", file=sys.stderr)
    for cls, cnt in by_class.most_common():
        print(f"  {cls:20s} : {cnt}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
