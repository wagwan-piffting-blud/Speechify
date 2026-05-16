"""Stage-by-stage JSONL trace diff (native vs oracle).

  python stage_compare.py NATIVE.jsonl ORACLE.jsonl --stage hash_probe

The oracle JSONLs come from spfy/test/oracle/traces/<hook>/<id>.jsonl.
The native JSONLs come from running the C build with -DSPFY_TRACE=1
(or its CLI wrapper) on the same corpus entry.

Strict mode: every native record must field-for-field equal the oracle
record at the same index, with float comparisons within ULP_TOL ULPs.
On divergence, prints the first mismatching record and surrounding
context, then exits non-zero.

Schema is documented at spfy/test/oracle/TRACE_SCHEMA.md.
"""
from __future__ import annotations

import argparse
import json
import math
import struct
import sys
from pathlib import Path

ULP_TOL = 1   # tolerance for f32 equality (in ULPs)


def f32_ulps_apart(a: float, b: float) -> int | float:
    """Distance between two f32 values measured in ULPs. Returns inf if
    the operands have different signs or one is NaN."""
    if math.isnan(a) or math.isnan(b):
        return math.inf
    if a == b:
        return 0
    pa = struct.unpack("<i", struct.pack("<f", a))[0]
    pb = struct.unpack("<i", struct.pack("<f", b))[0]
    if (pa < 0) != (pb < 0):
        return math.inf
    return abs(pa - pb)


def load_stage(path: Path, stage: str | None) -> list[dict]:
    out: list[dict] = []
    with path.open("r", encoding="utf-8") as fp:
        for line in fp:
            line = line.strip()
            if not line:
                continue
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            if stage is None or d.get("type") == stage:
                out.append(d)
    return out


def field_diff(a: dict, b: dict, path: str = "") -> list[str]:
    """Recursive structural diff. Returns a list of human-readable
    differences, empty if equal."""
    diffs: list[str] = []
    keys = set(a.keys()) | set(b.keys())
    for k in sorted(keys):
        ka = path + "." + k if path else k
        if k not in a:
            diffs.append(f"+{ka} only-in-oracle")
            continue
        if k not in b:
            diffs.append(f"-{ka} only-in-native")
            continue
        va, vb = a[k], b[k]
        if isinstance(va, dict) and isinstance(vb, dict):
            diffs.extend(field_diff(va, vb, ka))
        elif isinstance(va, list) and isinstance(vb, list):
            if len(va) != len(vb):
                diffs.append(f"~{ka} list-len native={len(va)} oracle={len(vb)}")
                continue
            for i, (xa, xb) in enumerate(zip(va, vb)):
                if isinstance(xa, dict) and isinstance(xb, dict):
                    diffs.extend(field_diff(xa, xb, f"{ka}[{i}]"))
                elif xa != xb:
                    diffs.append(f"~{ka}[{i}] native={xa!r} oracle={xb!r}")
        elif isinstance(va, float) or isinstance(vb, float):
            try:
                ulps = f32_ulps_apart(float(va), float(vb))
            except (TypeError, ValueError):
                ulps = math.inf
            if ulps > ULP_TOL:
                diffs.append(f"~{ka} f32 native={va!r} oracle={vb!r} "
                             f"ulps={ulps}")
        elif va != vb:
            diffs.append(f"~{ka} native={va!r} oracle={vb!r}")
    return diffs


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("native", type=Path)
    ap.add_argument("oracle", type=Path)
    ap.add_argument("--stage", default=None,
                    help="filter to records of this 'type' field; default all")
    ap.add_argument("--max-diffs", type=int, default=10,
                    help="stop after this many diverging records")
    return ap.parse_args()


def main() -> int:
    args = parse_args()
    nat = load_stage(args.native, args.stage)
    ora = load_stage(args.oracle, args.stage)

    label = args.stage or "<all>"
    if len(nat) != len(ora):
        print(f"{label}: COUNT MISMATCH native={len(nat)} oracle={len(ora)}")
        return 1

    n = len(nat)
    diverged = 0
    for i, (a, b) in enumerate(zip(nat, ora)):
        diffs = field_diff(a, b)
        if diffs:
            diverged += 1
            print(f"{label}: record #{i} diverges ({len(diffs)} fields):")
            for d in diffs[:8]:
                print(f"   {d}")
            if diverged >= args.max_diffs:
                break

    if diverged == 0:
        print(f"{label}: {n}/{n} records identical")
        return 0
    print(f"\n{label}: {n - diverged}/{n} identical, {diverged} diverged")
    return 1


if __name__ == "__main__":
    sys.exit(main())
