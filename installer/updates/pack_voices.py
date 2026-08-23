#!/usr/bin/env python3
"""Pack every shipped voice into a downloadable .zip and emit the catalog the
update manifest is built from.

    python installer/updates/pack_voices.py --out C:\\tmp\\spfy_voice_dist

Why per VOICE and not per language: a language bundle makes every rebuild of
one in-house voice a 383 MB download for en-US. Per voice it is 101 MB for the
voice that actually changed. The language bundles are still produced (--bundles)
because they are the pleasant way to install everything the first time; they
cost storage on the release and nothing else.

WHAT IS HASHED IS NOT WHAT IS PACKED. The catalog hashes only the files that
define the voice -- vin, vdb, vcf, and the pitch marks where a voice has them.
The zip also carries the vendor .xml, the README and voice.json, so fixing a
typo in a README cannot make five hundred people re-download 101 MB.

Zip layout is `<lang>/<id>/<file>`, so the whole thing unzips directly into
%USERPROFILE%\\Documents\\Speechify\\ and lands where the SAPI DLL scans.

Identity (display name, language) comes from stage_voices.py, which reads the
voice's own VCF -- imported rather than reimplemented so the web catalog and
the update catalog can never disagree about what a voice is called.
"""

import argparse
import hashlib
import json
import os
import sys
import zipfile
from concurrent.futures import ProcessPoolExecutor, as_completed
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "spfy" / "wasm" / "tools"))
from stage_voices import LANG_DIR, display_name, vcf_params   # noqa: E402

# Voices whose PAYLOAD is published but whose SOURCE is not in the repo.
#
# Paulina is the case this exists for: her 264 MB VDB is over GitHub's 100 MB
# per-file push limit, so `es-MX/paulina/` is gitignored and a CI runner never
# sees it. That is a limit on files pushed INTO the repo -- it says nothing
# about release assets, which are not git objects and may be up to 2 GB. So the
# zip publishes perfectly well; it just has to be packed on a machine that has
# the files (`--emit-external paulina`), and the entry carried forward here.
#
# Deliberately a committed file rather than "keep whatever the last catalog
# said": the hashes are then reviewable in a diff, and a voice that genuinely
# goes away is removed by editing this, not by hoping something notices.
# Mirrors spfy/wasm/external_voices.json, which solves the same problem for
# the web demo.
EXTERNAL = Path(__file__).resolve().parent / "external_voices.json"

try:
    from tqdm import tqdm
except ImportError:                      # CI images without it still work
    def tqdm(it, **kw):
        return it

CHUNK = 8 * 1024 * 1024


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fp:
        for block in iter(lambda: fp.read(CHUNK), b""):
            h.update(block)
    return h.hexdigest()


def voice_members(d, vid):
    """(hashed, extra) -- what identifies the voice, and what merely ships
    beside it.

    ⚠ `<vid>8.vdb` by name, never a glob: en-US/tom also holds tom16.vdb,
    which is 237 MB, is gitignored, and is not what any shipped voice loads.
    """
    hashed = [d / f"{vid}.vin", d / f"{vid}8.vdb", d / f"{vid}.vcf"]
    for pm in (d / f"{vid}8.pmindex", d / f"{vid}8.pmdata"):
        if pm.is_file():
            hashed.append(pm)          # Speechify 4 mode will not start without them
    extra = [p for p in (d / f"{vid}8.xml", d / "README.md") if p.is_file()]
    return hashed, extra


def write_voice_json(d, entry, hashed):
    """Record what this voice is, beside the voice, so an installed copy can
    answer "which build am I?" without hashing 96 MB.

    Merges: `display`, `skip`, `default` and anything else a human put there
    are preserved -- stage_voices.py reads the same file.
    """
    path = d / "voice.json"
    doc = {}
    if path.is_file():
        try:
            doc = json.loads(path.read_text(encoding="utf-8"))
        except ValueError:
            doc = {}
    doc.update({
        "schema": 1,
        "id": entry["id"],
        "display": entry["display"],
        "lang": entry["lang"],
        "version": entry["version"],
        "files": [{"name": p.name,
                   "bytes": p.stat().st_size,
                   "sha256": h}
                  for p, h in zip(hashed, entry["_hashes"])],
    })
    path.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    return path


