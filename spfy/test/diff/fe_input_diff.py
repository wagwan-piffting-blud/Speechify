"""Per-slot 5-tuple FE-input diff classifier (plan 03-02 D-02 / D-05 / D-07).

For each (corpus_id, utt_idx, slot_idx) in the phase3_handoff slice of
02-DP-AUDIT.jsonl, reconstruct the engine and our
(ctx[5], sp[5], voicing, cart_leaf, pool_n) tuples from already-captured
oracle traces, classify the FIRST diverging field in canonical order
ctx -> sp -> voicing -> leaf -> pool, and emit one record per slot to
03-FE-AUDIT.jsonl. Aggregate per-class counts to stderr; the dominant
class drives plan 03-03's attack ordering (D-01).

D-02: One class per record, deterministic, idempotent. Multi-tag rejected.
D-05: Existing traces only; no new Frida captures.
D-08: Informational; this harness is NOT a non-regression gate.

Inputs (existing-traces-only per D-05):
  spfy/test/oracle/traces/prsl_slot/<id>.jsonl     -- ctx[5] + n_cands + uids
  spfy/test/oracle/traces/inner_scorer/<id>.jsonl  -- sp_target[5]
  spfy/test/oracle/traces/cart_walks/<id>.jsonl    -- (leaf_mean, leaf_var) per (slot, tree)
  spfy/test/oracle/traces/fe_tree/<id>.jsonl       -- relation graph (used for fe_evidence_trace_id)
  spfy/test/oracle/traces/fe_token/<id>.jsonl      -- FE token-stream (auxiliary)
  spfy/test/oracle/traces/accent_decision/<id>.jsonl -- per-syllable accent (auxiliary)
  c:/tmp/spfy_build/src/cli/spfy_synth.exe         -- our pipeline (debug stream)
  .planning/phases/02-dp-fidelity-closure/02-DP-AUDIT.jsonl -- frozen Phase 2 ledger

Outputs:
  stdout: one JSONL record per (corpus_id, slot_idx) when --jsonl-out is absent
  stderr: aggregate per-class counts when --dump-summary is set; dominant class
  --jsonl-out PATH: writes 03-FE-AUDIT.jsonl (with .bak snapshot on overwrite)
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import os
import re
import shutil
import subprocess
import sys
from collections import Counter
from pathlib import Path
from typing import Optional

# Paths
PROJECT_ROOT = Path(__file__).resolve().parents[3]
TR = PROJECT_ROOT / "spfy" / "test" / "oracle" / "traces"
CORPUS_FILE = PROJECT_ROOT / "spfy" / "test" / "oracle" / "corpus.jsonl"
PHASE2_LEDGER = (PROJECT_ROOT / ".planning" / "phases" /
                 "02-dp-fidelity-closure" / "02-DP-AUDIT.jsonl")

# Our-pipeline binary + voice files (mirror audit_focused.py paths)
SYNTH_EXE = Path("c:/tmp/spfy_build/src/cli/spfy_synth.exe")
VIN = PROJECT_ROOT / "en-US" / "tom" / "tom.vin"
VDB = PROJECT_ROOT / "en-US" / "tom" / "tom8.vdb"
VCF = PROJECT_ROOT / "en-US" / "tom" / "tom.vcf"
HPC = PROJECT_ROOT / "spfy" / "data" / "tom_hpclass.bin"
VOC = PROJECT_ROOT / "spfy" / "build" / "fe_symbol_table.json"
TA = PROJECT_ROOT / "spfy" / "data" / "fe_tables_a"
TB = PROJECT_ROOT / "spfy" / "data" / "fe_tables"

ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")

# Float-equality tolerance for CART leaf (mean, var) comparison; matches
# audit_focused.py:fclose epsilon=0.005 (relative, with abs floor 1.0).
LEAF_EPS = 0.005

# Silence-pad halfphone classes (engine doesn't emit cart_walk for these).
SILENCE_HP_CLASSES = {64, 65}


# ---------------------------------------------------------------------------
# Trace loaders (existing-traces-only per D-05; mirror pool_gen_diff.py:43-60)
# ---------------------------------------------------------------------------

def _load_jsonl(path: Path, type_filter: Optional[str] = None) -> list[dict]:
    """Return list of records from a JSONL file, optionally filtered by 'type' field.

    Skips missing files (returns []) and malformed JSON lines without crashing.
    """
    if not path.exists():
        return []
    out: list[dict] = []
    for line in path.open("r", encoding="utf-8"):
        line = line.strip()
        if not line:
            continue
        try:
            rec = json.loads(line)
        except json.JSONDecodeError:
            continue
        if type_filter is not None and rec.get("type") != type_filter:
            continue
        out.append(rec)
    return out


def load_fe_tree(pid: str) -> list[dict]:
    """fe_tree records: {type:'fe_tree', n_call, utt_ptr, relations, irs, n_irs}."""
    return _load_jsonl(TR / "fe_tree" / f"{pid}.jsonl", "fe_tree")


def load_fe_token(pid: str) -> list[dict]:
    """fe_token records: {type:'fe_tokens', probe:{...}}."""
    return _load_jsonl(TR / "fe_token" / f"{pid}.jsonl", "fe_tokens")


def load_accent_decision(pid: str) -> list[dict]:
    """accent_decision records: {type:'accent_decision', state_ptr, entry, exit_124f, slots}."""
    return _load_jsonl(TR / "accent_decision" / f"{pid}.jsonl", "accent_decision")


def load_prsl_slot(pid: str) -> list[dict]:
    """prsl_slot records: {type:'prsl_slot', n, slot, this_ptr, n_cands, ctx, uids}."""
    return _load_jsonl(TR / "prsl_slot" / f"{pid}.jsonl", "prsl_slot")


def load_cart_walks(pid: str) -> list[dict]:
    """cart_walks records: {type:'cart_walk', n, tree, slot, questions, phone_idx, leaf_mean, leaf_var}."""
    return _load_jsonl(TR / "cart_walks" / f"{pid}.jsonl", "cart_walk")


def load_inner_scorer(pid: str) -> list[dict]:
    """inner_scorer per-slot records: {type:'inner_scorer', slot, sp_target, ...}."""
    return _load_jsonl(TR / "inner_scorer" / f"{pid}.jsonl", "inner_scorer")


# ---------------------------------------------------------------------------
# 5-tuple data class + builders
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class FETuple:
    """Per-slot 5-tuple per D-02. Any field can be None when the trace
    didn't capture that hook (the classifier treats None == None as match)."""
    ctx5: Optional[list[int]] = None
    sp5: Optional[list[int]] = None
    voicing: Optional[int] = None
    leaf_id: Optional[tuple] = None  # (durt_mean, durt_var) approximation
    pool_n: Optional[int] = None


