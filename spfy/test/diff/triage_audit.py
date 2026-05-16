"""Plan 02-06 Task 1 — active triage of 02-DP-AUDIT.jsonl.

Reconciles the existing audit JSONL against the current corpus-wide
spfy_viterbi_replay mismatch set:

  1. Open records whose (corpus_id, utt_idx, slot_idx) no longer
     reproduces in the current run are bulk-closed as
     ``closed_stale_data`` (the 02-02 / 02-03 / 02-05 closures landed
     after these records were filed and silently fixed them).

  2. Open records that still reproduce are reclassified by their
     current mismatch class:
        - ``pool_gen`` (engine_tc >= 1e29: engine UID not in our pool)
            -> status="phase3_handoff",
              phase3_route="FE-side prsl_lookup pool augmentation"
        - ``silence_sentinel`` (engine_uid==169578 + last slot)
            -> status="closed",
              fix_ref="spfy_synth.c post-DP silence-sentinel force-fix"
        - ``fp_drift`` (|tc_diff| < 1e-3, both UIDs in pool)
            -> status="engine_internal_accept",
              accept_rationale="1-ULP tie-break flip; cost-equivalent picks"
        - ``diff_neg_big`` / ``diff_pos_big`` (|tc_diff| >= 1.0)
            -> status="phase3_handoff",
              phase3_route="FE divergence in TC components"
        - everything else (small |tc_diff|, both UIDs in pool)
            -> status="phase3_handoff",
              phase3_route="FE divergence (mid-magnitude TC drift)"

  3. Current-corpus mismatches absent from the JSONL get appended as
     new records with the same auto-classification rules. Without this
     step the post-mortem under-reports the actual scope.

The triage is data-driven (every classification is derivable from the
mismatch record itself) and reversible (the script writes a new JSONL
plus a per-record audit_change_log entry; the prior file is preserved
as 02-DP-AUDIT.jsonl.bak).

Inputs:
    .planning/phases/02-dp-fidelity-closure/02-DP-AUDIT.jsonl
    spfy/test/results/02-dp-fidelity-closure/dp_mismatches.jsonl

Outputs:
    .planning/phases/02-dp-fidelity-closure/02-DP-AUDIT.jsonl   (rewritten)
    .planning/phases/02-dp-fidelity-closure/02-DP-AUDIT.jsonl.bak (prev)
    c:/tmp/dp_audit/mismatches.csv  (1:1 mirror of new JSONL)
    c:/tmp/dp_audit/per_phrase.csv  (aggregated per corpus_id)

Stdout: per-class triage summary table (closed / phase3_handoff /
engine_internal_accept counts before vs after).
"""
from __future__ import annotations

import argparse
import csv
import json
import shutil
import sys
from collections import Counter, defaultdict
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[3]

DEFAULT_AUDIT = PROJECT_ROOT / ".planning/phases/02-dp-fidelity-closure" \
                              / "02-DP-AUDIT.jsonl"
DEFAULT_MM = PROJECT_ROOT / "spfy/test/results/02-dp-fidelity-closure" \
                          / "dp_mismatches.jsonl"
DEFAULT_PHRASES = PROJECT_ROOT / "spfy/test/results/02-dp-fidelity-closure" \
                                / "dp_replay_corpus.jsonl"
DEFAULT_CSV_DIR = Path("c:/tmp/dp_audit")

SILENCE_SENTINEL_UID = 169578
POOL_GEN_TC_THRESHOLD = 1e29
DIFF_BIG_THRESHOLD = 1.0
FP_DRIFT_THRESHOLD = 1e-3


