#!/usr/bin/env python3
"""Clone a voice with a MODIFIED VCF, at no disk cost, for a weight sweep.

A VCF weight is per-voice engine configuration -- the real Speechify reads
these keys -- so sweeping one is not an engine change and needs no rebuild and
no parity re-run. What it does need is a voice DIRECTORY per arm, because
every tool here resolves `<dir>/<name>.{vin,vcf}` and `<dir>/<name>8.vdb`.

The VIN and VDB are HARDLINKED (same bytes, one copy on disk), so an arm costs
the size of a VCF -- about 50 kB -- instead of 170 MB. Only the VCF is a real
file, which is also what makes the arm a controlled comparison: the inventory
is not merely identical, it is the same blocks.

⚠ vcf_variant.write_variant REFUSES a key the VCF does not already contain.
That is deliberate: an unknown `tts.voiceCfg.*` name makes the real server exit
rc=5 against its DTD, so a typo would produce an arm that silently is not what
it claims.

    python vb_vcfarm.py --voice donnaf0a --set F0_EDGE_CHANGE_WEIGHT=6 \
        --name donnaf0a_e6
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "wayback"))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)

import vb_listen as L                      # noqa: E402
from vcf_variant import write_variant      # noqa: E402

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass


def link_or_copy(src, dst):
    if os.path.exists(dst):
        os.remove(dst)
    try:
        os.link(src, dst)
        return "hardlink"
    except OSError:
        import shutil
        shutil.copy2(src, dst)
        return "copy"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--voice", required=True)
    ap.add_argument("--set", action="append", default=[],
                    metavar="KEY=VALUE")
    ap.add_argument("--name", required=True)
    ap.add_argument("--out", default=None,
                    help="parent directory (default: beside the source voice)")
    ap.add_argument("--add", action="store_true",
                    help="permit inserting a key the source VCF lacks, if a "
                         "SHIPPED voice attests it (vcf_variant."
                         "ATTESTED_ADDITIONS). jill omits "
                         "HALFPHONE_CAND_MAX_UNITS; aimara2 sets it to 200")
    a = ap.parse_args()

    # A build directory is as good as a registry name -- most arms worth
    # sweeping live in C:\tmp and were never registered under en-US/.
    src = L.VOICES.get(a.voice)
    stem = a.voice
    if src is None:
        from pathlib import Path
        d = Path(a.voice)
        vins = sorted(d.glob("*.vin")) if d.is_dir() else []
        if len(vins) == 1:
            src, stem = d, vins[0].stem
        else:
            print(f"{a.voice}: not a voice name or a directory holding exactly "
                  f"one .vin. have: {', '.join(sorted(L.VOICES))}")
            return 2
    params = {}
    for kv in a.set:
        if "=" not in kv:
            print(f"--set wants KEY=VALUE, got {kv!r}")
            return 2
        k, v = kv.split("=", 1)
        params[k.strip()] = v.strip()

    outdir = os.path.join(a.out or str(src.parent), a.name)
    os.makedirs(outdir, exist_ok=True)
    how = link_or_copy(str(src / f"{stem}.vin"),
                       os.path.join(outdir, f"{a.name}.vin"))
    link_or_copy(str(src / f"{stem}8.vdb"),
                 os.path.join(outdir, f"{a.name}8.vdb"))
    write_variant(src / f"{stem}.vcf",
                  os.path.join(outdir, f"{a.name}.vcf"), params,
                  allow_add=a.add)
    print(f"{a.name}: vin/vdb by {how}, VCF with "
          + ", ".join(f"{k}={v}" for k, v in params.items()))
    print(f"-> {outdir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
