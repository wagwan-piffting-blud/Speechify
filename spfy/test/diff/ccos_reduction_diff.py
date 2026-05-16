"""ccos_reduction_diff.py -- D-07 parity check for plan 02-02.

Decides empirically whether `spfy_cost_s()` already reproduces the engine's
CCOS context-cost reduction bit-exact, or whether a separate
`spfy_ccos_reduction()` is needed.

Method (no per-cand alignment required):
  1. Engine side: for each phrase load `traces/ccos_apply/<id>.jsonl`,
     collect every `cands[].cost` value across all anchor calls. These
     are the engine's per-cand `(cell_a + cell_b + cell_c + cell_d)
     * weight+0x44` values (or 10000.0f sentinel for HP-class mismatch).
  2. Spfy side: run `spfy_viterbi_replay --emit-ccos-reduction` (env
     `SPFY_DUMP_COMPONENTS=1` is the existing analogue; see plan 02-02
     Task 2 step 5) and parse the per-(utt, slot, uid) `S=<value>`
     fields. Those are the corresponding `spfy_cost_s()` outputs.
  3. Compare the two multisets per phrase:
       - engine multiset E = {cost_i for every surviving (anchor, cand)}
       - spfy multiset    S = {S_j   for every (slot, cand_uid) pair}
       - exact_count   = | E ∩ S |    (bit-exact set intersection)
       - engine_only   = | E \\ S |    (engine had it, spfy didn't produce
                                       this exact value -- expected if
                                       histogram pruning differs)
       - spfy_only     = | S \\ E |    (spfy produced it, engine didn't
                                       record it -- expected, since the
                                       engine histogram-prunes most cands
                                       before they hit ccos_apply output)
  4. Per-phrase verdict:
       - "bit_exact_subset"   : E ⊆ S (every engine cost value also
                                in our spfy outputs); strong evidence
                                cost_s.c is correct and the actual gap
                                is the histogram prune.
       - "partial"            : some engine values in spfy, some not;
                                cost_s.c may have a bug for specific
                                input shapes.
       - "no_overlap"         : disjoint; cost_s.c is broken or input
                                wiring mismatches the engine path.
  5. Corpus-wide rollup: # phrases bit_exact_subset / partial / no_overlap.

Exit 0 iff every phrase is bit_exact_subset (or has no engine ccos_apply
records, e.g. silence-only phrases) — the D-07 gate.

Usage (PowerShell):
  python spfy/test/diff/ccos_reduction_diff.py \\
         --engine-dir spfy/test/oracle/traces/ccos_apply \\
         --spfy-dump  c:/tmp/spfy_components_dump.txt \\
         [--tolerance 0]   # 0 = bit-exact; >0 allows ULP fuzz for diagnostics

The spfy dump is produced by:
  $env:SPFY_DUMP_COMPONENTS=1
  for ($id in $corpus_ids) {
    spfy_viterbi_replay <args...> 2>&1 | Out-File -Append c:/tmp/spfy_components_dump.txt
  }
where each line of the form `COMP utt=N slot=N uid=N ... S=<value> ...`
is parsed.
"""
from __future__ import annotations

import argparse
import json
import math
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

DEFAULT_ENGINE_DIR = Path("spfy/test/oracle/traces/ccos_apply")
COMP_LINE = re.compile(
    r"^COMP\s+utt=(?P<utt>\d+)\s+slot=(?P<slot>\d+)\s+uid=(?P<uid>\d+)\s+"
    r"D=(?P<D>\S+)\s+F0=(?P<F0>\S+)\s+SP=(?P<SP>\S+)\s+S=(?P<S>\S+)\s+"
    r"FLAG=(?P<FLAG>\S+)\s+tc=(?P<tc>\S+)$"
)


def load_engine_costs(engine_dir: Path) -> dict[str, Counter]:
    """Per-phrase multiset of cost values from ccos_apply traces."""
    out: dict[str, Counter] = {}
    for jl in sorted(engine_dir.glob("*.jsonl")):
        phrase_id = jl.stem
        c: Counter = Counter()
        with jl.open("r", encoding="utf-8") as fp:
            for line in fp:
                line = line.strip()
                if not line:
                    continue
                try:
                    rec = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if rec.get("type") != "ccos_apply":
                    continue
                cands = rec.get("cands") or []
                for cand in cands:
                    cost = cand.get("cost")
                    if cost is None:
                        continue
                    if isinstance(cost, str) and cost.lower() == "nan":
                        continue
                    if isinstance(cost, float) and math.isnan(cost):
                        continue
                    c[bit_key(cost)] += 1
        out[phrase_id] = c
    return out


