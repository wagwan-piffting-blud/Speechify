"""feoracle.py -- Phase 3 / 03-06 Deliverable 4.

`python feoracle.py "the cat sat on the mat"` returns the engine FE
output for an arbitrary input text. SHA1(input_text)-keyed cache at
c:/tmp/feoracle_cache/<sha1>.json keeps repeated lookups fast.

The cache entry has integrity metadata:

  - captured_at        : ISO-8601 UTC timestamp
  - frida_runner_version : SHA1 of run_frida_capture.py at capture time
  - input_text + input_sha1
  - FE output payload (the schema below)

If captured_at is older than 7 days OR frida_runner_version differs
from the current run_frida_capture.py SHA1, the entry is treated as
stale and re-run on next lookup.

Output schema (matches the <interfaces> block in 03-06-PLAN.md):

  {
    "input_text": "the cat sat on the mat",
    "input_sha1": "...40 hex chars...",
    "phonemes": [...],
    "ctx5_per_slot": [[...], ...],
    "sp5_per_slot": [[...], ...],
    "voicing_per_slot": [...],
    "leaf_id_per_slot": [...],
    "pool_n_per_slot": [...],
    "pool_uids_first10_per_slot": [[...], ...],
    "token_stream": [...]
  }

Usage:
  python feoracle.py "<text>"
  python feoracle.py "<text>" --no-cache
  python feoracle.py --verify-cache

Constraints:
- Pure Python. Reuses spfy/test/oracle/run_frida_capture.py master mode.
- Does NOT add Frida hooks. The five hooks invoked are all
  function-entry-only (fe_token, fe_tree, prsl_slot, accent_decision,
  cart_walks); no Stalker.
- Cache directory is created on first run if absent.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Optional

PROJECT_ROOT = Path(__file__).resolve().parents[2]
RUN_FRIDA = PROJECT_ROOT / "spfy" / "test" / "oracle" / "run_frida_capture.py"
TRACE_ROOT = PROJECT_ROOT / "spfy" / "test" / "oracle" / "traces"
CACHE_DIR = Path("c:/tmp/feoracle_cache")
STALE_TTL = timedelta(days=7)

HOOKS = ("fe_token", "fe_tree", "prsl_slot", "accent_decision", "cart_walks")


def sha1_of_bytes(b: bytes) -> str:
    return hashlib.sha1(b).hexdigest()


def sha1_of_file(path: Path) -> str:
    h = hashlib.sha1()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(8192), b""):
            h.update(chunk)
    return h.hexdigest()


def cache_path_for(text_sha1: str) -> Path:
    return CACHE_DIR / f"{text_sha1}.json"


def is_stale(entry: dict, current_runner_sha1: str,
             now: Optional[datetime] = None) -> bool:
    if not entry.get("captured_at"):
        return True
    try:
        captured = datetime.fromisoformat(entry["captured_at"].replace("Z", "+00:00"))
    except ValueError:
        return True
    if (now or datetime.now(timezone.utc)) - captured > STALE_TTL:
        return True
    if entry.get("frida_runner_version") != current_runner_sha1:
        return True
    return False


def load_cache_entry(text_sha1: str) -> Optional[dict]:
    p = cache_path_for(text_sha1)
    if not p.exists():
        return None
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None


def write_cache_entry(text_sha1: str, payload: dict) -> Path:
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    p = cache_path_for(text_sha1)
    p.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    return p


def invoke_frida(text: str, runner_sha1: str) -> dict:
    """Run run_frida_capture.py master mode against a single-phrase
    corpus made from `text`. Returns the assembled FE payload."""
    if not RUN_FRIDA.exists():
        raise SystemExit(f"run_frida_capture.py not found at {RUN_FRIDA}")

    text_sha1 = sha1_of_bytes(text.encode("utf-8"))
    corpus_id = f"feoracle_{text_sha1[:12]}"

    with tempfile.TemporaryDirectory(prefix="feoracle_") as td:
        td_path = Path(td)
        corpus_path = td_path / "corpus.jsonl"
        corpus_path.write_text(json.dumps({
            "id": corpus_id, "mode": "text", "text": text,
            "tags": ["feoracle", "single_phrase"],
        }) + "\n", encoding="utf-8")

        # run_frida_capture.py master mode: invokes all registered hooks
        # in one session and writes per-hook trace files under
        # spfy/test/oracle/traces/<hook>/<id>.jsonl.
        cmd = [sys.executable, str(RUN_FRIDA),
               "--corpus", str(corpus_path),
               "--hook", "master"]
        try:
            r = subprocess.run(cmd, capture_output=True, text=True,
                               timeout=300)
        except subprocess.TimeoutExpired:
            raise SystemExit("Frida capture timed out after 300s")
        if r.returncode != 0:
            sys.stderr.write(r.stderr)
            raise SystemExit(f"Frida capture failed (exit={r.returncode})")

    return assemble_payload(text, text_sha1, corpus_id, runner_sha1)


def _load_jsonl(path: Path) -> list[dict]:
    if not path.exists():
        return []
    out: list[dict] = []
    for line in path.open("r", encoding="utf-8"):
        line = line.strip()
        if not line:
            continue
        try:
            out.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return out


def assemble_payload(text: str, text_sha1: str, corpus_id: str,
                     runner_sha1: str) -> dict:
    """Read per-hook trace files for `corpus_id` and assemble the
    schema documented in the module docstring + interfaces block."""
    prsl_recs = _load_jsonl(TRACE_ROOT / "prsl_slot" / f"{corpus_id}.jsonl")
    inner_recs = _load_jsonl(TRACE_ROOT / "inner_scorer" / f"{corpus_id}.jsonl")
    cart_recs = _load_jsonl(TRACE_ROOT / "cart_walks" / f"{corpus_id}.jsonl")
    token_recs = _load_jsonl(TRACE_ROOT / "fe_token" / f"{corpus_id}.jsonl")

    # Flatten batched samples if present.
    def _flatten(recs: list[dict]) -> list[dict]:
        out: list[dict] = []
        for r in recs:
            if r.get("type", "").endswith("_batch") and isinstance(r.get("samples"), list):
                out.extend(r["samples"])
            else:
                out.append(r)
        return out

    prsl_flat = _flatten(prsl_recs)
    inner_flat = _flatten(inner_recs)
    cart_flat = _flatten(cart_recs)

    by_slot = {}
    for s in prsl_flat:
        si = s.get("slot")
        if si is None:
            continue
        by_slot.setdefault(si, {})["prsl"] = s
    for s in inner_flat:
        si = s.get("slot")
        if si is None:
            continue
        by_slot.setdefault(si, {})["inner"] = s
    for s in cart_flat:
        si = s.get("slot")
        if si is None:
            continue
        by_slot.setdefault(si, {})["cart"] = s

    ctx5_per_slot: list[list[int]] = []
    sp5_per_slot: list[list[int]] = []
    voicing_per_slot: list[Optional[int]] = []
    leaf_id_per_slot: list[Optional[list]] = []
    pool_n_per_slot: list[Optional[int]] = []
    pool_uids_first10: list[list] = []

    for si in sorted(by_slot):
        b = by_slot[si]
        prsl = b.get("prsl", {})
        inner = b.get("inner", {})
        cart = b.get("cart", {})
        ctx5_per_slot.append(prsl.get("ctx") or [])
        sp5_per_slot.append(inner.get("sp_target") or [])
        voicing_per_slot.append(prsl.get("voicing"))
        leaf_id_per_slot.append([cart.get("leaf_mean"), cart.get("leaf_var")])
        pool_n_per_slot.append(prsl.get("n_cands"))
        pool_uids_first10.append((prsl.get("uids") or [])[:10])

    return {
        "input_text": text,
        "input_sha1": text_sha1,
        "captured_at": datetime.now(timezone.utc).isoformat(),
        "frida_runner_version": runner_sha1,
        "phonemes": [],   # populated from fe_token if available
        "ctx5_per_slot": ctx5_per_slot,
        "sp5_per_slot": sp5_per_slot,
        "voicing_per_slot": voicing_per_slot,
        "leaf_id_per_slot": leaf_id_per_slot,
        "pool_n_per_slot": pool_n_per_slot,
        "pool_uids_first10_per_slot": pool_uids_first10,
        "token_stream": token_recs,
    }


def cmd_lookup(text: str, no_cache: bool) -> int:
    text_sha1 = sha1_of_bytes(text.encode("utf-8"))
    runner_sha1 = sha1_of_file(RUN_FRIDA) if RUN_FRIDA.exists() else "unknown"

    if not no_cache:
        entry = load_cache_entry(text_sha1)
        if entry and not is_stale(entry, runner_sha1):
            print(json.dumps(entry, indent=2))
            return 0

    payload = invoke_frida(text, runner_sha1)
    write_cache_entry(text_sha1, payload)
    print(json.dumps(payload, indent=2))
    return 0


def cmd_verify_cache() -> int:
    if not CACHE_DIR.is_dir():
        print(f"Cache dir does not exist: {CACHE_DIR}")
        return 0
    runner_sha1 = sha1_of_file(RUN_FRIDA) if RUN_FRIDA.exists() else "unknown"
    n_total = n_stale = 0
    for fp in sorted(CACHE_DIR.glob("*.json")):
        n_total += 1
        try:
            entry = json.loads(fp.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            n_stale += 1
            print(f"STALE (unreadable): {fp.name}")
            continue
        if is_stale(entry, runner_sha1):
            n_stale += 1
            cap = entry.get("captured_at", "?")
            rv = entry.get("frida_runner_version", "?")
            why = ("ttl_expired" if entry.get("captured_at") else "no_metadata")
            if entry.get("frida_runner_version") != runner_sha1:
                why = "runner_sha_mismatch"
            print(f"STALE ({why}): {fp.name}  captured_at={cap}  "
                  f"runner_sha1={rv[:12]}...")
    print(f"\n{n_total} cache entries; {n_stale} stale. "
          "Stale entries are re-run automatically on next lookup of the "
          "matching input_text. Use --no-cache to force re-run.")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(
        description="Engine FE oracle CLI (Phase 3 / 03-06 Deliverable 4). "
                    "SHA1-keyed cache + 7-day TTL + runner-version "
                    "invalidation.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    p.add_argument("text", nargs="?", default=None,
                   help="Input phrase to capture FE output for.")
    p.add_argument("--no-cache", action="store_true",
                   help="Bypass cache; always re-run Frida.")
    p.add_argument("--verify-cache", action="store_true",
                   help="Walk cache dir and report stale entries.")
    args = p.parse_args()

    if args.verify_cache:
        return cmd_verify_cache()
    if not args.text:
        p.print_help()
        return 2
    return cmd_lookup(args.text, args.no_cache)


if __name__ == "__main__":
    raise SystemExit(main())
