"""Extract one-shot pre-initStage1 raw-byte blobs from an fe_ctrl_watch
JSONL trace and write them as raw binary files.

Used by the K2 verbose-mode differential-bisection work: load Speechify's
pre-init ctrl/state into our hosted-FE process via SPFY_FE_HOST_BISECT_*
env vars. See RESUME_K2.md.

Usage:
    # default — extract ctrl blob
    python spfy/tools/extract_pre_init_ctrl.py \
        spfy/test/oracle/traces/fe_ctrl_watch/nat_018.jsonl \
        spfy/test/oracle/traces/fe_pre_init_ctrl/nat_018.bin

    # state blob
    python spfy/tools/extract_pre_init_ctrl.py \
        spfy/test/oracle/traces/fe_ctrl_watch/nat_018.jsonl \
        spfy/test/oracle/traces/fe_pre_init_state/nat_018.bin --kind state
"""
from __future__ import annotations

import json
import sys
from pathlib import Path


EVENT_KINDS = {"ctrl": "ctrl_pre_init_raw", "state": "state_pre_init_raw"}


def main(argv: list[str]) -> int:
    kind = "ctrl"
    args = list(argv[1:])
    if "--kind" in args:
        i = args.index("--kind")
        kind = args[i + 1]
        del args[i:i + 2]
    if len(args) != 2 or kind not in EVENT_KINDS:
        print(__doc__, file=sys.stderr)
        return 2
    src = Path(args[0])
    dst = Path(args[1])
    if not src.exists():
        print(f"source not found: {src}", file=sys.stderr)
        return 1

    want_type = EVENT_KINDS[kind]
    found = None
    with src.open(encoding="utf-8") as fp:
        for line in fp:
            line = line.strip()
            if not line:
                continue
            try:
                ev = json.loads(line)
            except json.JSONDecodeError:
                continue
            if ev.get("type") == want_type:
                found = ev
                break
    if not found:
        print(f"no {want_type} event in {src}", file=sys.stderr)
        return 1
    hex_str = found.get("hex") or ""
    n_bytes = int(found.get("n_bytes") or 0)
    blob = bytes.fromhex(hex_str)
    if len(blob) != n_bytes:
        print(f"len mismatch: hex decoded to {len(blob)} bytes, "
              f"event reported {n_bytes}", file=sys.stderr)
        return 1

    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(blob)
    print(f"wrote {len(blob)} bytes -> {dst}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
