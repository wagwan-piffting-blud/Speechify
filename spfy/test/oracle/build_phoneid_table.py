"""build_phoneid_table.py - derive ARPAbet -> engine_phone_id from existing traces.

The hosted-FE 'ctx mismatch' bug (1782 mismatches in master_compare.py audit)
has a single root cause: Tom's VCF phoneset (used by spfy_phoneset_lookup
in fe_parse.c) doesn't match the engine's internal ARPAbet -> phone-id
mapping. RESUME.md proposed two fix paths; this script implements Path B
(empirical) using offline data we already have.

Method
------
For each corpus phrase with both an `fe_tree` and a `prsl_slot` trace:
  1. Walk fe_tree's `Segment` relation in order; record phoneme name list.
  2. Walk prsl_slot utt-0 events; record `ctx[2]` per slot.
  3. Engine encoding: slot[2k] = left half, slot[2k+1] = right half;
     ctx[2] = 2*phone_id (left) or 2*phone_id+1 (right).
     So phone_id = ctx[2] >> 1, and phoneme_idx = slot >> 1.
  4. Tally observations: arpabet -> {phone_id: count}.

Then writes spfy/data/tom_engine_phone_ids.csv with one row per
ARPAbet symbol. Aborts if any symbol maps to multiple phone-ids
(would indicate the encoding is context-dependent and Path B is the
wrong approach - we'd need Path A static analysis instead).

This is read-only against the trace corpus and only writes the CSV;
no synth code is touched.
"""

import json
import sys
from collections import defaultdict
from pathlib import Path

THIS = Path(__file__).resolve()
ORACLE = THIS.parent
REPO = THIS.parents[3]
TRACES = ORACLE / "traces"
CORPUS = ORACLE / "corpus.jsonl"
OUT_CSV = REPO / "spfy" / "data" / "en_us_engine_phone_ids.csv"


def load_segments(tid):
    """Return list of phoneme names from fe_tree's Segment relation."""
    p = TRACES / "fe_tree" / f"{tid}.jsonl"
    if not p.exists():
        return None
    with open(p) as f:
        for ln in f:
            d = json.loads(ln)
            if d.get("type") != "fe_tree":
                continue
            segs = []
            for ir in d.get("irs", []):
                if ir["rel"] == "Segment":
                    name = ir["feat"].get("name", {}).get("str")
                    if name:
                        segs.append(name)
            return segs
    return None


def load_ctx2(tid):
    """Return list of ctx[2] values per slot (utt-0 only), indexed by slot."""
    p = TRACES / "prsl_slot" / f"{tid}.jsonl"
    if not p.exists():
        return None
    by_slot = {}
    seen_zero = False
    in_utt0 = True
    with open(p) as f:
        for ln in f:
            if not ln.strip():
                continue
            d = json.loads(ln)
            if d.get("type") != "prsl_slot":
                continue
            if d["slot"] == 0:
                if seen_zero:
                    in_utt0 = False
                seen_zero = True
            if not in_utt0:
                continue
            s = d["slot"]
            ctx = d.get("ctx")
            if s not in by_slot and ctx and len(ctx) >= 3:
                by_slot[s] = ctx[2]
    if not by_slot:
        return None
    n = max(by_slot.keys()) + 1
    return [by_slot.get(i) for i in range(n)]


def main():
    items = []
    with open(CORPUS) as f:
        for ln in f:
            d = json.loads(ln)
            if d.get("mode") == "text":
                items.append(d)

    # arpabet -> {phone_id: count}
    obs = defaultdict(lambda: defaultdict(int))
    # arpabet -> set of (tid, phone_idx) for evidence
    examples = defaultdict(list)

    n_phrases_used = 0
    n_phrases_skipped = 0
    n_phon_misalign = 0

    for it in items:
        tid = it["id"]
        segs = load_segments(tid)
        ctx2 = load_ctx2(tid)
        if segs is None or ctx2 is None:
            n_phrases_skipped += 1
            continue
        # Each phoneme should map to 2 slots (L + R halfphone).
        # If counts don't agree, skip the phrase rather than fabricate
        # a mapping that might be wrong.
        if len(ctx2) != 2 * len(segs):
            n_phon_misalign += 1
            continue
        n_phrases_used += 1
        for k, ph in enumerate(segs):
            left = ctx2[2 * k]
            right = ctx2[2 * k + 1]
            # Validate halfphone encoding: right == left + 1
            if left is None or right is None:
                continue
            if right != left + 1:
                # Encoding violated - bail loudly
                print(f"WARN {tid} seg[{k}]={ph}: "
                      f"ctx[2] L={left} R={right} (expected R=L+1)",
                      file=sys.stderr)
                continue
            phone_id = left // 2
            obs[ph][phone_id] += 1
            if len(examples[ph]) < 3:
                examples[ph].append((tid, k))

    print(f"# Phrases used: {n_phrases_used}")
    print(f"# Phrases skipped (missing trace): {n_phrases_skipped}")
    print(f"# Phrases with phon/slot mismatch:  {n_phon_misalign}")
    print(f"# Distinct ARPAbet symbols seen:   {len(obs)}")
    print()

    # Consistency check: each ARPAbet should map to exactly one phone-id.
    ambiguous = []
    rows = []
    for ph in sorted(obs):
        m = obs[ph]
        if len(m) > 1:
            ambiguous.append((ph, dict(m)))
        # Take the dominant mapping (almost always the only one)
        best_id, best_n = max(m.items(), key=lambda x: x[1])
        total = sum(m.values())
        rows.append((ph, best_id, total, total == best_n))

    if ambiguous:
        print(f"WARN: {len(ambiguous)} symbols map to multiple phone-ids:")
        for ph, m in ambiguous:
            print(f"   {ph!r}: {m}   examples: {examples[ph]}")
        print()
        print("If counts are very imbalanced, the minority is probably noise")
        print("(e.g. trace artefacts on phrase boundaries). If they're close,")
        print("the encoding is context-dependent and Path B is wrong; revisit")
        print("with Path A static analysis of the DLL.")
        print()

    # Write CSV
    OUT_CSV.parent.mkdir(parents=True, exist_ok=True)
    with open(OUT_CSV, "w", newline="") as f:
        f.write("arpabet,engine_phone_id,n_observations,unanimous\n")
        for ph, pid, n, unan in rows:
            f.write(f"{ph},{pid},{n},{int(unan)}\n")
    print(f"# wrote {OUT_CSV}  ({len(rows)} symbols)")
    print()
    print(f"{'arpabet':<8} {'pid':>4} {'n_obs':>6} {'unan':>5}")
    for ph, pid, n, unan in rows:
        print(f"{ph:<8} {pid:>4} {n:>6} {'y' if unan else 'N':>5}")


if __name__ == "__main__":
    main()
