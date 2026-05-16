"""Generate a BATCHED sweep-corpus for POS capture.

Single calls to spfy_dumpwav cost ~6 sec each on the user's box, so a
naive 36,724-word individual sweep would take ~60 hours. We batch by
packing N words per utterance, period-separated:

    text: "word1. word2. word3. ... wordN."

Engine splits at `.` into N separate phrases (same as in stage_textnorm),
so each word becomes its own utterance with a single Word IR. The
fe_tree hook captures all Word IRs from all phrases in the same call,
giving us N (word, pos) pairs per synth invocation.

Default batch size 100: 36k / 100 = 368 synth calls × 6 sec = ~37 min.
With --shard 0/4..3/4 across 4 sessions: ~10 min total wall clock.

Usage:
  python pos_sweep_corpus.py [--limit N] [--out PATH] [--shard k/N]
                              [--batch-size N]

Single-word context note: even period-separated, the engine's POS
tagger is mostly lexical (a/the/is are det/det/aux regardless of
context). Truly ambiguous words ("falls" = noun_verb, "today" = adv/noun)
will land on the engine's default tagging for the isolated context;
that matches what our LTS pipeline sees too.
"""
from __future__ import annotations
import argparse
import json
import os

DEFAULT_DICT = "c:/tmp/fe_dict.jsonl"
DEFAULT_OUT  = "c:/tmp/pos_sweep_corpus.jsonl"
DEFAULT_BATCH = 100


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--dict", default=DEFAULT_DICT)
    p.add_argument("--out",  default=DEFAULT_OUT)
    p.add_argument("--limit", type=int, default=0,
                   help="Only emit first N words (0 = all)")
    p.add_argument("--shard", default="",
                   help="k/N — emit only shard k of N (0-indexed)")
    p.add_argument("--batch-size", type=int, default=DEFAULT_BATCH,
                   help=f"words per synth call (default {DEFAULT_BATCH})")
    args = p.parse_args()

    shard_k = shard_n = None
    if args.shard:
        try:
            k_s, n_s = args.shard.split("/")
            shard_k = int(k_s); shard_n = int(n_s)
        except Exception:
            raise SystemExit("invalid --shard, expected k/N")

    seen_lower = set()
    rows = []
    with open(args.dict) as fp:
        for line in fp:
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            w = (d.get("word") or "").strip()
            if not w: continue
            wl = w.lower()
            if wl in seen_lower: continue
            seen_lower.add(wl)
            rows.append(wl)

    rows.sort()
    if args.limit > 0:
        rows = rows[:args.limit]
    if shard_n is not None:
        rows = [w for i, w in enumerate(rows) if i % shard_n == shard_k]

    # Sanitise: skip words with characters that would break the batched
    # text (spaces, periods, quotes, etc.). Most registry-A words are
    # alphabetic, but be defensive.
    def is_clean(w: str) -> bool:
        return all(c.isalpha() or c in "'-" for c in w)
    clean_rows = [w for w in rows if is_clean(w)]
    skipped = len(rows) - len(clean_rows)

    # Pack into batches of batch_size words per text entry.
    bsz = max(1, args.batch_size)
    batches = [clean_rows[i:i + bsz] for i in range(0, len(clean_rows), bsz)]

    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".",
                exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as fp:
        for i, batch in enumerate(batches):
            text = ". ".join(batch) + "."
            fp.write(json.dumps({
                "id":    f"pos_{i:05d}",
                "mode":  "text",
                "text":  text,
                "tags":  ["pos_sweep", "batched"],
                "words": batch,           # list of words in this batch
            }) + "\n")

    n_words = len(clean_rows)
    n_batches = len(batches)
    sec_per_batch = 6  # user-reported on best Windows box
    est_sec = n_batches * sec_per_batch
    print(f"Wrote {n_batches} batched entries ({n_words} words, "
          f"{bsz}/batch) -> {args.out}")
    if skipped:
        print(f"  skipped {skipped} words with non-alpha chars")
    print()
    print(f"Estimate at {sec_per_batch}s/batch:")
    print(f"  serial  : {est_sec // 60}m {est_sec % 60}s "
          f"({n_batches} synth calls)")
    print(f"  4-shard : ~{est_sec // 4 // 60}m {(est_sec // 4) % 60}s "
          f"(use --shard 0/4..3/4)")
    print()
    print("Next steps:")
    print(f"  1. Start Speechify.exe (server) on Windows")
    print(f"  2. python spfy/test/oracle/run_frida_capture.py "
          f"--hook fe_tree --corpus {args.out}")
    print(f"     (populates spfy/test/oracle/traces/fe_tree/pos_*.jsonl)")
    print(f"  3. python spfy/tools/pos_sweep_aggregate.py "
          f"--traces spfy/test/oracle/traces/fe_tree "
          f"--out spfy/data/baked_pos.csv")
    print(f"  4. python spfy/tools/baked_pos_to_c.py "
          f"--csv spfy/data/baked_pos.csv "
          f"--out spfy/src/fe/baked_pos.c")
    print(f"  5. (Re)build spfy_synth and re-run audit_focused.py")


if __name__ == "__main__":
    main()