def _isolate_utt0(records: list[dict], slot_field: str = "slot") -> list[dict]:
    """Filter to utterance-0 only, using the standard seen_zero/in_utt0 pattern
    from audit_focused.py:45-67. Most engine traces concatenate multi-utt;
    we pick the first run."""
    out: list[dict] = []
    seen_zero = False
    in_utt0 = True
    for r in records:
        s = r.get(slot_field)
        if s == 0:
            if seen_zero:
                in_utt0 = False
            seen_zero = True
        if not in_utt0:
            break
        out.append(r)
    return out


def engine_tuple_for_slot(slot_idx: int, traces: dict) -> FETuple:
    """Build engine 5-tuple from oracle traces for one slot."""
    prsl = traces["prsl_slot"].get(slot_idx)
    inner = traces["inner_scorer"].get(slot_idx)
    durt = traces["cart_durt"].get(slot_idx)
    f0tr = traces["cart_f0tr"].get(slot_idx)

    ctx5 = prsl.get("ctx") if prsl else None
    pool_n = prsl.get("n_cands") if prsl else None
    sp5 = inner.get("sp_target") if inner else None
    leaf_id = (round(durt.get("leaf_mean", 0.0), 4),
               round(durt.get("leaf_var", 0.0), 4)) if durt else None
    # Voicing heuristic: engine emits f0tr cart_walk only for voiced phones
    # per MEMORY.md / audit_focused.py finding. So presence of f0tr leaf at
    # this slot means voicing=1; absence on a non-silence slot means 0.
    if ctx5 and ctx5[2] in SILENCE_HP_CLASSES:
        voicing = None  # silence pad — voicing not meaningful
    else:
        voicing = 1 if f0tr is not None else 0
    return FETuple(ctx5=ctx5, sp5=sp5, voicing=voicing,
                   leaf_id=leaf_id, pool_n=pool_n)


