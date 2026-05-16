"""Generate the stratified validation corpus (TEST-03 / D-01..D-05).

Calls each stratum's generate() in deterministic order and atomically
rewrites corpus.jsonl while preserving the existing seed rows
(text_001..text_030 + spr_001 + spr_002, 32 rows total) verbatim.

Order in output file:
    seed rows (in their original order)
    -> phn_001..phn_050   (D-01)
    -> mp_001..mp_050     (D-02)
    -> nat_001..nat_050   (D-03)
    -> edge_001..edge_NNN (D-04, >=50)

Usage:
  python generate_corpus.py [--corpus PATH]
                            [--stratum {phn,mp,nat,edge,all}]
                            [--regenerate]
                            [--verify-counts]

Atomic write: writes to corpus.jsonl.tmp + os.replace() so a partial
run cannot corrupt the file (atomic on Windows + POSIX).
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path


# Make `from strata import ...` work when this file is run from any cwd.
_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

from strata import single_phoneme, minimal_pairs, natural_phrases, edge_cases  # noqa: E402


DEFAULT_CORPUS: Path = _HERE / "corpus.jsonl"
SEED_ID_PREFIX: str = "text_"
SEED_SPR_PREFIX: str = "spr_"

# Order is the on-disk emit order after the seed block.
STRATA: dict[str, callable] = {
    "phn":  single_phoneme.generate,
    "mp":   minimal_pairs.generate,
    "nat":  natural_phrases.generate,
    "edge": edge_cases.generate,
}

# Map stratum short-name -> id-prefix for partition + replace logic.
_PREFIXES: dict[str, str] = {
    "phn":  "phn_",
    "mp":   "mp_",
    "nat":  "nat_",
    "edge": "edge_",
}


def _is_seed(row_id: str) -> bool:
    return row_id.startswith(SEED_ID_PREFIX) or row_id.startswith(SEED_SPR_PREFIX)


def _load_existing(path: Path) -> list[tuple[dict, str]]:
    """Read existing JSONL rows; preserves order AND original raw line bytes.

    Returns a list of (parsed_row_dict, raw_line_string_without_newline)
    tuples. The raw string is what we write back for seed rows so they
    remain byte-exact (the seed rows use compact separators that
    json.dumps(default) does NOT reproduce).
    """
    if not path.exists():
        return []
    out: list[tuple[dict, str]] = []
    with path.open("r", encoding="utf-8") as fp:
        for ln_num, line in enumerate(fp, 1):
            raw = line.rstrip("\n").rstrip("\r")
            stripped = raw.strip()
            if not stripped or stripped.startswith("#"):
                continue
            try:
                out.append((json.loads(stripped), raw))
            except json.JSONDecodeError as exc:
                raise SystemExit(
                    f"{path}:{ln_num}: JSON decode error: {exc}"
                ) from exc
    return out


def _atomic_write(path: Path, lines: list[str]) -> None:
    """Write JSONL `lines` (each WITHOUT trailing newline) atomically.

    Uses tmp file + os.replace() so a partial run cannot corrupt the
    target. Forces LF line terminators (newline="\n") so the same input
    yields byte-identical output on Windows and POSIX.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    with tmp.open("w", encoding="utf-8", newline="\n") as fp:
        for ln in lines:
            fp.write(ln + "\n")
    os.replace(tmp, path)


def _verify_counts(path: Path) -> int:
    """Re-read file; assert >=200 rows, no duplicate ids, per-stratum mins.

    Prints one line per stratum with its count. Returns 0 on success, 1
    on failure.
    """
    rows = [r for (r, _raw) in _load_existing(path)]
    n_total = len(rows)
    ids = [r["id"] for r in rows]
    n_unique = len(set(ids))
    counts = {p: 0 for p in _PREFIXES}
    n_seed = 0
    for r in rows:
        rid = r["id"]
        if _is_seed(rid):
            n_seed += 1
            continue
        for short, prefix in _PREFIXES.items():
            if rid.startswith(prefix):
                counts[short] += 1
                break

    print(f"seed={n_seed}")
    for short in ("phn", "mp", "nat", "edge"):
        print(f"{short}={counts[short]}")
    print(f"total={n_total}")

    ok = True
    if n_total < 200:
        print(f"FAIL: total {n_total} < 200", file=sys.stderr)
        ok = False
    if n_unique != n_total:
        dup = [i for i in ids if ids.count(i) > 1]
        print(f"FAIL: duplicate ids: {sorted(set(dup))[:10]}", file=sys.stderr)
        ok = False
    if counts["phn"] != 50:
        print(f"FAIL: phn={counts['phn']} != 50", file=sys.stderr)
        ok = False
    if counts["mp"] != 50:
        print(f"FAIL: mp={counts['mp']} != 50", file=sys.stderr)
        ok = False
    if counts["nat"] != 50:
        print(f"FAIL: nat={counts['nat']} != 50", file=sys.stderr)
        ok = False
    if counts["edge"] < 50:
        print(f"FAIL: edge={counts['edge']} < 50", file=sys.stderr)
        ok = False
    return 0 if ok else 1


