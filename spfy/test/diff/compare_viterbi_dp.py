"""Compare engine Viterbi DP state (from viterbi_dp_hook.js trace)
against our spfy_viterbi_replay's per-cand totals + chosen path.

This is the M3.4r investigation tool. Per-cand totals are bit-exact
at InnerScorer time (M3.4q), but our DP's chosen UID still mismatches
the engine on ~20% of slots. Two non-mutually-exclusive hypotheses:

  H1: PostScoringAdj rewrites cand totals between InnerScorer and
      Viterbi. The Viterbi recurrence reads `cand+0x2c` (= "pre_dp"
      in our trace), which is what PostScoringAdj writes. If pre_dp
      != our InnerScorer-time total, PostScoringAdj is rewriting and
      we need to model it.

  H2: Our DP's same-rec aug or join formula differs from the engine,
      changing the path through the lattice even when per-cand totals
      match.

What this script reports per corpus entry:

  * Engine chosen path (reconstructed by walking back from
    best_slot/best_idx via cand+0x24 predecessor pointers).
  * Per-(slot, uid): delta = engine.pre_dp - our.tc
    (our.tc from SPFY_DUMP_CAND_TOTALS=1 dump). If H1 is wrong,
    delta should be zero for all matched cands.
  * Slots where engine pool overlaps our pool, with chosen-UID
    divergence flagged.

Usage:
  python compare_viterbi_dp.py <our_dump_dir> <viterbi_dp_dir>
                               [--ids text_001,text_002,...]

  <our_dump_dir>     directory of per-entry SPFY_DUMP_CAND_TOTALS dumps
                     (text_NNN.txt). Capture them via:
                       SPFY_DUMP_CAND_TOTALS=1 spfy_viterbi_replay ... > X.txt
  <viterbi_dp_dir>   spfy/test/oracle/traces/viterbi_dp/
"""
from __future__ import annotations

import argparse
import json
import re
import statistics
import sys
from pathlib import Path


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


def reconstruct_engine_path(leave_event: dict) -> list:
    """Walk back from (best_slot, best_idx) using cand+0x24 (predec)
    pointers. Engine's slice graph is a DAG -- predecessor cands may
    live in any earlier slot, not necessarily slot - 1. So we build
    a {cand_ptr -> (slot, idx, uid)} index and chase predec pointers
    until we hit slot 0 or an unresolvable predec.

    Returns a list of {slot, uid, dp_20, pre_dp} from slot 0 upward."""
    slots = leave_event.get("slots") or []
    best_slot = leave_event.get("best_slot")
    best_idx  = leave_event.get("best_idx")
    if best_slot is None or best_idx is None:
        return []

    ptr_index: dict = {}
    for s in slots:
        if s.get("error") or "cands" not in s:
            continue
        for c in s["cands"]:
            if c.get("error") is not None:
                continue
            ptr = c.get("cand_ptr")
            if ptr is not None:
                ptr_index[ptr] = (s["slot"], c.get("i", -1),
                                  c.get("uid"),
                                  c.get("dp_20"), c.get("pre_dp"))

    # Find the starting cand at (best_slot, best_idx).
    start = None
    for s in slots:
        if s.get("slot") == best_slot and "cands" in s:
            if best_idx < len(s["cands"]):
                start = s["cands"][best_idx]
            break
    if start is None:
        return []

    path: list = []
    cur = start
    cur_slot = best_slot
    seen = set()
    while cur is not None and cur_slot >= 0:
        if cur.get("cand_ptr") in seen:
            break
        seen.add(cur.get("cand_ptr"))
        path.append({
            "slot":   cur_slot,
            "uid":    cur.get("uid"),
            "dp_20":  cur.get("dp_20"),
            "pre_dp": cur.get("pre_dp"),
        })
        if cur_slot == 0:
            break
        predec = cur.get("predec")
        if not predec or predec not in ptr_index:
            break
        prev_slot, _idx, _uid, _dp, _pre = ptr_index[predec]
        # Re-fetch the cand object for traversal continuity.
        prev_obj = None
        for s in slots:
            if s.get("slot") == prev_slot and "cands" in s:
                for c in s["cands"]:
                    if c.get("cand_ptr") == predec:
                        prev_obj = c
                        break
                break
        if prev_obj is None:
            break
        cur = prev_obj
        cur_slot = prev_slot

    path.reverse()
    return path


