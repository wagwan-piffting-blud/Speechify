"""Post-pass: re-bake any words in fe_dict.jsonl whose ARPAbet came back
empty (truncated by the engine's 256-token output cap when other long
words filled the buffer first).

Strategy: collect all empty entries, run them through --g2p in batches
of 10 (well under the cap regardless of word length), and overwrite the
empty entries in place.
"""
import json, subprocess, time, sys, importlib.util
from pathlib import Path
import os

JSONL = "c:/tmp/fe_dict.jsonl"
EXE = os.path.expanduser("~/Documents/Speechify/bin/spfy_dumpwav.exe")
SMALL_BATCH = 10

# Reuse parser from the main bake script.
spec = importlib.util.spec_from_file_location("fe_dict_bake",
    os.path.expanduser("~/Documents/Speechify/spfy/tools/fe_dict_bake.py"))
bake = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bake)

def main():
    rows = []
    with open(JSONL, encoding="utf-8") as f:
        for line in f:
            rows.append(json.loads(line))
    print(f"loaded {len(rows)} rows")
    empties_idx = [i for i, r in enumerate(rows)
                    if not r.get("arpabet", "").strip()]
    print(f"empty entries to re-bake: {len(empties_idx)}")
    if not empties_idx:
        print("nothing to repair.")
        return

    # Override batch size in the bake helper for this pass.
    bake.BATCH_SIZE = SMALL_BATCH

    fixed = 0
    still_empty = 0
    n_total = len(empties_idx)
    t0 = time.time()
    for i in range(0, n_total, SMALL_BATCH):
        chunk = empties_idx[i:i + SMALL_BATCH]
        words = [rows[j]["word"] for j in chunk]
        groups, elapsed = bake.run_batch(words)
        for j, g in zip(chunk, groups):
            rows[j]["arpabet"] = g
            if g.strip(): fixed += 1
            else: still_empty += 1
        rate = (i + len(chunk)) / max(time.time() - t0, 0.01)
        eta = (n_total - (i + len(chunk))) / rate / 60
        print(f"  chunk {i // SMALL_BATCH + 1:4d}: "
              f"fixed_so_far={fixed} still_empty={still_empty} "
              f"({(i + len(chunk)) / n_total * 100:.1f}%, ETA {eta:.1f} min)",
              flush=True)

    # Rewrite JSONL
    with open(JSONL, "w", encoding="utf-8") as f:
        for r in rows:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")
    print(f"\ndone. fixed {fixed}, still empty {still_empty}.")

if __name__ == "__main__":
    main()
