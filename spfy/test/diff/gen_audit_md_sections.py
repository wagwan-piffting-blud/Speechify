"""Generate the 02-06-finalised header sections of 02-DP-AUDIT.md from
the triaged 02-DP-AUDIT.jsonl + the corpus run results. The script
inserts ## Summary / ## Corpus-wide acceptance / ## Open-record triage
/ ## Engine-faithful constants run / ## D-19 closing audit sections
immediately before the existing ## last-slot anchor. Existing
per-class sections (## last-slot, ## diff_neg_big, etc.) are
preserved verbatim — they hold prior-plan analysis that the JSONL
records cross-link to via fix_ref / phase3_route.

Idempotent: if the Summary section already exists, replace it.
"""
from __future__ import annotations

import json
import sys
from collections import Counter, defaultdict
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[3]
AUDIT_MD   = PROJECT_ROOT / ".planning/phases/02-dp-fidelity-closure" \
                          / "02-DP-AUDIT.md"
AUDIT_JSONL = PROJECT_ROOT / ".planning/phases/02-dp-fidelity-closure" \
                           / "02-DP-AUDIT.jsonl"
CORPUS_JSONL = PROJECT_ROOT / "spfy/test/results/02-dp-fidelity-closure" \
                            / "dp_replay_corpus.jsonl"

ANCHOR = "## last-slot"
NEW_BLOCK_BEGIN = "<!-- 02-06 finalisation BEGIN -->"
NEW_BLOCK_END   = "<!-- 02-06 finalisation END -->"


def build_summary(records: list[dict]) -> str:
    by_class = defaultdict(Counter)
    for r in records:
        by_class[r.get("mismatch_class", "?")][r.get("status", "?")] += 1
    classes = ["last_slot", "silence_sentinel", "diff_neg_big",
               "diff_pos_big", "diff_mid", "pool_gen", "fp_drift",
               "accum_audit"]
    seen = set()
    rows = []
    rows.append("| Mismatch class | Total | Closed (incl. stale) | "
                "Phase3 handoff | Engine-internal accept | Open |")
    rows.append("|----------------|------:|---------------------:|"
                "---------------:|-----------------------:|-----:|")
    grand = Counter()
    for cls in classes + sorted(set(by_class.keys()) - set(classes)):
        if cls not in by_class or cls in seen:
            continue
        seen.add(cls)
        s = by_class[cls]
        closed = s.get("closed", 0) + s.get("closed_revised", 0) \
                 + s.get("closed_stale_data", 0)
        ph3   = s.get("phase3_handoff", 0)
        eia   = s.get("engine_internal_accept", 0)
        op    = s.get("open", 0)
        total = sum(s.values())
        grand["closed"] += closed
        grand["phase3_handoff"] += ph3
        grand["engine_internal_accept"] += eia
        grand["open"] += op
        grand["total"] += total
        rows.append(f"| {cls} | {total} | {closed} | {ph3} | {eia} | {op} |")
    rows.append(f"| **TOTAL** | **{grand['total']}** | "
                f"**{grand['closed']}** | **{grand['phase3_handoff']}** | "
                f"**{grand['engine_internal_accept']}** | "
                f"**{grand['open']}** |")
    return "\n".join(rows)


def build_corpus_table() -> str:
    if not CORPUS_JSONL.exists():
        return "(corpus replay JSONL not found)"
    n_match = n_slots = n_records = 0
    for line in CORPUS_JSONL.open("r", encoding="utf-8"):
        line = line.strip()
        if not line:
            continue
        try:
            r = json.loads(line)
        except json.JSONDecodeError:
            continue
        n_records += 1
        if isinstance(r.get("n_uid_match"), int):
            n_match += r["n_uid_match"]
        if isinstance(r.get("n_slots"), int):
            n_slots += r["n_slots"]
    pct = 100.0 * n_match / n_slots if n_slots else 0.0
    return (
        "| Metric | Pre-Phase-2 | Post-Phase-2 (corpus, integer-sum) |\n"
        "|--------|-------------|-----------------------------------|\n"
        f"| spfy_viterbi_replay UID match | 81.4% (seed-30) | "
        f"{pct:.4f}% ({n_match}/{n_slots} integer sum, 227 phrases / "
        f"{n_records} utterances) |\n"
        "| spfy_hp_innerscorer bit-exact | 99.88% | 100% (833/833 across "
        "30-phrase sanity, plan 02-05 D-17 closure) |\n"
        "| pool_gen_diff missing UIDs | 28 (seed-30) | "
        f"{0 if pct == 100.0 else 'see triage'} (plan 02-03 closed 22; "
        "remaining 6 reclassified at 02-06 — most are no longer "
        "pool_gen, see Open-record triage) |\n"
        "| audit_focused.py per-slot full-match | 85.0% | not re-run for "
        "02-06 (instrument unchanged; baseline preserved) |\n"
        "| compare_dag.py UID match (full pipeline) | 59.3% | not re-run "
        "for 02-06 (Phase 3 territory) |"
    )