def pack_one(job):
    """One voice: hash, stamp, zip. Runs in a worker process -- zlib on a
    96 MB VDB is the whole cost of this script."""
    d = Path(job["dir"])
    vid, lang, version = job["id"], job["lang"], job["version"]
    out = Path(job["out"])

    hashed, extra = voice_members(d, vid)
    hashes = [sha256_file(p) for p in hashed]

    entry = {
        "id": vid,
        "display": job["display"],
        "lang": lang,
        "version": version,
        "_hashes": hashes,
        "files": [{"name": p.name, "bytes": p.stat().st_size, "sha256": h}
                  for p, h in zip(hashed, hashes)],
    }
    if job["write_voice_json"]:
        vj = write_voice_json(d, entry, hashed)
        extra = extra + [vj]

    zip_path = out / f"{vid}-{version}.zip"
    tmp = zip_path.with_suffix(".zip.part")
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for p in hashed + extra:
            z.write(p, f"{lang}/{vid}/{p.name}")
    os.replace(tmp, zip_path)

    entry["zip"] = zip_path.name
    entry["zip_bytes"] = zip_path.stat().st_size
    entry["zip_sha256"] = sha256_file(zip_path)
    entry.pop("_hashes")
    return entry


def pack_bundle(job):
    """Every voice of one language in a single zip: the first-install path."""
    lang, version, out = job["lang"], job["version"], Path(job["out"])
    zip_path = out / f"{lang}-voices-{version}.zip"
    tmp = zip_path.with_suffix(".zip.part")
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for v in job["voices"]:
            d = Path(v["dir"])
            hashed, extra = voice_members(d, v["id"])
            vj = d / "voice.json"
            if vj.is_file():
                extra = extra + [vj]
            for p in hashed + extra:
                z.write(p, f"{lang}/{v['id']}/{p.name}")
    os.replace(tmp, zip_path)
    return {"lang": lang, "version": version, "zip": zip_path.name,
            "zip_bytes": zip_path.stat().st_size,
            "zip_sha256": sha256_file(zip_path),
            "voices": [v["id"] for v in job["voices"]]}