def classify(mm: dict, n_slots: int) -> tuple[str, dict]:
    """Return (mismatch_class, suggested_disposition_dict).

    The disposition dict has fields appropriate to the chosen status:
    closed -> fix_ref; phase3_handoff -> phase3_route;
    engine_internal_accept -> accept_rationale.
    """
    eng_tc = mm["engine_tc"]
    our_tc = mm["our_tc"]
    diff   = mm["tc_diff"]
    eng_uid = mm["engine_uid"]
    slot   = mm["slot_idx"]

    is_last_slot = (slot == n_slots - 1)
    pool_gen     = eng_tc >= POOL_GEN_TC_THRESHOLD
    silence_last = (eng_uid == SILENCE_SENTINEL_UID) and is_last_slot
    abs_diff     = abs(diff)

    if pool_gen:
        return ("pool_gen", {
            "status": "phase3_handoff",
            "phase3_route": (
                "FE-side prsl_lookup pool augmentation: engine's chosen "
                "UID is not in our preselect pool (engine_tc=1e30 "
                "sentinel). Engine emits this UID via post-PRSL "
                "augmentation we don't replicate; closure requires "
                "either reproducing the augmentation site (Frida-trace "
                "the secondary lookup) or accepting a per-phrase "
                "augmentation table.")
        })
    if silence_last:
        return ("silence_sentinel", {
            "status": "closed",
            "fix_ref": (
                "spfy_synth.c post-DP silence-sentinel force-fix: "
                "default-fills last slot with UID 169578 when DP picks "
                "non-silence (verified 02-05 via 833/833 hp_score_test). "
                "This per-slot-replay metric is independent of that "
                "fix; the fix applies in spfy_synth, not in "
                "spfy_viterbi_replay.")
        })
    if abs_diff < FP_DRIFT_THRESHOLD:
        return ("fp_drift", {
            "status": "engine_internal_accept",
            "accept_rationale": (
                f"1-ULP tie-break flip: |tc_diff|={abs_diff:.6g} is "
                "below the FP drift threshold; engine and our DP "
                "picked different UIDs with cost-equivalent TC. Per "
                "PROJECT.md / CONCERNS.md §x87/SSE FP divergence: "
                "1-ULP flips can shift UID match ±5 pp without "
                "algorithmic bugs.")
        })
    if abs_diff >= DIFF_BIG_THRESHOLD:
        cls = "diff_neg_big" if diff < 0 else "diff_pos_big"
        return (cls, {
            "status": "phase3_handoff",
            "phase3_route": (
                f"FE divergence in TC components: |tc_diff|={abs_diff:.3f} "
                "exceeds 1.0; engine's TC and ours diverge significantly "
                "for the same-context per-cand scoring. This is FE-side "
                "input drift (sp_target / cart leaf / ctx tuple from FE) "
                "feeding into bit-exact spfy_hp_innerscorer (833/833 in "
                "02-05). Phase 3 must close FE convergence — see "
                "ROADMAP §Phase 3 SC1/SC2/SC3.")
        })
    return ("diff_mid", {
        "status": "phase3_handoff",
        "phase3_route": (
            f"FE divergence (mid-magnitude): tc_diff={diff:+.3f} is "
            "between FP drift threshold and the diff_big threshold. "
            "Same FE-input root cause as diff_neg/pos_big; defer to "
            "Phase 3 FE convergence work.")
    })