def _row_to_line(row: dict) -> str:
    """Serialize a freshly-generated row to a JSONL line.

    Uses default separators -- compact mode is reserved for trace output;
    this corpus file is operator-readable.
    """
    return json.dumps(row)


def _build_output_lines(
    existing: list[tuple[dict, str]],
    stratum_filter: str,
    regenerate: bool,
) -> list[str]:
    """Compose final list of JSONL line strings (no trailing newline).

    Behaviour:
      * Seed rows (id starts with text_/spr_) are written back as their
        ORIGINAL raw line bytes (preserves byte-exact seed verbatim --
        json.dumps with default separators does NOT match the existing
        seed-row formatting).
      * Strata rows are emitted via _row_to_line(row).
      * If `regenerate` is True OR `stratum_filter == "all"`, every
        non-seed row is dropped from the existing file and ALL strata
        are regenerated.
      * If `stratum_filter` is a single stratum name (phn/mp/nat/edge),
        only that stratum's rows are replaced; the other strata keep
        whatever rows already exist on disk (preserves their order
        within the file).
    """
    seed_lines: list[str] = [raw for (row, raw) in existing if _is_seed(row["id"])]

    if stratum_filter == "all" or regenerate:
        new_blocks = [STRATA[k]() for k in ("phn", "mp", "nat", "edge")]
        new_lines = [_row_to_line(r) for blk in new_blocks for r in blk]
        return seed_lines + new_lines

    # Per-stratum replacement: keep existing non-seed rows AS-IS (their
    # raw bytes, so we don't perturb already-written strata), replace
    # only the target stratum.
    target_prefix = _PREFIXES[stratum_filter]
    by_short: dict[str, list[str]] = {"phn": [], "mp": [], "nat": [], "edge": []}
    for (row, raw) in existing:
        rid = row["id"]
        if _is_seed(rid):
            continue
        if rid.startswith(target_prefix):
            continue   # discard; will be regenerated
        for short, prefix in _PREFIXES.items():
            if rid.startswith(prefix):
                by_short[short].append(raw)
                break
    by_short[stratum_filter] = [_row_to_line(r) for r in STRATA[stratum_filter]()]

    out = list(seed_lines)
    for short in ("phn", "mp", "nat", "edge"):
        out.extend(by_short[short])
    return out


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS)
    ap.add_argument(
        "--stratum",
        choices=["phn", "mp", "nat", "edge", "all"],
        default="all",
    )
    ap.add_argument(
        "--regenerate",
        action="store_true",
        help="rewrite ALL non-seed rows; preserves seed (text_*/spr_*) rows.",
    )
    ap.add_argument("--verify-counts", action="store_true")
    args = ap.parse_args()

    corpus_path: Path = args.corpus.resolve()

    existing = _load_existing(corpus_path)
    final_lines = _build_output_lines(existing, args.stratum, args.regenerate)

    # Sanity guards before writing -- parse each line to extract its id.
    ids: list[str] = [json.loads(ln)["id"] for ln in final_lines]
    if len(set(ids)) != len(ids):
        dup = sorted({i for i in ids if ids.count(i) > 1})
        print(f"ERROR: duplicate ids in output: {dup[:10]}", file=sys.stderr)
        return 1

    _atomic_write(corpus_path, final_lines)
    print(f"Wrote {len(final_lines)} rows -> {corpus_path}")

    if args.verify_counts:
        return _verify_counts(corpus_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
