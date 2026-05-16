"""Make-style dependency tracker for the Phase 1 engine-trace capture pipeline.

Wraps `run_frida_capture.HookSession` (DO NOT re-implement Frida lifecycle,
synthesis invocation, or JSONL flattening — they are imported verbatim).
The 200-phrase corpus times 6 LIVE hooks ~= 1,200 trace files; Frida is
flaky and the capture is long-running, so partial failure must be
tolerable. This script turns "1,200 captures in one shot" into "any
subset on demand" and skips already-fresh (id, hook) pairs on rerun.

Outer loop = HOOK (one Frida attach per hook), inner loop = corpus
(amortizes the ~3s attach cost per Pitfall 4 / D-08). State is persisted
per-(id, hook) atomically via os.replace so a kill mid-batch leaves a
consistent state file. CONTEXT.md decisions D-07 (Make-style tracker),
D-08 (< 30s wall per re-capture; never crash the engine), and D-15
(cart_walks RETIRED for new phrases; cart_walker_args is the active
substitute) are the canonical references.

The 32-phrase seed retains its existing `cart_walks` traces. capture.py
NEVER deletes them; it simply does not re-capture under the cart_walks
hook for new corpus phrases. Operators that explicitly request
`--only <id>::cart_walks` get a RETIRED warning and a non-zero exit.

Usage (PowerShell):
  python spfy/test/oracle/capture.py [--status]
                                     [--dry-run]
                                     [--only <id>::<hook>]
                                     [--force]
                                     [--hooks <hook> ...]
                                     [--max-attempts <int>]
                                     [--state-file PATH]

Engine prerequisite: bin/Speechify.exe must be running for live capture
(NOT required for --status / --dry-run).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

# Reuse the existing battle-tested Frida driver — DO NOT re-implement.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_frida_capture import (  # noqa: E402
    HookSession,
    synth_one,
    write_jsonl,
    load_corpus,
    HOOK_JS,
    DEFAULT_CORPUS,
    DEFAULT_OUT,
    DEFAULT_DUMPWAV,
)

# CONTEXT.md D-15: 6 LIVE hooks for the 168 new corpus phrases.
PHASE1_HOOKS = [
    "wsola_buffer",
    "prsl_slot",
    "cart_walker_args",
    "inner_scorer",
    "fe_tree",
    "viterbi_dp",
]

# CONTEXT.md D-15: cart_walks is RETIRED for new phrases (preserved
# in run_frida_capture.HOOK_JS but does NOT appear in PHASE1_HOOKS).
# Listed here so --only ...::cart_walks still emits a RETIRED warning.
RETIRED_HOOKS = ["cart_walks"]

DEFAULT_STATE = Path(__file__).resolve().with_name(".capture_state.json")
DEFAULT_SCRATCH = Path("C:/tmp/spfy_frida_wavs")
MAX_CONSECUTIVE_FAILURES = 2  # threshold for "engine appears dead"

# Pattern E (PATTERNS.md): 1MB-chunked sha256.
_HASH_CHUNK = 1 << 20

# WARNING-05: --only id must not contain path separators or traversal.
_SAFE_ID_RE = re.compile(r"^[A-Za-z0-9_]+$")


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def iso_now() -> str:
    """UTC ISO timestamp with trailing 'Z' (no '+00:00')."""
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace(
        "+00:00", "Z")


def file_sha256(path: Path) -> str:
    """Streaming sha256 of a file (Pattern E)."""
    h = hashlib.sha256()
    with path.open("rb") as fp:
        for chunk in iter(lambda: fp.read(_HASH_CHUNK), b""):
            h.update(chunk)
    return h.hexdigest()


def sha_of_row(entry: dict) -> str:
    """sha256 of a corpus row, canonical-JSON encoded for stability."""
    canon = json.dumps(entry, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(canon.encode("utf-8")).hexdigest()


def load_state(path: Path) -> dict:
    """Load state file; return a fresh skeleton if absent. SystemExit on
    JSON parse failure (consistent with load_corpus error semantics)."""
    if not path.exists():
        return {"version": 1, "entries": {}}
    try:
        with path.open("r", encoding="utf-8") as fp:
            data = json.load(fp)
    except json.JSONDecodeError as exc:
        raise SystemExit(f"{path}: malformed state file: {exc}") from exc
    if not isinstance(data, dict) or "entries" not in data:
        raise SystemExit(f"{path}: state file missing 'entries'")
    data.setdefault("version", 1)
    if not isinstance(data["entries"], dict):
        raise SystemExit(f"{path}: 'entries' must be an object")
    return data


def atomic_write_state(state: dict, path: Path) -> None:
    """tmp + os.replace = atomic on Windows + POSIX (RESEARCH.md Risk R9)."""
    tmp = path.with_suffix(".json.tmp")
    path.parent.mkdir(parents=True, exist_ok=True)
    with tmp.open("w", encoding="utf-8") as fp:
        json.dump(state, fp, indent=2, sort_keys=True)
    os.replace(tmp, path)


def needs_recapture(state: dict, key: str, input_sha: str,
                    hook_js_sha: str, out_path: Path) -> bool:
    """True if (id, hook) pair must be (re-)captured.

    Triggers: missing state entry, input sha drift, hook JS sha drift,
    output file deleted, output sha drift (RESEARCH Q4 RESOLVED), or
    previous attempt recorded ok=False.
    """
    entries = state.get("entries", {})
    rec = entries.get(key)
    if rec is None:
        return True
    if rec.get("input_sha") != input_sha:
        return True
    if rec.get("hook_js_sha") != hook_js_sha:
        return True
    if not out_path.exists():
        return True
    recorded = rec.get("output_sha")
    if recorded is not None:
        try:
            if file_sha256(out_path) != recorded:
                return True
        except OSError:
            return True
    if not rec.get("ok", False):
        return True
    return False


def capture_one(sess: HookSession, entry: dict, dumpwav: Path,
                scratch: Path, out_dir: Path) -> tuple[bool, int]:
    """Per-entry capture (PATTERNS.md "Per-entry capture pattern")."""
    sess.reset_events()
    ok, _msg = synth_one(dumpwav, entry, scratch)
    events = sess.flush_and_collect(settle_s=0.3)
    n = write_jsonl(out_dir / f"{entry['id']}.jsonl", events)
    return ok, n


def run_one_hook(hook: str, entries: list[dict], out_root: Path,
                 dumpwav: Path, scratch: Path, state: dict,
                 state_path: Path, max_attempts: int,
                 hook_js_sha: str, force: bool) -> dict:
    """Capture all stale entries for a single hook. Outer-loop=hook so
    Frida attach is amortized across 168+ corpus rows.

    Bails (returns) early if MAX_CONSECUTIVE_FAILURES hit — engine
    likely dead; resume picks up here on next run. State is written
    after every (id, hook) for crash safety.
    """
    out_dir = out_root / hook
    out_dir.mkdir(parents=True, exist_ok=True)
    sess = HookSession(hook, quiet_rpc=True)
    n_consec_fail = 0
    try:
        for entry in entries:
            key = f"{entry['id']}::{hook}"
            input_sha = sha_of_row(entry)
            out_path = out_dir / f"{entry['id']}.jsonl"
            if not force and not needs_recapture(
                    state, key, input_sha, hook_js_sha, out_path):
                continue
            attempts = 0
            ok = False
            n_lines = 0
            while attempts < max_attempts and not ok:
                attempts += 1
                ok, n_lines = capture_one(
                    sess, entry, dumpwav, scratch, out_dir)
                if ok:
                    n_consec_fail = 0
                    break
                n_consec_fail += 1
                if n_consec_fail >= MAX_CONSECUTIVE_FAILURES:
                    print(
                        "  [warn] engine appears dead — restart "
                        "bin/Speechify.exe and re-run capture.py "
                        "(will resume)",
                        file=sys.stderr,
                    )
                    state["entries"][key] = {
                        "input_sha":   input_sha,
                        "hook_js_sha": hook_js_sha,
                        "captured_at": iso_now(),
                        "n_lines":     0,
                        "ok":          False,
                        "attempts":    attempts,
                    }
                    atomic_write_state(state, state_path)
                    return state
                time.sleep(0.5)  # brief breather before retry
            state["entries"][key] = {
                "input_sha":   input_sha,
                "hook_js_sha": hook_js_sha,
                "output_sha":  (file_sha256(out_path)
                                if out_path.exists() else None),
                "captured_at": iso_now(),
                "n_lines":     n_lines,
                "ok":          ok,
                "attempts":    attempts,
            }
            atomic_write_state(state, state_path)
            status = "ok " if ok else "FAIL"
            print(f"  {status} {hook}/{entry['id']:14s} "
                  f"lines={n_lines:7d} attempts={attempts}")
    finally:
        try:
            sess.detach(timeout_s=5.0)
        except Exception:
            pass
    return state


# ---------------------------------------------------------------------------
# Argparse + subcommands
# ---------------------------------------------------------------------------

def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--corpus",  type=Path, default=DEFAULT_CORPUS)
    ap.add_argument("--out",     type=Path, default=DEFAULT_OUT)
    ap.add_argument("--dumpwav", type=Path, default=DEFAULT_DUMPWAV)
    ap.add_argument("--scratch", type=Path, default=DEFAULT_SCRATCH)
    ap.add_argument(
        "--hooks", nargs="+", default=list(PHASE1_HOOKS),
        choices=list(PHASE1_HOOKS) + list(RETIRED_HOOKS),
        help=("hooks to capture (default: 6 LIVE PHASE1_HOOKS); "
              "RETIRED hooks are accepted but skipped with a warning"),
    )
    ap.add_argument(
        "--only", default=None,
        help='single key "<id>::<hook>" — restrict capture to one row',
    )
    ap.add_argument("--force", action="store_true",
                    help="re-capture even fresh entries")
    ap.add_argument("--dry-run", action="store_true",
                    help="walk corpus + state; print plan; no Frida attach")
    ap.add_argument("--status", action="store_true",
                    help="walk state file + traces dir; report freshness; "
                         "no Frida attach")
    ap.add_argument("--max-attempts", type=int, default=3)
    ap.add_argument("--state-file", type=Path, default=DEFAULT_STATE)
    return ap.parse_args(argv)


def _entry_index(entries: list[dict]) -> dict[str, dict]:
    return {e["id"]: e for e in entries}


def cmd_status(args: argparse.Namespace) -> int:
    """Walk state + traces dir; report per-hook freshness. No Frida."""
    state = load_state(args.state_file)
    entries: list[dict] = []
    if args.corpus.exists():
        entries = load_corpus(args.corpus)
    by_id = _entry_index(entries)
    print(f"state: {args.state_file}")
    print(f"corpus: {args.corpus} ({len(entries)} entries)")
    print(f"traces: {args.out}")
    for hook in PHASE1_HOOKS:
        out_dir = args.out / hook
        # hook JS sha — guard for missing JS file (e.g. partial install)
        hook_js_sha = (file_sha256(HOOK_JS[hook])
                       if hook in HOOK_JS and HOOK_JS[hook].exists()
                       else None)
        fresh = stale = missing = 0
        total = 0
        for entry in entries:
            total += 1
            key = f"{entry['id']}::{hook}"
            input_sha = sha_of_row(entry)
            out_path = out_dir / f"{entry['id']}.jsonl"
            if hook_js_sha is None:
                stale += 1
                continue
            if needs_recapture(state, key, input_sha, hook_js_sha, out_path):
                if out_path.exists():
                    stale += 1
                else:
                    missing += 1
            else:
                fresh += 1
        print(f"  {hook:20s} {fresh}/{total} fresh, "
              f"{stale} stale, {missing} missing")
    # also report seed-32 cart_walks coverage as informational
    cart_walks_dir = args.out / "cart_walks"
    n_cart_walks = (sum(1 for _ in cart_walks_dir.glob("*.jsonl"))
                    if cart_walks_dir.exists() else 0)
    print(f"  {'cart_walks (RETIRED)':20s} "
          f"{n_cart_walks} preserved trace files (seed-32 only; D-15)")
    # silence the lint about unused vars in by_id
    _ = by_id
    return 0


def cmd_dry_run(args: argparse.Namespace) -> int:
    """Print the would-capture plan; never attach Frida.

    Accounting is explicit per plan-zero D-04: print n_planned alongside
    n_total (all considered pairs) and n_fresh (pairs skipped because
    they were already fresh). The off-by-4 noted in 01-UAT.md (1400 vs
    expected 1404 = 234 × 6 LIVE hooks) is now visible: when
    n_total - n_planned != 0, the difference is shown as the n_fresh /
    n_skipped breakdown rather than buried.
    """
    state = load_state(args.state_file)
    if not args.corpus.exists():
        print(f"corpus not found: {args.corpus}", file=sys.stderr)
        return 1
    entries = load_corpus(args.corpus)
    n_planned = 0
    n_total = 0
    n_fresh = 0
    n_hook_skipped = 0
    for hook in args.hooks:
        if hook in RETIRED_HOOKS:
            print(f"  [skip] {hook}: RETIRED — no new captures (D-15)")
            n_hook_skipped += 1
            continue
        if hook not in HOOK_JS:
            print(f"  [skip] {hook}: not registered in HOOK_JS")
            n_hook_skipped += 1
            continue
        if not HOOK_JS[hook].exists():
            print(f"  [skip] {hook}: JS file missing at {HOOK_JS[hook]}")
            n_hook_skipped += 1
            continue
        hook_js_sha = file_sha256(HOOK_JS[hook])
        out_dir = args.out / hook
        for entry in entries:
            n_total += 1
            key = f"{entry['id']}::{hook}"
            input_sha = sha_of_row(entry)
            out_path = out_dir / f"{entry['id']}.jsonl"
            if args.force or needs_recapture(
                    state, key, input_sha, hook_js_sha, out_path):
                print(f"would capture {key}")
                n_planned += 1
            else:
                n_fresh += 1
    n_live_hooks = len(args.hooks) - n_hook_skipped
    print(f"\n{n_planned} (id, hook) pairs would be captured")
    print(f"  ({n_total} total considered = {len(entries)} corpus rows × "
          f"{n_live_hooks} live hooks; {n_fresh} already fresh)")
    return 0


def _parse_only(only: str) -> tuple[str, str]:
    """Split '<id>::<hook>'; harden against path traversal (WARNING-05)."""
    if "::" not in only:
        raise SystemExit(
            f"--only must be '<id>::<hook>' (got {only!r})")
    id_, hook = only.split("::", 1)
    if not _SAFE_ID_RE.match(id_):
        raise SystemExit(
            f"--only id must match [A-Za-z0-9_]+ "
            f"(rejected: {id_!r}; path-traversal mitigation per WARNING-05)")
    if not _SAFE_ID_RE.match(hook):
        raise SystemExit(
            f"--only hook must match [A-Za-z0-9_]+ (rejected: {hook!r})")
    return id_, hook


def _preflight_engine(dumpwav: Path, scratch: Path) -> int:
    """Run small text + SPR probes to confirm engine is live and the
    `\\!` escape round-trips. Returns 0 on text-probe success (SPR
    failure is a warning per RESEARCH Q5 RESOLVED), 3 if the engine is
    not running."""
    if not dumpwav.exists():
        print(f"oracle binary not found: {dumpwav}", file=sys.stderr)
        return 3
    text_probe = {"id": "_probe", "mode": "text", "text": "Hi.", "tags": []}
    ok, msg = synth_one(dumpwav, text_probe, scratch)
    if not ok:
        print("engine not running — start bin/Speechify.exe first "
              f"(probe failed: {msg})", file=sys.stderr)
        return 3
    spr_probe = {"id": "_probe_spr", "mode": "spr",
                 "text": "\\![.1Sa.0kIG]", "tags": []}
    ok2, msg2 = synth_one(dumpwav, spr_probe, scratch)
    if not ok2:
        print("  [warn] SPR probe failed — `\\!` escape may not "
              f"round-trip on this shell ({msg2})", file=sys.stderr)
    return 0


def cmd_live(args: argparse.Namespace) -> int:
    """Live capture path: attach Frida, iterate hooks, persist state."""
    if not args.corpus.exists():
        print(f"corpus not found: {args.corpus}", file=sys.stderr)
        return 1
    entries = load_corpus(args.corpus)
    state = load_state(args.state_file)

    # --only short-circuit: validate, restrict to one (id, hook).
    if args.only is not None:
        id_, hook = _parse_only(args.only)
        if hook in RETIRED_HOOKS:
            print(f"  [warn] hook '{hook}' is RETIRED for new captures "
                  "(D-15); the seed-32 traces are preserved on disk. "
                  "Use cart_walker_args for new phrases.",
                  file=sys.stderr)
            return 2
        if hook not in HOOK_JS:
            print(f"  [error] hook '{hook}' not registered in HOOK_JS",
                  file=sys.stderr)
            return 2
        by_id = _entry_index(entries)
        if id_ not in by_id:
            print(f"  [error] id '{id_}' not found in corpus {args.corpus}",
                  file=sys.stderr)
            return 2
        entries = [by_id[id_]]
        args.hooks = [hook]

    # Pre-flight probes (skip for --only too — operator wants speed).
    rc = _preflight_engine(args.dumpwav, args.scratch)
    if rc != 0:
        return rc

    for hook in args.hooks:
        if hook in RETIRED_HOOKS:
            print(f"  [warn] {hook}: RETIRED for new captures — "
                  "skipping (D-15)", file=sys.stderr)
            continue
        if hook not in HOOK_JS:
            print(f"  [warn] {hook}: not registered in HOOK_JS — skipping",
                  file=sys.stderr)
            continue
        if not HOOK_JS[hook].exists():
            print(f"  [warn] {hook}: JS file missing at {HOOK_JS[hook]} — "
                  "skipping", file=sys.stderr)
            continue
        hook_js_sha = file_sha256(HOOK_JS[hook])
        print(f"\n=== hook: {hook} ({len(entries)} corpus entries) ===")
        run_one_hook(
            hook=hook,
            entries=entries,
            out_root=args.out,
            dumpwav=args.dumpwav,
            scratch=args.scratch,
            state=state,
            state_path=args.state_file,
            max_attempts=args.max_attempts,
            hook_js_sha=hook_js_sha,
            force=args.force,
        )
    return 0


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)

    if args.status:
        return cmd_status(args)
    if args.dry_run:
        return cmd_dry_run(args)

    # Live path requires frida.
    try:
        import frida  # noqa: F401
    except ImportError:
        print("frida not installed: pip install frida frida-tools",
              file=sys.stderr)
        return 1

    return cmd_live(args)


if __name__ == "__main__":
    sys.exit(main())
