"""ctx_diff_subcluster.py -- Phase 3 / 03-06 Deliverable 2.

Sub-classify the 232 open ctx_diff records in
`.planning/phases/03-fe-convergence/03-FE-AUDIT.jsonl` into seven buckets
so plan 03-07 can target the dominant sub-class first instead of
chasing every record individually.

The sub-clusters, in priority order (FIRST match wins):

  1. boundary_silence_suspect : engine_ctx5[0] or [4] is in {64,65}
                                 (silence-pad halfphone classes) XOR
                                 our_ctx5 at the same position is in {64,65}.
                                 This is a partial-silence-pad presence
                                 mismatch -- a class-of-divergence rather
                                 than a positional pattern.
  2. numeral_acronym_suspect  : the corpus phrase contains a digit run or
                                 an all-caps acronym (>=2 chars). Without
                                 fe_token traces (none exist for any of
                                 the 46 ctx_diff corpora) we cannot map
                                 a slot to a specific token, so this
                                 bucket is phrase-level coarse evidence;
                                 a positive hit means at least one slot
                                 in the phrase is plausibly from a numeral
                                 / acronym tokenisation path. False
                                 positives are expected -- plan 03-07
                                 narrows via fe_token re-capture.
  3. compound_decomp_suspect  : phrase contains a hyphenated word or one
                                 of the known compound-decomp targets
                                 (`syn-`, `seashells`, `seashore`,
                                 `lighthouse`, `synthesizing`,
                                 `important`, `Sssss`). Coarse same way
                                 as #2; positive means the phrase has a
                                 compound candidate, not that the slot
                                 is necessarily one.
  4. one_symbol_shift         : exactly 1 of the 5 positions differs.
  5. prefix_mismatch          : there exists k in [1,4] such that
                                 engine_ctx5[0:k] == our_ctx5[0:k] AND
                                 set(engine_ctx5[k:]) is disjoint from
                                 set(our_ctx5[k:]). Engine and ours
                                 agree on the left context but the right
                                 context diverges completely.
  6. full_mismatch            : the 5-element sets are disjoint -- zero
                                 positional overlap at all.
  7. other                    : none of the above.

The priority order matters: compound_decomp_suspect is checked BEFORE
positional buckets because canonical FE-05 records (the `syn-` /
`seashells` family from REQUIREMENTS.md) would otherwise fire
prefix_mismatch first and miss the compound classification.

Inputs
------
- `.planning/phases/03-fe-convergence/03-FE-AUDIT.jsonl` (residue ledger)
- `spfy/test/oracle/corpus.jsonl`                          (phrase text)

Outputs
-------
- `c:/tmp/ctx_diff_subcluster.csv` (record_id, sub_cluster, evidence_field,
  shift_positions, engine_ctx5, our_ctx5, top_token_text)
- stdout: per-sub-cluster counts in descending order
- (with --write-back) `ctx_sub_cluster` field added to each ledger record;
  ledger snapshot via .bak rotation

CLI
---
  python ctx_diff_subcluster.py
    [--audit PATH]
    [--out PATH]
    [--corpus PATH]            # spfy/test/oracle/corpus.jsonl
    [--write-back]             # default: do not write back
    [--dry-run]                # explicit; same as omitting --write-back

Acceptance: at least one sub-cluster has >= 30 records. Below threshold
the classifier is too granular and the user is surfaced to.
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import re
import shutil
import sys
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

PROJECT_ROOT = Path(__file__).resolve().parents[3]
LEDGER_DEFAULT = (PROJECT_ROOT / ".planning" / "phases" /
                  "03-fe-convergence" / "03-FE-AUDIT.jsonl")
CORPUS_DEFAULT = PROJECT_ROOT / "spfy" / "test" / "oracle" / "corpus.jsonl"
OUT_DEFAULT = Path("c:/tmp/ctx_diff_subcluster.csv")
PLAN_TAG = "03-06"

SILENCE_HP_CLASSES = {64, 65}

# REQUIREMENTS.md-cited compound targets for the FE-05 case family.
COMPOUND_LEXEMES = (
    "syn-", "seashells", "seashore", "lighthouse",
    "synthesizing", "important", "sssss",
)

NUMERAL_RE = re.compile(r"\b\d+(?:\.\d+)?\b")
ACRONYM_RE = re.compile(r"\b[A-Z]{2,}\b")
HYPHEN_RE = re.compile(r"\w-\w")


def load_jsonl(path: Path) -> list[dict]:
    out: list[dict] = []
    for line in path.open("r", encoding="utf-8"):
        line = line.strip()
        if not line:
            continue
        try:
            out.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return out


def snapshot_ledger(path: Path) -> Path:
    n = 1
    while True:
        dst = (path.with_suffix(path.suffix + ".bak") if n == 1
               else path.with_suffix(path.suffix + f".bak.r{n}"))
        if not dst.exists():
            break
        n += 1
    shutil.copy2(path, dst)
    return dst


def record_id(rec: dict) -> str:
    return rec.get("phase2_record_id") or \
        f"{rec.get('corpus_id')}::{rec.get('utt_idx')}::{rec.get('slot_idx')}"


def load_corpus_text(path: Path) -> dict[str, str]:
    """Map corpus_id -> phrase text. Skips ill-formed rows."""
    out: dict[str, str] = {}
    if not path.exists():
        return out
    for rec in load_jsonl(path):
        cid = rec.get("id")
        txt = rec.get("text") or rec.get("phrase") or ""
        if cid and txt:
            out[cid] = txt
    return out


def shift_positions(engine: list[int], ours: list[int]) -> list[int]:
    """Indexes where engine_ctx5[i] != our_ctx5[i]."""
    n = min(len(engine), len(ours))
    return [i for i in range(n) if engine[i] != ours[i]]


def classify(rec: dict, corpus_text: dict[str, str]) -> tuple[str, str]:
    """Returns (sub_cluster, evidence_field).

    evidence_field is a short human-readable tag pointing at the input
    that triggered the bucket -- useful when scanning the CSV.
    """
    eng = rec.get("engine_ctx5") or []
    our = rec.get("our_ctx5") or []
    if not eng or not our:
        return ("other", "no_ctx5")

    # 1. boundary_silence_suspect
    for pos in (0, 4):
        if pos >= len(eng) or pos >= len(our):
            continue
        eng_sil = eng[pos] in SILENCE_HP_CLASSES
        our_sil = our[pos] in SILENCE_HP_CLASSES
        if eng_sil != our_sil:
            return ("boundary_silence_suspect", f"ctx5[{pos}]_silence_xor")

    cid = rec.get("corpus_id")
    phrase = corpus_text.get(cid, "") if cid else ""

    # 2. numeral_acronym_suspect (phrase-level coarse evidence)
    if phrase:
        if NUMERAL_RE.search(phrase):
            return ("numeral_acronym_suspect", "numeral_in_phrase")
        if ACRONYM_RE.search(phrase):
            return ("numeral_acronym_suspect", "acronym_in_phrase")

    # 3. compound_decomp_suspect (FE-05 case family)
    if phrase:
        plow = phrase.lower()
        if HYPHEN_RE.search(phrase):
            return ("compound_decomp_suspect", "hyphenated_token_in_phrase")
        for lex in COMPOUND_LEXEMES:
            if lex in plow:
                return ("compound_decomp_suspect", f"compound_lexeme={lex}")

    # 4. one_symbol_shift
    sp = shift_positions(eng, our)
    if len(sp) == 1:
        return ("one_symbol_shift", f"shift@{sp[0]}")

    # 5. prefix_mismatch
    n = min(len(eng), len(our))
    for k in range(1, n):
        if eng[:k] != our[:k]:
            continue
        if not set(eng[k:]) & set(our[k:]):
            return ("prefix_mismatch", f"prefix_match_until_k={k}")

    # 6. full_mismatch
    if not set(eng) & set(our):
        return ("full_mismatch", "set_disjoint")

    return ("other", "fallthrough")


def main(argv: Optional[list[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        description="Sub-classify open ctx_diff records into 7 buckets "
                    "(Phase 3 / 03-06 Deliverable 2).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("--audit", type=Path, default=LEDGER_DEFAULT)
    ap.add_argument("--out", type=Path, default=OUT_DEFAULT)
    ap.add_argument("--corpus", type=Path, default=CORPUS_DEFAULT)
    ap.add_argument("--write-back", action="store_true",
                    help="Write ctx_sub_cluster field back into the ledger "
                         "with .bak snapshot rotation.")
    ap.add_argument("--dry-run", action="store_true",
                    help="Explicit no-write mode; same as omitting --write-back.")
    args = ap.parse_args(argv)

    if not args.audit.exists():
        print(f"ERROR: audit ledger not found at {args.audit}", file=sys.stderr)
        return 2

    ledger = load_jsonl(args.audit)
    ctx_open = [r for r in ledger
                if r.get("fe_class") == "ctx_diff"
                and r.get("fe_status") == "open"]
    if not ctx_open:
        print("WARNING: no open ctx_diff records found", file=sys.stderr)
        return 0

    corpus_text = load_corpus_text(args.corpus)

    rows_out: list[dict] = []
    for rec in ctx_open:
        cluster, evidence = classify(rec, corpus_text)
        rows_out.append({
            "record_id": record_id(rec),
            "corpus_id": rec.get("corpus_id"),
            "utt_idx": rec.get("utt_idx"),
            "slot_idx": rec.get("slot_idx"),
            "sub_cluster": cluster,
            "evidence_field": evidence,
            "top_token_text": (corpus_text.get(rec.get("corpus_id"), "")
                               [:60].replace("\n", " ")),
            "engine_ctx5": json.dumps(rec.get("engine_ctx5") or []),
            "our_ctx5": json.dumps(rec.get("our_ctx5") or []),
            "shift_positions": json.dumps(shift_positions(
                rec.get("engine_ctx5") or [],
                rec.get("our_ctx5") or [])),
        })

    # CSV out.
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows_out[0].keys()))
        w.writeheader()
        for r in rows_out:
            w.writerow(r)

    # Distribution stdout.
    cnt = Counter(r["sub_cluster"] for r in rows_out)
    total = len(rows_out)
    print(f"ctx_diff sub-classification: {total} records")
    for sub, n in cnt.most_common():
        print(f"  {sub:32s}: {n:>4d} ({100.0*n/total:5.1f}%)")

    # Behavior gate.
    top_sub, top_n = cnt.most_common(1)[0]
    if top_n < 30:
        print(f"WARNING: dominant sub-cluster {top_sub}={top_n} < 30 "
              f"threshold; classifier likely too granular.", file=sys.stderr)
        # Still write the CSV; the verify gate is the assert.

    # Optional write-back.
    if args.write_back and not args.dry_run:
        snap = snapshot_ledger(args.audit)
        print(f"snapshot: {snap}", file=sys.stderr)

        # Build a map record_id -> sub_cluster.
        sc_map = {r["record_id"]: r["sub_cluster"] for r in rows_out}

        stamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        n_upd = 0
        for rec in ledger:
            rid = record_id(rec)
            if rid not in sc_map:
                continue
            rec["ctx_sub_cluster"] = sc_map[rid]
            prior = rec.get("audit_change_log") or ""
            rec["audit_change_log"] = (
                prior +
                f"\n{stamp} {PLAN_TAG}: ctx_diff sub-cluster="
                f"{sc_map[rid]} (Task 3)"
            )
            n_upd += 1

        tmp = args.audit.with_suffix(args.audit.suffix + ".tmp")
        with tmp.open("w", encoding="utf-8") as f:
            for rec in ledger:
                f.write(json.dumps(rec) + "\n")
        os.replace(tmp, args.audit)
        print(f"updated {n_upd} ledger records with ctx_sub_cluster",
              file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
