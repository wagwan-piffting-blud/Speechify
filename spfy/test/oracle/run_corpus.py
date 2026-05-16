"""Generate oracle WAVs by invoking the original Speechify engine.

Run on Windows where bin/spfy_dumpwav.exe is the byte-exact oracle.
Reads test/oracle/corpus.jsonl, runs each phrase through the oracle exe,
and writes WAVs into test/oracle/wavs/<id>.wav.

Once generated the WAVs are static -- commit them and reuse them as the
acceptance bar for every CORE-track milestone.

PREREQUISITE: bin/Speechify.exe (the server) must be running before this
script is invoked. spfy_dumpwav.exe is a client that connects to it via
SWIttsOpenPortEx; without the server you get rc=6 'SWIttsOpenPortEx
failed' on every entry. Start the server via BalRegisterVoice.bat or
manually before running this script.

  python run_corpus.py [--corpus PATH] [--out DIR] [--dumpwav PATH]
                       [--filter REGEX]
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_CORPUS = Path(__file__).resolve().with_name("corpus.jsonl")
DEFAULT_OUT    = Path(__file__).resolve().with_name("wavs")
DEFAULT_DUMPWAV = PROJECT_ROOT / "bin" / "spfy_dumpwav.exe"


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--corpus",  type=Path, default=DEFAULT_CORPUS)
    ap.add_argument("--out",     type=Path, default=DEFAULT_OUT)
    ap.add_argument("--dumpwav", type=Path, default=DEFAULT_DUMPWAV)
    ap.add_argument("--filter",  default=None,
                    help="regex applied to id; only matching entries are run")
    return ap.parse_args()


def load_corpus(path: Path) -> list[dict]:
    entries = []
    with path.open("r", encoding="utf-8") as fp:
        for ln, line in enumerate(fp, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            try:
                entries.append(json.loads(line))
            except json.JSONDecodeError as exc:
                raise SystemExit(f"{path}:{ln}: {exc}") from exc
    return entries


def run_one(dumpwav: Path, entry: dict, out_dir: Path) -> tuple[bool, str]:
    out_wav = out_dir / f"{entry['id']}.wav"
    out_dir.mkdir(parents=True, exist_ok=True)

    if entry["mode"] == "text":
        argv = [str(dumpwav), entry["text"], str(out_wav)]
    elif entry["mode"] == "spr":
        argv = [str(dumpwav), "--pron", entry["text"], str(out_wav)]
    else:
        return False, f"unknown mode {entry['mode']!r}"

    try:
        cp = subprocess.run(argv, capture_output=True, text=True, timeout=30,
                            check=False)
    except subprocess.TimeoutExpired:
        return False, "timeout"
    if cp.returncode != 0:
        return False, f"rc={cp.returncode} stderr={cp.stderr[:200]!r}"
    if not out_wav.exists():
        return False, "no output wav"
    return True, str(out_wav)


def main() -> int:
    args = parse_args()
    if not args.dumpwav.exists():
        print(f"oracle binary not found: {args.dumpwav}", file=sys.stderr)
        print("this script must run on Windows where the engine is installed.",
              file=sys.stderr)
        return 1

    entries = load_corpus(args.corpus)
    flt = re.compile(args.filter) if args.filter else None
    n_run = n_ok = 0
    for entry in entries:
        if flt and not flt.search(entry["id"]):
            continue
        n_run += 1
        ok, msg = run_one(args.dumpwav, entry, args.out)
        status = "ok " if ok else "FAIL"
        print(f"  {status} {entry['id']:12s} {msg}")
        if ok:
            n_ok += 1
    print(f"\n{n_ok}/{n_run} oracle WAVs generated -> {args.out}")
    return 0 if n_ok == n_run else 1


if __name__ == "__main__":
    sys.exit(main())
