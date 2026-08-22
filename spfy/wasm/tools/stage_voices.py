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
import re
import sys
from pathlib import Path

# ⭐ VOICES ARE DISCOVERED, NOT LISTED. The repo convention is
# `<lang>/<name>/<name>.vin` + `<name>8.vdb` + `<name>.vcf`, which is the same
# rule the registry tools resolve by, so a new voice appears on the site by
# existing rather than by being added here. A hard-coded table silently omits
# every voice someone forgets to append to it, and that failure looks exactly
# like a build that worked.
#
# Identity comes from the VOICE'S OWN VCF -- name, language, gender are stored
# there and are what the engine itself reads, so the site cannot disagree with
# the synthesiser about what a voice is called.
#
# Voices too large to commit / serve from Pages (e.g. Paulina, whose
# 253 MB VDB exceeds GitHub's 100 MB limits) are declared in
# external_voices.json instead and fetched from a CORS-enabled host.

# Language directories to scan. A dir is a language iff it looks like a BCP-47
# tag; that keeps `spfy/`, `bin/`, `doc/` and friends out without a blocklist
# that would need maintaining.
LANG_DIR = re.compile(r"^[a-z]{2}-[A-Z]{2}$")

# The 2:1 nibble cipher a .vcf is stored in. Inlined rather than imported from
# reveng/ so this stays runnable in a CI checkout that has no reveng tree.
_VCF_ENC = [0xDD, 0xDC, 0xDF, 0xDE, 0xD9, 0xD8, 0xDB, 0xDA,
            0xD5, 0xD4, 0xAC, 0xAF, 0xAE, 0xA9, 0xA8, 0xAB]
_VCF_DEC = {c: n for n, c in enumerate(_VCF_ENC)}


def vcf_params(path):
    """{param: value} from a .vcf, or {} if it cannot be read."""
    try:
        raw = Path(path).read_bytes()
        txt = bytes(((_VCF_DEC[raw[i]] << 4) | _VCF_DEC[raw[i + 1]])
                    for i in range(0, len(raw) - 1, 2)).decode("latin1")
    except (OSError, KeyError, IndexError, UnicodeDecodeError):
        return {}
    return dict(re.findall(
        r'<param name="tts\.voiceCfg\.([A-Za-z0-9_]+)">\s*<value>\s*'
        r'([^\s<]*)\s*</value>', txt))


def display_name(vcf, fallback):
    """A human label. The VCF is authoritative but not always presentable --
    `javier` ships lowercase and `CRS_Mara` uses an underscore."""
    name = (vcf.get("name") or "").replace("_", " ").strip()
    if not name:
        name = fallback
    return name.title() if name.islower() else name


def discover(root, large_bytes):
    """Every voice in the working tree, by convention. Sorted for a stable
    manifest: language first, then id, so a diff of manifest.json is readable.

    An optional `voice.json` beside the voice files overrides anything here;
    `{"skip": true}` keeps a work-in-progress voice off the site without
    deleting it or editing this script.
    """
    found = []
    for lang_dir in sorted(p for p in Path(root).iterdir()
                           if p.is_dir() and LANG_DIR.match(p.name)):
        for d in sorted(x for x in lang_dir.iterdir() if x.is_dir()):
            vid = d.name
            vin, vdb, vcf = d / f"{vid}.vin", d / f"{vid}8.vdb", d / f"{vid}.vcf"
            if not (vin.is_file() and vdb.is_file() and vcf.is_file()):
                continue
            over = {}
            ov = d / "voice.json"
            if ov.is_file():
                try:
                    over = json.loads(ov.read_text(encoding="utf-8"))
                except ValueError:
                    print(f"  ⚠ {ov} is not valid JSON; ignoring",
                          file=sys.stderr)
            if over.get("skip"):
                print(f"  - {lang_dir.name}/{vid}: skipped by voice.json")
                continue
            p = vcf_params(vcf)
            total = sum(f.stat().st_size for f in (vin, vdb, vcf))
            found.append({
                "id": vid,
                "display": over.get("display") or display_name(p, vid),
                "lang": over.get("lang") or p.get("language") or lang_dir.name,
                "prefix": vid,
                "dir": f"{lang_dir.name}/{vid}",
                # Not a hand-set flag any more: the UI gates a download behind
                # a confirm when the voice is actually big.
                "large": bool(over.get("large", total >= large_bytes)),
                "bytes": total,
            })
    # ⚠ Two voices showing the same label is a UI trap, and it happens for a
    # real reason: a voice built from another's template inherits that
    # template's VCF `name` until it is overridden. Say so rather than shipping
    # a picker with two identical entries.
    seen = {}
    for v in found:
        seen.setdefault(v["display"], []).append(v["dir"])
    for label, dirs in seen.items():
        if len(dirs) > 1:
            print(f"  ⚠ display name {label!r} is used by {len(dirs)} voices "
                  f"({', '.join(dirs)}) -- set tts.voiceCfg.name in the VCF, "
                  f"or `display` in voice.json", file=sys.stderr)
    return found