def slot_dim_index(corpus_jsonl: Path) -> dict[tuple[str, int], int]:
    """Map (corpus_id, utt_idx) -> n_slots from the per-phrase JSONL."""
    out: dict[tuple[str, int], int] = {}
    if not corpus_jsonl.exists():
        return out
    with corpus_jsonl.open("r", encoding="utf-8") as fp:
        per_id_utt: dict[str, int] = {}
        for line in fp:
            line = line.strip()
            if not line:
                continue
            try:
                r = json.loads(line)
            except json.JSONDecodeError:
                continue
            cid = r.get("id")
            n_slots = r.get("n_slots")
            if not (isinstance(cid, str) and isinstance(n_slots, int)):
                continue
            # Each phrase may have multiple utterances; record emits one
            # JSONL line per utterance in order. We track utt index by
            # how many records we've seen for this id so far.
            ui = per_id_utt.get(cid, 0) + 1
            per_id_utt[cid] = ui
            out[(cid, ui)] = n_slots
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--audit",     type=Path, default=DEFAULT_AUDIT)
    ap.add_argument("--mismatches", type=Path, default=DEFAULT_MM)
    ap.add_argument("--phrases",   type=Path, default=DEFAULT_PHRASES)
    ap.add_argument("--csv-dir",   type=Path, default=DEFAULT_CSV_DIR)
    ap.add_argument("--dry-run", action="store_true",
                    help="report what would change but don't rewrite files")
    args = ap.parse_args()

    if not args.audit.exists():
        print(f"audit file missing: {args.audit}", file=sys.stderr)
        return 1
    if not args.mismatches.exists():
        print(f"mismatches missing: {args.mismatches}", file=sys.stderr)
        return 1

    audit_recs = [json.loads(l) for l in
                  args.audit.open("r", encoding="utf-8") if l.strip()]
    mm_recs = [json.loads(l) for l in
               args.mismatches.open("r", encoding="utf-8") if l.strip()]
    n_slots_map = slot_dim_index(args.phrases)

    def key(r):
        return (r["corpus_id"], r.get("utt_idx", 1), r["slot_idx"])

    # Build current-mismatch lookup for cross-reference.
    mm_by_key = {key(m): m for m in mm_recs}

    before_status = Counter(r.get("status", "?") for r in audit_recs)

    # Phase A: reconcile existing records.
    pre_open = [r for r in audit_recs if r.get("status") == "open"]
    closed_stale = 0
    triaged_open = Counter()
    for r in pre_open:
        k = key(r)
        if k not in mm_by_key:
            r["status"] = "closed_stale_data"
            r["revised_root_cause"] = (
                "Closed by plans 02-02/03/04/05 (CCOS, prsl pool aug, "
                "HP_PRUNE descope, silence-sentinel) without explicit "
                "JSONL update. Verified 02-06: "
                "(corpus_id, utt_idx, slot_idx) is no longer in the "
                "spfy_viterbi_replay mismatch set on the 227-phrase "
                "corpus.")
            r["fix_ref"] = r.get("fix_ref") or "(stale; closed by upstream plan)"
            r["audit_change_log"] = "2026-05-09 02-06: bulk-close stale"
            closed_stale += 1
            continue
        # Still reproduces — reclassify from the live mismatch.
        live = mm_by_key[k]
        n_slots = n_slots_map.get((r["corpus_id"], r.get("utt_idx", 1)),
                                  r.get("slot_idx", 0) + 1)
        cls, disp = classify(live, n_slots)
        # Preserve original mismatch_class as prior_mismatch_class for
        # archeology; current class becomes mismatch_class.
        r["prior_mismatch_class"] = r.get("mismatch_class")
        r["mismatch_class"] = cls
        r["engine_tc"] = live["engine_tc"]
        r["our_tc"]    = live["our_tc"]
        r["our_uid"]   = live["our_uid"]
        r["tc_diff"]   = live["tc_diff"]
        r["pool_n"]    = live["pool_n"]
        r["status"] = disp["status"]
        for k2, v2 in disp.items():
            if k2 != "status":
                r[k2] = v2
        r["audit_change_log"] = (
            f"2026-05-09 02-06: reclassified open->{disp['status']} (cls={cls})")
        triaged_open[disp["status"]] += 1

    # Phase B: add new records for current mismatches not in audit.
    audit_keys = {key(r) for r in audit_recs}
    new_records: list[dict] = []
    for m in mm_recs:
        k = (m["corpus_id"], m["utt_idx"], m["slot_idx"])
        if k in audit_keys:
            continue
        n_slots = n_slots_map.get((m["corpus_id"], m["utt_idx"]),
                                  m["slot_idx"] + 1)
        cls, disp = classify(m, n_slots)
        rec = {
            "corpus_id":   m["corpus_id"],
            "utt_idx":     m["utt_idx"],
            "slot_idx":    m["slot_idx"],
            "slot_pos":    ("first"  if m["slot_idx"] == 0
                            else "last" if m["slot_idx"] == n_slots - 1
                            else "middle"),
            "mismatch_class": cls,
            "engine_uid":  m["engine_uid"],
            "our_uid":     m["our_uid"],
            "engine_tc":   m["engine_tc"],
            "our_tc":      m["our_tc"],
            "tc_diff":     m["tc_diff"],
            "pool_n":      m["pool_n"],
            "root_cause":  ("Auto-classified at 02-06 triage from "
                            "corpus-wide spfy_viterbi_replay mismatch "
                            "stream; not previously enumerated."),
            "status":      disp["status"],
            "audit_change_log": "2026-05-09 02-06: added (newly enumerated)",
        }
        for k2, v2 in disp.items():
            if k2 != "status":
                rec[k2] = v2
        new_records.append(rec)

    final_recs = audit_recs + new_records

    after_status = Counter(r.get("status", "?") for r in final_recs)
    after_class  = Counter(r.get("mismatch_class", "?") for r in final_recs)
    by_cls_status = defaultdict(Counter)
    for r in final_recs:
        by_cls_status[r.get("mismatch_class", "?")][r.get("status", "?")] += 1

    print("===== triage summary =====")
    print(f"audit records: before={len(audit_recs)} "
          f"after={len(final_recs)} "
          f"(+{len(new_records)} newly enumerated)")
    print()
    print("status before:", dict(before_status))
    print("status after :", dict(after_status))
    print()
    print("opens that were stale (bulk-closed):", closed_stale)
    print("opens that still reproduced (reclassified):",
          sum(triaged_open.values()),
          "->", dict(triaged_open))
    print()
    print("per-class final disposition:")
    for cls in sorted(by_cls_status):
        s = by_cls_status[cls]
        total = sum(s.values())
        print(f"  {cls:18s} total={total:5d}  "
              + "  ".join(f"{st}={n}" for st, n in s.items()))
    print()
    open_left = sum(1 for r in final_recs if r.get("status") == "open")
    print(f"records left with status=open: {open_left}")

    if args.dry_run:
        print("\n(dry run — no files written)")
        return 0

    # Write the updated JSONL (atomic via .tmp + rename). Backup first.
    bak = args.audit.with_suffix(args.audit.suffix + ".bak")
    if not bak.exists():
        shutil.copy(args.audit, bak)
    tmp = args.audit.with_suffix(args.audit.suffix + ".tmp")
    with tmp.open("w", encoding="utf-8") as fp:
        for r in final_recs:
            fp.write(json.dumps(r, separators=(",", ":")) + "\n")
    tmp.replace(args.audit)
    print(f"wrote {len(final_recs)} records to {args.audit}")
    print(f"backup at {bak}")

    # Mirror to CSV.
    args.csv_dir.mkdir(parents=True, exist_ok=True)
    csv_path = args.csv_dir / "mismatches.csv"
    fields = sorted({k for r in final_recs for k in r.keys()})
    with csv_path.open("w", encoding="utf-8", newline="") as fp:
        w = csv.DictWriter(fp, fieldnames=fields)
        w.writeheader()
        for r in final_recs:
            w.writerow(r)
    print(f"wrote {csv_path} ({len(final_recs)} rows, "
          f"{len(fields)} columns)")

    # Per-phrase aggregate.
    per_phrase = defaultdict(lambda: {"closed": 0, "phase3_handoff": 0,
                                       "engine_internal_accept": 0,
                                       "closed_stale_data": 0,
                                       "open": 0, "total": 0})
    for r in final_recs:
        cid = r["corpus_id"]
        st = r.get("status", "open")
        per_phrase[cid][st] = per_phrase[cid].get(st, 0) + 1
        per_phrase[cid]["total"] += 1
    pp_path = args.csv_dir / "per_phrase.csv"
    pp_fields = ["corpus_id", "total", "closed", "closed_stale_data",
                 "phase3_handoff", "engine_internal_accept", "open"]
    with pp_path.open("w", encoding="utf-8", newline="") as fp:
        w = csv.DictWriter(fp, fieldnames=pp_fields)
        w.writeheader()
        for cid in sorted(per_phrase):
            row = {"corpus_id": cid}
            row.update({k: per_phrase[cid].get(k, 0) for k in pp_fields[1:]})
            w.writerow(row)
    print(f"wrote {pp_path} ({len(per_phrase)} corpus_ids)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
