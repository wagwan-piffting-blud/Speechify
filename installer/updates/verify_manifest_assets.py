#!/usr/bin/env python3
"""Fail if update.json points at a release asset that is not published.

    gh release view voices --json assets --jq '.assets[].name' > published.txt
    python installer/updates/verify_manifest_assets.py \
        --manifest update.json --assets published.txt

The failure this catches is quiet and one-sided: the manifest is generated from
a catalog, and the catalog can legitimately carry an entry whose zip was
uploaded by hand from another machine (Paulina). If that upload never happened
-- or the asset was deleted, or renamed by a version bump -- nothing in the
publish log looks wrong. The first symptom is a user clicking a 404.

Only URLs on the release host are checked. An entry pointing somewhere else
entirely (a mirror, wagspuzzle.space) is reported as skipped rather than
failed: this script knows what is attached to one release, not what exists on
the internet.
"""

import argparse
import json
import sys
from pathlib import Path
from urllib.parse import unquote, urlparse


def asset_name(url, tag):
    """The asset filename this URL resolves to, or None if it is not an asset
    of the release we were handed."""
    p = urlparse(url)
    if p.netloc != "github.com":
        return None
    parts = [unquote(x) for x in p.path.split("/") if x]
    # /<owner>/<repo>/releases/download/<tag>/<name>
    if len(parts) < 6 or parts[2] != "releases" or parts[3] != "download":
        return None
    if parts[4] != tag:
        return None
    return parts[5]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--assets", required=True,
                    help="file of published asset names, one per line")
    ap.add_argument("--tag", default="voices")
    args = ap.parse_args()

    doc = json.loads(Path(args.manifest).read_text(encoding="utf-8"))
    published = {ln.strip() for ln in
                 Path(args.assets).read_text(encoding="utf-8").splitlines()
                 if ln.strip()}

    missing, checked, skipped = [], 0, []
    for kind, key in (("voice", "voices"), ("bundle", "bundles")):
        for e in doc.get(key, []):
            url = e.get("url", "")
            if not url:
                missing.append(f"{kind} {e.get('id') or e.get('lang')}: no url")
                continue
            name = asset_name(url, args.tag)
            if name is None:
                skipped.append(f"{kind} {e.get('id') or e.get('lang')}: {url}")
                continue
            checked += 1
            if name not in published:
                missing.append(
                    f"{kind} {e.get('id') or e.get('lang')}: {name} is not "
                    f"attached to the '{args.tag}' release")

    for s in skipped:
        print(f"  skipped (not a '{args.tag}' asset): {s}")
    if missing:
        print(f"\n{len(missing)} manifest entr(ies) point at nothing:",
              file=sys.stderr)
        for m in missing:
            print(f"  - {m}", file=sys.stderr)
        print("\nUpload the missing zip(s) to that release, or remove the "
              "entry from installer/updates/external_voices.json.",
              file=sys.stderr)
        return 1
    print(f"all {checked} manifest asset(s) are published"
          f"{f', {len(skipped)} skipped' if skipped else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
