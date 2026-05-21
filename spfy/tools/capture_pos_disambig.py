"""Capture (word, prev_word, next_word) -> POS triples from the DLL FE.

Drives spfy_fe_host_dump (which runs SWIttsFe-en-US.dll in-process via
our PE loader) over the audit corpus, extracts each <word(...) POS,stress
[...]> emission, and aggregates into a context-keyed POS table.

Output: spfy/src/fe_internal/pos_context.{h,c} — sorted-by-key bsearch
lookup table consumed by analyze_word() as a pre-disambig step.

Falls back across three lookup tiers:
  1. (word, prev_word, next_word) — most specific
  2. (word, prev_word)             — bigram
  3. word alone                    — unigram (engine's single-best choice)
"""
from __future__ import annotations

import argparse
import collections
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Dict, Tuple

REPO = Path(__file__).resolve().parents[2]
CORPUS = REPO / "spfy" / "test" / "oracle" / "corpus.jsonl"
DUMP_EXE = Path(r"C:\tmp\spfy_build32\src\cli\spfy_fe_host_dump.exe")
VCF = REPO.parent / "Speechify" / "en-US" / "tom" / "tom.vcf"

TAGGED_RE = re.compile(
    r"\[fe_host\] tagged output \(\d+ bytes\): (.+?)(?=\n\[host_dump\]|\Z)",
    re.DOTALL,
)
WORD_RE = re.compile(
    r"<([A-Za-z'][A-Za-z0-9_'.]*)\s*\(\s*-?\d+\s*,\s*\d+\s*\)\s*([A-Za-z_]+)\s*,"
)


def parse_tagged(stream: str):
    """Return list of (word_lower, pos) in order of appearance."""
    return [(m.group(1).lower(), m.group(2)) for m in WORD_RE.finditer(stream)]


