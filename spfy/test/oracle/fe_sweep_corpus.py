"""Generate a BATCHED sweep-corpus for FE-input capture.

Mirror of pos_sweep_corpus.py shape, but assembles its vocabulary from
the regression corpus + every trace phrase (227 phrases) rather than
from a pre-built dict file. Output is a JSONL of one entry per batch,
each batch packing N words period-separated so the engine emits N
single-word phrases per synth call (engine splits at "." in
stage_textnorm).

Default batch size 100. ETA at ~6 sec/synth-call on the user's box:
  - 8k unique words / 100 -> 80 batches -> ~8 min serial
  - 4-way shard: ~2 min wall

After this script writes c:/tmp/fe_sweep_corpus.jsonl, the user runs:

  1. Start Speechify.exe (server) on Windows
  2. python spfy/test/oracle/run_frida_capture.py --hook fe_tree \
       --corpus c:/tmp/fe_sweep_corpus.jsonl
  3. python spfy/tools/fe_sweep_aggregate.py
  4. python spfy/tools/baked_fe_to_c.py
  5. (Optionally rebuild and re-run audit_focused.py)

Usage:
  python fe_sweep_corpus.py [--limit N] [--out PATH]
                            [--shard k/N] [--batch-size N]
                            [--corpus PATH]
                            [--include-traces]

Only words containing alphabetic characters + apostrophe + hyphen are
emitted (sanitiser matches pos_sweep_corpus.py).
"""
from __future__ import annotations
import argparse
import json
import os
import tempfile
import re
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_CORPUS = PROJECT_ROOT / "spfy" / "test" / "oracle" / "corpus.jsonl"
DEFAULT_TRACE_DIR = PROJECT_ROOT / "spfy" / "test" / "oracle" / "traces" / "fe_tree"
DEFAULT_OUT = str(Path(tempfile.gettempdir()) / "fe_sweep_corpus.jsonl")
DEFAULT_BATCH = 100

WORD_RE = re.compile(r"[A-Za-z][A-Za-z'\-]*")


def words_from_corpus(corpus_path: Path) -> set[str]:
    out: set[str] = set()
    if not corpus_path.exists():
        return out
    for line in corpus_path.open("r", encoding="utf-8"):
        line = line.strip()
        if not line:
            continue
        try:
            d = json.loads(line)
        except json.JSONDecodeError:
            continue
        txt = d.get("text") or d.get("phrase") or ""
        for w in WORD_RE.findall(txt):
            out.add(w.lower())
    return out


def words_from_traces(trace_dir: Path) -> set[str]:
    """Pull word names from fe_tree relation IRs in existing trace files."""
    out: set[str] = set()
    if not trace_dir.is_dir():
        return out
    for fp in trace_dir.iterdir():
        if fp.suffix != ".jsonl":
            continue
        try:
            for line in fp.open("r", encoding="utf-8"):
                line = line.strip()
                if not line:
                    continue
                try:
                    d = json.loads(line)
                except json.JSONDecodeError:
                    continue
                for ir in d.get("irs", []):
                    if ir.get("rel") != "Word":
                        continue
                    feat = ir.get("feat", {})
                    nm_v = feat.get("name")
                    nm = nm_v.get("str") if isinstance(nm_v, dict) else nm_v
                    if nm and nm != "_NULL_" and isinstance(nm, str):
                        out.add(nm.lower())
        except OSError:
            continue
    return out


def is_clean(w: str) -> bool:
    return bool(w) and all(c.isalpha() or c in "'-" for c in w)


def main() -> int:
    p = argparse.ArgumentParser(
        description="Assemble the FE-bake sweep corpus from corpus.jsonl + "
                    "existing fe_tree traces.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    p.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS)
    p.add_argument("--trace-dir", type=Path, default=DEFAULT_TRACE_DIR)
    p.add_argument("--include-traces", action="store_true",
                   help="Also pull words from fe_tree trace IRs (default: on; "
                        "use --no-include-traces to disable).")
    p.add_argument("--no-include-traces", action="store_false",
                   dest="include_traces")
    p.add_argument("--out", default=DEFAULT_OUT)
    p.add_argument("--limit", type=int, default=0)
    p.add_argument("--shard", default="",
                   help="k/N -- emit only shard k of N (0-indexed)")
    p.add_argument("--batch-size", type=int, default=DEFAULT_BATCH)
    p.set_defaults(include_traces=True)
    args = p.parse_args()

    shard_k = shard_n = None
    if args.shard:
        try:
            k_s, n_s = args.shard.split("/")
            shard_k = int(k_s); shard_n = int(n_s)
        except Exception:
            raise SystemExit("invalid --shard, expected k/N")

    vocab: set[str] = set()
    n_corpus = 0
    if args.corpus and args.corpus.exists():
        vocab |= words_from_corpus(args.corpus)
        n_corpus = len(vocab)
    if args.include_traces:
        from_traces = words_from_traces(args.trace_dir)
        vocab |= from_traces

    rows = sorted(w for w in vocab if is_clean(w))
    if args.limit > 0:
        rows = rows[:args.limit]
    if shard_n is not None:
        rows = [w for i, w in enumerate(rows) if i % shard_n == shard_k]

    bsz = max(1, args.batch_size)
    batches = [rows[i:i + bsz] for i in range(0, len(rows), bsz)]

    out_path = args.out
    os.makedirs(os.path.dirname(os.path.abspath(out_path)) or ".",
                exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as fp:
        for i, batch in enumerate(batches):
            text = ". ".join(batch) + "."
            fp.write(json.dumps({
                "id":    f"fe_sweep_{i:05d}",
                "mode":  "text",
                "text":  text,
                "tags":  ["fe_sweep", "batched"],
                "words": batch,
            }) + "\n")

    print(f"Wrote {len(batches)} batched entries ({len(rows)} words, "
          f"{bsz}/batch) -> {out_path}")
    print(f"  source: corpus={n_corpus} words; "
          f"traces={'enabled' if args.include_traces else 'disabled'}")

    sec_per_batch = 6
    est = len(batches) * sec_per_batch
    print()
    print(f"Estimate at {sec_per_batch}s/batch:")
    print(f"  serial : {est // 60}m {est % 60}s ({len(batches)} synth calls)")
    print(f"  4-shard: ~{est // 4 // 60}m {(est // 4) % 60}s "
          f"(use --shard 0/4..3/4)")
    print()
    print("Next steps:")
    print(f"  1. Start Speechify.exe (server) on Windows")
    print(f"  2. python spfy/test/oracle/run_frida_capture.py "
          f"--hook fe_tree --corpus {out_path}")
    print(f"  3. python spfy/tools/fe_sweep_aggregate.py")
    print(f"  4. python spfy/tools/baked_fe_to_c.py")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
