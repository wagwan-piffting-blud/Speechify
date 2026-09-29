#!/usr/bin/env python3
"""`\\!pN` at phrase edges, byte-compared against the VENDOR engine.

Renders each case through bin\\spfy_dumpwav.exe (vendor SWIttsEngine.dll) and
through spfy_synth, then compares samples.

Until 2026-09-28 a pause right after sentence-ending punctuation was DROPPED:
`First. \\!p1000 Second.` rendered byte-identical to `First. Second.`, while
vendor adds 991 ms. That is the EAS bulletin pattern (sentence, silence,
sentence). The fix puts the pause at the head of the next phrase.

KNOWN is the one case not byte-identical: after a COMMA we land within ~1.2 ms
of vendor, and the residual is unit selection at the end of the word before
the comma. It is reported but does not fail the gate unless the length gap
grows past 5 ms.

    python spfy/test/pause_edge_vendor_test.py
"""

import argparse
import os
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[2]
CASES = [
    ("base",        "First. Second.",            "exact"),
    ("dot",         "First. \\!p1000 Second.",   "exact"),
    ("dot_nospace", "First.\\!p1000 Second.",    "exact"),
    ("dot_500",     "First. \\!p500 Second.",    "exact"),
    ("question",    "First? \\!p1000 Second.",   "exact"),
    ("exclaim",     "Stop! \\!p700 Now go.",      "exact"),
    ("mid",         "First \\!p1000 second.",    "exact"),
    ("lead",        "\\!p500 First second.",      "exact"),
    ("trail",       "First. Second. \\!p1000",   "exact"),
    ("two",         "One. \\!p300 Two. \\!p600 Three.", "exact"),
    ("comma",       "First, \\!p1000 second.",   "known"),
]


def load(p):
    with wave.open(str(p)) as w:
        return np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=r"C:\tmp\spfy_build32\src\cli\spfy_synth.exe")
    ap.add_argument("--vendor", default=str(REPO / "bin" / "spfy_dumpwav.exe"))
    args = ap.parse_args()
    tmp = Path(tempfile.mkdtemp(prefix="pauedge_"))
    env = dict(os.environ, SPFY_NO_UPDATE_CHECK="1")
    failed = 0
    for name, text, kind in CASES:
        v, s = tmp / f"v_{name}.wav", tmp / f"s_{name}.wav"
        subprocess.run([args.vendor, text, str(v)], cwd=str(REPO / "bin"),
                       capture_output=True, env=env)
        subprocess.run([args.exe, "tom", text, str(s)], capture_output=True, env=env)
        if not v.exists() or not s.exists():
            print(f"FAIL  {name:12s} render missing")
            failed += 1
            continue
        a, b = load(v), load(s)
        same = len(a) == len(b) and np.array_equal(a, b)
        dms = 1000.0 * (len(b) - len(a)) / 8000.0
        ok = same or (kind == "known" and abs(dms) <= 5.0)
        tag = "PASS" if ok else "FAIL"
        note = "identical" if same else f"length {dms:+.1f} ms" + (" (known)" if kind == "known" else "")
        print(f"{tag}  {name:12s} vendor {len(a) / 8000:.3f}s  {note}   {text}")
        failed += not ok
    print("-" * 70)
    print(f"{len(CASES) - failed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
