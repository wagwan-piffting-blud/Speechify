#!/usr/bin/env python3
"""List the .zip assets on a release that the new catalog no longer names.

    gh release view voices --json assets --jq '.assets[].name' > attached.txt
    python installer/updates/prune_release_assets.py \
        --catalog dist/voices.json --assets attached.txt --keep-dir dist \
        -o stale.txt

Zips are named <id>-<version>.zip, so bumping a voice's version publishes a new
asset beside the old one rather than replacing it. Re-publishing every voice at
once therefore doubles the release: the manifest points only at the new names,
but every superseded build stays attached and downloadable forever.

This prints what is safe to delete; the caller runs `gh release delete-asset`.
Keeping the deletion outside means this tool is offline and testable, and the
same reason pack_voices.py is offline: a failed publish is never ambiguous
about whether GitHub was reachable.

SAFE MEANS NAMED BY THE CATALOG. The catalog carries entries forward for voices
this run did not pack -- a --voices subset, and the externals (Paulina) whose
source is not in the repo and whose zip CI cannot rebuild. Those zips are
therefore keeps, not stale. Anything not a .zip (voices.json) is never touched.
"""

import argparse
import json
import sys
from pathlib import Path


def catalog_zips(doc):
    """Every asset name the catalog points at."""
    names = set()
    for e in doc.get("voices", []) + doc.get("bundles", []):
        if e.get("zip"):
            names.add(e["zip"])
    if doc.get("all", {}).get("zip"):
        names.add(doc["all"]["zip"])
    return names


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--catalog", required=True,
                    help="the catalog about to go live (dist/voices.json)")
    ap.add_argument("--assets", required=True,
                    help="file of attached asset names, one per line")
    ap.add_argument("--keep-dir", default="",
                    help="also keep anything present in this directory")
    ap.add_argument("-o", "--out", default="",
                    help="write the stale names here, one per line")
    args = ap.parse_args()

    doc = json.loads(Path(args.catalog).read_text(encoding="utf-8"))
    keep = catalog_zips(doc)
    if not keep:
        print(f"{args.catalog} names no zips at all -- refusing to prune "
              f"against an empty catalog", file=sys.stderr)
        return 1
    if args.keep_dir:
        d = Path(args.keep_dir)
        if d.is_dir():
            keep |= {p.name for p in d.iterdir() if p.is_file()}

    attached = []
    ap_path = Path(args.assets)
    if ap_path.is_file():
        attached = [ln.strip() for ln in
                    ap_path.read_text(encoding="utf-8").splitlines()
                    if ln.strip()]

    stale = [n for n in attached if n.endswith(".zip") and n not in keep]

    print(f"{len(attached)} asset(s) attached, {len(keep)} name(s) kept")
    for n in sorted(keep & set(attached)):
        print(f"  keep   {n}")
    for n in stale:
        print(f"  STALE  {n}")
    if not stale:
        print("nothing to prune")

    if args.out:
        Path(args.out).write_text("".join(f"{n}\n" for n in stale),
                                  encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