def index_traces_by_slot(pid: str) -> dict:
    """Load and index all engine traces by slot_idx for utterance 0."""
    prsl_recs = _isolate_utt0(load_prsl_slot(pid), "slot")
    inner_recs = _isolate_utt0(load_inner_scorer(pid), "slot")
    cart_recs = load_cart_walks(pid)  # cart_walks doesn't have utt-0 boundary

    prsl_by_slot = {r["slot"]: r for r in prsl_recs}
    inner_by_slot = {r["slot"]: r for r in inner_recs}
    cart_durt: dict[int, dict] = {}
    cart_f0tr: dict[int, dict] = {}
    for r in cart_recs:
        s = r.get("slot")
        if s is None:
            continue
        if r.get("tree") == "durt" and s not in cart_durt:
            cart_durt[s] = r
        elif r.get("tree") == "f0tr" and s not in cart_f0tr:
            cart_f0tr[s] = r
    return {
        "prsl_slot": prsl_by_slot,
        "inner_scorer": inner_by_slot,
        "cart_durt": cart_durt,
        "cart_f0tr": cart_f0tr,
    }


# ---------------------------------------------------------------------------
# Our-side tuple via spfy_synth debug stream (mirror audit_focused.py:24-42)
# ---------------------------------------------------------------------------

def _run_synth(text: str, timeout: float = 60.0) -> list[dict]:
    """Run spfy_synth with SPFY_SYNTH_DEBUG=1; return list of per-HP-slot dicts.

    Returns empty list on timeout / nonzero exit. Mirrors audit_focused.py
    run_synth() exactly so the upstream finding (per-slot ctx/sp/cart fields)
    holds.
    """
    if not SYNTH_EXE.exists():
        return []
    env = os.environ.copy()
    env["SPFY_SYNTH_DEBUG"] = "1"
    env["SPFY_FIRST_PHRASE_ONLY"] = "1"
    out_wav = "c:/tmp/_fe_input_diff.wav"
    try:
        r = subprocess.run(
            [str(SYNTH_EXE), str(VIN), str(VDB), str(VCF), str(HPC),
             str(VOC), str(TA), str(TB), text, out_wav],
            capture_output=True, text=True, env=env, timeout=timeout)
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return []
    slots: list[dict] = []
    for ln in (r.stdout + "\n" + r.stderr).splitlines():
        ln = ANSI_RE.sub("", ln).strip()
        if not (ln.startswith("{") and ln.endswith("}")):
            continue
        try:
            d = json.loads(ln)
        except json.JSONDecodeError:
            continue
        if "hp" in d and "ctx" in d:
            slots.append(d)
    return slots


def our_tuple_for_slot(slot_idx: int, our_slots: list[dict]) -> FETuple:
    """Build our 5-tuple from spfy_synth's per-HP-slot debug records."""
    if slot_idx >= len(our_slots):
        return FETuple()
    s = our_slots[slot_idx]
    ctx5 = list(s.get("ctx") or [])
    sp5 = list(s.get("sp") or [])
    pool_n = s.get("pool_n")
    durt_m = s.get("durt_mean", 0.0)
    durt_v = s.get("durt_var", 0.0)
    f0tr_m = s.get("f0tr_mean", 0.0)
    f0tr_v = s.get("f0tr_var", 0.0)
    leaf_id = (round(durt_m, 4), round(durt_v, 4)) if (durt_m or durt_v) else None
    if ctx5 and ctx5[2] in SILENCE_HP_CLASSES:
        voicing = None
    else:
        voicing = 1 if (f0tr_m != 0 or f0tr_v != 0) else 0
    return FETuple(ctx5=ctx5 or None, sp5=sp5 or None, voicing=voicing,
                   leaf_id=leaf_id, pool_n=pool_n)


# ---------------------------------------------------------------------------
# Classifier (D-02: canonical first-divergence; deterministic + idempotent)
# ---------------------------------------------------------------------------

def _fclose(a: float, b: float, eps: float = LEAF_EPS) -> bool:
    return abs(a - b) <= eps * max(1.0, abs(b))


def _leaf_match(eng: Optional[tuple], our: Optional[tuple]) -> bool:
    if eng is None and our is None:
        return True
    if eng is None or our is None:
        return False
    return _fclose(eng[0], our[0]) and _fclose(eng[1], our[1])


