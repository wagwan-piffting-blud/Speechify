"""Aggregate POS captures from a Frida sweep into a flat (word, pos) CSV.

Reads spfy/test/oracle/traces/fe_tree/pos_*.jsonl files (and any
text_*/spr_* files for additional coverage), extracts the dominant POS
per word.

Usage:
  python pos_sweep_aggregate.py [--traces DIR] [--out CSV]
                                [--include-corpus]

Output columns:
  word          — lowercase
  pos           — most-common POS observed
  n_observed    — how many times the (word, pos) pairing was seen
  n_total       — how many times the word was seen across all POS
  alt_pos       — semicolon-list of secondary POS (pos:count)
  open_class    — 1 if pos in {noun, adj, verb, adv, interj, quant,
                  noun_adj, noun_verb}, else 0 (drives accent gating)

The open_class bit is the actionable output: at LTS time, gate accent
on this bit. Closed-class words (det/aux/most-prep) deaccent.
"""
from __future__ import annotations
import argparse
import csv
import json
import os
from collections import Counter, defaultdict

DEFAULT_TRACES = os.path.expanduser("~/Documents/Speechify/spfy/test/oracle/traces/fe_tree")
DEFAULT_OUT    = os.path.expanduser("~/Documents/Speechify/spfy/data/baked_pos.csv")

OPEN_CLASS_POS = {
    # Pure open classes (always accent)
    "noun", "adj", "verb", "adv", "interj", "quant",
    # Combinations of open classes (always accent)
    "noun_adj", "noun_verb", "verb_adj", "noun_verb_adj", "adj_adv",
    # Context-dependent (kept "open" so spfy_synth's FUNC_WORDS fallback
    # handles them — baked_pos can't disambiguate from single-word context).
    "pro", "pro2", "wh", "undef",
}


def load_traces(trace_dir: str, only_pos: bool) -> dict:
    word_pos = defaultdict(Counter)  # word -> Counter[pos]
    if not os.path.isdir(trace_dir):
        raise SystemExit(f"trace dir not found: {trace_dir}")
    for fn in sorted(os.listdir(trace_dir)):
        if only_pos and not fn.startswith("pos_"):
            continue
        if os.path.getsize(os.path.join(trace_dir, fn)) == 0:
            continue
        with open(os.path.join(trace_dir, fn)) as fp:
            for line in fp:
                try: d = json.loads(line)
                except json.JSONDecodeError: continue
                for ir in d.get("irs", []):
                    if ir.get("rel") != "Word": continue
                    feat = ir.get("feat", {})
                    nm_v = feat.get("name")
                    nm = nm_v.get("str") if isinstance(nm_v, dict) else nm_v
                    if not nm or nm == "_NULL_": continue
                    pos_v = feat.get("pos")
                    pos = pos_v.get("str") if isinstance(pos_v, dict) else pos_v
                    if not pos: continue
                    word_pos[nm.lower()][pos] += 1
    return word_pos


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--traces", default=DEFAULT_TRACES)
    p.add_argument("--out",    default=DEFAULT_OUT)
    p.add_argument("--include-corpus", action="store_true",
                   help="Also include text_*/spr_* traces (default: only "
                        "pos_* sweep entries)")
    args = p.parse_args()

    word_pos = load_traces(args.traces, only_pos=not args.include_corpus)
    if not word_pos:
        raise SystemExit("No POS data found. Did you run the Frida sweep?")

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", newline="", encoding="utf-8") as fp:
        w = csv.writer(fp)
        w.writerow(["word", "pos", "n_observed", "n_total",
                    "alt_pos", "open_class"])
        for word in sorted(word_pos.keys()):
            c = word_pos[word]
            primary, n_obs = c.most_common(1)[0]
            n_total = sum(c.values())
            alt = ";".join(f"{p}:{n}" for p, n in c.most_common()[1:])
            open_class = 1 if primary in OPEN_CLASS_POS else 0
            w.writerow([word, primary, n_obs, n_total, alt, open_class])

    print(f"Wrote {len(word_pos)} entries -> {args.out}")
    open_n = sum(1 for word, c in word_pos.items()
                 if c.most_common(1)[0][0] in OPEN_CLASS_POS)
    closed_n = len(word_pos) - open_n
    print(f"  open-class: {open_n}, closed-class: {closed_n}")
    pos_dist = Counter()
    for word, c in word_pos.items():
        pos_dist[c.most_common(1)[0][0]] += 1
    print("  POS distribution:")
    for pos, n in pos_dist.most_common():
        oc = " (open)" if pos in OPEN_CLASS_POS else " (closed)"
        print(f"    {pos:<15} {n:>5}{oc}")


if __name__ == "__main__":
    main()
