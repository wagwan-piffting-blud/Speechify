#!/usr/bin/env python3
"""Stage the lazy-loadable voice assets for the WASM demo.

For each voice below, copy its three runtime files
(<prefix>.vin / <prefix>8.vdb / <prefix>.vcf) from the repo working tree
into  <out>/<lang>/<id>/  and emit  <out>/manifest.json  describing them.

Any single file larger than --threshold bytes is split into
`<name>.partNNN` chunks so it stays under GitHub Pages' hard 100 MB /
file limit (the deploy target). The browser loader fetches the parts in
order and stitches them back together in the virtual FS, so a 253 MB VDB
(Paulina) ships as three ~85 MB objects with no server-side support.

A voice whose source files are not present is SKIPPED with a note (CI
checkouts don't carry the gitignored large voices, e.g. Paulina) — the
manifest simply omits it, so the web UI only offers what actually shipped.

Nothing here is Speechify-specific beyond the voice table; it is a plain
copy/split/manifest step. Incremental: a chunk or copy whose size already
matches on disk is left untouched.

Usage:
    stage_voices.py --root <repo-root> --out <dist/voices> [--threshold N]

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import json
import sys
from pathlib import Path

# Voices staged LOCALLY: files copied from the repo working tree into
# dist/voices/ and served same-origin from the site. id, display name,
# VCF language tag, on-disk file prefix, dir under repo root, and whether
# the UI should gate the download behind a confirm.
#
# Voices too large to commit / serve from Pages (e.g. Paulina, whose
# 253 MB VDB exceeds GitHub's 100 MB limits) are declared in
# external_voices.json instead and fetched from a CORS-enabled host.
VOICES = [
    {"id": "tom",     "display": "Tom",     "lang": "en-US",
     "prefix": "tom",     "dir": "en-US/tom",     "large": False},
    {"id": "jill",    "display": "Jill",    "lang": "en-US",
     "prefix": "jill",    "dir": "en-US/jill",    "large": False},
    {"id": "javier",  "display": "Javier",  "lang": "es-MX",
     "prefix": "javier",  "dir": "es-MX/javier",  "large": False},
    {"id": "felix",   "display": "Felix",   "lang": "fr-CA",
     "prefix": "felix",   "dir": "fr-CA/felix",   "large": False},
]

# 90 MiB. GitHub blocks pushes of files >100 MB and Pages refuses to serve
# them, so keep every emitted object comfortably under that.
DEFAULT_THRESHOLD = 90 * 1024 * 1024
COPY_CHUNK = 8 * 1024 * 1024


def voice_files(v):
    """The three runtime filenames for a voice, in load order."""
    p = v["prefix"]
    return [f"{p}.vin", f"{p}8.vdb", f"{p}.vcf"]


def same_size(path, n):
    try:
        return path.is_file() and path.stat().st_size == n
    except OSError:
        return False


def copy_whole(src, dst):
    """Copy src -> dst unless dst already has the same size."""
    n = src.stat().st_size
    if same_size(dst, n):
        return
    with src.open("rb") as fi, dst.open("wb") as fo:
        while True:
            b = fi.read(COPY_CHUNK)
            if not b:
                break
            fo.write(b)


def split_file(src, dst_dir, base, threshold):
    """Split src into dst_dir/<base>.partNNN chunks of <= threshold bytes.

    Returns the list of part filenames (basename only). Skips writing a
    chunk whose file already has the expected size (incremental rebuild).
    """
    total = src.stat().st_size
    n_parts = (total + threshold - 1) // threshold
    parts = []
    with src.open("rb") as fi:
        for i in range(n_parts):
            name = f"{base}.part{i:03d}"
            parts.append(name)
            dst = dst_dir / name
            want = min(threshold, total - i * threshold)
            if same_size(dst, want):
                fi.seek((i + 1) * threshold)
                continue
            fi.seek(i * threshold)
            remaining = want
            with dst.open("wb") as fo:
                while remaining > 0:
                    b = fi.read(min(COPY_CHUNK, remaining))
                    if not b:
                        break
                    fo.write(b)
                    remaining -= len(b)
    return parts


def stage_voice(v, root, out, threshold):
    """Stage one voice. Returns its manifest entry, or None if skipped."""
    vdir = root / v["dir"]
    files = voice_files(v)
    srcs = [vdir / f for f in files]
    missing = [f for f, s in zip(files, srcs) if not s.is_file()]
    if missing:
        print(f"  skip {v['id']}: missing {', '.join(missing)} in {vdir}",
              file=sys.stderr)
        return None

    dst_dir = out / v["lang"] / v["id"]
    dst_dir.mkdir(parents=True, exist_ok=True)

    file_entries = []
    total_bytes = 0
    for name, src in zip(files, srcs):
        n = src.stat().st_size
        total_bytes += n
        if n > threshold:
            parts = split_file(src, dst_dir, name, threshold)
            print(f"  {v['id']}/{name}: {n} B -> {len(parts)} parts",
                  file=sys.stderr)
        else:
            copy_whole(src, dst_dir / name)
            parts = [name]
        file_entries.append({"name": name, "bytes": n, "parts": parts})

    return {
        "id": v["id"],
        "display": v["display"],
        "lang": v["lang"],
        "prefix": v["prefix"],
        "large": v["large"],
        "dir": f"{v['lang']}/{v['id']}",
        "totalBytes": total_bytes,
        "files": file_entries,
    }


def external_entry(v):
    """Build a manifest entry for an off-site voice from external_voices.json.

    Nothing is copied: each file's single "part" is the absolute URL the
    browser fetches (baseUrl + name). The loader (web/index.js) recognises
    an http(s) part and fetches it directly.
    """
    base = v["baseUrl"]
    if not base.endswith("/"):
        base += "/"
    files = []
    total = 0
    for f in v["files"]:
        total += f["bytes"]
        files.append({"name": f["name"], "bytes": f["bytes"],
                      "parts": [base + f["name"]]})
    return {
        "id": v["id"],
        "display": v["display"],
        "lang": v["lang"],
        "prefix": v["prefix"],
        "large": v.get("large", False),
        "external": True,
        "dir": v["id"],           # unused for absolute parts; kept for shape
        "totalBytes": total,
        "files": files,
    }


def load_external(path):
    """Read external_voices.json; return its voice list (empty if absent)."""
    if not path or not Path(path).is_file():
        return []
    data = json.loads(Path(path).read_text())
    return data.get("voices", [])


def main():
    default_ext = Path(__file__).resolve().parent.parent / "external_voices.json"
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True, help="repo root (holds en-US/, es-MX/, fr-CA/)")
    ap.add_argument("--out", required=True, help="output dir (dist/voices)")
    ap.add_argument("--threshold", type=int, default=DEFAULT_THRESHOLD,
                    help="max bytes per emitted object before splitting")
    ap.add_argument("--external", default=str(default_ext),
                    help="JSON of off-site voices to append (absolute URLs)")
    args = ap.parse_args()

    root = Path(args.root).resolve()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)

    print(f"staging voices from {root} -> {out} "
          f"(split >{args.threshold} B)", file=sys.stderr)

    entries = []
    for v in VOICES:
        e = stage_voice(v, root, out, args.threshold)
        if e:
            entries.append(e)

    # Append off-site voices verbatim (they need no local files, so they
    # appear on CI-built manifests too).
    staged_ids = {e["id"] for e in entries}
    for v in load_external(args.external):
        if v["id"] in staged_ids:
            continue   # a local copy already won; don't duplicate
        entries.append(external_entry(v))
        print(f"  external {v['id']}: {v['baseUrl']}", file=sys.stderr)

    manifest = {"version": 1, "voices": entries}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2))
    ids = ", ".join(e["id"] for e in entries) or "(none)"
    print(f"manifest.json: {len(entries)} voice(s) staged: {ids}",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