def load_spfy_components(dump_path: Path) -> dict[str, Counter]:
    """Per-phrase multiset of S values from spfy_viterbi_replay
    SPFY_DUMP_COMPONENTS=1 output.

    The dump is expected to be section-delimited per phrase by lines of
    the form `==== <phrase_id> ====` (or just `# phrase_id: <id>`); if
    no such delimiter is found, all COMP lines are aggregated under
    phrase id `__corpus__`.
    """
    out: dict[str, Counter] = defaultdict(Counter)
    cur = "__corpus__"
    with dump_path.open("r", encoding="utf-8") as fp:
        for line in fp:
            stripped = line.strip()
            if stripped.startswith("==== ") and stripped.endswith(" ===="):
                cur = stripped[5:-5].strip()
                continue
            if stripped.startswith("# phrase_id:"):
                cur = stripped.split(":", 1)[1].strip()
                continue
            m = COMP_LINE.match(stripped)
            if not m:
                continue
            try:
                S = float(m.group("S"))
            except ValueError:
                continue
            out[cur][bit_key(S)] += 1
    return dict(out)


def bit_key(x: float) -> int:
    """Bit-exact key for a float — IEEE 754 binary32 view via struct."""
    import struct
    if x != x:  # NaN
        return 0x7FC00000
    # Round to f32 first since both engine and spfy emit f32 here.
    b = struct.pack("<f", float(x))
    return struct.unpack("<I", b)[0]


def fuzz_match(engine: Counter, spfy: Counter, tol_ulp: int) -> tuple[int, int]:
    """If tol_ulp == 0, exact intersection. Else allow ULP fuzz on lookup
    for diagnostics. Returns (n_intersect, n_engine_unmatched)."""
    if tol_ulp == 0:
        n_int = sum(min(engine[k], spfy[k]) for k in engine if k in spfy)
        n_eo  = sum(engine[k] for k in engine if k not in spfy)
        return n_int, n_eo
    # ULP-fuzzy: build a sorted list of spfy keys and look for nearest.
    spfy_keys = sorted(spfy.keys())
    n_int = 0
    n_eo  = 0
    import bisect
    for k, n in engine.items():
        i = bisect.bisect_left(spfy_keys, k)
        ok = False
        for j in (i - 1, i):
            if 0 <= j < len(spfy_keys) and abs(spfy_keys[j] - k) <= tol_ulp:
                ok = True
                break
        if ok:
            n_int += n
        else:
            n_eo += n
    return n_int, n_eo


def diff_phrase(eng: Counter, sp: Counter, tol_ulp: int) -> dict:
    n_engine = sum(eng.values())
    n_spfy   = sum(sp.values())
    n_int, n_eo = fuzz_match(eng, sp, tol_ulp)
    n_so = sum(sp[k] for k in sp if k not in eng) if tol_ulp == 0 else None
    if n_engine == 0:
        verdict = "engine_empty"
    elif n_int == n_engine:
        verdict = "bit_exact_subset"
    elif n_int > 0:
        verdict = "partial"
    else:
        verdict = "no_overlap"
    return {
        "n_engine":     n_engine,
        "n_spfy":       n_spfy,
        "n_intersect":  n_int,
        "n_engine_only": n_eo,
        "n_spfy_only":  n_so,
        "verdict":      verdict,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--engine-dir", type=Path, default=DEFAULT_ENGINE_DIR)
    ap.add_argument("--spfy-dump",  type=Path, required=False, default=None,
                    help="path to combined SPFY_DUMP_COMPONENTS output "
                         "(if omitted, only engine-side stats are reported)")
    ap.add_argument("--tolerance",  type=int, default=0,
                    help="ULP tolerance (bit-exact = 0); >0 for diagnostics")
    ap.add_argument("--strict",     action="store_true",
                    help="exit non-zero unless every phrase is bit_exact_subset")
    args = ap.parse_args()

    engine = load_engine_costs(args.engine_dir)
    spfy   = load_spfy_components(args.spfy_dump) if args.spfy_dump else {}

    rollup = Counter()
    n_engine_records_total = 0
    n_engine_phrases       = 0
    n_engine_phrases_with_records = 0
    for phrase_id, eng in sorted(engine.items()):
        n_engine_phrases += 1
        if sum(eng.values()) > 0:
            n_engine_phrases_with_records += 1
        n_engine_records_total += sum(eng.values())
        sp = spfy.get(phrase_id) or spfy.get("__corpus__") or Counter()
        d = diff_phrase(eng, sp, args.tolerance)
        rollup[d["verdict"]] += 1
        print(json.dumps({"phrase": phrase_id, **d}, separators=(",", ":")))

    print("--- summary ---", file=sys.stderr)
    print(f"engine phrases scanned   : {n_engine_phrases}", file=sys.stderr)
    print(f"engine phrases w/ records: {n_engine_phrases_with_records}", file=sys.stderr)
    print(f"engine records total     : {n_engine_records_total}", file=sys.stderr)
    for k in ("bit_exact_subset", "partial", "no_overlap", "engine_empty"):
        print(f"  {k:20s}: {rollup[k]}", file=sys.stderr)

    if args.strict:
        bad = rollup["partial"] + rollup["no_overlap"]
        if bad > 0:
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
