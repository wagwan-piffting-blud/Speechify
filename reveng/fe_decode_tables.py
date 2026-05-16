"""fe_decode_tables.py -- decode every carved FE table to human-readable
text using the 469-symbol vocabulary.

Format (verified on t000 + t001 of registry-A in F2 breakthrough):
  Each table is a sequence of NUL-terminated records. Each record byte is
  an index into the FE's 469-entry symbol vocabulary at .data offset
  0x4d0f0 (saved to spfy/build/fe_symbol_table.json). Letters use
  IDs 1-31 (a-z), accented letters 97-161, phonemes 229+, etc.

  Some tables instead use 16-bit u16 records (offset-array headers etc.
  per the t008/t009 multi-level pattern). We try byte-decoding first
  and label tables that don't decode cleanly.

Usage:
  python reveng/fe_decode_tables.py \\
    --tables-a spfy/data/fe_tables_a/ \\
    --tables-b spfy/data/fe_tables/ \\
    --vocab spfy/build/fe_symbol_table.json \\
    --out spfy/build/fe_tables_decoded.txt
"""

import argparse
import csv
import json
import sys
from pathlib import Path


def load_vocab(vocab_path):
    """Returns id -> name dict; bytes-not-mapped get '?' placeholder."""
    raw = json.loads(Path(vocab_path).read_text(encoding="utf-8"))
    out = {}
    for e in raw:
        out[e["idx"]] = e["name"] if e["name"] is not None else f"<0x{e['idx']:02x}>"
    return out


def decode_record(rec, vocab):
    """Turn a list of bytes into either a concatenated string (if all
    bytes are valid letter symbol IDs) or a fallback ID list."""
    chars = []
    all_ok = True
    for b in rec:
        name = vocab.get(b)
        if name is None or len(name) > 4:
            all_ok = False
            chars.append(f"<{b}>")
        else:
            chars.append(name)
    if all_ok:
        return "".join(chars)
    return " ".join(chars)


def decode_table(buf, vocab):
    """Walk NUL-separated records. Returns list of decoded strings."""
    records = []
    cur = []
    for b in buf:
        if b == 0:
            if cur:
                records.append(decode_record(cur, vocab))
                cur = []
        else:
            cur.append(b)
    if cur:
        records.append(decode_record(cur, vocab))
    return records


def heuristic_summary(records):
    """Classify the table by content."""
    if not records:
        return "EMPTY"
    n = len(records)
    avg_len = sum(len(r) for r in records) / max(n, 1)
    n_short = sum(1 for r in records if len(r) <= 4)
    n_long  = sum(1 for r in records if len(r) >= 8)
    n_with_id = sum(1 for r in records if "<" in r)
    if n_with_id > n * 0.5:
        return f"BINARY (mostly out-of-range bytes; {n} records)"
    if n <= 5 and avg_len <= 4:
        return f"TINY ({n} small entries)"
    if n_short > n * 0.7:
        return f"SHORT-CODES ({n} entries, avg {avg_len:.1f} chars)"
    if n_long > n * 0.4:
        return f"WORDS ({n} entries, avg {avg_len:.1f} chars)"
    return f"MIXED ({n} entries, avg {avg_len:.1f} chars)"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tables-a", required=True)
    ap.add_argument("--tables-b", required=True)
    ap.add_argument("--vocab", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--max-records-per-table", type=int, default=20,
                    help="Cap shown records per table in the report")
    args = ap.parse_args()

    vocab = load_vocab(args.vocab)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)

    summary_rows = []
    with out.open("w", encoding="utf-8") as fp:
        for label, dir_path in [("A", args.tables_a), ("B", args.tables_b)]:
            d = Path(dir_path)
            files = sorted(p for p in d.iterdir() if p.suffix == ".bin")
            print(f"Decoding {len(files)} tables from registry-{label} ({d})...")
            fp.write(f"\n{'='*76}\n")
            fp.write(f"REGISTRY-{label}: {len(files)} tables from {d}\n")
            fp.write(f"{'='*76}\n")
            for path in files:
                idx = int(path.stem.lstrip("t"))
                buf = path.read_bytes()
                records = decode_table(buf, vocab)
                tag = heuristic_summary(records)
                summary_rows.append({"reg": label, "idx": idx,
                                     "size": len(buf), "n_records": len(records),
                                     "kind": tag.split()[0]})
                fp.write(f"\nt{idx:03d}  size={len(buf):>5}  {tag}\n")
                cap = args.max_records_per_table
                shown = records[:cap]
                for r in shown:
                    fp.write(f"  {r}\n")
                if len(records) > cap:
                    fp.write(f"  ... ({len(records) - cap} more)\n")

    # Write summary CSV.
    csv_out = out.with_name("fe_tables_summary.csv")
    with csv_out.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=["reg", "idx", "size", "n_records", "kind"])
        w.writeheader()
        for r in summary_rows:
            w.writerow(r)

    print(f"wrote {out}")
    print(f"wrote {csv_out}")
    # Quick stats
    from collections import Counter
    kinds = Counter(r["kind"] for r in summary_rows)
    print(f"\nKind distribution:")
    for k, v in kinds.most_common():
        print(f"  {k:<10s} {v}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