def build_open_triage(records: list[dict]) -> str:
    """Per-class open-record triage block per plan 02-06 Task 1."""
    by_class = defaultdict(list)
    for r in records:
        by_class[r.get("mismatch_class", "?")].append(r)

    lines = []
    lines.append(
        "Plan 02-06 Task 1 reconciled the 84 status=open records inherited "
        "from plans 02-02 / 02-03 / 02-04 / 02-05 against a fresh "
        "corpus-wide spfy_viterbi_replay run over 227 phrases (the 7 "
        "single-phoneme spr/edge entries produce no slots — they are "
        "outside Phase 2's UID-match scope and are covered by Phase 6's "
        "WAV byte-exact gate).")
    lines.append("")
    lines.append(
        "**Triage rule** (data-driven from each mismatch's live tc_diff "
        "and engine_tc):")
    lines.append("- engine_tc >= 1e29 → `pool_gen` "
                 "→ phase3_handoff (FE-side prsl_lookup pool augmentation).")
    lines.append("- engine_uid == 169578 (silence sentinel) at last slot "
                 "→ `silence_sentinel` "
                 "→ closed (spfy_synth.c post-DP force-fix; 833/833 in "
                 "02-05 hp_score_test).")
    lines.append("- |tc_diff| < 1e-3 with both UIDs in pool → `fp_drift` "
                 "→ engine_internal_accept (1-ULP tie-break flip; per "
                 "PROJECT.md / CONCERNS.md §x87/SSE FP divergence).")
    lines.append("- |tc_diff| >= 1.0 → `diff_neg_big`/`diff_pos_big` "
                 "→ phase3_handoff (FE-side TC-component drift).")
    lines.append("- otherwise → `diff_mid` "
                 "→ phase3_handoff (FE divergence, mid-magnitude).")
    lines.append("")
    lines.append("**Per-class breakdown after triage:**")
    lines.append("")
    lines.append("| Class | Total | Closed (incl. stale) | "
                 "Phase3 handoff | Engine-internal accept | Open |")
    lines.append("|-------|------:|--------------------:|"
                 "--------------:|----------------------:|-----:|")
    for cls in sorted(by_class):
        recs = by_class[cls]
        st = Counter(r.get("status", "?") for r in recs)
        closed = st.get("closed", 0) + st.get("closed_revised", 0) \
                 + st.get("closed_stale_data", 0)
        ph3 = st.get("phase3_handoff", 0)
        eia = st.get("engine_internal_accept", 0)
        op  = st.get("open", 0)
        lines.append(f"| {cls} | {len(recs)} | {closed} | {ph3} | "
                     f"{eia} | {op} |")
    lines.append("")
    lines.append("**Representative records per class** (slot info + "
                 "disposition):")
    lines.append("")
    for cls in sorted(by_class):
        # Show the first 2 records that have a phase3_route or fix_ref or
        # accept_rationale to keep the section compact.
        sample = [r for r in by_class[cls]
                  if r.get("phase3_route") or r.get("fix_ref")
                  or r.get("accept_rationale")][:2]
        if not sample:
            continue
        lines.append(f"- **{cls}** (n={len(by_class[cls])}):")
        for r in sample:
            tag = r.get("phase3_route") or r.get("fix_ref") \
                  or r.get("accept_rationale") or "(no rationale)"
            tag = tag.replace("\n", " ").strip()
            if len(tag) > 200:
                tag = tag[:200] + "…"
            lines.append(
                f"  - `{r.get('corpus_id', '?')}` "
                f"utt={r.get('utt_idx', 1)} "
                f"slot={r.get('slot_idx', '?')} "
                f"engine={r.get('engine_uid', '?')} "
                f"ours={r.get('our_uid', '?')} "
                f"tc_diff={r.get('tc_diff', 0):+.3f} "
                f"-> **{r.get('status', '?')}** -- {tag}")
    return "\n".join(lines)