def run_dll(text: str) -> str:
    r = subprocess.run(
        [str(DUMP_EXE), str(VCF), text],
        capture_output=True, text=True, encoding="utf-8",
        errors="replace", timeout=60,
    )
    haystack = (r.stderr or "") + "\n" + (r.stdout or "")
    m = TAGGED_RE.search(haystack)
    return m.group(1).strip() if m else ""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--corpus", type=Path, default=CORPUS)
    ap.add_argument("--out-c",  type=Path,
                    default=REPO / "spfy" / "src" / "fe_internal" / "pos_context.c")
    ap.add_argument("--out-h",  type=Path,
                    default=REPO / "spfy" / "src" / "fe_internal" / "pos_context.h")
    args = ap.parse_args()

    if not DUMP_EXE.exists():
        sys.exit(f"missing: {DUMP_EXE}")

    rows = []
    with args.corpus.open(encoding="utf-8") as f:
        for ln in f:
            d = json.loads(ln)
            if d.get("mode") == "text":
                rows.append((d["id"], d["text"]))

    # Collect counts at three granularities so the bigram/trigram tier
    # selects the engine's most-common disambiguation.
    triple_counts: Dict[Tuple[str, str, str], collections.Counter] = \
        collections.defaultdict(collections.Counter)
    bigram_counts: Dict[Tuple[str, str], collections.Counter] = \
        collections.defaultdict(collections.Counter)
    unigram_counts: Dict[str, collections.Counter] = \
        collections.defaultdict(collections.Counter)

    n_phrases_ok = n_phrases_fail = 0
    n_words_seen = 0
    BOS = "^"
    EOS = "$"

    for tid, text in rows:
        tagged = run_dll(text)
        if not tagged:
            n_phrases_fail += 1
            continue
        n_phrases_ok += 1
        words_pos = parse_tagged(tagged)
        n_words_seen += len(words_pos)
        # Pad with BOS/EOS so triples at sentence edges have valid context
        padded = [(BOS, "")] + words_pos + [(EOS, "")]
        for i in range(1, len(padded) - 1):
            w, pos = padded[i]
            prev, _ = padded[i - 1]
            nxt, _ = padded[i + 1]
            triple_counts[(w, prev, nxt)][pos] += 1
            bigram_counts[(w, prev)][pos] += 1
            unigram_counts[w][pos] += 1

    # Resolve: pick most-common POS for each key; break ties by first-seen
    def winner(counter: collections.Counter) -> str:
        return counter.most_common(1)[0][0]

    triples = {k: winner(v) for k, v in triple_counts.items()}
    bigrams = {k: winner(v) for k, v in bigram_counts.items()}
    unigrams = {k: winner(v) for k, v in unigram_counts.items()}

    # Map POS strings to enum values matching spfy_pos_class_t in baked_pos.h.
    pos_enum_name = {
        "noun": "POS_NOUN", "adj": "POS_ADJ", "verb": "POS_VERB",
        "adv":  "POS_ADV",  "interj": "POS_INTERJ", "quant": "POS_QUANT",
        "noun_adj":      "POS_NOUN_ADJ",
        "noun_verb":     "POS_NOUN_VERB",
        "verb_adj":      "POS_VERB_ADJ",
        "noun_verb_adj": "POS_NOUN_VERB_ADJ",
        "adj_adv":       "POS_ADJ_ADV",
        "det": "POS_DET", "aux": "POS_AUX", "prep": "POS_PREP",
        "pro": "POS_PRO", "pro2": "POS_PRO2", "wh": "POS_WH",
        "conj": "POS_CONJ", "dem": "POS_DEM",
        "there": "POS_THERE", "not": "POS_NOT", "postpos": "POS_POSTPOS",
        "disambig": "POS_DISAMBIG", "other": "POS_OTHER", "undef": "POS_UNDEF",
    }

    def safe_c_string(s: str) -> str:
        # All keys come from regex [A-Za-z][A-Za-z0-9_'.]* and BOS/EOS sentinels.
        # Escape backslash and double-quote defensively.
        return s.replace("\\", "\\\\").replace("\"", "\\\"")

    def emit_table(name: str, items, key_to_string) -> str:
        sorted_keys = sorted(items.keys())
        lines = [f"static const struct {{ const char *key; uint8_t pos; }}\n    "
                 f"{name}[] = {{"]
        for k in sorted_keys:
            pos_str = items[k]
            enum_name = pos_enum_name.get(pos_str, "POS_UNKNOWN")
            key_s = key_to_string(k)
            lines.append(f'    {{ "{safe_c_string(key_s)}", {enum_name} }},')
        lines.append("};")
        lines.append(f"static const size_t {name}_n = "
                     f"sizeof({name}) / sizeof({name}[0]);")
        return "\n".join(lines)

    out_h = """\
/* AUTO-GENERATED by spfy/tools/capture_pos_disambig.py — do not edit.
 * Three-tier POS lookup captured from the DLL FE (SWIttsFe-en-US.dll).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SPFY_POS_CONTEXT_H
#define SPFY_POS_CONTEXT_H

#include "baked_pos.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the engine's empirically-observed POS for a word in the
 * given context (`prev_word` and `next_word` may be NULL; pass the
 * sentinel "^"/"$" strings for BOS/EOS). Falls through:
 *   triple → bigram → unigram → POS_UNKNOWN.
 * Returns POS_UNKNOWN only when the word was never seen during capture. */
spfy_pos_class_t spfy_pos_context_lookup(const char *word_lower,
                                          const char *prev_word_lower,
                                          const char *next_word_lower);

#ifdef __cplusplus
}
#endif

#endif
"""
    args.out_h.write_text(out_h, encoding="utf-8")

    out_c_lines = [
        "/* AUTO-GENERATED by spfy/tools/capture_pos_disambig.py — do not edit.",
        " * SPDX-License-Identifier: GPL-3.0-or-later */",
        '#include "pos_context.h"',
        "",
        "#include <stdint.h>",
        "#include <stdlib.h>",
        "#include <string.h>",
        "#include <stdio.h>",
        "",
        "/* Three tables: triple (word|prev|next), bigram (word|prev),",
        " * unigram (word). Each sorted lexicographically for bsearch. */",
        "",
        emit_table("TRIPLES", triples,
                   lambda k: f"{k[0]}|{k[1]}|{k[2]}"),
        "",
        emit_table("BIGRAMS", bigrams,
                   lambda k: f"{k[0]}|{k[1]}"),
        "",
        emit_table("UNIGRAMS", unigrams,
                   lambda k: k),
        "",
        "static int cmp_entry(const void *key, const void *elem) {",
        "    const char *k = (const char *)key;",
        "    const struct { const char *key; uint8_t pos; } *e = elem;",
        "    return strcmp(k, e->key);",
        "}",
        "",
        "spfy_pos_class_t spfy_pos_context_lookup(const char *word_lower,",
        "                                         const char *prev_word_lower,",
        "                                         const char *next_word_lower) {",
        "    if (!word_lower) return POS_UNKNOWN;",
        "    char buf[160];",
        "    void *hit;",
        "    /* Tier 1: triple */",
        "    if (prev_word_lower && next_word_lower) {",
        '        snprintf(buf, sizeof buf, "%s|%s|%s", word_lower, prev_word_lower, next_word_lower);',
        "        hit = bsearch(buf, TRIPLES, TRIPLES_n, sizeof(TRIPLES[0]), cmp_entry);",
        "        if (hit) return (spfy_pos_class_t)((const struct { const char *k; uint8_t p; } *)hit)->p;",
        "    }",
        "    /* Tier 2: bigram */",
        "    if (prev_word_lower) {",
        '        snprintf(buf, sizeof buf, "%s|%s", word_lower, prev_word_lower);',
        "        hit = bsearch(buf, BIGRAMS, BIGRAMS_n, sizeof(BIGRAMS[0]), cmp_entry);",
        "        if (hit) return (spfy_pos_class_t)((const struct { const char *k; uint8_t p; } *)hit)->p;",
        "    }",
        "    /* Tier 3: unigram */",
        "    hit = bsearch(word_lower, UNIGRAMS, UNIGRAMS_n, sizeof(UNIGRAMS[0]), cmp_entry);",
        "    if (hit) return (spfy_pos_class_t)((const struct { const char *k; uint8_t p; } *)hit)->p;",
        "    return POS_UNKNOWN;",
        "}",
    ]
    args.out_c.write_text("\n".join(out_c_lines), encoding="utf-8")

    print(f"# corpus={args.corpus.name}  phrases ok={n_phrases_ok} fail={n_phrases_fail}", file=sys.stderr)
    print(f"# words seen: {n_words_seen}", file=sys.stderr)
    print(f"# triples:  {len(triples):>6}", file=sys.stderr)
    print(f"# bigrams:  {len(bigrams):>6}", file=sys.stderr)
    print(f"# unigrams: {len(unigrams):>6}", file=sys.stderr)
    print(f"# wrote {args.out_c}", file=sys.stderr)
    print(f"# wrote {args.out_h}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
