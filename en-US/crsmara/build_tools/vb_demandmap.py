#!/usr/bin/env python3
"""How often does real speech ASK for each prsl context? Dump it as JSON.

Every size decision on this voice comes down to "which units can go", and the
flat answer -- protect every stored group equally -- is wrong in a way that is
measurable: 64,995 of our 93,385 groups are WIDE FALLBACKS, and a fallback key
nobody visits costs bytes and buys nothing. jill's inventory is demand-weighted
and ours is flat, so the demand map is the missing term in every floor.

⚠ THE SAMPLE MUST BE HELD OUT. Text that was folded into the corpus asks for
exactly the contexts that text created, so measuring demand on it protects the
material that was just added and calls the circularity a result. Pass Brown
lines the build never saw.

⚠ AND THE VOICE ONLY REPORTS CONTEXTS IT WAS ASKED FOR. A key absent from this
map was not visited by THIS sample; on a broader sample it might be. Treat a
zero as "unseen", not as "never".

    py vb_demandmap.py --voice C:\\tmp\\crsmara_it2b18 \\
        --texts C:\\tmp\\vocab_gap\\texts_brown.txt --skip 1000 \\
        --out C:\\tmp\\demand_heldout.json
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
_SLOT = re.compile(r'^\{"hp":(\d+),.*?"ctx":\[([0-9,]+)\]')


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
    ap.add_argument("--texts", required=True,
                    help="one line per sentence, optional `id|` prefix")
    ap.add_argument("--skip", type=int, default=0,
                    help="drop the first N lines -- the ones already folded "
                         "into the corpus")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--group", type=int, default=20,
                    help="lines per render, so each one is a real multi-phrase "
                         "utterance rather than a one-liner")
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
        env["SPFY_SYNTH_DEBUG"] = "1"
        loc = Counter()
        try:
            r = subprocess.run([SYNTH, str(d / f"{stem}.vin"),
                                str(d / f"{stem}8.vdb"), str(d / f"{stem}.vcf"),
                                "-f", src, wav], capture_output=True, text=True,
                               encoding="utf-8", errors="replace", env=env,
                               timeout=3600)
            for line in (r.stdout + "\n" + r.stderr).splitlines():
                m = _SLOT.match(line.strip())
                if not m:
                    continue
                c = [int(x) for x in m.group(2).split(",")]
                if len(c) >= 4:
                    loc[c[1] * 10000 + c[2] * 100 + c[3]] += 1
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
                print(f"  {done}/{len(docs)}  {len(cnt):,} keys, "
                      f"{sum(cnt.values()):,} requests")

    print(f"  {len(cnt):,} distinct contexts requested, "
          f"{sum(cnt.values()):,} slot requests")
    Path(a.out).write_text(json.dumps({str(k): v for k, v in cnt.items()}),
                           encoding="utf-8")
    print(f"  wrote {a.out}")


if __name__ == "__main__":
    main()