def classify_fe(slot_idx: int, engine: FETuple, our: FETuple) -> tuple[str, str]:
    """Return (fe_class, fe_first_divergent_field).

    Canonical order per D-02: ctx -> sp -> voicing -> leaf -> pool.
    First divergence wins. If a field is None on either side, treat
    None == None as match; None vs non-None is a divergence in that field.
    """
    e_ctx = engine.ctx5 or []
    o_ctx = our.ctx5 or []
    if len(e_ctx) == len(o_ctx) == 5:
        for i in range(5):
            if e_ctx[i] != o_ctx[i]:
                return ("ctx_diff", f"ctx[{i}]")
    elif (engine.ctx5 is None) != (our.ctx5 is None):
        return ("ctx_diff", "ctx[*]")
    elif e_ctx != o_ctx:
        return ("ctx_diff", "ctx[len]")

    e_sp = engine.sp5 or []
    o_sp = our.sp5 or []
    if len(e_sp) == len(o_sp) == 5:
        for i in range(5):
            if e_sp[i] != o_sp[i]:
                return ("sp_diff", f"sp[{i}]")
    elif (engine.sp5 is None) != (our.sp5 is None):
        return ("sp_diff", "sp[*]")
    elif e_sp != o_sp:
        return ("sp_diff", "sp[len]")

    if engine.voicing != our.voicing:
        return ("voicing_diff", "voicing")

    if not _leaf_match(engine.leaf_id, our.leaf_id):
        return ("leaf_diff", "cart_leaf_id")

    if engine.pool_n != our.pool_n:
        return ("pool_n_diff", "pool_n")

    return ("unknown", "")


# ---------------------------------------------------------------------------
# Record builder
# ---------------------------------------------------------------------------

def detect_same_rec(engine_uid: Optional[int],
                    prev_engine_uid: Optional[int],
                    our_pool_uids: Optional[list]) -> Optional[str]:
    """Return same_rec subtype if applicable, else None.

    Two engine same_rec patterns surface in the audit (Phase 2 D-13 +
    Phase 3 03-02 unknown-bucket investigation):

    - 'seq_continuation': engine picks UID == prev_slot_engine_UID + 1 —
      engine continues using sequential UIDs from the same recording chunk
      without re-querying PRSL. Documented in pool_gen_diff.py:117-122.

    - 'outside_pool': engine picks a UID that is NOT in our PRSL pool for
      this slot — engine has hidden candidates beyond what prsl_slot trace
      emits. Captures the secondary_lookup class from pool_gen_diff.

    Both subtypes are DP/USel-side phenomena, NOT FE-input divergence.
    These records are not addressable by Phase 3's FE convergence work.
    """
    if engine_uid is None:
        return None
    if prev_engine_uid is not None and engine_uid == prev_engine_uid + 1:
        return "seq_continuation"
    if our_pool_uids and engine_uid not in our_pool_uids:
        return "outside_pool"
    return None


def build_record(corpus_id: str, utt_idx: int, slot_idx: int,
                 engine: FETuple, our: FETuple,
                 phase2_record_id: Optional[str],
                 fe_evidence_trace_id: Optional[str],
                 ts_iso: str,
                 engine_uid: Optional[int] = None,
                 prev_engine_uid: Optional[int] = None,
                 our_pool_uids: Optional[list] = None) -> dict:
    fe_class, first_field = classify_fe(slot_idx, engine, our)
    same_rec_subtype = None
    # When the FE-input classifier returns "unknown" (all five fields match)
    # but the engine still picked a UID outside our pool / sequential to
    # prev, override the class to same_rec_diff. DP/USel-side phenomenon.
    if fe_class == "unknown":
        same_rec_subtype = detect_same_rec(engine_uid, prev_engine_uid,
                                           our_pool_uids)
        if same_rec_subtype is not None:
            fe_class = "same_rec_diff"
            first_field = same_rec_subtype
    return {
        "corpus_id": corpus_id,
        "utt_idx": utt_idx,
        "slot_idx": slot_idx,
        "phase2_record_id": phase2_record_id,
        "fe_class": fe_class,
        "fe_first_divergent_field": first_field,
        "same_rec_subtype": same_rec_subtype,
        "engine_uid": engine_uid,
        "prev_engine_uid": prev_engine_uid,
        "engine_ctx5": engine.ctx5,
        "our_ctx5": our.ctx5,
        "engine_sp5": engine.sp5,
        "our_sp5": our.sp5,
        "engine_voicing": engine.voicing,
        "our_voicing": our.voicing,
        "engine_leaf_id": list(engine.leaf_id) if engine.leaf_id else None,
        "our_leaf_id": list(our.leaf_id) if our.leaf_id else None,
        "engine_pool_n": engine.pool_n,
        "our_pool_n": our.pool_n,
        "our_pool_uids_first10": (our_pool_uids[:10]
                                  if our_pool_uids else None),
        "fe_root_cause": "",
        "fe_fix_ref": None,
        "fe_status": "open",
        "fe_evidence_trace_id": fe_evidence_trace_id,
        "fe_constant_provenance": None,
        "audit_change_log": (f"{ts_iso} 03-02: classified open "
                             f"(cls={fe_class}, first_div={first_field})"),
    }


