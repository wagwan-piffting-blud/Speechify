"""Per-stage required-field + type validator for engine-trace JSONLs.

Hand-rolled schema dict transcribed from `TRACE_SCHEMA.md` per CONTEXT.md
D-13. NO `jsonschema`, NO `pydantic` — pure stdlib. The schema doc remains
the human-readable source of truth; this validator is a runtime
transcription. When the doc and the validator disagree, the validator
wins at runtime (Pitfall 2 in 01-RESEARCH.md). BLOCKER-05 mandates that
SCHEMAS be a true superset of TRACE_SCHEMA.md per record type, so
`prsl_slot.sp_target` and `inner_scorer.components` are present here as
nullable fields.

Walks `spfy/test/oracle/traces/<hook>/<id>.jsonl`, validates each
record, and emits one JSONL summary line per file. `--strict` exits
non-zero with `schema_violations: <N>` to stderr if any record is
malformed.

Unknown record types are reported as a WARNING (not failure); the
validator is forward-compatible with new hook record types added
between schema-doc updates.

Usage (PowerShell):
  python spfy/test/oracle/schema_check.py [TRACES_DIR]
                                          [--strict]
                                          [--report PATH]
                                          [--type STR]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Schema dict — transcribed from TRACE_SCHEMA.md (D-13).
# Each entry: record `type` -> { field_name: type | tuple[type, ...] }.
# Nullable fields use `type(None)` in the tuple per Pitfall 2 / RESEARCH Q*.
# BLOCKER-05: `prsl_slot.sp_target` and `inner_scorer.components` MUST
# be present here as nullable fields.
# ---------------------------------------------------------------------------
SCHEMAS: dict[str, dict[str, object]] = {
    # SOURCE: TRACE_SCHEMA.md ## hash_probe
    "hash_probe": {
        "n":         int,
        "uid_left":  int,
        "uid_right": int,
        "esi":       str,
        "stored":    int,
        "hit":       bool,
        "cost":      (float, int, type(None)),
    },
    # SOURCE: TRACE_SCHEMA.md ## prsl_lookup
    "prsl_lookup": {
        "n":      int,
        "args":   dict,
        "retval": (int, type(None)),
    },
    # SOURCE: TRACE_SCHEMA.md ## wsola_in / wsola_out
    "wsola_in": {
        "utt":     int,
        "n_units": int,
        "units":   list,
    },
    "wsola_out": {
        "utt":    int,
        "retval": (int, type(None)),
    },
    # SOURCE: TRACE_SCHEMA.md ## fe_tokens
    "fe_tokens": {
        "probe": dict,
    },
    # SOURCE: TRACE_SCHEMA.md ## ulaw_pair
    "ulaw_pair": {
        "sym":   str,
        "pairs": list,
    },
    # SOURCE: TRACE_SCHEMA.md ## prsl_slot — BLOCKER-05: sp_target added
    "prsl_slot": {
        "utt":       int,
        "slot":      int,
        "ctx":       list,
        "n_pool":    int,
        "pool":      list,
        "sp_target": (list, type(None)),
    },
    # SOURCE: TRACE_SCHEMA.md ## cart_walks (RETIRED — seed-32 only per D-15)
    "cart_walks": {
        "utt":       int,
        "slot":      int,
        "tree":      str,
        "leaf_id":   int,
        "questions": list,
    },
    # SOURCE: TRACE_SCHEMA.md ## cart_walker_args
    # ACTIVE — covers all 200 phrases per D-15
    "cart_walker_args": {
        "utt":  int,
        "slot": int,
        "tree": str,
        "args": dict,
    },
    # SOURCE: TRACE_SCHEMA.md ## inner_scorer — BLOCKER-05: components added
    "inner_scorer": {
        "utt":        int,
        "slot":       int,
        "uid":        int,
        "tc":         (float, int),
        "components": (dict, type(None)),
    },
    # SOURCE: TRACE_SCHEMA.md ## inner_scorer_durt — plan 02-05 D-17 Path R
    "inner_scorer_durt": {
        "utt":     int,
        "slot":    int,
        "mean":    (float, int),
        "inv_std": (float, int),
    },
    # SOURCE: TRACE_SCHEMA.md ## fe_tree
    "fe_tree": {
        "utt":   int,
        "words": list,
    },
    # SOURCE: TRACE_SCHEMA.md ## viterbi_dp
    "viterbi_dp": {
        "utt":   int,
        "phase": str,
    },
    # SOURCE: TRACE_SCHEMA.md ## ccos_apply — plan 02-02 D-05
    "ccos_apply": {
        "n_call":            int,
        "type_arg":          int,
        "first_hp":          int,
        "last_hp":           int,
        "group_idx":         int,
        "cklx_match_idx":    int,
        "n_input":           int,
        "n_kept":            int,
        "cost_w_44":         (float, int),
        "anchor_max_syl":    (float, int),
        "anchor_slope_syl":  (float, int),
        "anchor_max_word":   (float, int),
        "anchor_slope_word": (float, int),
        "cands":             list,
    },
}


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------

def _isinstance_strict(value: object, expected: object) -> bool:
    """Type check that handles the bool-is-int gotcha.

    Python's `bool` is a subclass of `int`, so `isinstance(True, int)`
    returns True. For schema fields declared as `int` (or a tuple
    containing `int` but NOT `bool`), reject `bool` values. For fields
    declared exactly as `bool` (or a tuple containing `bool`), accept
    bools normally.
    """
    if isinstance(expected, tuple):
        types = expected
    else:
        types = (expected,)
    if not isinstance(value, types):
        return False
    # Bool-narrows-int: if value is bool but bool is NOT in the
    # expected types, reject (since the field declares int, not bool).
    if isinstance(value, bool) and (bool not in types):
        return False
    return True


def validate_file(path: Path,
                  type_filter: str | None = None) -> dict:
    """Validate a single JSONL trace file.

    Returns: {hook, id, n_records, n_malformed, missing_fields,
              unknown_types}.

    Unknown record types accumulate in `unknown_types` (WARN; not
    counted toward `n_malformed`). Missing/typed-wrong fields each
    increment `n_malformed` and add an entry like `{type}.{fname}` or
    `{type}.{fname}:type` to `missing_fields`.
    """
    n_records = 0
    n_malformed = 0
    missing_fields: set[str] = set()
    unknown_types: set[str] = set()
    with path.open("r", encoding="utf-8") as fp:
        for line in fp:
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                n_malformed += 1
                continue
            if not isinstance(rec, dict):
                n_malformed += 1
                continue
            t = rec.get("type")
            if type_filter is not None and t != type_filter:
                continue
            n_records += 1
            schema = SCHEMAS.get(t) if isinstance(t, str) else None
            if schema is None:
                unknown_types.add(t if isinstance(t, str) else "<missing>")
                continue
            for fname, ftype in schema.items():
                if fname not in rec:
                    missing_fields.add(f"{t}.{fname}")
                    n_malformed += 1
                elif not _isinstance_strict(rec[fname], ftype):
                    missing_fields.add(f"{t}.{fname}:type")
                    n_malformed += 1
    return {
        "hook":           path.parent.name,
        "id":             path.stem,
        "n_records":      n_records,
        "n_malformed":    n_malformed,
        "missing_fields": sorted(missing_fields),
        "unknown_types":  sorted(unknown_types),
    }


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument(
        "traces_dir", nargs="?", type=Path,
        default=Path("spfy/test/oracle/traces"),
        help="root directory containing <hook>/<id>.jsonl files",
    )
    ap.add_argument(
        "--strict", action="store_true",
        help="exit non-zero on any malformed record",
    )
    ap.add_argument(
        "--report", type=Path, default=None,
        help="write per-file JSONL summary to PATH",
    )
    ap.add_argument(
        "--type", default=None,
        help="filter to a single record `type` (e.g. viterbi_dp)",
    )
    return ap.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)

    if not args.traces_dir.exists():
        # Empty / missing tree: report 0 files, exit 0 (acceptance).
        print("0 files, 0 malformed records, 0 unknown record types")
        return 0

    summaries: list[dict] = []
    n_malformed_total = 0
    unknown_total: set[str] = set()
    # Flat 2-level glob — DO NOT rglob (avoid sweeping unrelated files).
    for path in sorted(args.traces_dir.glob("*/*.jsonl")):
        summary = validate_file(path, args.type)
        summaries.append(summary)
        n_malformed_total += summary["n_malformed"]
        for ut in summary["unknown_types"]:
            unknown_total.add(ut)

    if args.report is not None:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        with args.report.open("w", encoding="utf-8") as fp:
            for s in summaries:
                fp.write(json.dumps(s, separators=(",", ":")) + "\n")

    print(f"{len(summaries)} files, {n_malformed_total} malformed records, "
          f"{len(unknown_total)} unknown record types")

    if args.strict and n_malformed_total > 0:
        print(f"schema_violations: {n_malformed_total}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