# 90 MiB. GitHub blocks pushes of files >100 MB and Pages refuses to serve
# them, so keep every emitted object comfortably under that.
DEFAULT_THRESHOLD = 90 * 1024 * 1024
COPY_CHUNK = 8 * 1024 * 1024

# Total voice size at or above which the UI asks before downloading. 150 MB is
# roughly "will not finish quickly on a phone"; the four historic voices sit
# well under it and were all flagged large:false by hand.
LARGE_BYTES = 150 * 1024 * 1024


def voice_files(v):
    """The three runtime filenames for a voice, in load order."""
    p = v["prefix"]
    return [f"{p}.vin", f"{p}8.vdb", f"{p}.vcf"]


def voice_optional_files(v):
    """Pitch marks: required by Speechify 4 mode, absent for most voices.

    Without these staged, S4 has no effect in the browser at all -- the mode
    turns on, spfy_pmarks_load finds nothing, and synthesis silently proceeds
    as 3.0.5. That is the whole reason `\\!s4m` appeared to do nothing in the
    WASM build.

    OPTIONAL on purpose: only Tom ships marks, and treating them as required
    would skip every other voice out of the manifest entirely."""
    p = v["prefix"]
    return [f"{p}8.pmdata", f"{p}8.pmindex"]


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

    # Append whichever optional files actually exist, so a voice without
    # pitch marks still ships and one with them gains S4 support.
    for name in voice_optional_files(v):
        src = vdir / name
        if src.is_file():
            files.append(name)
            srcs.append(src)

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
    ap.add_argument("--large-bytes", type=int, default=LARGE_BYTES,
                    help="total size at or above which the UI confirms before "
                         "downloading a voice")
    ap.add_argument("--only", default=None,
                    help="comma list of voice ids; stage only these")
    ap.add_argument("--list", action="store_true",
                    help="print what discovery finds and exit, staging nothing")
    args = ap.parse_args()

    root = Path(args.root).resolve()
    out = Path(args.out).resolve()

    voices = discover(root, args.large_bytes)
    if args.only:
        want = {s.strip() for s in args.only.split(",") if s.strip()}
        missing = want - {v["id"] for v in voices}
        if missing:
            print(f"--only names voices that were not discovered: "
                  f"{', '.join(sorted(missing))}", file=sys.stderr)
            return 2
        voices = [v for v in voices if v["id"] in want]

    if args.list:
        for v in voices:
            print(f"  {v['dir']:<24s} {v['display']:<14s} {v['lang']:<7s} "
                  f"{v['bytes'] / 1e6:8.1f} MB"
                  + ("  [large]" if v["large"] else ""))
        print(f"{len(voices)} voice(s) discovered under {root}")
        return 0

    out.mkdir(parents=True, exist_ok=True)
    print(f"staging voices from {root} -> {out} "
          f"(split >{args.threshold} B)", file=sys.stderr)
    print(f"  discovered: "
          + (", ".join(f"{v['id']}({v['lang']})" for v in voices) or "(none)"),
          file=sys.stderr)

    entries = []
    for v in voices:
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