# ---------------------------------------------------------------------------
# Phase 2 handoff slice
# ---------------------------------------------------------------------------

def load_phase2_handoff() -> list[dict]:
    """Return phase3_handoff records from 02-DP-AUDIT.jsonl (frozen)."""
    if not PHASE2_LEDGER.exists():
        return []
    out: list[dict] = []
    for line in PHASE2_LEDGER.open("r", encoding="utf-8"):
        line = line.strip()
        if not line:
            continue
        try:
            rec = json.loads(line)
        except json.JSONDecodeError:
            continue
        if rec.get("status") == "phase3_handoff":
            out.append(rec)
    return out


def phase2_record_id(rec: dict) -> str:
    """Stable id back to 02-DP-AUDIT row: corpus_id::utt_idx::slot_idx.

    Some legacy phase3_handoff records (e.g. older pool_gen_class entries
    from Phase 2 plan 02-03) lack `utt_idx`; fall back to '?' so the id is
    still emitted without crashing.
    """
    return (f"{rec['corpus_id']}::"
            f"{rec.get('utt_idx', '?')}::{rec['slot_idx']}")


# ---------------------------------------------------------------------------
# Orchestration
# ---------------------------------------------------------------------------

def classify_phrase(pid: str, handoff_slots: Optional[set],
                    text: Optional[str], ts_iso: str,
                    handoff_index: dict,
                    ph2_full_index: Optional[dict] = None
                    ) -> tuple[list, list]:
    """Classify all (or only handoff-listed) slots for one phrase.

    handoff_slots: if None, classify every slot the engine has data for;
                   else, restrict to these slot indices.
    text: if None, skip running spfy_synth (our-tuple will be empty -> all
          fields drift -> ctx_diff at index 0).
    handoff_index: map slot_idx -> phase2_record dict (handoff slots only).
    ph2_full_index: map slot_idx -> phase2_record for ALL slots in this
        phrase (not just handoff). Required for same_rec detection
        (prev_engine_uid lookup at slot_idx-1 may not be a handoff slot).
    Returns (records, warnings).
    """
    warnings: list = []
    traces = index_traces_by_slot(pid)
    if not traces["prsl_slot"]:
        warnings.append(f"{pid}: no prsl_slot trace; skipping")
        return [], warnings

    our_slots = _run_synth(text) if text else []
    if text and not our_slots:
        warnings.append(f"{pid}: spfy_synth produced no debug slots")

    fe_tree_records = load_fe_tree(pid)
    fe_evidence = (f"fe_tree:{pid}.jsonl:row_0"
                   if fe_tree_records else f"prsl_slot:{pid}.jsonl")

    slots_to_classify = (sorted(handoff_slots) if handoff_slots
                         else sorted(traces["prsl_slot"].keys()))
    full_idx = ph2_full_index or {}
    records: list = []
    for slot_idx in slots_to_classify:
        eng = engine_tuple_for_slot(slot_idx, traces)
        our = our_tuple_for_slot(slot_idx, our_slots)
        ph2_rec = handoff_index.get(slot_idx)
        ph2_id = phase2_record_id(ph2_rec) if ph2_rec else None
        utt_idx = ph2_rec.get("utt_idx", 1) if ph2_rec else 1
        # Same_rec inputs: engine_uid for this slot, prev-slot's engine_uid
        # (from FULL Phase 2 index, not just handoff), our pool UIDs.
        engine_uid = ph2_rec.get("engine_uid") if ph2_rec else None
        prev_rec = full_idx.get(slot_idx - 1) if slot_idx > 0 else None
        prev_engine_uid = (prev_rec.get("engine_uid")
                           if prev_rec else None)
        our_pool_uids = (our_slots[slot_idx].get("cands")
                         if slot_idx < len(our_slots) else None)
        records.append(build_record(
            corpus_id=pid, utt_idx=utt_idx, slot_idx=slot_idx,
            engine=eng, our=our,
            phase2_record_id=ph2_id,
            fe_evidence_trace_id=fe_evidence,
            ts_iso=ts_iso,
            engine_uid=engine_uid,
            prev_engine_uid=prev_engine_uid,
            our_pool_uids=our_pool_uids))
    return records, warnings


