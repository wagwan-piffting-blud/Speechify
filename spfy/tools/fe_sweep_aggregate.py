"""Aggregate FE-input captures from a Frida sweep into spfy/data/baked_fe.csv.

Reads spfy/test/oracle/traces/fe_tree/*.jsonl, extracts per-word FE
output (phoneme sequence + sub-phone states + voicing + stress +
syllable boundaries), and writes a flat CSV consumed by
spfy/tools/baked_fe_to_c.py.

Output columns (one row per unique word):

  word              -- lowercase
  phoneme_seq       -- pipe-delimited vocab IDs (uint16)
  sub_phone_states  -- pipe-delimited per-phoneme HMM-state counts (uint8)
  voicing_per_state -- pipe-delimited 0/1 per state (flattened)
  stress_per_syl    -- pipe-delimited 0/1/2 per syllable
  syl_boundaries    -- pipe-delimited phon-index where each syllable starts
  n_observed        -- count of times this word's (canonical) sequence was seen
  alt_seq_count     -- number of distinct phoneme sequences observed
                       (>1 flags ambiguity for downstream review)

The aggregation strategy is "take the modal phoneme sequence per word",
mirroring pos_sweep_aggregate.py's "most-common POS per word". When a
word's sub-phone-states / voicing / stress / syl_boundaries vary across
observations of the same modal phoneme sequence, we likewise take the
modal value.

Usage:
  python fe_sweep_aggregate.py [--traces DIR] [--out CSV]
                                [--include-corpus]

By default we read only fe_sweep_* traces (the sweep entries from
fe_sweep_corpus.py). With --include-corpus, regular text_*/edge_*/etc.
traces are merged in for additional coverage.

User-driven sweep procedure (this script is step 3 of 5):

  1. Start Speechify.exe (server) on Windows
  2. python spfy/test/oracle/fe_sweep_corpus.py
  3. python spfy/test/oracle/run_frida_capture.py \\
         --hook fe_tree --corpus c:/tmp/fe_sweep_corpus.jsonl
  4. python spfy/tools/fe_sweep_aggregate.py        <-- you are here
  5. python spfy/tools/baked_fe_to_c.py
"""
from __future__ import annotations
import argparse
import csv
import json
import os
from collections import Counter, defaultdict
from pathlib import Path
from typing import Optional

PROJECT_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_TRACES = PROJECT_ROOT / "spfy" / "test" / "oracle" / "traces" / "fe_tree"
DEFAULT_OUT = PROJECT_ROOT / "spfy" / "data" / "baked_fe.csv"


def _join_ints(seq: list[int]) -> str:
    return "|".join(str(int(x)) for x in seq)


def _load_word_obs(trace_dir: Path, only_sweep: bool) -> dict:
    """Walk fe_tree jsonl files. For each Word IR, harvest:
       - phoneme_seq (from descendants via the SylStructure relation)
       - sub_phone_states (per-phoneme state count)
       - voicing_per_state
       - stress_per_syl
       - syl_boundaries

    The current fe_tree hook emits raw relation IRs only; phoneme /
    sub-phone / voicing / stress data is reachable via the SylStructure
    relation (parent=Syllable, daughter=Segment) but the hook does NOT
    flatten that into per-word arrays. So this aggregator must traverse
    the IR graph itself.

    Returns:
        dict[word_lower] -> list of observation tuples
            (phoneme_seq, sub_phone_states, voicing_per_state,
             stress_per_syl, syl_boundaries)
    """
    if not trace_dir.is_dir():
        raise SystemExit(f"trace dir not found: {trace_dir}")

    word_obs: dict[str, list[tuple]] = defaultdict(list)

    for fn in sorted(os.listdir(trace_dir)):
        if only_sweep and not fn.startswith("fe_sweep_"):
            continue
        full = os.path.join(trace_dir, fn)
        if os.path.getsize(full) == 0:
            continue
        try:
            with open(full, encoding="utf-8") as fp:
                for line in fp:
                    try:
                        rec = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if rec.get("type") != "fe_tree":
                        continue
                    obs_by_word = _harvest_words(rec)
                    for word, obs in obs_by_word.items():
                        word_obs[word].append(obs)
        except OSError:
            continue
    return word_obs


