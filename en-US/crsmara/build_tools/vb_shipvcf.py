#!/usr/bin/env python3
"""Write the SHIPPING copy of a built .vcf: add the keys the builder cannot.

`spfy_vb_build --vcf-set` refuses any key the embedded en-US payload does not
already carry (`vb_vcf.c:99`, deliberately -- a typo that changed nothing would
look exactly like a weight that is inert). `MISSING_JOIN_COST` is such a key:
the engine reads it by name and defaults to 1000.0 in the constructor, but no
shipped SpeechWorks voice sets it, so it is not in the payload. The approved
voice runs at 1, which means the shipped file needs one pass that ADDS it.

Without this the voice only sounds right with `SPFY_MISSING_JOIN=1` in the
environment, which is not something a user of the voice will have set.

⚠ Additions are limited to `vcf_variant.ATTESTED_ADDITIONS` -- a key is only
addable when a real voice file or the engine binary is the evidence for it.

    py vb_shipvcf.py --in  C:\\tmp\\crsmara_px2Ow\\crsmara.vcf \\
                     --out en-US\\crsmara\\crsmara.vcf \\
                     --set MISSING_JOIN_COST=1.0
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import vcf_variant as V      # noqa: E402

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="src", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--set", action="append", default=[],
                    help="KEY=VALUE; repeatable")
    a = ap.parse_args()

    params = {}
    for kv in a.set:
        if "=" not in kv:
            raise SystemExit(f"--set wants KEY=VALUE, got {kv}")
        k, v = kv.split("=", 1)
        params[k.strip()] = v.strip()

    before = V.read_params(a.src)
    V.write_variant(a.src, a.out, params, allow_add=True)
    after = V.read_params(a.out)

    print(f"{a.src} -> {a.out}")
    for k, v in params.items():
        was = before.get(k)
        print(f"  {k:<24s} {was if was is not None else '(absent)':>10s}"
              f"  ->  {after.get(k)}")
    print(f"  {len(after)} params, {os.path.getsize(a.out):,} B")


if __name__ == "__main__":
    main()