def _load_corpus_texts() -> dict[str, str]:
    """Map corpus_id -> text from spfy/test/oracle/corpus.jsonl."""
    out: dict[str, str] = {}
    if not CORPUS_FILE.exists():
        return out
    for line in CORPUS_FILE.open("r", encoding="utf-8"):
        line = line.strip()
        if not line:
            continue
        try:
            d = json.loads(line)
        except json.JSONDecodeError:
            continue
        if d.get("mode") == "text" and "id" in d:
            out[d["id"]] = d["text"]
    return out


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: Optional[list[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        description="Per-slot 5-tuple FE-input diff classifier (plan 03-02).",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=False)
    g.add_argument("--corpus-id", help="Single corpus id (e.g. text_001)")
    g.add_argument("--all", action="store_true",
                   help="Iterate every text-mode corpus entry")
    ap.add_argument("--jsonl-out", type=Path,
                    help="Write 03-FE-AUDIT.jsonl to this path "
                         "(.bak snapshot of any pre-existing file)")
    ap.add_argument("--first-divergence-only", action="store_true",
                    default=True,
                    help="D-02 default: one class per slot, first divergence wins")
    ap.add_argument("--phase2-handoff-only", action="store_true",
                    help="Restrict to phase3_handoff slots from 02-DP-AUDIT.jsonl")
    ap.add_argument("--dump-summary", action="store_true",
                    help="Emit per-class aggregate counts to stderr")
    ap.add_argument("--no-our-tuple", action="store_true",
                    help="Skip running spfy_synth (engine-only side; for testing)")
    ap.add_argument("--ts-iso", default="2026-05-09",
                    help="Timestamp prefix for audit_change_log")
    args = ap.parse_args(argv)

    if not args.corpus_id and not args.all:
        ap.print_usage(sys.stderr)
        print("\nfe_input_diff.py: choose --corpus-id ID or --all",
              file=sys.stderr)
        return 0

    # Phase 2 handoff index (per-corpus_id slot list + record lookup).
    # Also build a FULL Phase 2 index keyed by (corpus_id, slot_idx) for
    # same_rec detection — prev-slot's engine_uid may live at a slot that
    # is NOT in the handoff set.
    handoff_records = (load_phase2_handoff()
                       if args.phase2_handoff_only else [])
    handoff_by_pid: dict = {}
    handoff_slots_by_pid: dict = {}
    for r in handoff_records:
        pid = r["corpus_id"]
        handoff_by_pid.setdefault(pid, {})[r["slot_idx"]] = r
        handoff_slots_by_pid.setdefault(pid, set()).add(r["slot_idx"])
    # Full ledger (for prev_engine_uid lookup): every Phase 2 record,
    # regardless of status, indexed by (corpus_id, slot_idx).
    full_ph2_by_pid: dict = {}
    if PHASE2_LEDGER.exists():
        for line in PHASE2_LEDGER.open("r", encoding="utf-8"):
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            pid = rec.get("corpus_id")
            sl = rec.get("slot_idx")
            if pid is None or sl is None:
                continue
            full_ph2_by_pid.setdefault(pid, {})[sl] = rec

    # Determine which phrases to scan
    corpus_texts = _load_corpus_texts()
    if args.corpus_id:
        pids = [args.corpus_id]
    elif args.phase2_handoff_only:
        pids = sorted(handoff_by_pid.keys())
    else:
        pids = sorted(corpus_texts.keys())

    # Output sink: stdout vs --jsonl-out (with .bak snapshot)
    if args.jsonl_out:
        if args.jsonl_out.exists():
            bak = args.jsonl_out.with_suffix(args.jsonl_out.suffix + ".bak")
            shutil.copy(args.jsonl_out, bak)
            print(f"snapshot {args.jsonl_out} -> {bak}", file=sys.stderr)
        args.jsonl_out.parent.mkdir(parents=True, exist_ok=True)
        sink = args.jsonl_out.open("w", encoding="utf-8")
    else:
        sink = sys.stdout

    # Run classifier
    counts: Counter = Counter()
    total = 0
    skipped: list[str] = []
    try:
        for pid in pids:
            text = None if args.no_our_tuple else corpus_texts.get(pid)
            slots = (handoff_slots_by_pid.get(pid)
                     if args.phase2_handoff_only else None)
            handoff_idx = handoff_by_pid.get(pid, {})
            full_idx = full_ph2_by_pid.get(pid, {})
            records, warns = classify_phrase(pid, slots, text,
                                             args.ts_iso, handoff_idx,
                                             ph2_full_index=full_idx)
            for w in warns:
                print(w, file=sys.stderr)
                if "skipping" in w or "no debug" in w:
                    skipped.append(pid)
            for rec in records:
                sink.write(json.dumps(rec, ensure_ascii=False,
                                      separators=(",", ":")) + "\n")
                counts[rec["fe_class"]] += 1
                total += 1
    finally:
        if args.jsonl_out:
            sink.close()

    if args.dump_summary or args.phase2_handoff_only:
        print("", file=sys.stderr)
        print("===== fe_input_diff aggregate =====", file=sys.stderr)
        print(f"phrases scanned    : {len(pids)}", file=sys.stderr)
        print(f"records emitted    : {total}", file=sys.stderr)
        print(f"phrases skipped    : {len(set(skipped))}", file=sys.stderr)
        print("by fe_class:", file=sys.stderr)
        for cls, cnt in counts.most_common():
            pct = 100.0 * cnt / max(total, 1)
            print(f"  {cls:16s} : {cnt:>5d}  ({pct:5.1f}%)", file=sys.stderr)
        if total > 0:
            dom = counts.most_common(1)[0]
            print(f"\nDominant fe_class: {dom[0]} ({dom[1]} records, "
                  f"{100.0*dom[1]/total:.1f}% of {total}). "
                  f"Plan 03-03 attack target: {dom[0]}.", file=sys.stderr)

    return 0


# ---------------------------------------------------------------------------
# Inline tests (run via `python fe_input_diff.py --self-test` or doctest)
# ---------------------------------------------------------------------------

def _self_test() -> int:
    """Run the 8 behaviour tests from the plan."""
    failures: list[str] = []

    # Test 1: load_fe_tree returns [] on missing
    if load_fe_tree("__nonexistent__") != []:
        failures.append("Test 1: load_fe_tree should return [] for missing")

    # Test 2: load_prsl_slot smoke (will only pass if traces exist)
    recs = load_prsl_slot("text_001")
    if recs:
        sample = recs[0]
        if not all(k in sample for k in ("ctx", "n_cands", "uids")):
            failures.append(f"Test 2: prsl_slot sample missing required keys: {sample}")

    # Test 3: classify_fe ctx_diff at ctx[2]
    e = FETuple(ctx5=[1, 2, 3, 4, 5], sp5=[0]*5, voicing=1, leaf_id=(1.0, 0.1), pool_n=10)
    o = FETuple(ctx5=[1, 2, 9, 4, 5], sp5=[0]*5, voicing=1, leaf_id=(1.0, 0.1), pool_n=10)
    cls, fld = classify_fe(0, e, o)
    if cls != "ctx_diff" or fld != "ctx[2]":
        failures.append(f"Test 3: expected (ctx_diff, ctx[2]); got ({cls}, {fld})")

    # Test 4: sp_diff first-divergence wins (canonical order ignores leaf even if also different)
    e = FETuple(ctx5=[1, 2, 3, 4, 5], sp5=[0]*5, voicing=1, leaf_id=(1.0, 0.1), pool_n=10)
    o = FETuple(ctx5=[1, 2, 3, 4, 5], sp5=[1, 0, 0, 0, 0], voicing=1, leaf_id=(2.0, 0.5), pool_n=20)
    cls, fld = classify_fe(0, e, o)
    if cls != "sp_diff" or fld != "sp[0]":
        failures.append(f"Test 4: first-divergence should be sp_diff/sp[0]; got ({cls}, {fld})")

    # Test 5: unknown when all match
    e = FETuple(ctx5=[1, 2, 3, 4, 5], sp5=[0]*5, voicing=1, leaf_id=(1.0, 0.1), pool_n=10)
    cls, fld = classify_fe(0, e, e)
    if cls != "unknown":
        failures.append(f"Test 5: expected unknown for all-equal; got ({cls}, {fld})")

    # Test 6: voicing_diff comes after sp but before leaf
    e = FETuple(ctx5=[1, 2, 3, 4, 5], sp5=[0]*5, voicing=1, leaf_id=(1.0, 0.1), pool_n=10)
    o = FETuple(ctx5=[1, 2, 3, 4, 5], sp5=[0]*5, voicing=0, leaf_id=(99.0, 9.9), pool_n=99)
    cls, fld = classify_fe(0, e, o)
    if cls != "voicing_diff":
        failures.append(f"Test 6: expected voicing_diff; got ({cls}, {fld})")

    # Test 7: leaf_diff with fclose tolerance (0.001 difference < eps)
    e = FETuple(ctx5=[1, 2, 3, 4, 5], sp5=[0]*5, voicing=1, leaf_id=(100.0, 0.1), pool_n=10)
    o = FETuple(ctx5=[1, 2, 3, 4, 5], sp5=[0]*5, voicing=1, leaf_id=(100.001, 0.1), pool_n=10)
    cls, _ = classify_fe(0, e, o)
    if cls != "unknown":
        failures.append(f"Test 7: leaf within fclose should match; got ({cls})")

    # Test 8: pool_n_diff after all earlier match
    e = FETuple(ctx5=[1, 2, 3, 4, 5], sp5=[0]*5, voicing=1, leaf_id=(1.0, 0.1), pool_n=10)
    o = FETuple(ctx5=[1, 2, 3, 4, 5], sp5=[0]*5, voicing=1, leaf_id=(1.0, 0.1), pool_n=15)
    cls, fld = classify_fe(0, e, o)
    if cls != "pool_n_diff" or fld != "pool_n":
        failures.append(f"Test 8: expected pool_n_diff/pool_n; got ({cls}, {fld})")

    # Test 9: detect_same_rec — seq_continuation (engine_uid == prev+1)
    sub = detect_same_rec(engine_uid=109699, prev_engine_uid=109698,
                          our_pool_uids=[1, 2, 3])
    if sub != "seq_continuation":
        failures.append(f"Test 9: expected seq_continuation; got {sub}")

    # Test 10: detect_same_rec — outside_pool (engine_uid not in our pool)
    sub = detect_same_rec(engine_uid=42, prev_engine_uid=10,
                          our_pool_uids=[1, 2, 3])
    if sub != "outside_pool":
        failures.append(f"Test 10: expected outside_pool; got {sub}")

    # Test 11: detect_same_rec — None when neither pattern fires
    sub = detect_same_rec(engine_uid=2, prev_engine_uid=10,
                          our_pool_uids=[1, 2, 3])
    if sub is not None:
        failures.append(f"Test 11: expected None when in-pool and non-sequential; got {sub}")

    # Test 12: build_record overrides unknown -> same_rec_diff when applicable
    e = FETuple(ctx5=[1, 2, 3, 4, 5], sp5=[0]*5, voicing=1, leaf_id=(1.0, 0.1), pool_n=10)
    rec = build_record(corpus_id="t", utt_idx=1, slot_idx=2,
                       engine=e, our=e,
                       phase2_record_id=None, fe_evidence_trace_id=None,
                       ts_iso="x",
                       engine_uid=109699, prev_engine_uid=109698,
                       our_pool_uids=[1, 2, 3])
    if rec["fe_class"] != "same_rec_diff" or rec["same_rec_subtype"] != "seq_continuation":
        failures.append(f"Test 12: expected same_rec_diff override; got "
                        f"{rec['fe_class']}/{rec.get('same_rec_subtype')}")

    # Test 13: build_record stays "unknown" when no same_rec inputs given
    rec2 = build_record(corpus_id="t", utt_idx=1, slot_idx=2,
                        engine=e, our=e,
                        phase2_record_id=None, fe_evidence_trace_id=None,
                        ts_iso="x")
    if rec2["fe_class"] != "unknown":
        failures.append(f"Test 13: expected unknown without same_rec inputs; got {rec2['fe_class']}")

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print(f"PASS: 13 self-tests OK", file=sys.stderr)
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--self-test":
        sys.exit(_self_test())
    sys.exit(main())
