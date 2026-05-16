"""Run spfy_viterbi_replay over the 200-phrase corpus and aggregate
per-phrase UID match.

Plan 02-02 closure measurement instrument. Used to quantify the
DP-on-engine-inputs UID match rate before/after the dag_join_cb +
load_f0_hist_curve port (THIRD scope revision in 02-DP-AUDIT.md).

Usage:
  python spfy/test/diff/run_replay_corpus.py
         [--replay PATH]
         [--limit N]
         [--filter REGEX]
         [--env KEY=VALUE ...]   # propagate env var to each invocation

Output: per-phrase summary lines + final aggregate to stdout.
Returns 0 always (errors counted, not fatal).
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

VIN     = Path("en-US/tom/tom.vin")
VCF     = Path("en-US/tom/tom.vcf")
TRACES  = Path("spfy/test/oracle/traces")
DEFAULT = Path("C:/tmp/spfy_build/src/cli/spfy_viterbi_replay.exe")

UTT_RE = re.compile(
    r"^utt\s+(\d+):\s+slots=\s*(\d+)\s+prsl=\s*(\d+)\s+"
    r"chosen_in_pool=\s*(\d+)\s+pool_eq_cap=\s*(\d+)\s+matched=\s*(\d+)\s+"
    r"rc=(\S+)\s+cost=(\S+)"
)

# Per-slot mismatch line emitted to stderr by SPFY_DEBUG_MISMATCH=1.
# Source: spfy_viterbi_replay.c around line 1830.
#   "  utt U slot SS: chosen=CHO (...) tc=TC_C  best=BST (...) tc=TC_B
#    diff=+DIFF  pool_n=N"
_FLOAT = r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?"
MISMATCH_RE = re.compile(
    r"^\s*utt\s+(\d+)\s+slot\s+(\d+):\s+"
    r"chosen=\s*(\d+)\b.*?tc=(" + _FLOAT + ").*?"
    r"best=\s*(\d+)\b.*?tc=(" + _FLOAT + ").*?"
    r"diff=(" + _FLOAT + ").*?pool_n=(\d+)"
)


REQUIRED_HOOKS = ("wsola_buffer", "prsl_slot", "cart_walks", "inner_scorer")


def trace_paths(phrase_id: str) -> dict[str, Path]:
    return {h: TRACES / h / f"{phrase_id}.jsonl" for h in REQUIRED_HOOKS}


def run_one(replay: Path, phrase_id: str, env: dict,
            mode: str = "?",
            mismatch_writer=None
            ) -> tuple[int, int, int, int]:
    """Returns (n_utts, n_slots, n_matched, n_complete).

    Uses the cart_walks trace (engine-faithful safe-hook captures land
    here; spfy_viterbi_replay.c::parse_cart_walk_line consumes
    slot/tree/leaf_mean/leaf_var from this schema). The earlier driver
    pointed at cart_walker_args/ — wrong schema — which silently fell
    back to chosen-UID-as-target proxy and depressed UID match.

    If mismatch_writer is given (a callable taking dict), it is invoked
    once per per-slot mismatch line emitted by the binary under
    SPFY_DEBUG_MISMATCH=1. The dict has corpus_id, utt_idx, slot_idx,
    engine_uid, engine_tc, our_uid, our_tc, tc_diff, pool_n — the
    fields the 02-DP-AUDIT.jsonl schema needs for triage.
    """
    tp = trace_paths(phrase_id)
    args = [str(replay), str(VIN), str(VCF),
            str(tp["wsola_buffer"]),
            str(tp["prsl_slot"]),
            str(tp["cart_walks"]),
            str(tp["inner_scorer"])]
    full_env = os.environ.copy()
    full_env.update(env)
    if "SPFY_VR_JSONL_OUT" in full_env:
        full_env["SPFY_VR_ID"]   = phrase_id
        full_env["SPFY_VR_MODE"] = mode
    if mismatch_writer is not None:
        # spfy_viterbi_replay.c interprets numeric values as utt-index
        # filters; "0" (atoi("0") == 0) is the documented "all utterances"
        # sentinel. SPFY_DEBUG_MISMATCH=1 would print only utt 1.
        full_env["SPFY_DEBUG_MISMATCH"] = "0"
    cp = subprocess.run(args, capture_output=True, text=True, timeout=60,
                        env=full_env, check=False)
    if mismatch_writer is not None and cp.stderr:
        for line in cp.stderr.splitlines():
            m = MISMATCH_RE.match(line)
            if not m:
                continue
            mismatch_writer({
                "corpus_id":  phrase_id,
                "utt_idx":    int(m.group(1)),
                "slot_idx":   int(m.group(2)),
                "engine_uid": int(m.group(3)),
                "engine_tc":  float(m.group(4)),
                "our_uid":    int(m.group(5)),
                "our_tc":     float(m.group(6)),
                "tc_diff":    float(m.group(7)),
                "pool_n":     int(m.group(8)),
            })
    n_utts = n_slots = n_matched = n_complete = 0
    for line in cp.stdout.splitlines():
        m = UTT_RE.match(line)
        if not m:
            continue
        n_utts    += 1
        n_slots   += int(m.group(2))
        n_matched += int(m.group(6))
        if m.group(7) == "ok":
            n_complete += 1
    return n_utts, n_slots, n_matched, n_complete


def load_corpus_ids(corpus_path: Path) -> list[str]:
    ids = []
    with corpus_path.open("r", encoding="utf-8") as fp:
        for line in fp:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            try:
                ids.append(json.loads(line)["id"])
            except (json.JSONDecodeError, KeyError):
                pass
    return ids


def load_corpus_entries(corpus_path: Path) -> list[dict]:
    out: list[dict] = []
    with corpus_path.open("r", encoding="utf-8") as fp:
        for line in fp:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--replay",  type=Path, default=DEFAULT)
    ap.add_argument("--corpus",  type=Path,
                    default=Path("spfy/test/oracle/corpus.jsonl"))
    ap.add_argument("--limit",   type=int, default=0,
                    help="process only first N phrases (0 = all)")
    ap.add_argument("--filter",  default=None,
                    help="regex applied to phrase id; only matches run")
    ap.add_argument("--env",     action="append", default=[],
                    help="K=V env var to set per invocation")
    ap.add_argument("--quiet",   action="store_true",
                    help="suppress per-phrase summary lines")
    ap.add_argument("--jsonl-out", type=Path, default=None,
                    help="if set, the binary appends per-phrase D-12-schema "
                         "JSONL records (with integer n_uid_match) here. "
                         "Path is also read by this script to compute the "
                         "exact-integer-sum corpus aggregate at the end.")
    ap.add_argument("--require-all-traces", action="store_true",
                    default=True,
                    help="skip phrases missing any of the 4 required "
                         "engine traces (default on)")
    ap.add_argument("--mismatches-jsonl", type=Path, default=None,
                    help="if set, write per-slot mismatches here (one "
                         "JSON object per line) for triage. Sets "
                         "SPFY_DEBUG_MISMATCH=1 on each binary call and "
                         "parses the stderr output, tagging each record "
                         "with the corpus phrase id.")
    args = ap.parse_args()

    if not args.replay.exists():
        print(f"replay binary not found: {args.replay}", file=sys.stderr)
        return 1

    env = {}
    for kv in args.env:
        if "=" not in kv:
            print(f"--env expects K=V, got {kv!r}", file=sys.stderr)
            return 1
        k, v = kv.split("=", 1)
        env[k] = v
    if args.jsonl_out is not None:
        args.jsonl_out.parent.mkdir(parents=True, exist_ok=True)
        # Truncate the aggregate JSONL fresh; the binary appends per phrase.
        args.jsonl_out.write_text("", encoding="utf-8")
        env["SPFY_VR_JSONL_OUT"] = str(args.jsonl_out)

    mismatch_writer = None
    mismatch_fp = None
    if args.mismatches_jsonl is not None:
        args.mismatches_jsonl.parent.mkdir(parents=True, exist_ok=True)
        mismatch_fp = args.mismatches_jsonl.open("w", encoding="utf-8")
        def mismatch_writer(rec):
            mismatch_fp.write(json.dumps(rec, separators=(",", ":")) + "\n")

    entries = load_corpus_entries(args.corpus)
    flt = re.compile(args.filter) if args.filter else None
    if flt:
        entries = [e for e in entries if flt.search(e["id"])]
    if args.limit:
        entries = entries[: args.limit]

    tot_phrases = tot_utts = tot_slots = tot_matched = tot_complete = 0
    n_skipped_missing = 0
    for entry in entries:
        pid = entry["id"]
        # Trace existence guard: the binary will silently fall back if a
        # cart_walks file is missing; we want SC1's denominator to reflect
        # only phrases with full engine inputs.
        if args.require_all_traces:
            tps = trace_paths(pid)
            missing = [h for h, p in tps.items() if not p.exists()]
            if missing:
                n_skipped_missing += 1
                if not args.quiet:
                    print(f"  SKIP {pid:12s}  missing traces: {missing}",
                          file=sys.stderr)
                continue
        try:
            nu, ns, nm, nc = run_one(args.replay, pid, env,
                                     mode=entry.get("mode", "?"),
                                     mismatch_writer=mismatch_writer)
        except subprocess.TimeoutExpired:
            print(f"  TIMEOUT {pid}", file=sys.stderr)
            continue
        except FileNotFoundError as e:
            if not args.quiet:
                print(f"  SKIP {pid}: {e}", file=sys.stderr)
            continue
        tot_phrases  += 1
        tot_utts     += nu
        tot_slots    += ns
        tot_matched  += nm
        tot_complete += nc
        if not args.quiet and ns > 0:
            pct = 100.0 * nm / ns
            print(f"  {pid:12s}  utts={nu:2d}  slots={ns:5d}  "
                  f"matched={nm:5d}  ({pct:5.1f}%)")

    if mismatch_fp is not None:
        mismatch_fp.close()

    if tot_slots == 0:
        print("no slots aggregated", file=sys.stderr)
        return 0
    pct = 100.0 * tot_matched / tot_slots
    print("")
    print("===== aggregate (regex over stdout) =====")
    print(f"phrases       : {tot_phrases}")
    print(f"skipped (missing traces): {n_skipped_missing}")
    print(f"utterances    : {tot_utts} (complete: {tot_complete})")
    print(f"slots total   : {tot_slots}")
    print(f"slots matched : {tot_matched}")
    print(f"UID match     : {pct:.2f}%")

    # Exact-integer-sum aggregate per plan 02-06 Threat T-02-26.
    if args.jsonl_out is not None and args.jsonl_out.exists():
        agg_match = agg_slots = 0
        n_records = 0
        with args.jsonl_out.open("r", encoding="utf-8") as fp:
            for line in fp:
                line = line.strip()
                if not line:
                    continue
                try:
                    r = json.loads(line)
                except json.JSONDecodeError:
                    continue
                n_records += 1
                if isinstance(r.get("n_uid_match"), int):
                    agg_match += r["n_uid_match"]
                if isinstance(r.get("n_slots"), int):
                    agg_slots += r["n_slots"]
        print()
        print("===== exact integer sum (D-12 schema, n_uid_match field) =====")
        print(f"out               : {args.jsonl_out}")
        print(f"records           : {n_records}")
        print(f"sum(n_uid_match)  : {agg_match}")
        print(f"sum(n_slots)      : {agg_slots}")
        if agg_slots:
            pct = 100.0 * agg_match / agg_slots
            print(f"corpus UID match  : {pct:.4f}% (exact integer)")
            if agg_match == agg_slots:
                print("RESULT: 100.0% — ROADMAP §Phase 2 SC1 met.")
            else:
                print(f"RESULT: SC1 NOT met — gap = {agg_slots - agg_match}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