def discover(root, only):
    """The repo convention, same as stage_voices.py: <lang>/<id>/<id>.vin +
    <id>8.vdb + <id>.vcf. A voice appears here by existing."""
    found = []
    for lang_dir in sorted(p for p in Path(root).iterdir()
                           if p.is_dir() and LANG_DIR.match(p.name)):
        for d in sorted(x for x in lang_dir.iterdir() if x.is_dir()):
            vid = d.name
            if only and vid not in only:
                continue
            trip = [d / f"{vid}.vin", d / f"{vid}8.vdb", d / f"{vid}.vcf"]
            if not all(p.is_file() for p in trip):
                continue
            over = {}
            vj = d / "voice.json"
            if vj.is_file():
                try:
                    over = json.loads(vj.read_text(encoding="utf-8"))
                except ValueError:
                    print(f"  ! {vj} is not valid JSON; ignoring", file=sys.stderr)
            if over.get("skip"):
                print(f"  - {lang_dir.name}/{vid}: skipped by voice.json")
                continue
            params = vcf_params(trip[2])
            found.append({
                "id": vid,
                "dir": str(d),
                "lang": over.get("lang") or params.get("language") or lang_dir.name,
                "display": over.get("display") or display_name(params, vid),
                "notify": bool(over.get("notify", True)),
                "prev_version": over.get("version", ""),
            })
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=str(REPO),
                    help="repo root holding the <lang>/<voice>/ tree")
    ap.add_argument("--out", required=True, help="directory to write zips into")
    ap.add_argument("--version", default=None,
                    help="calver stamped into new/changed voices "
                         "(default: today, UTC)")
    ap.add_argument("--voices", default="",
                    help="comma-separated ids; default is every voice found")
    ap.add_argument("--bundles", action="store_true",
                    help="also write one <lang>-voices-<version>.zip per language")
    ap.add_argument("--force", action="store_true",
                    help="repack even when the voice data is unchanged")
    ap.add_argument("--no-write-voice-json", action="store_true",
                    help="do not stamp voice.json into the source tree")
    ap.add_argument("--jobs", type=int, default=0,
                    help="worker processes (default: one per voice, capped at "
                         "the CPU count)")
    ap.add_argument("--emit-external", default="",
                    help="comma-separated ids to record in external_voices.json "
                         "after packing, for voices whose source is not in the "
                         "repo (paulina). Run this on a machine that HAS them; "
                         "commit the result and upload the zip once")
    ap.add_argument("--no-external", action="store_true",
                    help="ignore external_voices.json entirely")
    args = ap.parse_args()

    version = args.version or datetime.now(timezone.utc).strftime("%Y.%m.%d")
    only = {v.strip() for v in args.voices.split(",") if v.strip()}
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    ext_doc = {}
    if not args.no_external and EXTERNAL.is_file():
        try:
            ext_doc = json.loads(EXTERNAL.read_text(encoding="utf-8"))
        except ValueError:
            print(f"  ! {EXTERNAL} is not valid JSON; ignoring", file=sys.stderr)

    voices = discover(args.root, only)
    if not voices:
        print("no voices found", file=sys.stderr)
        return 1

    # Reuse of an unchanged voice keeps its OLD version string. Bumping every
    # voice on every publish would be dishonest -- and the checker compares
    # hashes, so a version bump alone would tell nobody anything.
    prev, prev_doc = {}, {}
    cat_path = out / "voices.json"
    if cat_path.is_file():
        try:
            prev_doc = json.loads(cat_path.read_text(encoding="utf-8"))
            if not args.force:
                for e in prev_doc.get("voices", []):
                    prev[e["id"]] = e
        except ValueError:
            pass

    jobs, reused = [], []
    for v in voices:
        old = prev.get(v["id"])
        if old and not args.force:
            hashed, _ = voice_members(Path(v["dir"]), v["id"])
            want = {p.name: (p.stat().st_size, None) for p in hashed}
            same = (len(old.get("files", [])) == len(want) and
                    all(f["name"] in want and f["bytes"] == want[f["name"]][0]
                        for f in old["files"]))
            if same and (out / old.get("zip", "")).is_file():
                # Sizes match and the zip is still there. Confirm with the
                # hashes before trusting it -- same size, different bytes is
                # exactly the case the whole feature exists to catch.
                if all(f["sha256"] == sha256_file(Path(v["dir"]) / f["name"])
                       for f in old["files"]):
                    old["display"] = v["display"]
                    old["notify"] = v["notify"]
                    reused.append(old)
                    print(f"  = {v['lang']}/{v['id']}: unchanged "
                          f"({old['version']})")
                    continue
        jobs.append({**v, "version": version, "out": str(out),
                     "write_voice_json": not args.no_write_voice_json})

    entries = list(reused)
    if jobs:
        workers = args.jobs or min(len(jobs), os.cpu_count() or 4)
        print(f"packing {len(jobs)} voice(s) with {workers} worker(s) "
              f"as version {version}")
        with ProcessPoolExecutor(max_workers=workers) as ex:
            futs = {ex.submit(pack_one, j): j for j in jobs}
            for f in tqdm(as_completed(futs), total=len(futs), unit="voice"):
                e = f.result()
                e["notify"] = futs[f]["notify"]
                entries.append(e)
                print(f"  + {e['lang']}/{e['id']}: {e['zip']} "
                      f"({e['zip_bytes'] / 1048576:.0f} MB)")

    # ⚠ --voices publishes a SUBSET; it does not delete the rest. The catalog
    # is the complete list, and make_manifest.py replaces the manifest's voices
    # array wholesale -- so an unmerged subset here would quietly remove every
    # other voice from update.json and stop notifying anyone about them.
    have = {e["id"] for e in entries}
    if only:
        for vid, old in sorted(prev.items()):
            if vid not in have:
                entries.append(old)
                have.add(vid)
                print(f"  ~ {old.get('lang','?')}/{vid}: kept from the "
                      f"published catalog ({old.get('version','?')}) -- not "
                      f"selected by --voices")

    # Externals LAST, and only for ids nothing else supplied, so a voice that
    # has since been committed wins over its stale external entry. Not filtered
    # by --voices, for the same reason as above.
    absent_ext_langs = set()
    for e in ext_doc.get("voices", []):
        if e.get("id") in have:
            continue
        entries.append(e)
        have.add(e["id"])
        absent_ext_langs.add(e.get("lang", ""))
        print(f"  ~ {e.get('lang','?')}/{e['id']}: carried forward from "
              f"external_voices.json ({e.get('version','?')}, "
              f"{e.get('zip','?')}) -- source not in this tree, NOT repacked")

    entries.sort(key=lambda e: (e["lang"], e["id"]))

    # Keyed by language, seeded with what is already published: "not asked to
    # rebuild" is not "there are none", and emitting an empty list would drop
    # every bundle from the manifest on the next publish. A rebuilt bundle
    # REPLACES its language's entry -- appending to a list left duplicates.
    by_bundle = {b["lang"]: b for b in prev_doc.get("bundles", []) if "lang" in b}

    if not args.bundles:
        if by_bundle:
            print(f"  ~ bundles: not rebuilt (--bundles not given); keeping "
                  f"the {len(by_bundle)} already published")
    elif only:
        # A bundle is "every voice of this language". It cannot be assembled
        # from a subset, so keep the published ones rather than replacing them
        # with something smaller wearing the same name.
        print("  ~ bundles: NOT rebuilt -- --voices selects a subset, so any "
              "bundle built now would be missing the rest. Keeping the "
              "published ones.")
    else:
        by_lang = {}
        for v in voices:
            by_lang.setdefault(v["lang"], []).append(v)

        # Same rule for a language whose source is only partly here: on a
        # runner that cannot see Paulina, an es-MX bundle would be Javier alone
        # -- still named es-MX-voices-*.zip, and wrong about it.
        for l in sorted(l for l in by_lang if l in absent_ext_langs):
            del by_lang[l]
            print(f"  ~ bundle {l}: NOT rebuilt -- an external voice of this "
                  f"language is not in this tree, so the bundle would be "
                  f"incomplete. Carrying the published one forward.")
        for b in ext_doc.get("bundles", []):
            if b.get("lang") in absent_ext_langs:
                by_bundle[b["lang"]] = b

        bjobs = [{"lang": l, "voices": vs, "version": version, "out": str(out)}
                 for l, vs in sorted(by_lang.items())]
        if bjobs:
            with ProcessPoolExecutor(max_workers=len(bjobs)) as ex:
                for f in as_completed([ex.submit(pack_bundle, j) for j in bjobs]):
                    b = f.result()
                    by_bundle[b["lang"]] = b
                    print(f"  + bundle {b['lang']}: {b['zip']} "
                          f"({b['zip_bytes'] / 1048576:.0f} MB)")

    bundles = [by_bundle[k] for k in sorted(by_bundle)]

    # Record the entries CI will never be able to produce for itself. Written
    # after the bundles so a language holding an external voice carries its
    # complete bundle across too.
    if args.emit_external:
        want = {v.strip() for v in args.emit_external.split(",") if v.strip()}
        keep = [e for e in entries if e["id"] in want and "zip_sha256" in e]
        missing = want - {e["id"] for e in keep}
        if missing:
            print(f"--emit-external: not packed on this machine, so not "
                  f"recorded: {', '.join(sorted(missing))}", file=sys.stderr)
        if keep:
            langs = {e["lang"] for e in keep}
            EXTERNAL.write_text(json.dumps({
                "schema": 1,
                "_comment": "Voices whose payload is published as a release "
                            "asset but whose source is NOT in the repo (over "
                            "GitHub's 100 MB per-file push limit). "
                            "pack_voices.py merges these into the catalog when "
                            "it cannot find the files, and skips rebuilding "
                            "any language bundle that would be incomplete "
                            "without them. Regenerate with --emit-external on "
                            "a machine that has the voice; upload the zips to "
                            "the voices release once. No git-lfs involved: "
                            "release assets are not git objects.",
                "voices": keep,
                "bundles": [b for b in bundles if b.get("lang") in langs],
            }, indent=2) + "\n", encoding="utf-8")
            print(f"wrote {EXTERNAL} ({', '.join(e['id'] for e in keep)})")

    cat = {"schema": 1,
           "generated": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
           "voices": entries,
           "bundles": bundles}
    cat_path.write_text(json.dumps(cat, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {cat_path} ({len(entries)} voice(s), {len(bundles)} bundle(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
