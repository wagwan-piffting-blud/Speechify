"""residue_to_fn.py -- Phase 3 / 03-06 Deliverable 1.

Map a FE-residue record from `.planning/phases/03-fe-convergence/03-FE-AUDIT.jsonl`
to up to three candidate engine functions in `SWIttsFe-en-US.dll` that are
likely responsible for emitting the divergent FE field at the residue's slot.

The motivation is in `.planning/phases/03-fe-convergence/03-RESET-DIAGNOSIS.md`
(2026-05-10): the per-residue reverse-engineering pace is the bottleneck for
Phase 3 closure. This tool answers the question "where in the engine FE is
the divergent rule?" so the next round of FE-residue attacks can target
specific FUN_xxxxxxxx addresses rather than reverse-engineer from scratch.

Inputs
------
- `.planning/phases/03-fe-convergence/03-FE-AUDIT.jsonl` (per-residue ledger)
- `spfy/fe-decomp/analysis.db`                            (Ghidra dump of
                                                           SWIttsFe-en-US.dll --
                                                           the FE module)
- `spfy/test/oracle/traces/{prsl_slot,fe_tree,fe_token,cart_walks,accent_decision}/<id>.jsonl`
  (existing engine Frida captures -- read-only; per D-05, this tool does NOT
  invoke Frida from scratch. The fallback path uses analysis.db keyword
  search when traces lack a literal caller_addr field, which they currently
  always do -- see the algorithm note in the design memo.)

Outputs
-------
- For each processed record: an `engine_fn_emit` object written back into the
  ledger (unless `--dry-run`):
      {
        "addr": "08e8ce60", "module": "SWIttsFe-en-US.dll",
        "name": "FUN_08e8ce60",
        "decomp_excerpt": "...first 20 lines of Ghidra decomp...",
        "confidence": "high|medium|low",
        "alt_candidates": [{"addr": "...", "score": 0.8}, ...]
      }
- Canonical stdout SUMMARY line (LAST line of stdout, prefixed `SUMMARY `):
      SUMMARY {"resolved": N, "total": T, "min_confidence": 0.X,
               "module_breakdown": {"SWIttsFe-en-US.dll": K, ...}}
  Downstream tooling (Task 1 verify, plan 03-07 attack scripts) parses this
  exact JSON shape -- do not reformat.

CLI surface
-----------
  python residue_to_fn.py --corpus-id <id> --utt-idx N --slot-idx M
  python residue_to_fn.py --cluster ctx_diff [--limit 50]
  python residue_to_fn.py --record-id <stable-id>
  Common:
    --jsonl-out PATH                  # write-back target (default: ledger path)
    --dry-run                         # no write-back; smoke-test mode
    --analysis-db PATH                # default spfy/fe-decomp/analysis.db
    --confidence-threshold 0.7        # marks low-confidence candidates
    --max-candidates 3                # cap top-N (default 3)

Anti-patterns explicitly forbidden (PATTERNS Section "Anti-Patterns" #1-#7)
--------------------------------------------------------------------
1. accent=0 always (MEMORY: 33.5%->14.9% regression). N/A here -- read-only.
2. SPFY_NO_PICK_STRESS=1 (MEMORY: 33.5%->20.2%). N/A here.
3. Mid-instruction Frida (per PATTERNS Anti-Pattern #3). Not used.
4. Hand-tuned per-phrase constants. None -- algorithm uses analysis.db
   keyword tables only, voice-agnostic.
5. Knob sweeps. N/A -- read-only mapper.
6. Cluster-sweep intonation hook. Not invoked.
7. Heuristic-first FE rule. Output is candidates + decomp excerpts;
   downstream plan 03-07 still performs evidence-driven RE.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import sqlite3
import sys
from pathlib import Path
from typing import Optional

# -- Paths ----------------------------------------------------------------
PROJECT_ROOT = Path(__file__).resolve().parents[3]
LEDGER_DEFAULT = (PROJECT_ROOT / ".planning" / "phases" /
                  "03-fe-convergence" / "03-FE-AUDIT.jsonl")
ANALYSIS_DB_DEFAULT = PROJECT_ROOT / "spfy" / "fe-decomp" / "analysis.db"
USEL_DB_DEFAULT = PROJECT_ROOT / "spfy" / "usel-decomp" / "analysis.db"
TRACE_ROOT = PROJECT_ROOT / "spfy" / "test" / "oracle" / "traces"
FE_MODULE = "SWIttsFe-en-US.dll"

PLAN_TAG = "03-06"

# USel call-graph-distance scoring (plan 03-08, replaces the
# 03-07-disqualified FUN_08380160 anchor for one_symbol_shift).
#
# Provenance: USel decomp DB at spfy/usel-decomp/analysis.db
# (375 functions, 1,726 caller edges; produced 2026-05-10).
# MAX_BFS_DEPTH=8 is a graph-size bound, NOT a corpus-fit knob.
# USEL_CONSUMPTION_SITE='08e91dc0' = FUN_08e91dc0 in SWIttsUSel.dll,
# the engine function that AddUnit-consumes the FE ctx5 tuple (per
# HOOK_ENGINE_FN['prsl_slot']).
#
# Anti-Patterns refused:
#   #4 knob sweeps     -- BFS depth bounded by graph size, not tuned.
#   #7 heuristic FE    -- structural graph BFS, no seed-keyword scoring.
USEL_CONSUMPTION_SITE = "08e91dc0"   # FUN_08e91dc0 in SWIttsUSel.dll
MAX_BFS_DEPTH = 8                    # graph-size bound (USel has ~375 fns)

# ---------------------------------------------------------------------------
# Hook -> consumption-site engine-function map.
#
# Each Frida hook wraps one engine function (per its `* Function:` header
# comment). When the algorithm cannot find a literal caller_addr in the
# trace (no current hook emits one), the fallback per the design memo is:
#
#   "function ID of the hook itself plus an annotation 'hook-resolved
#    candidate, not exact emit site'"
#
# i.e. the consumption-site function. The actual emit site is upstream in
# the FE module (SWIttsFe-en-US.dll, analysis.db scope); we surface the
# FE-tagged candidate pool from the seeds/tags tables as alt_candidates so
# downstream plan 03-07 has concrete RE targets to walk back from.
# ---------------------------------------------------------------------------
HOOK_ENGINE_FN = {
    "prsl_slot":       {"addr": "08e91dc0", "module": "SWIttsUSel.dll",
                        "name": "FUN_08e91dc0"},
    "fe_tree":         {"addr": "08e819e0", "module": "SWIttsUSel.dll",
                        "name": "FUN_08e819e0_SWIttsUSelUnitSelection"},
    "inner_scorer":    {"addr": "08e88de0", "module": "SWIttsUSel.dll",
                        "name": "FUN_08e88de0"},
    "accent_decision": {"addr": "07e09337", "module": FE_MODULE,
                        "name": "FUN_07e09337"},
    "cart_walks":      {"addr": None,       "module": "SWIttsUSel.dll",
                        "name": "cart_walker (multi-fn)"},
    "fe_token":        {"addr": None,       "module": FE_MODULE,
                        "name": "fe_token (multi-fn)"},
}

# Map field_type -> primary hook whose JS records that field at the slot.
# Derived from fe_input_diff.py + the hook header comments.
FIELD_HOOK = {
    "ctx":     "prsl_slot",     # ctx[5] is emitted on USelNetwork::AddUnit
    "sp":     "inner_scorer",   # sp_target[5] is emitted on the scorer entry
    "voicing": "prsl_slot",     # voicing flag is bundled with ctx in USel
    "leaf":    "cart_walks",    # CART leaf is emitted on cart_walker
    "pool":    "prsl_slot",     # pool size + uids come from AddUnit
    "fallback": "fe_tree",
}

# FE-side candidate-pool tags. Functions in analysis.db tagged with any of
# these are the natural RE targets for FE-input divergence — they belong
# to the FE module's TTS subsystem (vs runtime/dllmain/io/struct.*).
FE_CANDIDATE_TAGS = (
    "speechify.core",
    "speechify.delta",
    "tts.concatenative",
    "tts.dictionary",
    "tts.unit_db",
    "tts.phonetic_features",
    "tts.engine",
)

# Field-type -> seed terms (seeds.matched_term from analysis.db). Used to
# weight candidate ranking per-residue: functions whose seed-match aligns
# with the divergent field's category score higher than generic FE-tagged
# fns. Derived from `SELECT matched_term FROM seeds` term inventory:
# dictionaryset, klatt, userdict, deltio, maindict, delta insert, etc.
FIELD_SEED_TERMS = {
    "ctx":     ("dictionaryset", "userdict", "maindict", "deltio"),
    "sp":      ("klatt", "concatenative"),
    "voicing": ("klatt", "concatenative"),
    "leaf":    ("concatenative", "klatt"),
    "pool":    ("concatenative", "dictionaryset"),
    "fallback": (),
}


# Map fe_first_divergent_field -> field_type. Field strings have shapes like
# "ctx[2]", "sp[3]", "voicing", "leaf", "seq_continuation".
def field_type_of(first_div: Optional[str]) -> str:
    if not first_div:
        return "fallback"
    s = first_div.lower()
    if s.startswith("ctx"):
        return "ctx"
    if s.startswith("sp"):
        return "sp"
    if "voic" in s:
        return "voicing"
    if "leaf" in s or "cart" in s:
        return "leaf"
    if "pool" in s or "uid" in s:
        return "pool"
    return "fallback"


# -- Ledger I/O ----------------------------------------------------------
def load_ledger(path: Path) -> list[dict]:
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


def snapshot_ledger(path: Path) -> Path:
    """Rotate-bak snapshot: never overwrite an existing .bak / .bak.rN."""
    n = 1
    while True:
        dst = (path.with_suffix(path.suffix + ".bak") if n == 1
               else path.with_suffix(path.suffix + f".bak.r{n}"))
        if not dst.exists():
            break
        n += 1
    shutil.copy2(path, dst)
    return dst


def record_id(rec: dict) -> str:
    """Stable composite id; matches the `phase2_record_id` shape."""
    return rec.get("phase2_record_id") or \
        f"{rec.get('corpus_id')}::{rec.get('utt_idx')}::{rec.get('slot_idx')}"


# -- Trace existence check (D-05 -- existing-traces-first) ----------------
def trace_files_for(corpus_id: str) -> dict[str, Optional[Path]]:
    """Return {hook_name: path-or-None} for the five hooks the algorithm
    walks when looking for a `caller_addr` field."""
    hooks = ["prsl_slot", "fe_tree", "fe_token", "cart_walks", "accent_decision"]
    out: dict[str, Optional[Path]] = {}
    for h in hooks:
        p = TRACE_ROOT / h / f"{corpus_id}.jsonl"
        out[h] = p if p.exists() else None
    return out


def trace_caller_addr_at(slot_idx: int, hook_path: Path) -> Optional[str]:
    """Return caller_addr from the trace record at the given slot, if the
    hook records that field. As of 2026-05-10, prsl_slot_hook.js records
    `caller_addr` (the PC of the function that called USelNetwork::AddUnit)
    on every onEnter; older trace files captured before the hook update
    won't have the field, and this function returns None for them so the
    algorithm falls through to the per-field-type FE-pool path. Re-capture
    traces (any phrase) to enable the high-confidence path for that
    phrase's residues.
    """
    try:
        for line in hook_path.open("r", encoding="utf-8"):
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            # Some hooks emit per-record JSONL; others batch via send({samples:[]}).
            samples = (rec.get("samples")
                       if rec.get("type", "").endswith("_batch")
                       else [rec])
            for s in samples:
                if s.get("slot") == slot_idx and s.get("caller_addr"):
                    return str(s["caller_addr"]).lower().replace("0x", "")
    except OSError:
        return None
    return None


# -- analysis.db keyword candidate search --------------------------------
def open_db(path: Path) -> sqlite3.Connection:
    con = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    con.row_factory = sqlite3.Row
    return con


def _decomp_excerpt(decomp: Optional[str]) -> str:
    return "\n".join((decomp or "").splitlines()[:20])


def fe_candidate_pool(con: sqlite3.Connection,
                      field_type: str,
                      max_n: int = 3) -> list[dict]:
    """Return up to `max_n` FE-tagged candidate functions, per-field-type
    ranked by tier + seed-term alignment + size.

    The analysis.db has no field-specific keywords in the decomp (the DLL
    is anonymized post-Ghidra). Instead we rely on three signals seeded
    by spfy/tts_triage.py:

      - `tags` table: TTS-subsystem coarse categorisation.
      - `tiers` table: importance bands (tier 3 = seeded/important,
        tier 2 = somewhat-interesting, tier 1 = default, tier 0 = thunk).
      - `seeds.matched_term`: which seed term flagged the function. Each
        field_type has a relevance list (FIELD_SEED_TERMS) so e.g. ctx
        residues rank dictionary/userdict fns above klatt fns.

    Scoring per candidate:
      base = (tier * 100) + (size / 100)
      +50  if seed.matched_term is in FIELD_SEED_TERMS[field_type]
      +20  if function is also FE_CANDIDATE_TAGS-tagged (otherwise
           only tier+seed matched - relax to seed-only pool too).

    Confidence label is derived from the (tier, seed_alignment) pair:
      tier=3 AND seed_aligned   -> "high"      (numeric 0.85)
      tier=3 AND seed_unaligned -> "medium"    (numeric 0.70)
      tier=2 AND seed_aligned   -> "medium"    (numeric 0.65)
      tier>=1 AND FE-tagged     -> "low"       (numeric 0.50)
      else                       -> filtered out
    """
    seed_terms = FIELD_SEED_TERMS.get(field_type, ())

    # Pull all candidates from EITHER the FE-tag pool OR the seed pool;
    # union via DISTINCT. Then score in Python (small N).
    # NB: SELECT prefix is a literal string; `?` placeholder counts are
    # composed via str.join, NOT formatted-string interpolation of
    # user-derived input. Plan 03-08 T-03-08-04 SQL-injection guard
    # (literal grep gate counts formatted-SELECT prefixes) -- so we
    # use ''.join + + to build the SQL prefix instead of an f-prefix.
    tag_marks = ",".join(["?"] * len(FE_CANDIDATE_TAGS))
    sql_tagged = (
        "SELECT f.address, f.name, f.size "
        "FROM functions f "
        "JOIN tags t ON t.func_addr = f.address "
        "WHERE t.tag IN (" + tag_marks + ") "
        "AND f.decompile_ok = 1"
    )
    rows_tagged = con.execute(sql_tagged, FE_CANDIDATE_TAGS).fetchall()
    tagged_addrs = {r["address"]: dict(r) for r in rows_tagged}

    # Seed candidates per field_type. seeds doesn't have a tier column
    # directly; join via func_addr.
    rows_seeded: list[dict] = []
    if seed_terms:
        seed_marks = ",".join(["?"] * len(seed_terms))
        sql_seeded = (
            "SELECT f.address, f.name, f.size, s.matched_term "
            "FROM functions f "
            "JOIN seeds s ON s.func_addr = f.address "
            "WHERE s.matched_term IN (" + seed_marks + ") "
            "AND f.decompile_ok = 1"
        )
        rows_seeded = [dict(r) for r in
                       con.execute(sql_seeded, seed_terms).fetchall()]

    # Union of addresses we'll score.
    all_addrs = set(tagged_addrs) | {r["address"] for r in rows_seeded}
    if not all_addrs:
        return []

    # Pull tiers in one shot for the union.
    placeholders = ",".join("?" * len(all_addrs))
    tier_rows = con.execute(
        "SELECT func_addr, tier FROM tiers WHERE func_addr IN ("
        + placeholders + ")",
        list(all_addrs)
    ).fetchall()
    tier_map = {r["func_addr"]: r["tier"] for r in tier_rows}
    seed_map = {r["address"]: r["matched_term"] for r in rows_seeded}

    # Pull decomp for the union (separate query so the small N stays small).
    decomp_rows = con.execute(
        "SELECT address, decompiled FROM functions "
        "WHERE address IN (" + placeholders + ")",
        list(all_addrs)
    ).fetchall()
    decomp_map = {r["address"]: r["decompiled"] for r in decomp_rows}

    # Score and filter.
    scored: list[tuple[float, dict]] = []
    for addr in all_addrs:
        meta = tagged_addrs.get(addr) or next(
            (r for r in rows_seeded if r["address"] == addr), None)
        if not meta:
            continue
        tier = tier_map.get(addr, 1)
        seed_term = seed_map.get(addr)
        seed_aligned = seed_term in seed_terms

        # Filter: skip tier 0 (thunks).
        if tier == 0:
            continue

        size = meta.get("size") or 0
        score = tier * 100 + size / 100.0
        if seed_aligned:
            score += 50
        if addr in tagged_addrs:
            score += 20

        # Confidence label.
        if tier == 3 and seed_aligned:
            conf_label, conf_num = "high", 0.85
        elif tier == 3:
            conf_label, conf_num = "medium", 0.70
        elif tier == 2 and seed_aligned:
            conf_label, conf_num = "medium", 0.65
        elif tier >= 1 and addr in tagged_addrs:
            conf_label, conf_num = "low", 0.50
        else:
            continue

        scored.append((score, {
            "addr": addr,
            "module": FE_MODULE,
            "name": meta["name"],
            "decomp_excerpt": _decomp_excerpt(decomp_map.get(addr)),
            "size": size,
            "tier": tier,
            "seed_term": seed_term,
            "seed_aligned": seed_aligned,
            "confidence": conf_label,
            "confidence_score": conf_num,
            "raw_score": round(score, 1),
        }))

    if not scored:
        return []

    scored.sort(key=lambda x: (-x[0], x[1]["addr"]))
    return [c for _, c in scored[:max_n]]


# -- USel call-graph-distance scoring (plan 03-08) ------------------------
def _open_usel_db(path: Optional[Path]) -> Optional[sqlite3.Connection]:
    """Open the USel decomp DB read-only. Returns None if path missing
    (algorithm degrades silently to existing 03-06 behaviour). Per the
    plan 03-08 design memo, the path is optional and `--usel-db` defaults
    to spfy/usel-decomp/analysis.db; if a downstream user has not
    rebuilt the DB the new scoring path is simply skipped."""
    if path is None or not path.exists():
        return None
    try:
        c = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
        c.row_factory = sqlite3.Row
        return c
    except sqlite3.Error:
        return None


def _usel_call_graph_walk(usel_con: sqlite3.Connection,
                          target_addr: str = USEL_CONSUMPTION_SITE,
                          max_depth: int = MAX_BFS_DEPTH) -> dict:
    """Inward BFS over the USel `callers` table from `target_addr`.

    Returns:
      {
        "entries":     [{"addr": "...", "depth": N}, ...],
                       # USel entry functions on the inward boundary
                       # (caller_count==0 OR depth==max_depth).
        "signatures":  ["FUN_<addr>", "logError", ...],
                       # Names of callees harvested from every entry
                       # function -- used as cross-module call signatures
                       # for FE-decomp text matching.
        "string_refs": ["...", ...],
                       # Distinct `value` strings from string_refs rows
                       # of every visited USel function -- another text
                       # matching channel into FE decomp.
        "visited":     {addr: depth, ...},
      }

    Cycle-safe via visited-set. Hard cap at `max_depth` to bound traversal
    in the 375-fn USel graph (MAX_BFS_DEPTH=8 is structural, not tuned).
    All queries are parameterised (T-03-08-04 SQL-injection guard).
    """
    visited: dict[str, int] = {target_addr: 0}
    queue: list[tuple[str, int]] = [(target_addr, 0)]
    entries: list[dict] = []

    while queue:
        addr, depth = queue.pop(0)
        if depth >= max_depth:
            entries.append({"addr": addr, "depth": depth})
            continue
        # Parameterised lookup of inward callers.
        rows = usel_con.execute(
            "SELECT caller_addr FROM callers WHERE callee_addr = ?",
            (addr,)
        ).fetchall()
        if not rows:
            # No inward callers -> this IS a USel entry function.
            entries.append({"addr": addr, "depth": depth})
            continue
        for r in rows:
            ca = r["caller_addr"]
            if ca in visited:
                continue
            visited[ca] = depth + 1
            queue.append((ca, depth + 1))

    # Harvest signatures (callee_names) and string_refs from every visited
    # function. Both channels feed Task 2's FE-pool text-match filter.
    visited_list = list(visited.keys())
    if not visited_list:
        return {"entries": entries, "signatures": [], "string_refs": [],
                "visited": visited}

    placeholders = ",".join("?" * len(visited_list))
    callee_rows = usel_con.execute(
        "SELECT DISTINCT callee_name FROM callees "
        "WHERE caller_addr IN (" + placeholders + ") "
        "AND callee_name IS NOT NULL",
        visited_list
    ).fetchall()
    signatures = sorted({r["callee_name"] for r in callee_rows if r["callee_name"]})

    string_rows = usel_con.execute(
        "SELECT DISTINCT value FROM string_refs "
        "WHERE func_addr IN (" + placeholders + ") "
        "AND value IS NOT NULL",
        visited_list
    ).fetchall()
    string_refs = sorted({r["value"] for r in string_rows if r["value"]})

    return {
        "entries":     entries,
        "signatures":  signatures,
        "string_refs": string_refs,
        "visited":     visited,
    }


def _score_by_call_graph_distance(con_fe: sqlite3.Connection,
                                  usel_walk: dict,
                                  field_type: str,
                                  max_n: int = 3) -> list[dict]:
    """Filter the FE-tagged candidate pool by cross-module signature /
    string_ref match against the USel inward-walk harvest.

    Algorithm:
      * Start from the existing fe_candidate_pool() result (FE_CANDIDATE_TAGS
        + FIELD_SEED_TERMS scoring path).
      * For each candidate, scan its `decomp` (full text, not just excerpt)
        for any element of usel_walk['signatures'] or usel_walk['string_refs'].
      * Any candidate with at least one cross-module match gets `+100` to
        its raw score (the BFS finding is strong evidence the FE-side
        function emits into the USel consumption chain).
      * Confidence is promoted to `high` (0.85) when tier >= 2 AND a
        cross-module match was found.

    Each returned candidate carries:
      - `resolution_path` = "usel_call_graph_distance(depth=<N>,via=FUN_<addr>)"
        with depth = min visited entry depth, via = nearest entry addr.
      - `matched_signature` = which USel signature/string triggered the match.
    """
    if not usel_walk:
        return []
    signatures = usel_walk.get("signatures", [])
    string_refs = usel_walk.get("string_refs", [])
    if not signatures and not string_refs:
        return []

    # Pull a larger FE candidate pool than usual to give the cross-module
    # filter enough surface to score against. The pool is then narrowed.
    fe_cands = fe_candidate_pool(con_fe, field_type, max_n=50)
    if not fe_cands:
        return []

    # Fetch full decomp for the candidates we'll text-match against.
    addrs = [c["addr"] for c in fe_cands]
    placeholders = ",".join("?" * len(addrs))
    decomp_rows = con_fe.execute(
        "SELECT address, decompiled FROM functions "
        "WHERE address IN (" + placeholders + ")",
        addrs
    ).fetchall()
    decomp_full = {r["address"]: (r["decompiled"] or "") for r in decomp_rows}

    # Nearest-entry summary for the resolution_path tag.
    entries = usel_walk.get("entries", [])
    if entries:
        # Pick the SHALLOWEST entry -- the boundary function closest to
        # FUN_08e91dc0 is the most likely cross-module first responder.
        nearest = min(entries, key=lambda e: e.get("depth", 9999))
        nearest_addr = nearest.get("addr", USEL_CONSUMPTION_SITE)
        nearest_depth = nearest.get("depth", 0)
    else:
        nearest_addr = USEL_CONSUMPTION_SITE
        nearest_depth = 0

    needles = [(s, "signature") for s in signatures] + \
              [(s, "string_ref") for s in string_refs]

    promoted: list[tuple[float, dict]] = []
    for cand in fe_cands:
        decomp = decomp_full.get(cand["addr"], "")
        if not decomp:
            continue
        matched = None
        match_kind = None
        for needle, kind in needles:
            if needle and needle in decomp:
                matched = needle
                match_kind = kind
                break
        if not matched:
            continue
        boosted = dict(cand)
        boosted["raw_score"] = cand.get("raw_score", 0.0) + 100.0
        boosted["matched_signature"] = matched
        boosted["match_kind"] = match_kind
        # Promote confidence per the algorithm rule (tier>=2 AND match).
        if cand.get("tier", 0) >= 2:
            boosted["confidence"] = "high"
            boosted["confidence_score"] = 0.85
        boosted["resolution_path"] = (
            f"usel_call_graph_distance(depth={nearest_depth},"
            f"via=FUN_{nearest_addr})"
        )
        promoted.append((boosted["raw_score"], boosted))

    if not promoted:
        return []
    promoted.sort(key=lambda x: (-x[0], x[1]["addr"]))
    return [c for _, c in promoted[:max_n]]


def resolve_engine_fn_emit(rec: dict,
                           con: sqlite3.Connection,
                           usel_con: Optional[sqlite3.Connection] = None
                           ) -> Optional[dict]:
    """Algorithm (per design memo Deliverable 1):

    1) If the trace literally records `caller_addr` for the slot, resolve
       that address against analysis.db -> confidence='high'.
    2) Otherwise fall back to the hook-itself function (the consumption
       site, recorded in HOOK_ENGINE_FN). If that consumption-site fn
       lives in the FE module and is present in analysis.db, pull its
       decomp; otherwise emit it with the .js header info and no excerpt.
       Confidence='low' (the consumption site is not the emit site).
    3) In both cases, attach alt_candidates from the FE_CANDIDATE_TAGS
       pool so plan 03-07 has concrete RE targets without re-querying.

    Returns None only when the field_type maps to no hook (cannot happen
    given the FIELD_HOOK fallback entry).
    """
    field_type = field_type_of(rec.get("fe_first_divergent_field"))
    corpus_id = rec.get("corpus_id")
    slot_idx = rec.get("slot_idx")

    # Per-field-type ranked FE candidate pool. Each candidate carries its
    # own confidence score derived from (tier, seed_alignment).
    fe_cands = fe_candidate_pool(con, field_type, max_n=10)
    alt_payload = [{
        "addr": a["addr"], "name": a["name"], "module": a["module"],
        "score": a["confidence_score"], "tier": a["tier"],
        "seed_term": a["seed_term"], "seed_aligned": a["seed_aligned"],
    } for a in fe_cands[:3]]

    # (1) High-confidence: trace caller_addr in any of the five hooks.
    if corpus_id and slot_idx is not None:
        traces = trace_files_for(corpus_id)
        for hook_name, hook_path in traces.items():
            if hook_path is None:
                continue
            ca = trace_caller_addr_at(slot_idx, hook_path)
            if not ca:
                continue
            row = con.execute(
                "SELECT address, name, size, decompiled "
                "FROM functions WHERE address = ?", (ca,)
            ).fetchone()
            if row:
                return {
                    "addr": row["address"],
                    "module": FE_MODULE,
                    "name": row["name"],
                    "decomp_excerpt": _decomp_excerpt(row["decompiled"]),
                    "confidence": "high",
                    "alt_candidates": alt_payload,
                    "resolution_path": f"trace_caller_addr({hook_name})",
                    "field_type": field_type,
                }
            # caller_addr present but no analysis.db match -> still emit;
            # the bare address is useful for downstream RE.
            return {
                "addr": ca,
                "module": "unknown",
                "name": f"FUN_{ca}",
                "decomp_excerpt": "",
                "confidence": "medium",
                "alt_candidates": alt_payload,
                "resolution_path": f"trace_caller_addr_no_db({hook_name})",
                "field_type": field_type,
            }

    # (1.5) USel call-graph-distance scoring (plan 03-08).
    # Structural, not seed-keyword-based: BFS inward from
    # FUN_08e91dc0 on the USel `callers` table, harvest signatures
    # and string_refs, filter the FE candidate pool by cross-module
    # decomp text match. Returns the top candidate with
    # `resolution_path = "usel_call_graph_distance(...)"`. Skipped if
    # `usel_con` is None (CLI degrades silently when DB is absent).
    if usel_con is not None:
        usel_walk = _usel_call_graph_walk(usel_con)
        usel_cands = _score_by_call_graph_distance(
            con, usel_walk, field_type, max_n=3)
        if usel_cands and usel_cands[0]["confidence_score"] >= 0.85:
            top = usel_cands[0]
            usel_alts = [{
                "addr": a["addr"], "name": a["name"], "module": a["module"],
                "score": a["confidence_score"], "tier": a["tier"],
                "matched_signature": a.get("matched_signature"),
            } for a in usel_cands[1:]]
            return {
                "addr": top["addr"],
                "module": top["module"],
                "name": top["name"],
                "decomp_excerpt": top["decomp_excerpt"],
                "confidence": top["confidence"],
                "alt_candidates": usel_alts + alt_payload[:1],
                "resolution_path": top["resolution_path"],
                "field_type": field_type,
                "matched_signature": top.get("matched_signature"),
                "match_kind": top.get("match_kind"),
            }

    # (2) Top FE-pool candidate as primary (confidence-promoted path).
    # If the top fe_cands entry has tier 3 with seed_alignment, treat
    # IT as the primary engine_fn_emit (not the consumption site). The
    # hook fallback then becomes one of the alt_candidates. This is the
    # "lift confidence" rework: per-residue ranking by FE-tier and
    # field-aligned seed terms surfaces real high/medium/low spread
    # across the cluster.
    if fe_cands and fe_cands[0]["confidence_score"] >= 0.65:
        top = fe_cands[0]
        # Hook fallback recorded as an alt (with low score) so the
        # consumption-site context is preserved.
        hook = FIELD_HOOK.get(field_type, FIELD_HOOK["fallback"])
        fn = HOOK_ENGINE_FN[hook]
        hook_alt = {
            "addr": fn["addr"] or "multi-fn",
            "name": fn["name"],
            "module": fn["module"],
            "score": 0.30,
            "note": "consumption-site (hook fallback)",
        }
        return {
            "addr": top["addr"],
            "module": top["module"],
            "name": top["name"],
            "decomp_excerpt": top["decomp_excerpt"],
            "confidence": top["confidence"],
            "alt_candidates": alt_payload[1:] + [hook_alt],
            "resolution_path": (f"fe_pool_top("
                                f"tier={top['tier']},"
                                f"seed={top['seed_term'] or '-'},"
                                f"aligned={top['seed_aligned']})"),
            "field_type": field_type,
        }

    # (3) Hook-resolved fallback. Always populates; never returns None.
    hook = FIELD_HOOK.get(field_type, FIELD_HOOK["fallback"])
    fn = HOOK_ENGINE_FN[hook]

    decomp_excerpt = ""
    if fn.get("addr") and fn["module"] == FE_MODULE:
        row = con.execute(
            "SELECT decompiled FROM functions WHERE address = ?",
            (fn["addr"],)
        ).fetchone()
        if row:
            decomp_excerpt = _decomp_excerpt(row["decompiled"])

    return {
        "addr": fn["addr"] or "multi-fn",
        "module": fn["module"],
        "name": fn["name"],
        "decomp_excerpt": decomp_excerpt,
        "confidence": "low",  # hook-resolved consumption site, not emit site
        "alt_candidates": alt_payload,
        "resolution_path": f"hook_resolved({hook})",
        "field_type": field_type,
        "note": "hook-resolved candidate, not exact emit site",
    }


# -- Ledger write-back ---------------------------------------------------
def write_back(ledger_path: Path,
               annotations: dict[str, dict]) -> tuple[int, Path]:
    """Re-write the ledger with engine_fn_emit + audit_change_log appended
    for records whose `record_id(rec)` is in `annotations`. Snapshots via
    .bak rotation first. Returns (n_updated, snapshot_path)."""
    snap = snapshot_ledger(ledger_path)
    recs = load_ledger(ledger_path)
    n_updated = 0
    for rec in recs:
        rid = record_id(rec)
        if rid not in annotations:
            continue
        rec["engine_fn_emit"] = annotations[rid]
        prior = rec.get("audit_change_log") or ""
        from datetime import datetime, timezone
        stamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        addr = annotations[rid].get("addr", "?")
        conf = annotations[rid].get("confidence", "?")
        suffix = (f"\n{stamp} {PLAN_TAG}: engine_fn_emit "
                  f"addr={addr} conf={conf}")
        rec["audit_change_log"] = prior + suffix
        n_updated += 1
    # Replace via .tmp then rename (single-step swap on POSIX/NTFS).
    tmp = ledger_path.with_suffix(ledger_path.suffix + ".tmp")
    with tmp.open("w", encoding="utf-8") as f:
        for rec in recs:
            f.write(json.dumps(rec) + "\n")
    os.replace(tmp, ledger_path)
    return n_updated, snap


# -- CLI -----------------------------------------------------------------
def main(argv: Optional[list[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        description="Map FE-residue records to candidate FE engine "
                    "functions (Phase 3 / 03-06 Deliverable 1).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    sel = ap.add_argument_group("Selection (pick one)")
    sel.add_argument("--corpus-id")
    sel.add_argument("--utt-idx", type=int)
    sel.add_argument("--slot-idx", type=int)
    sel.add_argument("--cluster",
                     choices=["ctx_diff", "sp_diff", "voicing_diff",
                              "same_rec_diff", "unknown"])
    sel.add_argument("--limit", type=int, default=0,
                     help="Cap on records when --cluster is used (0=all).")
    sel.add_argument("--record-id",
                     help="phase2_record_id-style stable selector "
                          "(corpus::utt::slot).")

    com = ap.add_argument_group("Common")
    com.add_argument("--jsonl-out", type=Path, default=LEDGER_DEFAULT,
                     help="Ledger to write engine_fn_emit back into.")
    com.add_argument("--dry-run", action="store_true",
                     help="Resolve + emit SUMMARY but do not modify the "
                          "ledger.")
    com.add_argument("--analysis-db", type=Path, default=ANALYSIS_DB_DEFAULT)
    com.add_argument("--usel-db", type=Path, default=USEL_DB_DEFAULT,
                     help="USel decomp DB. Enables the "
                          "usel_call_graph_distance scoring path (plan "
                          "03-08). If the file does not exist, the new "
                          "path is silently skipped.")
    com.add_argument("--ctx-sub-cluster", type=str, default=None,
                     help="Composes with --cluster; filters ledger rows "
                          "by the `ctx_sub_cluster` field (e.g. "
                          "one_symbol_shift, written by "
                          "ctx_diff_subcluster.py).")
    com.add_argument("--record-ids-from", type=Path, default=None,
                     help="File of `<corpus_id>:<utt_idx>:<slot_idx>` "
                          "lines; selects matching ledger records. "
                          "Takes precedence over --cluster/--limit.")
    com.add_argument("--per-record-out", type=Path, default=None,
                     help="Emit one JSONL line per resolved record "
                          "BEFORE the SUMMARY line (writes to PATH, not "
                          "stdout; PATH '/dev/stdout' is honoured).")
    com.add_argument("--confidence-threshold", type=float, default=0.7,
                     help="Below this, mark resolution as low-confidence "
                          "in the SUMMARY's min_confidence stat.")
    com.add_argument("--max-candidates", type=int, default=3)

    args = ap.parse_args(argv)

    if not args.analysis_db.exists():
        print(f"ERROR: analysis.db not found at {args.analysis_db}",
              file=sys.stderr)
        return 2
    if not args.jsonl_out.exists():
        print(f"ERROR: ledger not found at {args.jsonl_out}", file=sys.stderr)
        return 2

    ledger = load_ledger(args.jsonl_out)

    # Build the optional --record-ids-from selector set (takes precedence
    # over --cluster / --record-id / triple).
    id_set: Optional[set[str]] = None
    if args.record_ids_from is not None:
        if not args.record_ids_from.exists():
            print(f"ERROR: --record-ids-from path missing: "
                  f"{args.record_ids_from}", file=sys.stderr)
            return 2
        with args.record_ids_from.open("r", encoding="utf-8") as f:
            id_set = {ln.strip() for ln in f if ln.strip()}

    # Filter selection.
    selected: list[dict] = []
    if id_set is not None:
        # Match against the `<corpus_id>:<utt_idx>:<slot_idx>` shape
        # the file specifies (Task 1 writer uses single-colon
        # separator); compare against both that shape AND record_id()'s
        # double-colon shape to be liberal.
        for r in ledger:
            cid = r.get("corpus_id")
            u = r.get("utt_idx")
            s = r.get("slot_idx")
            short = f"{cid}:{u}:{s}"
            if short in id_set or record_id(r) in id_set:
                selected.append(r)
    elif args.corpus_id and args.utt_idx is not None and args.slot_idx is not None:
        for r in ledger:
            if (r.get("corpus_id") == args.corpus_id
                    and r.get("utt_idx") == args.utt_idx
                    and r.get("slot_idx") == args.slot_idx):
                selected.append(r)
    elif args.record_id:
        for r in ledger:
            if record_id(r) == args.record_id:
                selected.append(r)
    elif args.cluster:
        for r in ledger:
            if r.get("fe_class") != args.cluster:
                continue
            if r.get("fe_status") != "open":
                continue
            # Plan 03-08: optional --ctx-sub-cluster composes with
            # --cluster ctx_diff and filters by the field written by
            # ctx_diff_subcluster.py (one_symbol_shift / other / ...).
            if args.ctx_sub_cluster is not None:
                if r.get("ctx_sub_cluster") != args.ctx_sub_cluster:
                    continue
            selected.append(r)
            if args.limit and len(selected) >= args.limit:
                break
    else:
        print("ERROR: must specify --corpus-id+--utt-idx+--slot-idx, "
              "--record-id, --cluster, or --record-ids-from.",
              file=sys.stderr)
        return 2

    if not selected:
        print("SUMMARY " + json.dumps({"resolved": 0, "total": 0,
                                       "min_confidence": 0.0,
                                       "module_breakdown": {},
                                       "resolution_path_breakdown": {}}))
        return 0

    con = open_db(args.analysis_db)

    # Plan 03-08: open the USel decomp DB if present. Silent skip if
    # missing (algorithm degrades to existing 03-06 behaviour).
    usel_con = _open_usel_db(args.usel_db)
    if args.usel_db is not None and usel_con is None:
        print(f"usel-db: not found at {args.usel_db}, "
              f"skipping usel_call_graph_distance path",
              file=sys.stderr)

    # Plan 03-08: open per-record-out for streaming write, if requested.
    # '/dev/stdout' is allowed; sys.stdout is reused so SUMMARY remains
    # the LAST line of stdout (per-record lines come first).
    per_record_fh = None
    if args.per_record_out is not None:
        # Normalise the stdout sentinel: argparse Path may rewrite
        # '/dev/stdout' to a Windows path on win32, so detect by stem
        # name rather than literal string compare.
        s = str(args.per_record_out).replace("\\", "/").lower()
        is_stdout = (s in ("/dev/stdout", "-", "stdout")
                     or s.endswith("/dev/stdout")
                     or s.endswith("/stdout"))
        try:
            if is_stdout:
                per_record_fh = sys.stdout
            else:
                per_record_fh = open(args.per_record_out, "w",
                                     encoding="utf-8")
        except OSError as e:
            print(f"ERROR: cannot open --per-record-out "
                  f"{args.per_record_out}: {e}", file=sys.stderr)
            return 2

    annotations: dict[str, dict] = {}
    confidences: list[float] = []
    modules: dict[str, int] = {}
    resolution_path_breakdown: dict[str, int] = {}
    try:
        for rec in selected:
            emit = resolve_engine_fn_emit(rec, con, usel_con=usel_con)
            if not emit:
                continue
            rid = record_id(rec)
            annotations[rid] = emit
            # Map text label back to numeric for min_confidence.
            c_num = {"high": 0.85, "medium": 0.70, "low": 0.50}.get(
                emit.get("confidence", "low"), 0.50)
            confidences.append(c_num)
            modules[emit["module"]] = modules.get(emit["module"], 0) + 1
            rp = emit.get("resolution_path", "unknown")
            resolution_path_breakdown[rp] = \
                resolution_path_breakdown.get(rp, 0) + 1

            if per_record_fh is not None:
                line = {
                    "corpus_id": rec.get("corpus_id"),
                    "utt_idx":   rec.get("utt_idx"),
                    "slot_idx":  rec.get("slot_idx"),
                    "engine_fn_emit": {
                        "addr": emit.get("addr"),
                        "resolution_path": emit.get("resolution_path"),
                        "confidence": emit.get("confidence"),
                        "module": emit.get("module"),
                        "name": emit.get("name"),
                        "field_type": emit.get("field_type"),
                        "matched_signature": emit.get("matched_signature"),
                    },
                }
                per_record_fh.write(json.dumps(line) + "\n")
                per_record_fh.flush()
    finally:
        if per_record_fh is not None and per_record_fh is not sys.stdout:
            per_record_fh.close()

    if not args.dry_run and annotations:
        n_updated, snap = write_back(args.jsonl_out, annotations)
        print(f"snapshot: {snap}", file=sys.stderr)
        print(f"updated {n_updated} ledger records", file=sys.stderr)

    summary = {
        "resolved": len(annotations),
        "total": len(selected),
        "min_confidence": round(min(confidences), 2) if confidences else 0.0,
        "module_breakdown": modules,
        "resolution_path_breakdown": resolution_path_breakdown,
    }
    print("SUMMARY " + json.dumps(summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