def load_engine_per_utt(viterbi_path: Path) -> list:
    """Returns list of dicts (one per utt) with:
      enter_pre_dp: {(slot, uid): pre_dp}
      leave_dp_20:  {(slot, uid): dp_20}
      leave_pre_dp: {(slot, uid): pre_dp}
      n_cands:      {slot: n_cands}
      pool:         {slot: [uid, ...]}
      chosen_path:  [{slot, uid, dp_20, pre_dp}, ...]"""
    utts: list = []
    cur_enter: dict | None = None
    with viterbi_path.open("r", encoding="utf-8") as fp:
        for raw in fp:
            raw = raw.strip()
            if not raw:
                continue
            try:
                obj = json.loads(raw)
            except json.JSONDecodeError:
                continue
            t = obj.get("type")
            if t == "viterbi_enter":
                cur_enter = obj
            elif t == "viterbi_leave":
                u = {"enter_pre_dp": {}, "leave_dp_20": {},
                     "leave_pre_dp": {}, "n_cands": {}, "pool": {},
                     "chosen_path": []}
                en = cur_enter
                if en:
                    for s in en.get("slots") or []:
                        if s.get("error") or "cands" not in s:
                            continue
                        slot = s["slot"]
                        u["n_cands"][slot] = s.get("n_cands", 0)
                        u["pool"][slot]    = []
                        for c in s["cands"]:
                            uid = c.get("uid")
                            if uid is None:
                                continue
                            u["enter_pre_dp"][(slot, uid)] = c.get("pre_dp")
                            u["pool"][slot].append(uid)
                for s in obj.get("slots") or []:
                    if s.get("error") or "cands" not in s:
                        continue
                    slot = s["slot"]
                    for c in s["cands"]:
                        uid = c.get("uid")
                        if uid is None:
                            continue
                        u["leave_dp_20"][(slot, uid)] = c.get("dp_20")
                        u["leave_pre_dp"][(slot, uid)] = c.get("pre_dp")
                u["chosen_path"] = reconstruct_engine_path(obj)
                utts.append(u)
                cur_enter = None
    return utts


def report_entry(entry_id: str, our_dump: Path,
                 viterbi_path: Path) -> None:
    if not our_dump.exists():
        print(f"{entry_id}: missing our dump {our_dump}")
        return
    if not viterbi_path.exists():
        print(f"{entry_id}: missing viterbi trace {viterbi_path}")
        return

    our = parse_our_dump(our_dump)
    utts = load_engine_per_utt(viterbi_path)
    if not utts:
        print(f"{entry_id}: no viterbi events parsed")
        return

    print(f"=== {entry_id} ===  ({len(utts)} utterance(s))")
    for utt_idx, u in enumerate(utts, 1):
        n_slots_engine = len(u["pool"])
        chosen = u["chosen_path"]
        # Engine chosen UIDs in slot-order:
        engine_chain = {p["slot"]: p["uid"] for p in chosen}
        if not engine_chain:
            print(f"  utt {utt_idx}: chosen_path empty (best_slot/idx "
                  f"unresolved or backtrace broke)")
            continue

        # H1 check: pre_dp == our.tc?
        deltas = []
        big_deltas = []
        for (slot, uid), pre_dp in u["enter_pre_dp"].items():
            our_tc = our.get((utt_idx, slot, uid))
            if our_tc is None or pre_dp is None:
                continue
            d = pre_dp - our_tc
            deltas.append(d)
            if abs(d) > 0.01:
                big_deltas.append((slot, uid, our_tc, pre_dp, d))

        # Coverage:
        common = sum(1 for k in u["enter_pre_dp"]
                     if k in our and our[k] is not None)

        print(f"  utt {utt_idx}: engine_slots={n_slots_engine}  "
              f"engine_path_len={len(chosen)}  "
              f"common_cands={common}  "
              f"|d|>0.01={len(big_deltas)}")
        if deltas:
            ad = [abs(d) for d in deltas]
            ad.sort()
            print(f"    pre_dp - our.tc:  median={statistics.median(deltas):.6f}"
                  f"  max={max(ad):.6f}"
                  f"  p99={ad[min(len(ad)-1, int(0.99*len(ad)))]:.6f}")

        # Path divergence: which slots have engine.uid NOT in our.tc keys
        # for this utt? Or where engine.uid present but our chose different?
        # We don't have OUR chosen path here; just dump engine path for now.
        if len(big_deltas) > 0:
            print(f"    top |d| > 0.01 (max 8):")
            big_deltas.sort(key=lambda r: -abs(r[4]))
            for slot, uid, otc, pdp, d in big_deltas[:8]:
                print(f"      slot={slot} uid={uid}  ours={otc:.4f}  "
                      f"engine.pre_dp={pdp:.4f}  delta={d:+.4f}")

        # Show first few engine chosen-path entries:
        head = chosen[:5]
        tail = chosen[-3:] if len(chosen) > 5 else []
        if head:
            head_s = " ".join(f"{p['slot']}:{p['uid']}" for p in head)
            print(f"    engine chosen (head): {head_s}"
                  + (f"  ...  (tail) " +
                     " ".join(f"{p['slot']}:{p['uid']}" for p in tail)
                     if tail else ""))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("our_dump_dir", type=Path,
                    help="dir of {entry_id}.txt SPFY_DUMP_CAND_TOTALS dumps")
    ap.add_argument("viterbi_dp_dir", type=Path,
                    help="spfy/test/oracle/traces/viterbi_dp/")
    ap.add_argument("--ids", default=None,
                    help="comma-separated entry ids (default: all "
                         "*.jsonl in viterbi_dp_dir)")
    args = ap.parse_args()

    if not args.viterbi_dp_dir.is_dir():
        print(f"not a dir: {args.viterbi_dp_dir}", file=sys.stderr)
        return 1

    if args.ids:
        ids = [s.strip() for s in args.ids.split(",") if s.strip()]
    else:
        ids = sorted(p.stem for p in args.viterbi_dp_dir.glob("*.jsonl"))

    for entry_id in ids:
        our_path = args.our_dump_dir / f"{entry_id}.txt"
        vit_path = args.viterbi_dp_dir / f"{entry_id}.jsonl"
        report_entry(entry_id, our_path, vit_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