def _harvest_words(rec: dict) -> dict[str, tuple]:
    """Walk the IR graph in a single fe_tree record.

    Builds, per Word IR, a tuple (phoneme_seq, sub_phone_states,
    voicing_per_state, stress_per_syl, syl_boundaries). Returns empty
    dict for entries that don't have the SylStructure relation populated
    (which is normal for batched single-word phrases when the trace
    captured only Word IRs without descending).

    The traversal is permissive: if any sub-element is missing, that
    field is empty for the word. Downstream alt_seq_count flags such
    partial observations.
    """
    irs = rec.get("irs", [])
    if not irs:
        return {}

    by_ir = {ir.get("ir"): ir for ir in irs if ir.get("ir")}
    words = [ir for ir in irs if ir.get("rel") == "Word"]
    if not words:
        return {}

    out: dict[str, tuple] = {}
    for w in words:
        feat = w.get("feat", {})
        nm_v = feat.get("name")
        nm = nm_v.get("str") if isinstance(nm_v, dict) else nm_v
        if not nm or nm == "_NULL_" or not isinstance(nm, str):
            continue
        word = nm.lower()

        phoneme_seq: list[int] = []
        sub_phone_states: list[int] = []
        voicing: list[int] = []
        stress_per_syl: list[int] = []
        syl_boundaries: list[int] = []

        # Walk SylStructure: Word -> Syllable -> Segment (phoneme).
        # The exact traversal pattern depends on the engine's IR shape;
        # the conservative version below pulls anything reachable via
        # `daughter` chains within the same record.
        wid = w.get("ir")
        syl_start_phon_idx = 0
        syl_node = by_ir.get(w.get("daughter"))
        while syl_node and syl_node.get("rel") in ("Syllable", "SylStructure"):
            sfeat = syl_node.get("feat", {})
            stress = sfeat.get("stress")
            stress_val = stress.get("str") if isinstance(stress, dict) else stress
            try:
                stress_per_syl.append(int(stress_val) if stress_val is not None else 0)
            except (ValueError, TypeError):
                stress_per_syl.append(0)
            syl_boundaries.append(syl_start_phon_idx)

            seg = by_ir.get(syl_node.get("daughter"))
            while seg and seg.get("rel") in ("Segment", "SegmentStructure"):
                seg_feat = seg.get("feat", {})
                phon = seg_feat.get("phone") or seg_feat.get("name")
                phon_val = phon.get("payload") if isinstance(phon, dict) else phon
                try:
                    phoneme_seq.append(int(phon_val) if phon_val is not None else 0)
                except (ValueError, TypeError):
                    phoneme_seq.append(0)
                # sub-phone state count: assume 3 (HMM 3-state) when unknown.
                ns = seg_feat.get("n_states") or 3
                try:
                    ns_int = int(ns.get("payload")) if isinstance(ns, dict) else int(ns)
                except (ValueError, TypeError):
                    ns_int = 3
                sub_phone_states.append(ns_int)
                # voicing: per-state 0/1; default all voiced for vowel-class
                # phones (Ghidra LUTs would normally provide; here we leave
                # empty when not in trace).
                v = seg_feat.get("voicing")
                if isinstance(v, list):
                    voicing.extend(int(x) for x in v)
                syl_start_phon_idx += 1
                seg = by_ir.get(seg.get("next"))
            syl_node = by_ir.get(syl_node.get("next"))

        out[word] = (
            phoneme_seq,
            sub_phone_states,
            voicing,
            stress_per_syl,
            syl_boundaries,
        )
    return out


def _modal_observation(obs_list: list[tuple]) -> Optional[tuple]:
    """Return the modal (most-common) observation tuple from a list of
    observations for the same word. Ties broken by first-seen."""
    if not obs_list:
        return None
    counts = Counter(tuple((tuple(x) if isinstance(x, list) else x)
                           for x in obs) for obs in obs_list)
    modal, _ = counts.most_common(1)[0]
    # Find first obs matching the modal tuple to recover list form.
    for obs in obs_list:
        if tuple((tuple(x) if isinstance(x, list) else x) for x in obs) == modal:
            return obs
    return obs_list[0]


def main() -> int:
    p = argparse.ArgumentParser(
        description="Aggregate fe_tree captures into spfy/data/baked_fe.csv "
                    "(Phase 3 / 03-06 Deliverable 3b).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    p.add_argument("--traces", type=Path, default=DEFAULT_TRACES)
    p.add_argument("--out", type=Path, default=DEFAULT_OUT)
    p.add_argument("--include-corpus", action="store_true",
                   help="Also include text_*/edge_*/etc traces (default: "
                        "only fe_sweep_* sweep entries).")
    args = p.parse_args()

    only_sweep = not args.include_corpus
    word_obs = _load_word_obs(args.traces, only_sweep)
    if not word_obs:
        raise SystemExit(
            f"No FE data found under {args.traces}. Did you run the Frida "
            f"sweep? (step 3 of the user-driven procedure in --help)")

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", newline="", encoding="utf-8") as fp:
        w = csv.writer(fp)
        w.writerow(["word", "phoneme_seq", "sub_phone_states",
                    "voicing_per_state", "stress_per_syl",
                    "syl_boundaries", "n_observed", "alt_seq_count"])
        for word in sorted(word_obs.keys()):
            obs_list = word_obs[word]
            modal = _modal_observation(obs_list)
            if not modal:
                continue
            ph_seq, sps, voi, stress, bnds = modal
            # alt_seq_count = number of distinct phoneme sequences seen
            distinct = {tuple(o[0]) for o in obs_list}
            # n_observed = count of times THIS modal phoneme_seq was seen
            n_obs = sum(1 for o in obs_list
                        if tuple(o[0]) == tuple(ph_seq))
            w.writerow([
                word,
                _join_ints(ph_seq),
                _join_ints(sps),
                _join_ints(voi),
                _join_ints(stress),
                _join_ints(bnds),
                n_obs,
                len(distinct),
            ])

    n_total = len(word_obs)
    n_ambig = sum(1 for word, obs_list in word_obs.items()
                  if len({tuple(o[0]) for o in obs_list}) > 1)
    print(f"Wrote {n_total} word entries -> {args.out}")
    print(f"  ambiguous (>1 phoneme_seq observed): {n_ambig}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
