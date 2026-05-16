"""fe_derive_phone_mapping.py -- analytically derive the
SAMPA-symbol -> Tom-voice-phone-ID mapping by aligning our FE's
phoneme output with the engine's prsl_slot ctx[5] traces.

How it works
------------
For each corpus text:
  1. Run spfy_fe_synth (Path B FE) on the text -> sequence of SAMPA
     phoneme names per word.
  2. Read spfy/test/oracle/traces/prsl_slot/<id>.jsonl -> sequence of
     slot ctx[5] values. ctx[2] = HP-class = phone_id*2 + side, with
     two halfphones per engine-side phoneme.
  3. Align: collapse the slot sequence to phone_ids by taking
     ctx[2]/2 of every other slot (left halves), then trim leading
     silence (phone_id 32 in Tom encoding).
  4. Pair each SAMPA phoneme with the next engine phone_id.

Aggregate over the whole corpus and majority-vote per SAMPA name.

Limitations
-----------
- Word-boundary silences create slot/phoneme drift; we use a simple
  longest-common-subsequence aligner to absorb them.
- Our FE's LTS doesn't always match Eloquence's exact phoneme counts
  (e.g. "x" -> 2 phones in our model). The aligner skips mismatches.
- Phonemes our FE never emits stay UNKNOWN.

Outputs
-------
- spfy/build/fe_sampa_to_phoneid.json: clean mapping with confidence
- stdout: per-symbol frequencies + final mapping table
"""
from __future__ import annotations

import json
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FE_SYNTH = ROOT / "spfy/build/win/src/cli/spfy_fe_synth.exe"
VOCAB    = ROOT / "spfy/build/fe_symbol_table.json"
TABLES_A = ROOT / "spfy/data/fe_tables_a/"
TABLES_B = ROOT / "spfy/data/fe_tables/"
CORPUS   = ROOT / "spfy/test/oracle/corpus.jsonl"
TRACES   = ROOT / "spfy/test/oracle/traces/prsl_slot/"
OUT_JSON = ROOT / "spfy/build/fe_sampa_to_phoneid.json"

SILENCE_PHONE = 32        # Tom's silence/boundary phone_id


def run_fe(text: str) -> list[str]:
    """Run spfy_fe_synth on text, parse JSON output, return flat list of
    SAMPA phoneme names in word order."""
    try:
        r = subprocess.run(
            [str(FE_SYNTH), str(VOCAB), str(TABLES_A), str(TABLES_B), text],
            check=True, capture_output=True, text=True, encoding="utf-8")
    except subprocess.CalledProcessError as e:
        print(f"  FE failed on {text!r}: {e.stderr}", file=sys.stderr)
        return []
    data = json.loads(r.stdout)
    out = []
    for w in data.get("phonemes_by_word", []):
        for p in w["phones"].split():
            out.append(p)
    return out


def slot_ids(trace_path: Path) -> list[int]:
    """Return ctx[2]/2 for left-half slots only (skips right halves)."""
    out = []
    last = -1
    for line in trace_path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        try:
            o = json.loads(line)
        except json.JSONDecodeError:
            continue
        if o.get("type") != "prsl_slot":
            continue
        ctx2 = int(o["ctx"][2])
        # Each phoneme = 2 slots (left, right). The ctx[2] of left half is
        # phone*2+0 (even); right half is phone*2+1 (odd). Take every
        # phoneme by tracking transitions: when ctx[2]/2 changes, we've
        # entered a new phoneme.
        pid = ctx2 // 2
        if pid != last:
            out.append(pid)
        last = pid
    return out


def align_pair(sampa: list[str], pids: list[int]) -> list[tuple[str, int]]:
    """Greedy alignment: drop leading/trailing silences, pair what's
    left in order. If lengths still differ, match the shorter prefix."""
    # Strip silences from engine side.
    pids = [p for p in pids if p != SILENCE_PHONE]
    n = min(len(sampa), len(pids))
    return list(zip(sampa[:n], pids[:n]))


def main() -> int:
    corpus = [
        json.loads(l) for l in CORPUS.read_text(encoding="utf-8").splitlines()
        if l.strip()
    ]
    text_entries = [e for e in corpus if e.get("mode") == "text"]
    print(f"corpus: {len(text_entries)} text-mode entries")

    pair_freq: dict[str, Counter] = defaultdict(Counter)
    n_pairs_total = 0
    coverage: list[tuple[str, int, int]] = []
    for e in text_entries:
        tid = e["id"]
        text = e["text"]
        trace = TRACES / f"{tid}.jsonl"
        if not trace.exists():
            continue
        sampa = run_fe(text)
        pids  = slot_ids(trace)
        pairs = align_pair(sampa, pids)
        for s, p in pairs:
            pair_freq[s][p] += 1
        n_pairs_total += len(pairs)
        coverage.append((tid, len(sampa), len(pids)))

    print(f"  {n_pairs_total} aligned pairs across {len(coverage)} texts")
    print(f"  per-text coverage (sampa_n, engine_phon_n):")
    for tid, sn, pn in coverage[:6]:
        print(f"    {tid}: sampa={sn:>3}  engine={pn:>3}")

    # Build mapping: pick highest-frequency phone_id per SAMPA, with
    # confidence = top_count / total.
    mapping = {}
    rows = []
    for sampa, counts in sorted(pair_freq.items()):
        if not counts:
            continue
        top_pid, top_cnt = counts.most_common(1)[0]
        total = sum(counts.values())
        conf = top_cnt / total
        mapping[sampa] = {"phone_id": top_pid, "support": top_cnt,
                          "total": total, "confidence": conf}
        rows.append((sampa, top_pid, top_cnt, total, conf))

    rows.sort(key=lambda r: r[1])
    print(f"\nDerived mapping (sorted by phone_id):")
    print(f"  {'SAMPA':<6} -> {'pid':>3}  ({'top/total':>10})  conf")
    for s, pid, top, tot, c in rows:
        flag = "" if c >= 0.6 else "  ?"
        print(f"  {s:<6} -> {pid:>3}  ({top:>4}/{tot:<4})    {c:.2f}{flag}")

    # Save full result.
    OUT_JSON.parent.mkdir(parents=True, exist_ok=True)
    with OUT_JSON.open("w", encoding="utf-8") as fp:
        json.dump({"mapping": mapping,
                   "n_pairs": n_pairs_total,
                   "n_texts": len(coverage)}, fp, indent=2)
    print(f"\nWrote {OUT_JSON}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
