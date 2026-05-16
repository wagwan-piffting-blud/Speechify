"""Word-prefix bisection to localise divergence (D-09 / D-10 / D-11; D-16 stub).

Word-level binary search over the input phrase: synthesise prefixes of length
``words[:k]`` and apply a pluggable failure predicate (``--mode``) to find
the smallest ``k`` for which synthesis diverges from the oracle.  Once the
minimal failing prefix is found, ``first_diff_byte`` (from ``wav_diff.py``)
provides a sample-index fallback inside that prefix's WAV — i.e. the hybrid
"word-then-sample" bisection of D-09.

Pluggable predicates (D-10):

* ``--mode=wav``    — byte-exact comparison vs ``oracle-wav-dir`` prefix WAV.
* ``--mode=stage``  — delegates to ``stage_compare_first.first_divergence``.
* ``--mode=uid``    — **D-16 Phase-1 stub**.  Wires the predicate plumbing
  and surfaces a stderr warning (WARNING-03), but full UID-path comparison
  via ``spfy_viterbi_replay.exe`` is deferred to Phase 6.  In Phase 1 the
  predicate treats a missing oracle ``prsl_slot`` trace OR synth failure as
  divergence and otherwise returns False.

Subprocess synth (D-11): ``--binary <path>`` swaps the synth executable.
The default points at the standalone ``c:/tmp/spfy_build/src/cli/spfy_synth.exe``
build (see TESTING.md pinned path) which loads VIN+VDB+VCF directly and does
NOT require Speechify.exe to be running.  ``bin/spfy_dumpwav.exe`` is the
engine-server alternative and DOES require the server up.  All subprocess
invocations use list-form argv with ``shell=False``; SPR rows are rejected
at argv parsing because ``\\!`` accents must never pass through a shell.

CAVEAT (RESEARCH.md Pitfall 3): word-level prefix-monotonicity does NOT hold
for all phrases.  The Speechify FE injects intonation breaks based on the
*full* phrase (text_029 is the canonical violator: stripping the trailing
clause changes the intonation contour over the kept prefix, so a prefix
that "works" alone may still mask an upstream causal divergence).  The
bisection therefore finds *A* shortest failing prefix, not necessarily
*THE* causal word — interpret results accordingly.

PRECONDITION: ``--binary <path>`` resolves to an executable accepting either
``<text> <out.wav>`` (text mode) or ``--pron <text> <out.wav>`` (SPR mode).
``--binary`` MUST be a trusted local build; this is a developer tool with
no signing/hash check.

Decisions (see ``.planning/phases/01-validation-infrastructure/01-CONTEXT.md``):
D-09 (word-then-sample bisection), D-10 (pluggable ``--mode``), D-11
(subprocess synth + ``--binary`` swap), D-16 (``--mode=uid`` Phase-1 stub).

Usage::

  python locate_divergence.py <id> --mode {wav|uid|stage}
                              [--binary PATH]
                              [--corpus PATH]
                              [--oracle-wav-dir PATH]
                              [--oracle-trace-dir PATH]
                              [--native-trace-dir PATH]
                              [--emit-jsonl PATH]
                              [--timeout SECONDS]
                              [--scratch DIR]
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

# Re-use byte-diff helpers — never duplicate them here.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from wav_diff import file_sha256, first_diff_byte, rms_error_s16  # noqa: E402,F401


# --- D-16 stub one-shot warning gate ---------------------------------------
#
# WARNING-03: ``--mode=uid`` is a Phase-1 stub.  We want the deferral to be
# loud (so a developer running the tool sees it) but not noisy (one line per
# run, not one per predicate call).  ``_uid_warned`` is flipped True after
# the first emit; it is intentionally module-level so tests can reset it.
_uid_warned = False


# Path-traversal hardening for ``--id``.  Argparse passes the id straight
# into filesystem path construction (``oracle_dir / 'prsl_slot' / f'{id}.jsonl'``);
# without validation a hostile id like ``../../etc/passwd`` would escape the
# trace tree.  Whitelist conservative chars used by the existing corpus.
_SAFE_ID_RE = re.compile(r"^[A-Za-z0-9_]+$")


def synth_with(
    binary: Path,
    text: str,
    mode: str,
    out_wav: Path,
    timeout_s: int = 30,
) -> bool:
    """Subprocess synth via list-form argv (``shell=False``).

    Returns True on success (rc==0 AND output WAV exists), False on synth
    failure or timeout.  Pattern is copied verbatim from
    ``spfy/test/oracle/run_frida_capture.py:synth_one`` (lines 191-208).
    """
    out_wav.parent.mkdir(parents=True, exist_ok=True)
    if mode == "text":
        argv = [str(binary), text, str(out_wav)]
    elif mode == "spr":
        argv = [str(binary), "--pron", text, str(out_wav)]
    else:
        return False
    try:
        cp = subprocess.run(
            argv,
            capture_output=True,
            text=True,
            timeout=timeout_s,
            check=False,
            shell=False,
        )
    except subprocess.TimeoutExpired:
        return False
    return cp.returncode == 0 and out_wav.exists()


def predicate_wav(
    prefix: str,
    native_bin: Path,
    oracle_wav: Path,
    scratch: Path,
    timeout_s: int = 30,
) -> bool:
    """True iff synthesis of ``prefix`` byte-diverges from ``oracle_wav``.

    Invariant: a non-existent ``oracle_wav`` is treated as divergence so the
    bisection treats "no oracle" as a failing prefix (caller fault, not a
    clean answer).  A synth failure also counts as divergence.
    """
    nat = scratch / f"native_{abs(hash(prefix)):x}.wav"
    if not synth_with(native_bin, prefix, "text", nat, timeout_s=timeout_s):
        return True   # synth failure counts as divergence
    if not oracle_wav.exists():
        return True
    return nat.read_bytes() != oracle_wav.read_bytes()


def predicate_uid(
    prefix: str,
    native_bin: Path,
    oracle_dir: Path,
    corpus_id: str,
    scratch: Path,
    timeout_s: int = 30,
) -> bool:
    """D-16 Phase-1 stub.

    True iff ``prefix`` synth fails OR the oracle ``prsl_slot`` trace is
    missing.  Full UID-path comparison via ``spfy_viterbi_replay.exe`` is
    deferred to Phase 6 — see CONTEXT.md D-16.

    Invariant: a one-shot stderr warning ('# WARNING: phase-1 stub') is
    printed on first invocation per run to surface the deferral
    (WARNING-03).  Subsequent calls in the same process stay silent so a
    bisection of N prefixes does not produce N copies of the warning.

    Implementation:
      * Resolve ``oracle_path = oracle_dir / 'prsl_slot' / f'{corpus_id}.jsonl'``;
        missing oracle => divergence (RESEARCH.md A1).
      * Synthesise the prefix; synth failure => divergence.
      * Otherwise return False (assume non-divergent in Phase 1).

    Phase 6 will replace this with prefix-aware engine PATH-walked vs DP
    UID-match comparison.
    """
    global _uid_warned
    if not _uid_warned:
        print(
            "# WARNING: --mode=uid is a phase-1 stub per CONTEXT.md D-16; "
            "full UID-path comparison defers to Phase 6.",
            file=sys.stderr,
        )
        _uid_warned = True

    oracle_path = oracle_dir / "prsl_slot" / f"{corpus_id}.jsonl"
    if not oracle_path.exists():
        print(
            f"  [warn] no oracle prsl_slot trace at {oracle_path} — "
            f"treating as divergence",
            file=sys.stderr,
        )
        return True

    nat = scratch / f"native_uid_{abs(hash(prefix)):x}.wav"
    if not synth_with(native_bin, prefix, "text", nat, timeout_s=timeout_s):
        return True
    return False


def predicate_stage(
    prefix: str,
    native_bin: Path,
    native_dir: Path,
    oracle_dir: Path,
    corpus_id: str,
    scratch: Path,
    timeout_s: int = 30,
) -> bool:
    """True iff ``stage_compare_first.first_divergence`` reports a non-null
    divergence for ``corpus_id``.

    Note: ``predicate_stage`` does not currently re-run native traces per
    prefix — it relies on whatever traces already exist under
    ``native_dir``.  Phase 6 will couple this with a per-prefix native
    capture; in Phase 1 it functions as a one-shot label, not a true
    per-prefix bisection signal.  Synth is still invoked once so the
    minimal-failing-prefix WAV exists for downstream byte-diff reporting.
    """
    from stage_compare_first import first_divergence  # local import: avoids
    # paying the cost when callers stick to wav/uid modes.

    # Best-effort native synth so a downstream wav_diff has a file to look at;
    # synth failure does NOT short-circuit (the stage check is the real signal).
    nat = scratch / f"native_stage_{abs(hash(prefix)):x}.wav"
    synth_with(native_bin, prefix, "text", nat, timeout_s=timeout_s)

    hook, d12 = first_divergence(corpus_id, native_dir, oracle_dir)
    return d12 is not None


def bisect_word_prefix(words: list[str], pred) -> int:
    """Return smallest ``k`` in ``[1..len(words)]`` such that
    ``pred(" ".join(words[:k]))`` is True; or ``len(words) + 1`` if no
    prefix diverges.

    Invariant: this is a closed-form O(log N) loop with a fixed termination
    condition (``lo < hi``); cannot loop infinitely.  The algorithm assumes
    monotonicity (``pred(prefix_k) == False`` implies ``pred(prefix_{<k}) ==
    False``); see RESEARCH.md Pitfall 3 for the violation case (text_029-style
    phrases where the FE injects intonation breaks based on the full phrase).
    Documented as WARNING-02 in the algorithm contract.

    Edge cases:
      * Empty ``words``: returns 1 (no prefix exists; convention "no prefix
        diverges" => ``len(words) + 1`` would equal 1 here, so the result is
        also 1 — empty input is degenerate).
      * First word already diverges: returns 1.
      * No prefix diverges: returns ``len(words) + 1``.
    """
    if not words:
        return 1
    if pred(" ".join(words[:1])):
        return 1
    if not pred(" ".join(words)):
        return len(words) + 1

    # Standard lower-bound bisection: invariant is
    #   pred(words[:lo]) is False AND pred(words[:hi+1]) is True.
    lo, hi = 1, len(words)
    while lo < hi:
        mid = (lo + hi) // 2
        if pred(" ".join(words[:mid + 1])):
            hi = mid
        else:
            lo = mid + 1
    return lo + 1


def _load_corpus_row(corpus_path: Path, corpus_id: str) -> dict | None:
    """Linear-scan the JSONL corpus for the row whose ``id`` matches.

    Returns ``None`` if not found.  Corpus is small enough (<1k rows in
    Phase 1) that an indexed lookup is unnecessary.
    """
    if not corpus_path.exists():
        return None
    with corpus_path.open("r", encoding="utf-8") as fp:
        for line in fp:
            line = line.strip()
            if not line:
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                continue
            if row.get("id") == corpus_id:
                return row
    return None


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("id", help="corpus id (e.g. text_029)")
    ap.add_argument(
        "--mode",
        choices=["wav", "uid", "stage"],
        default="wav",
        help="failure predicate (D-10): wav=byte-exact, uid=D-16 stub, "
             "stage=delegates to stage_compare_first",
    )
    ap.add_argument(
        "--binary",
        type=Path,
        default=Path("c:/tmp/spfy_build/src/cli/spfy_synth.exe"),
        help="synth executable (D-11). Default: standalone spfy_synth.exe "
             "(no engine server). Use bin/spfy_dumpwav.exe to drive the "
             "engine instead — REQUIRES Speechify.exe running.",
    )
    ap.add_argument(
        "--corpus",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "oracle" / "corpus.jsonl",
    )
    ap.add_argument(
        "--oracle-wav-dir",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "oracle" / "wavs",
    )
    ap.add_argument(
        "--oracle-trace-dir",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "oracle" / "traces",
    )
    ap.add_argument(
        "--native-trace-dir",
        type=Path,
        default=None,
        help="native trace tree (only used by --mode=stage)",
    )
    ap.add_argument(
        "--emit-jsonl",
        type=Path,
        default=None,
        help="write the bisection result record to this path",
    )
    ap.add_argument("--timeout", type=int, default=30)
    ap.add_argument(
        "--scratch",
        type=Path,
        default=Path(tempfile.gettempdir()) / "spfy_locate",
    )
    return ap.parse_args()


def main() -> int:
    args = parse_args()

    # --- Path-traversal hardening (T-04-02) -------------------------------
    # ``args.id`` is interpolated into multiple filesystem paths below; reject
    # anything outside ``[A-Za-z0-9_]`` before opening any file.
    if not _SAFE_ID_RE.match(args.id):
        print(
            f"error: invalid id {args.id!r}: must match [A-Za-z0-9_]+",
            file=sys.stderr,
        )
        return 2

    row = _load_corpus_row(args.corpus, args.id)
    if row is None:
        print(
            f"error: id {args.id!r} not found in corpus {args.corpus}",
            file=sys.stderr,
        )
        return 2

    if row.get("mode") != "text":
        print(
            "error: locate_divergence only supports text mode in Phase 1 "
            f"(corpus row mode={row.get('mode')!r}); SPR support deferred",
            file=sys.stderr,
        )
        return 2

    text = row.get("text", "")
    words = text.split()

    args.scratch.mkdir(parents=True, exist_ok=True)

    # --- Build a thin closure over the chosen predicate -------------------
    if args.mode == "wav":
        oracle_wav = args.oracle_wav_dir / f"{args.id}.wav"

        def pred(prefix: str) -> bool:
            return predicate_wav(
                prefix, args.binary, oracle_wav, args.scratch,
                timeout_s=args.timeout,
            )

    elif args.mode == "uid":
        def pred(prefix: str) -> bool:
            return predicate_uid(
                prefix, args.binary, args.oracle_trace_dir, args.id,
                args.scratch, timeout_s=args.timeout,
            )

    else:  # stage
        if args.native_trace_dir is None:
            print(
                "error: --mode=stage requires --native-trace-dir",
                file=sys.stderr,
            )
            return 2

        def pred(prefix: str) -> bool:
            return predicate_stage(
                prefix, args.binary, args.native_trace_dir,
                args.oracle_trace_dir, args.id, args.scratch,
                timeout_s=args.timeout,
            )

    k = bisect_word_prefix(words, pred)
    n_words = len(words)

    # --- Build the result record (D-12-aligned) ---------------------------
    rec: dict = {
        "id": args.id,
        "mode": args.mode,
        "shortest_failing_prefix": None,
        "n_words_total": n_words,
        "n_words_failing": None,
        "first_diff_byte": None,
        "binary": str(args.binary),
        "monotonicity_caveat": "see Pitfall 3 in RESEARCH.md",
        "phase1_stub_warning": (
            "see CONTEXT.md D-16" if args.mode == "uid" else None
        ),
    }

    if k > n_words:
        # No prefix diverged.
        print(
            f"{args.id} mode={args.mode} prefix=NONE/{n_words} "
            f"text={text!r} (no divergence found)"
        )
    else:
        prefix_text = " ".join(words[:k])
        rec["shortest_failing_prefix"] = prefix_text
        rec["n_words_failing"] = k

        # Sample-index fallback: re-synthesise the minimal failing prefix
        # and record first_diff_byte vs the oracle prefix WAV (only
        # available for --mode=wav since uid/stage don't have a per-prefix
        # oracle WAV mapping in Phase 1).
        if args.mode == "wav":
            oracle_wav = args.oracle_wav_dir / f"{args.id}.wav"
            nat = args.scratch / f"native_final_{abs(hash(prefix_text)):x}.wav"
            if synth_with(
                args.binary, prefix_text, "text", nat,
                timeout_s=args.timeout,
            ) and oracle_wav.exists():
                a = nat.read_bytes()
                b = oracle_wav.read_bytes()
                if a != b:
                    rec["first_diff_byte"] = first_diff_byte(a, b)

        print(
            f"{args.id} mode={args.mode} prefix={k}/{n_words} "
            f"text={prefix_text!r}"
        )

    if args.emit_jsonl is not None:
        args.emit_jsonl.parent.mkdir(parents=True, exist_ok=True)
        with args.emit_jsonl.open("w", encoding="utf-8") as fp:
            fp.write(json.dumps(rec) + "\n")

    return 0


if __name__ == "__main__":
    sys.exit(main())