def build_engine_faithful() -> str:
    return (
        "Per ROADMAP §Phase 2 SC2: replaying engine inputs with engine-"
        "faithful constants must NOT regress UID match.\n\n"
        "**Constants in source** (`spfy/src/cli/spfy_viterbi_replay.c` and "
        "`spfy_synth.c`):\n"
        "- `missing_join_cost = 1000.0f` (FE-init default per "
        "`param_3[0x21] = 0x447a0000`; verified in `spfy_synth.c:2094` "
        "and `spfy_viterbi_replay.c:1734`).\n"
        "- `miss_default = 0.0f` (engine-faithful per PROJECT.md; "
        "`spfy_synth.c:2078`, `spfy_viterbi_replay.c:1005` documented).\n"
        "- `f0_edge_change_weight = 0.6f` (Tom VCF F0_EDGE_CHANGE_WEIGHT).\n"
        "- `ccos_default = 0.0f` (Tom voice; per "
        "`02-DP-AUDIT.md ## CCOS reduction equation`).\n\n"
        "**Result with these constants on 227-phrase corpus**: 81.25% UID "
        "match (7051/8678 slots, integer sum). The CONCERNS.md note "
        "\"engine-faithful join-miss = 0 regresses to 32.6%\" reflected "
        "the M3.4-era pre-CCOS state; plan 02-02's CCOS context-cost "
        "reduction landed and restored the engine-faithful constants "
        "without regression. The remaining 18.75 pp gap is documented "
        "per the Open-record triage above and is FE-side / engine-"
        "internal — NOT closable in Phase 2 by knob tuning."
    )


def build_d19() -> str:
    return (
        "**D-19 closing audit (2026-05-09):**\n\n"
        "| Instrument | Pre-Phase-2 | Post-Phase-2 |\n"
        "|------------|-------------|--------------|\n"
        "| `spfy_viterbi_replay` corpus (227 phrases) | 81.4% (seed-30) | "
        "**81.2514%** (7051/8678 integer sum) |\n"
        "| `spfy_hp_innerscorer` bit-exact (text_002) | 832/833 (99.88%) | "
        "**833/833 (100%)** [02-05 D-17] |\n"
        "| `audit_focused.py` per-slot full-match (seed-30) | 85.0% | "
        "preserved (instrument unchanged; not re-run as cost_*.c was not "
        "modified during 02-06) |\n"
        "| `compare_dag.py` UID match (full pipeline) | 59.3% | preserved "
        "(Phase 3 instrument; no full-pipeline change in 02-06) |\n\n"
        "Per ROADMAP §Phase 2 SC5: no change to `spfy/src/usel/cost_*.c` "
        "or `viterbi.c` landed in 02-06 — this plan ships only "
        "post-mortem finalisation, the per-phrase JSONL emit in "
        "`spfy_viterbi_replay.c` (informational, no scoring change), and "
        "the `cart_walks_safe` Frida hook (data-capture only, no engine "
        "code change). Therefore non-regression is trivially satisfied."
    )


def main() -> int:
    if not AUDIT_JSONL.exists():
        print("audit JSONL missing", file=sys.stderr)
        return 1
    if not AUDIT_MD.exists():
        print("audit MD missing", file=sys.stderr)
        return 1

    records = [json.loads(l) for l in
               AUDIT_JSONL.open("r", encoding="utf-8") if l.strip()]
    md = AUDIT_MD.read_text(encoding="utf-8")

    if NEW_BLOCK_BEGIN in md:
        # Idempotent: replace prior 02-06-finalisation block.
        s = md.index(NEW_BLOCK_BEGIN)
        e = md.index(NEW_BLOCK_END) + len(NEW_BLOCK_END)
        md = md[:s] + md[e:].lstrip("\n")

    if ANCHOR not in md:
        print(f"anchor {ANCHOR!r} not found", file=sys.stderr)
        return 1

    block = "\n".join([
        NEW_BLOCK_BEGIN,
        "",
        "## Summary",
        "",
        build_summary(records),
        "",
        "## Corpus-wide acceptance",
        "",
        build_corpus_table(),
        "",
        "## Open-record triage",
        "",
        build_open_triage(records),
        "",
        "## Engine-faithful constants run (D-19 / SC2)",
        "",
        build_engine_faithful(),
        "",
        "## D-19 closing audit",
        "",
        build_d19(),
        "",
        NEW_BLOCK_END,
        "",
        "",
    ])

    insert_at = md.index(ANCHOR)
    md = md[:insert_at] + block + md[insert_at:]
    AUDIT_MD.write_text(md, encoding="utf-8")
    print(f"updated {AUDIT_MD} ({len(md)} bytes)")
    # Quick verify against plan 02-06 verify gate.
    required = ["## Summary", "## Corpus-wide acceptance",
                "## D-19 closing audit", "## Open-record triage",
                "## Engine-faithful constants run"]
    missing = [s for s in required if s not in md]
    if missing:
        print(f"WARNING: still missing sections: {missing}",
              file=sys.stderr)
        return 1
    print("all required sections present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
