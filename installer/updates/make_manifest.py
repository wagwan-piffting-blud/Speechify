#!/usr/bin/env python3
"""Build update.json -- the one file every installed copy of spfy polls.

    # after packing voices
    python installer/updates/make_manifest.py \
        --engine-version 2026.08.22 --catalog C:\\tmp\\spfy_voice_dist\\voices.json \
        -o update.json

    # engine-only publish: keep the voice catalog that is already live
    python installer/updates/make_manifest.py \
        --engine-version 2026.08.23 --merge-from current-update.json -o update.json

TWO WORKFLOWS WRITE THIS FILE and they know different halves of it. The engine
release runs on every push to main and knows the new engine version; the voice
publish runs rarely and knows the voice catalog. Whichever runs must carry the
other's half forward unchanged, which is what --merge-from is for. Losing the
voices array would silently stop every voice notification with nothing in the
output to say so, so a merge that finds no voices anywhere is an ERROR here,
not a warning -- pass --allow-empty-voices if that is genuinely what you want.

The checker ignores keys it does not know, so this file can grow. It must
never change the MEANING of a key without bumping "schema", which makes older
checkers stay quiet rather than misreport.
"""

import argparse
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

DEFAULT_REPO = "wagwan-piffting-blud/Speechify"


def load_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--engine-version", default=None,
                    help="calver of the engine release being published; "
                         "required unless --engine-from is given")
    ap.add_argument("--engine-from", default=None,
                    help="a previous update.json to take the ENTIRE engine "
                         "block from. The voice publish uses this: it knows "
                         "nothing about the engine and must not re-announce a "
                         "release the maintainer marked quiet")
    ap.add_argument("--catalog", default=None,
                    help="voices.json from pack_voices.py")
    ap.add_argument("--merge-from", default=None,
                    help="a previous update.json to take the voices array from "
                         "when --catalog is not given")
    ap.add_argument("--repo", default=DEFAULT_REPO)
    ap.add_argument("--voices-tag", default="voices",
                    help="release tag the voice zips are attached to")
    ap.add_argument("--engine-url", default=None,
                    help="default: the release page for --engine-version")
    ap.add_argument("--message", default="",
                    help="one line shown with the notification")
    ap.add_argument("--no-engine-notify", action="store_true",
                    help="publish the version but do not notify anyone about "
                         "it (routine push, docs-only release)")
    ap.add_argument("--allow-empty-voices", action="store_true")
    ap.add_argument("-o", "--out", required=True)
    args = ap.parse_args()

    base = f"https://github.com/{args.repo}"
    voices, bundles = [], []

    engine = None
    if args.engine_from and Path(args.engine_from).is_file():
        engine = load_json(args.engine_from).get("engine")
        if engine and args.engine_version:
            engine["version"] = args.engine_version
    if engine is None:
        if not args.engine_version:
            print("make_manifest: need --engine-version, or an --engine-from "
                  "file that already carries one", file=sys.stderr)
            return 1
        engine = {
            "version": args.engine_version,
            "notify": not args.no_engine_notify,
            "url": args.engine_url or
                   f"{base}/releases/tag/{args.engine_version}",
            "message": args.message,
        }
    else:
        if args.engine_url:
            engine["url"] = args.engine_url
        if args.no_engine_notify:
            engine["notify"] = False
        if args.message:
            engine["message"] = args.message

    if args.catalog:
        cat = load_json(args.catalog)
        for e in cat.get("voices", []):
            voices.append({
                "id": e["id"],
                "display": e.get("display", e["id"]),
                "lang": e.get("lang", ""),
                "version": e["version"],
                "notify": bool(e.get("notify", True)),
                "url": f"{base}/releases/download/{args.voices_tag}/{e['zip']}",
                "zip_bytes": e.get("zip_bytes", 0),
                "zip_sha256": e.get("zip_sha256", ""),
                "files": e.get("files", []),
            })
        for b in cat.get("bundles", []):
            bundles.append({
                "lang": b["lang"],
                "version": b["version"],
                "url": f"{base}/releases/download/{args.voices_tag}/{b['zip']}",
                "zip_bytes": b.get("zip_bytes", 0),
                "voices": b.get("voices", []),
            })
    elif args.merge_from and Path(args.merge_from).is_file():
        old = load_json(args.merge_from)
        voices = old.get("voices", [])
        bundles = old.get("bundles", [])

    if not voices and not args.allow_empty_voices:
        print("make_manifest: no voices, from either --catalog or --merge-from.\n"
              "               publishing this would silently switch off every "
              "voice notification.\n"
              "               pass --allow-empty-voices if that is intended.",
              file=sys.stderr)
        return 1

    doc = {
        "schema": 1,
        "generated": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "engine": engine,
        "voices": voices,
        "bundles": bundles,
    }
    Path(args.out).write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {args.out}: engine {engine['version']}"
          f"{'' if engine.get('notify', True) else ' (quiet)'}, "
          f"{len(voices)} voice(s), {len(bundles)} bundle(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
