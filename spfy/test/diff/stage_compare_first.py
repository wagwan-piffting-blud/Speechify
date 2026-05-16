"""First-divergence labeller for cross-stage trace comparison (D-14, D-15).

Walks STAGE_ORDER in fixed sequence and reports the FIRST stage whose JSONL
trace diverges between native and oracle for a given corpus id.  Field-level
diff math is delegated to ``stage_compare.py`` — this module is a thin wrapper
that adds (a) the canonical stage ordering, (b) a primary-then-fallback hook
lookup, and (c) the D-12 short-name mapping consumed by the per-phrase report.

Decisions referenced (see ``.planning/phases/01-validation-infrastructure/01-CONTEXT.md``):

* **D-12** — value contract for the ``stage_first_divergence`` field of the
  per-phrase JSONL: legal values are ``"cart"``, ``"prsl"``, ``"cost"``,
  ``"viterbi"``, ``"wsola"`` (or ``null``).  ``STAGE_TO_D12`` enforces the
  mapping from hook name -> D-12 short name.
* **D-14** — fixed STAGE_ORDER and "wrap, do not duplicate" rule: this file
  imports ``load_stage`` and ``field_diff`` from ``stage_compare`` and never
  re-implements them.
* **D-15** — for the 168 new corpus phrases the cart-stage trace lives under
  ``cart_walker_args/`` instead of ``cart_walks/``.  When the primary
  ``cart_walks`` trace is missing (on either side), we fall back to
  ``cart_walker_args`` *before* declaring "no trace at this stage".  Both
  hooks map to the D-12 short name ``"cart"`` because they capture the same
  logical CART-walk stage, just from different producers (the legacy seed-32
  hook vs. the wider cart_walker_args hook).

Comparison strategy: ``load_stage(path, None)`` returns ALL records in line
order (no ``type`` filter) — ``stage_compare.py``'s diff strategy is
line-index alignment, and we follow it verbatim.  A length mismatch at a
given stage is treated as divergence; otherwise records are compared
pairwise with ``field_diff`` and the first non-empty diff wins.

This module does NOT exit non-zero on divergence — D-14 is a *labelling*
tool, not a pass/fail gate.  Divergence on a known-divergent phrase is the
expected outcome; it is the caller (Phase 6 orchestrator) that decides
what to do with the resulting label.

Usage::

  python stage_compare_first.py <id> --native-dir <X> --oracle-dir <Y>
                                     [--emit-jsonl PATH]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# stage_compare.py is the field-diff authority; never re-implement field_diff.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from stage_compare import load_stage, field_diff  # noqa: E402


# D-14 fixed stage order.  ``cart_walks`` is FIRST even though Plan 03 retires
# it for new captures: the legacy seed-32 traces still live under that hook,
# and ``STAGE_FALLBACK`` wires ``cart_walker_args`` in for the new corpus.
STAGE_ORDER = [
    "cart_walks",
    "prsl_slot",
    "inner_scorer",
    "viterbi_dp",
    "wsola_buffer",
]

# Invariant: STAGE_TO_D12 maps every STAGE_ORDER element AND every
# STAGE_FALLBACK value to the D-12 short name.  ``cart_walker_args`` shares
# the ``"cart"`` label with ``cart_walks`` per D-15: same logical stage,
# different producer.
STAGE_TO_D12 = {
    "cart_walks":       "cart",
    "cart_walker_args": "cart",
    "prsl_slot":        "prsl",
    "inner_scorer":     "cost",
    "viterbi_dp":       "viterbi",
    "wsola_buffer":     "wsola",
}

# Invariant: when a primary hook's trace is missing for a given id, fall back
# to the listed substitute hook before concluding "no trace at this stage".
# Currently only cart_walks -> cart_walker_args (D-15).  Adding entries here
# in the future MUST be matched by a STAGE_TO_D12 entry for the substitute.
STAGE_FALLBACK = {"cart_walks": "cart_walker_args"}


def first_divergence(
    corpus_id: str,
    native_dir: Path,
    oracle_dir: Path,
) -> tuple[str | None, str | None]:
    """Walk STAGE_ORDER and return the first stage with a divergence.

    Returns a tuple ``(hook_name, d12_label)``:

    * ``hook_name`` is the actual hook directory walked (so the caller can
      see whether the D-15 fallback triggered, e.g. ``cart_walker_args`` vs
      ``cart_walks``).
    * ``d12_label`` is the D-12 short name (``"cart"``, ``"prsl"``,
      ``"cost"``, ``"viterbi"``, or ``"wsola"``).

    Both are ``None`` when no divergence is found across all stages OR no
    comparable trace pair exists at any stage (e.g. native traces have not
    been produced yet — Phase 1 may run with oracle-only data).
    """
    for stage in STAGE_ORDER:
        # Build the candidate hook list: the primary, then any fallback.
        candidates = [stage]
        if stage in STAGE_FALLBACK:
            candidates.append(STAGE_FALLBACK[stage])

        for hook in candidates:
            nat = native_dir / hook / f"{corpus_id}.jsonl"
            ora = oracle_dir / hook / f"{corpus_id}.jsonl"
            if not nat.exists() or not ora.exists():
                # Try fallback for this stage; if no fallback or fallback
                # also missing, the outer loop advances to the next stage.
                continue
            n = load_stage(nat, None)
            o = load_stage(ora, None)
            if len(n) != len(o):
                return hook, STAGE_TO_D12[hook]
            for a, b in zip(n, o):
                if field_diff(a, b):
                    return hook, STAGE_TO_D12[hook]
            # This hook's traces compared cleanly; do NOT try the fallback
            # for this stage — advance to the next stage in STAGE_ORDER.
            break

    return None, None


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("id", help="corpus id (e.g. text_029)")
    ap.add_argument(
        "--native-dir",
        type=Path,
        required=True,
        help="root containing <hook>/<id>.jsonl trees produced by the "
             "-DSPFY_TRACE=1 native build",
    )
    ap.add_argument(
        "--oracle-dir",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "oracle" / "traces",
        help="root containing <hook>/<id>.jsonl trees produced by "
             "Frida capture (default: spfy/test/oracle/traces)",
    )
    ap.add_argument(
        "--emit-jsonl",
        type=Path,
        default=None,
        help="write {id, stage_first_divergence, hook} as a single JSON line "
             "to this path",
    )
    return ap.parse_args()


def main() -> int:
    args = parse_args()
    hook, d12 = first_divergence(args.id, args.native_dir, args.oracle_dir)

    print(
        f"{args.id}: stage_first_divergence="
        f"{d12 if d12 is not None else 'null'} "
        f"(hook={hook if hook is not None else 'none'})"
    )

    if args.emit_jsonl is not None:
        args.emit_jsonl.parent.mkdir(parents=True, exist_ok=True)
        rec = {
            "id": args.id,
            "stage_first_divergence": d12,
            "hook": hook,
        }
        with args.emit_jsonl.open("w", encoding="utf-8") as fp:
            fp.write(json.dumps(rec) + "\n")

    # D-14 is a labelling tool, not a pass/fail gate.  Divergence is the
    # expected outcome on a known-divergent phrase; the caller decides.
    return 0


if __name__ == "__main__":
    sys.exit(main())
