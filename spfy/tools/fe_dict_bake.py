"""Bake a (text -> ARPAbet) dictionary by driving spfy_dumpwav --g2p.

Input: c:/tmp/fe_lts_records.csv (36,724 unique words from registry-A/B).
Output: c:/tmp/fe_dict.jsonl (one JSON object per word).

Strategy: batch ~100 words per --g2p call separated by '. ' to force one
ARPAbet group per word. Parse the output by splitting on 'pau' boundaries.
At ~5s/call, 36,724 words / 100 per batch = 367 calls = ~30 minutes.

Run:
  python spfy/tools/fe_dict_bake.py
"""
import csv, subprocess, json, time, sys
from pathlib import Path
import os

EXE = os.path.expanduser("~/Documents/Speechify/bin/spfy_dumpwav.exe")
CSV_IN = "c:/tmp/fe_lts_records.csv"
JSONL_OUT = "c:/tmp/fe_dict.jsonl"
BATCH_SIZE = 25    # engine caps ARPAbet output at 256 tokens; 35 hit the
                    # cap on ~14% of batches (long-word density). 25 keeps
                    # most batches under the cap with mid-syllable margin.
SEPARATOR = "? "   # '.' merges abbreviations (A. B. C. -> "A B C" as one
                    # acronym); '?' forces a real sentence boundary
TIMEOUT_S = 90

def load_words():
    """Return sorted list of unique words from registry CSV (strip trailing /)."""
    seen = set()
    with open(CSV_IN, encoding="utf-8") as f:
        for r in csv.DictReader(f):
            if r.get("is_word") not in ("1", "true", "True"): continue
            w = (r.get("decoded") or "").rstrip("/")
            if w and w not in seen:
                seen.add(w)
    return sorted(seen)

def parse_g2p(stdout, n_expected):
    """Return list of ARPAbet phoneme strings, one per word.

    Engine output shape:
      'ARPAbet: pau h(2) e(2) ... pau pau w(1) ... pau pau ... pau'

    Words are bordered by 'pau' tokens; consecutive 'pau pau' marks word
    boundaries (sentence-end + sentence-start in adjacent batched utterances).
    """
    arpa_line = next((l for l in stdout.splitlines()
                      if l.startswith("ARPAbet:")), "")
    arpa = arpa_line.replace("ARPAbet:", "").strip()
    if not arpa:
        return [""] * n_expected
    toks = arpa.split()
    groups = []
    cur = []
    for t in toks:
        if t == "pau":
            if cur:
                groups.append(" ".join(cur))
                cur = []
        else:
            cur.append(t)
    if cur:
        groups.append(" ".join(cur))
    return groups

def run_batch(words):
    """Run --g2p on a list of words. Returns list of ARPAbet strings, one per
    word (or empty string if parse failed for that slot)."""
    text = SEPARATOR.join(words) + SEPARATOR.strip()
    t0 = time.time()
    try:
        r = subprocess.run([EXE, "--g2p", text],
                           capture_output=True, timeout=TIMEOUT_S, check=False)
    except subprocess.TimeoutExpired:
        return [""] * len(words), -1.0
    elapsed = time.time() - t0
    stdout = r.stdout.decode("utf-8", "replace")
    groups = parse_g2p(stdout, len(words))
    # If group count doesn't match, log + pad/truncate
    if len(groups) != len(words):
        sys.stderr.write(f"  [warn] expected {len(words)} groups, got {len(groups)}\n")
        if len(groups) < len(words):
            groups = groups + [""] * (len(words) - len(groups))
        else:
            groups = groups[:len(words)]
    return groups, elapsed

def main():
    words = load_words()
    print(f"loaded {len(words)} unique words", flush=True)

    # Resume support: skip words already in JSONL
    done = set()
    if Path(JSONL_OUT).exists():
        with open(JSONL_OUT, encoding="utf-8") as f:
            for line in f:
                try:
                    done.add(json.loads(line)["word"])
                except Exception:
                    pass
        print(f"resume: {len(done)} words already done", flush=True)

    todo = [w for w in words if w not in done]
    print(f"todo: {len(todo)} words", flush=True)
    if not todo:
        print("nothing to do.")
        return

    out = open(JSONL_OUT, "a", encoding="utf-8", buffering=1)  # line-buffered
    n_total = len(todo)
    n_done = 0
    t_session = time.time()
    for i in range(0, n_total, BATCH_SIZE):
        batch = todo[i:i + BATCH_SIZE]
        groups, elapsed = run_batch(batch)
        for w, g in zip(batch, groups):
            out.write(json.dumps({"word": w, "arpabet": g}, ensure_ascii=False) + "\n")
        n_done += len(batch)
        rate = n_done / max(time.time() - t_session, 0.01)
        eta_min = (n_total - n_done) / rate / 60
        print(f"  batch {i//BATCH_SIZE+1:4d}: "
              f"{len(batch):3d} words in {elapsed:5.1f}s  "
              f"({n_done:6d}/{n_total} = {n_done/n_total*100:5.1f}%, "
              f"rate {rate:.1f} w/s, ETA {eta_min:5.1f} min)", flush=True)
    out.close()
    print(f"\ndone. wrote {JSONL_OUT}")

if __name__ == "__main__":
    main()
