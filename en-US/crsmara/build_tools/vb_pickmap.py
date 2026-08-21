#!/usr/bin/env python3
"""How often does the DP actually CHOOSE each unit? Dump it as JSON.

⭐ THIS IS DEMAND MEASURED AT THE UNIT, NOT AT THE CONTEXT. `vb_demandmap.py`
counts which prsl keys get requested, which is the right currency for "is this
context served". It cannot tell you that of the 40 candidates in a healthy pool,
39 are never picked by anything. For a SIZE cut that difference is everything:
a chunk whose units nothing selects is free to delete, whatever its pools say.

The first cut ordered chunks by bytes-per-pool-contribution and was blind to
this. It met the size rule and took the accent with it -- "NAtional" +6.88 ->
+4.17 -- because the units that make the rise are a small set the ordering had
no reason to keep.

`SPFY_DUMP_PATH=1` makes the engine print the chosen path as `slot -> uid`,
including the WORD and SYL anchor hops.

⚠ THE SAMPLE MUST BE HELD OUT, for the same reason as the context map: text
folded into the corpus picks the units that text created.

⚠ AND A ZERO IS "UNSEEN", NOT "USELESS". A unit no sample picked may be the only
thing that serves a word nobody happened to write. Rank by it; do not treat it
as proof.

    py vb_pickmap.py --voice C:\\tmp\\crsmara_pk1618 \\
        --texts C:\\tmp\\vocab_gap\\texts_brown.txt --skip 1000 \\
        --out C:\\tmp\\picks_heldout.json
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "wayback"))
sys.path.insert(0, HERE)

import vb_listen as L   # noqa: E402

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

SYNTH = r"C:\tmp\spfy_build32\src\cli\spfy_synth.exe"
_PICK = re.compile(r"^\[\d+\]\s+slot=\d+\s+kind=(\w+)\s+uid=(\d+)")


def resolve(spec):
    if not L.VOICES:
        L.VOICES = L.discover()
    d = L.VOICES.get(spec)
    if d is not None:
        return Path(d), spec
    p = Path(spec)
    return p, sorted(p.glob("*.vin"))[0].stem


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--voice", required=True)
    ap.add_argument("--texts", required=True)
    ap.add_argument("--skip", type=int, default=0)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--group", type=int, default=20)
    ap.add_argument("--workers", type=int, default=20)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    d, stem = resolve(a.voice)
    lines = [l.split("|", 1)[1] if "|" in l else l
             for l in Path(a.texts).read_text(encoding="utf-8").splitlines()
             if l.strip()]
    lines = lines[a.skip:]
    if a.limit:
        lines = lines[:a.limit]
    docs = ["\n".join(lines[i:i + a.group])
            for i in range(0, len(lines), a.group)]
    print(f"{stem}: {len(lines):,} held-out lines -> {len(docs)} documents, "
          f"{a.workers} workers")

    def one(t):
        fd, wav = tempfile.mkstemp(suffix=".wav"); os.close(fd)
        fd, src = tempfile.mkstemp(suffix=".txt"); os.close(fd)
        Path(src).write_text(t, encoding="utf-8")
        env = {k: v for k, v in os.environ.items() if not k.startswith("SPFY_")}
        env["SPFY_DUMP_PATH"] = "1"
        loc = Counter()
        try:
            r = subprocess.run([SYNTH, str(d / f"{stem}.vin"),
                                str(d / f"{stem}8.vdb"), str(d / f"{stem}.vcf"),
                                "-f", src, wav], capture_output=True, text=True,
                               encoding="utf-8", errors="replace", env=env,
                               timeout=3600)
            for line in (r.stdout + "\n" + r.stderr).splitlines():
                m = _PICK.match(line.strip())
                if m:
                    loc[int(m.group(2))] += 1
        finally:
            for f in (wav, src):
                try:
                    os.unlink(f)
                except OSError:
                    pass
        return loc

    cnt = Counter()
    done = 0
    with ThreadPoolExecutor(max_workers=a.workers) as ex:
        for loc in ex.map(one, docs):
            cnt.update(loc)
            done += 1
            if done % 20 == 0:
                print(f"  {done}/{len(docs)}  {len(cnt):,} distinct units "
                      f"picked, {sum(cnt.values()):,} picks")

    print(f"  {len(cnt):,} distinct units picked, {sum(cnt.values()):,} picks")
    Path(a.out).write_text(json.dumps({str(k): v for k, v in cnt.items()}),
                           encoding="utf-8")
    print(f"  wrote {a.out}")


if __name__ == "__main__":
    main()
