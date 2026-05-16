"""master_compare.py - single source of truth for spfy_synth <-> engine audit.

Replaces audit_focused.py, walk_engine_path.py, compare_dag.py. Produces all
three families of metrics from one synth invocation per phrase, with
parallel workers, apples-to-apples definitions, and a JSON dump for
deterministic run-to-run diffs.

Why this exists
---------------
The three legacy scripts in c:/tmp/ measured incomparable populations:
  - compare_dag.py was capped at 12 phrases AND its oracle dropped anchor
    slots while the synth output expanded them, so positional comparison
    was misaligned.
  - walk_engine_path.py walked predec correctly and expanded anchors on
    both sides, but ran one synth subprocess per phrase serially.
  - audit_focused.py's headline % conflated phrase-structure errors
    (synth produced a different n_hp than engine) with per-slot fidelity.

This script unifies the three by:
  1. Running each synth ONCE with SPFY_SYNTH_DEBUG=1 + SPFY_DUMP_PATH=1
     so the per-slot JSON and the path UID dump come from a single run.
  2. Loading the engine ground truth from prsl_slot / inner_scorer /
     cart_walks / viterbi_dp in the main process, then handing
     (text, env) to a multiprocessing pool of workers.
  3. Reporting THREE separate numbers, never blended:
       - Phrase-structure: phrases where n_hp(synth) == n_hp(engine).
       - Slot fidelity: per-slot ctx/sp/durt/f0tr/pool match, computed
         only over structure-matching phrases (avoids alignment-shift
         inflation). Categories broken out individually.
       - Path UID: positional UID match between synth's HP-expanded path
         and engine's predec-walked + anchor-expanded path, also only
         over structure-matching phrases.
     A "positional-aligned" slot-fidelity % over ALL phrases is also
     printed for backward-compat diffing against RESUME.md numbers.

Both hand-FE (64-bit) and hosted-FE (32-bit) builds are driven the same
way: SPFY_FIRST_PHRASE_ONLY=1 alone is sufficient because both code paths
honour it at spfy_synth.c's per-phrase loop, and the hosted-FE parser
assigns one phrase_id per #{...} block so utt-0 alignment matches the
engine traces. SPFY_FE_HOST_PHRASE_MERGE is for AUDIO concatenation only
and must NOT be set during audits.

Usage
-----
  python spfy/test/oracle/master_compare.py \
      --exe c:/tmp/spfy_build64_handfe/src/cli/spfy_synth.exe \
      --workers 8

Common flags:
  --exe PATH              spfy_synth.exe (or env SPFY_SYNTH_EXE)
  --workers N             parallel synth subprocesses (default: cpu_count/2)
  --filter REGEX          only run phrases whose id matches
  --modes slot,uid        which metric families to compute (default: both)
  --show-diff             dump first divergence per failing phrase
  --json OUT.json         write structured per-phrase results for diffing
  --quiet                 suppress per-phrase lines, only print totals

The script does NOT build. It will fail loudly if --exe doesn't exist;
build first with one of c:/tmp/build32.bat or c:/tmp/build64_handfe.bat
(see RESUME.md).
"""

from __future__ import annotations

import argparse
import json
import multiprocessing as mp
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass, field, asdict
from pathlib import Path

# ---------------------------------------------------------------------------
# Paths (all absolute, resolved relative to this file's repo root).
# ---------------------------------------------------------------------------

THIS = Path(__file__).resolve()
REPO = THIS.parents[3]                                  # .../Speechify
ORACLE = THIS.parent                                    # .../spfy/test/oracle

DEFAULT_CORPUS = ORACLE / "corpus.jsonl"
DEFAULT_TRACES = ORACLE / "traces"
DEFAULT_VIN    = REPO / "en-US" / "tom" / "tom.vin"
DEFAULT_VDB    = REPO / "en-US" / "tom" / "tom8.vdb"   # NEVER tom16.vdb
DEFAULT_VCF    = REPO / "en-US" / "tom" / "tom.vcf"
DEFAULT_HPC    = REPO / "spfy" / "data" / "tom_hpclass.bin"
DEFAULT_VOC    = REPO / "spfy" / "build" / "fe_symbol_table.json"
DEFAULT_TA     = REPO / "spfy" / "data" / "fe_tables_a"
DEFAULT_TB     = REPO / "spfy" / "data" / "fe_tables"

ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
PATH_LINE_RE = re.compile(r"^\s*hp\s+(\d+):\s*uid=(\d+)")

SLOT_CATS = ("ctx", "ctx_center", "ctx_neighbor", "sp",
             "durt_both_diff", "durt_synth_only", "durt_eng_only",
             "f0tr_both_diff", "f0tr_eng_only",   # f0tr_synth_only is benign
             "pool_n", "pool_uid0")

# ---------------------------------------------------------------------------
# Engine ground truth
# ---------------------------------------------------------------------------

def load_prsl_slot(path: Path):
    """Read utt-0 prsl_slot events: slot_idx -> {ctx, pool_uids, pool_n}."""
    if not path.exists():
        return {}
    by_slot = {}
    seen_zero = False
    in_utt0 = True
    with open(path) as f:
        for ln in f:
            if not ln.strip():
                continue
            d = json.loads(ln)
            if d.get("type") != "prsl_slot":
                continue
            if d["slot"] == 0:
                if seen_zero:
                    in_utt0 = False
                seen_zero = True
            if not in_utt0:
                continue
            s = d["slot"]
            if s not in by_slot:
                by_slot[s] = {
                    "ctx": d.get("ctx"),
                    "pool_uids": list(d.get("uids", [])),
                    "pool_n": d.get("n_cands", 0),
                }
    return by_slot


def load_inner_scorer(path: Path, by_slot: dict):
    if not path.exists():
        return
    seen_zero = False
    in_utt0 = True
    with open(path) as f:
        for ln in f:
            if not ln.strip():
                continue
            d = json.loads(ln)
            if d.get("type") != "inner_scorer":
                continue
            if d["slot"] == 0:
                if seen_zero:
                    in_utt0 = False
                seen_zero = True
            if not in_utt0:
                continue
            s = d["slot"]
            if s in by_slot and "sp_target" not in by_slot[s]:
                by_slot[s]["sp_target"] = d.get("sp_target")


def load_cart_walks(path: Path, by_slot: dict):
    """Load engine CART walks (durt + f0tr) per slot.

    The engine fires inner_scorer in TWO complete sweeps over all slots
    per phrase, with different sp_target per sweep (see
    project_f0tr_two_sweeps_2026_05_13_evening.md). The captured walks
    contain BOTH sweeps. We default to sweep-1 walks (the engine's
    pre-PSA scoring pass) -- that's what our spfy_synth's pass-1 sp
    matches and the correct comparison target. Set
    SPFY_CART_WALKS_MIX_SWEEPS=1 to revert to the legacy mixed-sweep
    behavior (loads the first walk per (slot, tree), which for
    slots where the engine skipped a tree in sweep 1 picks up sweep-2
    -- inflates f0tr_both_diff with audit artifacts).
    """
    if not path.exists():
        return
    sweep1_only = os.environ.get("SPFY_CART_WALKS_MIX_SWEEPS") != "1"
    prev_slot = -1
    seen_slot0 = False
    in_sweep1 = True
    with open(path) as f:
        for ln in f:
            if not ln.strip():
                continue
            d = json.loads(ln)
            if d.get("type") != "cart_walk":
                continue
            s = d["slot"]
            # Detect sweep boundary: slot resets to 0 after we've already
            # seen slot 0 (the engine's second full sweep starts here).
            if s == 0 and prev_slot != 0:
                if seen_slot0:
                    in_sweep1 = False
                seen_slot0 = True
            prev_slot = s
            if sweep1_only and not in_sweep1:
                continue
            if s not in by_slot:
                continue
            tree = d.get("tree")
            key = "durt" if tree == "durt" else "f0tr"
            if key not in by_slot[s]:
                by_slot[s][key] = (d.get("leaf_mean", 0.0),
                                   d.get("leaf_var", 0.0))


def engine_path_uids(viterbi_dp_path: Path):
    """Walk predec backward from last-slot argmin. Expand anchor cands
    (uid != join_key) into a dense range(uid, join_key+1) so the result
    is a flat list of HP-position UIDs - same convention as
    spfy_synth's `hp N: uid=X` dump (spfy_synth.c:2502-2516)."""
    if not viterbi_dp_path.exists():
        return []
    with open(viterbi_dp_path) as f:
        events = [json.loads(l) for l in f if l.strip()]
    leave = next((e for e in events if e.get("stage") == "leave"), None)
    if not leave:
        return []
    slots = leave["slots"]
    ptr_map = {}
    for s in slots:
        for ci, c in enumerate(s.get("cands") or []):
            ptr_map[c["cand_ptr"]] = (s["slot"], ci)
    last = None
    for i in range(len(slots) - 1, -1, -1):
        if slots[i].get("cands"):
            last = i; break
    if last is None:
        return []
    cands = slots[last]["cands"]
    best = min(range(len(cands)), key=lambda c: cands[c].get("dp_20", 1e30))
    path = []  # newest-first, reverse at end
    cur_slot, cur_idx = last, best
    visited = set()
    for _ in range(len(slots) + 1):
        key = (cur_slot, cur_idx)
        if key in visited:
            break
        visited.add(key)
        cd = slots[cur_slot]["cands"][cur_idx]
        uid = cd["uid"]
        jk  = cd.get("join_key", uid)
        if jk == uid:
            path.append([uid])
        else:
            path.append(list(range(uid, jk + 1)))
        predec = cd.get("predec", 0)
        if predec == 0 or predec not in ptr_map:
            break
        cur_slot, cur_idx = ptr_map[predec]
    path.reverse()
    out = []
    for span in path:
        out.extend(span)
    return out


def load_engine(tid: str, traces: Path):
    by_slot = load_prsl_slot(traces / "prsl_slot" / f"{tid}.jsonl")
    load_inner_scorer(traces / "inner_scorer" / f"{tid}.jsonl", by_slot)
    load_cart_walks(traces / "cart_walks" / f"{tid}.jsonl", by_slot)
    path = engine_path_uids(traces / "viterbi_dp" / f"{tid}.jsonl")
    return {"slots": by_slot, "path_uids": path}

# ---------------------------------------------------------------------------
# Synth runner (worker process)
# ---------------------------------------------------------------------------

@dataclass
class SynthResult:
    tid: str
    ok: bool
    slots: list = field(default_factory=list)     # per-HP-slot dicts
    path_uids: list = field(default_factory=list) # HP-expanded UIDs
    phrase_n_hp: list = field(default_factory=list)  # per-phrase HP count (multi-phrase)
    err: str = ""
    elapsed_s: float = 0.0


def run_one(args):
    """Worker: run spfy_synth once and parse stdout/stderr.
    Args is a tuple to keep multiprocessing serialization simple."""
    (tid, text, exe, vin, vdb, vcf, hpc, voc, ta, tb, tmpdir) = args
    out_wav = os.path.join(tmpdir, f"_master_{os.getpid()}_{tid}.wav")
    env = os.environ.copy()
    env["SPFY_SYNTH_DEBUG"]       = "1"
    env["SPFY_DUMP_PATH"]         = "1"
    env["SPFY_FIRST_PHRASE_ONLY"] = "1"
    # Suppress hosted-FE phrase-merge: audits need utt boundaries preserved.
    env.pop("SPFY_FE_HOST_PHRASE_MERGE", None)
    t0 = time.time()
    try:
        r = subprocess.run(
            [exe, vin, vdb, vcf, hpc, voc, ta, tb, text, out_wav],
            capture_output=True, text=True, env=env, timeout=120)
    except subprocess.TimeoutExpired:
        return SynthResult(tid=tid, ok=False, err="timeout",
                           elapsed_s=time.time() - t0)
    except Exception as e:
        return SynthResult(tid=tid, ok=False, err=f"spawn: {e}",
                           elapsed_s=time.time() - t0)
    elapsed = time.time() - t0
    slots = []
    path_uids = []
    for raw in (r.stdout + "\n" + r.stderr).splitlines():
        ln = ANSI_RE.sub("", raw).strip()
        if ln.startswith("{") and ln.endswith("}"):
            try:
                d = json.loads(ln)
            except Exception:
                continue
            if "hp" in d and "ctx" in d:
                slots.append(d)
            continue
        m = PATH_LINE_RE.match(raw)
        if m:
            path_uids.append(int(m.group(2)))
    try:
        os.unlink(out_wav)
    except OSError:
        pass
    if r.returncode != 0 and not slots:
        return SynthResult(tid=tid, ok=False,
                           err=f"rc={r.returncode}: {r.stderr[:200]}",
                           elapsed_s=elapsed)
    return SynthResult(tid=tid, ok=True, slots=slots,
                       path_uids=path_uids, elapsed_s=elapsed)

# ---------------------------------------------------------------------------
# Comparison
# ---------------------------------------------------------------------------

def fclose(a, b, eps=0.005):
    return abs(a - b) <= eps * max(1.0, abs(b))


def diff_slot(synth, eng):
    """Return list of category strings where this slot diverges. Silence
    pad slots (ctx[2] in {64,65}) skip the CART comparison since the
    engine doesn't run carts on them.

    ctx is broken into 3 categories so per-field fixes are visible:
    - ctx        = whole-array mismatch (any field differs)
    - ctx_center = ctx[2] (the slot's own phone-id) differs
    - ctx_neighbor = ctx[0,1,3,4] differ but ctx[2] matches
                     (cascade from a neighboring slot's phoneme being wrong)
    """
    cats = []
    sctx = synth["ctx"]
    ectx = eng.get("ctx")
    if sctx != ectx:
        cats.append("ctx")
        if ectx and len(sctx) >= 3 and len(ectx) >= 3:
            if sctx[2] != ectx[2]:
                cats.append("ctx_center")
            else:
                cats.append("ctx_neighbor")
    if synth["sp"] != eng.get("sp_target"):
        cats.append("sp")
    silence = (synth["ctx"][2] in (64, 65)
               or (eng.get("ctx") or [0,0,0])[2] in (64, 65))
    if not silence:
        synth_has_durt = synth["durt_mean"] != 0 or synth["durt_var"] != 0
        ed = eng.get("durt")
        if synth_has_durt and ed is None:
            cats.append("durt_synth_only")
        elif ed is not None and not synth_has_durt:
            cats.append("durt_eng_only")
        elif ed is not None and synth_has_durt:
            m, v = ed
            if not (fclose(synth["durt_mean"], m) and fclose(synth["durt_var"], v)):
                cats.append("durt_both_diff")
        synth_has_f0tr = synth["f0tr_mean"] != 0 or synth["f0tr_var"] != 0
        ef = eng.get("f0tr")
        # f0tr_synth_only is benign (engine doesn't emit f0tr for unvoiced
        # phones; we still compute one harmlessly). Don't flag it.
        if ef is not None and not synth_has_f0tr:
            cats.append("f0tr_eng_only")
        elif ef is not None and synth_has_f0tr:
            m, v = ef
            if not (fclose(synth["f0tr_mean"], m) and fclose(synth["f0tr_var"], v)):
                cats.append("f0tr_both_diff")
    if synth.get("pool_n", 0) != eng.get("pool_n", 0):
        cats.append("pool_n")
    else:
        ep = eng.get("pool_uids") or []
        sp = synth.get("cands") or []
        if ep and sp and ep[0] != sp[0]:
            cats.append("pool_uid0")
    return cats


@dataclass
class PhraseReport:
    tid: str
    text: str
    n_hp_synth: int
    n_hp_eng: int
    structure_match: bool                   # n_hp_synth == n_hp_eng
    # Per-slot fidelity (only meaningful when structure_match)
    slot_total: int = 0
    slot_match: int = 0
    cat_counts: dict = field(default_factory=dict)
    # Path UID match (only meaningful when structure_match) — POSITIONAL
    uid_total: int = 0
    uid_match: int = 0
    # Path UID match — LCS (longest common subsequence). Equals positional
    # when engine and synth path lengths match; for DIFF_PL phrases (engine
    # took anchors we didn't, or vice versa) LCS reflects engine-unit-set
    # faithfulness without being confused by anchor-span misalignment.
    uid_lcs_match: int = 0
    uid_lcs_denom: int = 0    # = max(len(eng_path), len(ours_path))
    # Always-computed positional fidelity (legacy compat with RESUME)
    pos_total: int = 0
    pos_match: int = 0
    # First divergence diagnostic
    first_div_slot: int = -1
    first_div_cats: list = field(default_factory=list)
    err: str = ""


def compare_phrase(tid, text, synth_res, eng):
    rep = PhraseReport(tid=tid, text=text,
                       n_hp_synth=len(synth_res.slots),
                       n_hp_eng=len(eng["slots"]),
                       structure_match=False)
    if not synth_res.ok:
        rep.err = synth_res.err
        return rep
    rep.structure_match = (rep.n_hp_synth == rep.n_hp_eng
                           and rep.n_hp_synth > 0)

    # Positional comparison (legacy RESUME-compat number, all phrases)
    aligned = min(rep.n_hp_synth, rep.n_hp_eng)
    for i in range(aligned):
        if i not in eng["slots"]:
            continue
        rep.pos_total += 1
        if not diff_slot(synth_res.slots[i], eng["slots"][i]):
            rep.pos_match += 1

    # Strict per-slot fidelity (structure-matching phrases only)
    if rep.structure_match:
        for i in range(rep.n_hp_synth):
            if i not in eng["slots"]:
                continue
            rep.slot_total += 1
            d = diff_slot(synth_res.slots[i], eng["slots"][i])
            if not d:
                rep.slot_match += 1
            else:
                if rep.first_div_slot < 0:
                    rep.first_div_slot = i
                    rep.first_div_cats = d
                for c in d:
                    rep.cat_counts[c] = rep.cat_counts.get(c, 0) + 1

        # Path UID match (only meaningful when structure matches AND we
        # have engine viterbi_dp trace). Two metrics:
        #   - positional: ep[i] == ours[i] at every common HP index.
        #     Strict but penalises anchor-span misalignment for DIFF_PL.
        #   - LCS: longest common subsequence of engine vs ours UIDs.
        #     Reflects "did we pick the same recordings as engine" even
        #     when anchor spans differ (e.g. engine takes a 6-HP anchor
        #     covering HPs 10-15, we take a 4-HP anchor covering HPs
        #     12-15 — same 4 UIDs end up in both paths, just at different
        #     positions; positional says 0/6, LCS says 4/6).
        eng_path = eng["path_uids"]
        # Optional engine-coverage mask: positions where engine has no
        # real DP path (e.g. truncated master capture missing the last
        # utterance) are False. We skip them in BOTH positional and LCS
        # counts so the audit measures real divergences, not fabricated
        # ones. See 2026-05-14 nat_035 trace-completeness finding.
        path_mask = eng.get("path_mask") or [True] * len(eng_path)
        eng_utt_n_hp = eng.get("utt_n_hp") or []
        syn_phrase_n_uid = getattr(synth_res, "phrase_n_uid", None) or []
        if eng_path and synth_res.path_uids:
            # Per-utt alignment when both sides have per-utt counts.
            # Required because partial-anchor wins on the synth side can
            # change synth's per-utt UID count (the global path_uids list
            # then has different per-utt offsets from engine's). Engine
            # slices path_uids using utt_n_hp; synth uses phrase_n_uid
            # (NOT phrase_n_hp — overshoot HPs from partial anchors don't
            # emit a path UID). See 2026-05-14 edge_042 alignment artifact.
            use_per_utt = (len(eng_utt_n_hp) == len(syn_phrase_n_uid)
                           and len(eng_utt_n_hp) > 0)
            from difflib import SequenceMatcher
            if use_per_utt:
                eng_off = 0
                syn_off = 0
                tot = mat = lcs_mat = lcs_den = 0
                for uidx in range(len(eng_utt_n_hp)):
                    en = eng_utt_n_hp[uidx]
                    sn = syn_phrase_n_uid[uidx]
                    eng_utt = eng_path[eng_off:eng_off + en]
                    syn_utt = synth_res.path_uids[syn_off:syn_off + sn]
                    eng_off += en
                    syn_off += sn
                    # Skip utts that engine didn't DP (path_mask all
                    # False or eng entries are -1 sentinels).
                    if not eng_utt or all(u == -1 for u in eng_utt):
                        continue
                    if not syn_utt:
                        continue
                    n = min(len(eng_utt), len(syn_utt))
                    tot += n
                    mat += sum(1 for i in range(n) if eng_utt[i] == syn_utt[i])
                    sm = SequenceMatcher(None, syn_utt, eng_utt,
                                         autojunk=False)
                    lcs_mat += sum(b.size for b in sm.get_matching_blocks())
                    lcs_den += max(len(eng_utt), len(syn_utt))
                rep.uid_total = tot
                rep.uid_match = mat
                rep.uid_lcs_match = lcs_mat
                rep.uid_lcs_denom = lcs_den
            else:
                # Fallback: legacy global-position compare with path_mask.
                n = min(len(eng_path), len(synth_res.path_uids), rep.n_hp_synth)
                covered = [i for i in range(n)
                           if i < len(path_mask) and path_mask[i]]
                rep.uid_total = len(covered)
                rep.uid_match = sum(1 for i in covered
                                    if eng_path[i] == synth_res.path_uids[i])
                eng_covered = [eng_path[i] for i in covered]
                ours_covered = [synth_res.path_uids[i] for i in covered]
                sm = SequenceMatcher(None, ours_covered, eng_covered,
                                     autojunk=False)
                rep.uid_lcs_match = sum(b.size
                                        for b in sm.get_matching_blocks())
                rep.uid_lcs_denom = max(len(eng_covered), len(ours_covered))
    return rep

# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--exe", default=os.environ.get("SPFY_SYNTH_EXE"),
                    help="path to spfy_synth.exe (or env SPFY_SYNTH_EXE)")
    ap.add_argument("--workers", type=int,
                    default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument("--corpus", default=str(DEFAULT_CORPUS))
    ap.add_argument("--traces", default=str(DEFAULT_TRACES))
    ap.add_argument("--vin", default=str(DEFAULT_VIN))
    ap.add_argument("--vdb", default=str(DEFAULT_VDB))
    ap.add_argument("--vcf", default=str(DEFAULT_VCF))
    ap.add_argument("--hpc", default=str(DEFAULT_HPC))
    ap.add_argument("--voc", default=str(DEFAULT_VOC))
    ap.add_argument("--ta",  default=str(DEFAULT_TA))
    ap.add_argument("--tb",  default=str(DEFAULT_TB))
    ap.add_argument("--filter", default=None,
                    help="regex on phrase id (e.g. '^text_0[0-9]+')")
    ap.add_argument("--modes", default="both",
                    choices=["slot", "uid", "both"])
    ap.add_argument("--show-diff", action="store_true")
    ap.add_argument("--json", default=None)
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--tmpdir", default=os.environ.get("TEMP", "c:/tmp"))
    args = ap.parse_args()

    if not args.exe:
        print("ERROR: --exe required (or set SPFY_SYNTH_EXE)", file=sys.stderr)
        sys.exit(2)
    exe = Path(args.exe)
    if not exe.exists():
        print(f"ERROR: exe not found: {exe}", file=sys.stderr)
        print("Build first: c:/tmp/build32.bat or c:/tmp/build64_handfe.bat",
              file=sys.stderr)
        sys.exit(2)
    # Sanity: confirm tom8.vdb (never tom16); the C-side guard catches it
    # too, but failing here means we don't waste minutes on parallel runs.
    if "tom16" in args.vdb.lower():
        print(f"ERROR: refuse to audit with 16k VDB: {args.vdb}", file=sys.stderr)
        sys.exit(2)

    # Print the audit header so users always know what they ran
    mtime = time.strftime("%Y-%m-%d %H:%M:%S",
                          time.localtime(exe.stat().st_mtime))
    print(f"# master_compare.py")
    print(f"# exe:     {exe} (built {mtime})")
    print(f"# corpus:  {args.corpus}")
    print(f"# traces:  {args.traces}")
    print(f"# vdb:     {args.vdb}")
    print(f"# workers: {args.workers}   modes: {args.modes}")
    print()

    # Load corpus
    items = []
    with open(args.corpus) as f:
        for ln in f:
            if not ln.strip():
                continue
            d = json.loads(ln)
            if d.get("mode") != "text":
                continue
            if args.filter and not re.search(args.filter, d["id"]):
                continue
            items.append(d)
    if not items:
        print("no phrases matched filter", file=sys.stderr)
        sys.exit(1)

    # Load engine ground truth in main process (small, fast, serial).
    traces = Path(args.traces)
    eng_by_tid = {}
    missing_traces = []
    for it in items:
        eng = load_engine(it["id"], traces)
        if not eng["slots"]:
            missing_traces.append(it["id"])
        eng_by_tid[it["id"]] = eng
    if missing_traces:
        print(f"# warning: {len(missing_traces)} phrases lack prsl_slot "
              f"traces (skipping): {missing_traces[:5]}{'...' if len(missing_traces)>5 else ''}")
        items = [it for it in items if it["id"] not in missing_traces]

    # Dispatch workers
    work = [
        (it["id"], it["text"], str(exe),
         args.vin, args.vdb, args.vcf, args.hpc, args.voc, args.ta, args.tb,
         args.tmpdir)
        for it in items
    ]
    text_by_tid = {it["id"]: it["text"] for it in items}

    t0 = time.time()
    results_by_tid = {}
    with mp.Pool(args.workers) as pool:
        for i, res in enumerate(pool.imap_unordered(run_one, work), 1):
            results_by_tid[res.tid] = res
            if not args.quiet:
                tag = "OK" if res.ok else f"FAIL ({res.err[:40]})"
                print(f"[{i:3d}/{len(work)}] {res.tid:<10} "
                      f"{res.elapsed_s:5.1f}s  {tag}", file=sys.stderr)
    wall = time.time() - t0

    # Compare in deterministic id order
    reports = []
    for it in items:
        res = results_by_tid.get(it["id"])
        if res is None:
            continue
        reports.append(compare_phrase(it["id"], it["text"],
                                       res, eng_by_tid[it["id"]]))

    # ---- Per-phrase report ----
    if not args.quiet:
        print()
        print(f"{'tid':<10} {'n_hp':>9} {'struct':>6} "
              f"{'slot':>11} {'uid':>11} {'pos':>11}  cats")
        print("-" * 100)
        for r in reports:
            n_str = f"{r.n_hp_synth}/{r.n_hp_eng}"
            struct = "OK" if r.structure_match else "MISS"
            slot_s = (f"{r.slot_match}/{r.slot_total}"
                      if r.structure_match and args.modes in ("slot","both")
                      else "-")
            uid_s = (f"{r.uid_match}/{r.uid_total}"
                     if r.structure_match and r.uid_total
                        and args.modes in ("uid","both")
                     else "-")
            pos_s = f"{r.pos_match}/{r.pos_total}" if r.pos_total else "-"
            cats = ",".join(f"{c}={n}" for c, n in
                            sorted(r.cat_counts.items(), key=lambda x: -x[1]))
            print(f"{r.tid:<10} {n_str:>9} {struct:>6} "
                  f"{slot_s:>11} {uid_s:>11} {pos_s:>11}  {cats}")
            if args.show_diff and r.first_div_slot >= 0:
                # Best-effort first-divergence dump using the cached results
                res = results_by_tid[r.tid]
                eng = eng_by_tid[r.tid]
                i = r.first_div_slot
                ss = res.slots[i]; es = eng["slots"][i]
                print(f"  @slot {i}: cats={r.first_div_cats}")
                print(f"    synth: ctx={ss['ctx']} sp={ss['sp']} "
                      f"durt=({ss['durt_mean']:.4f},{ss['durt_var']:.4f}) "
                      f"f0tr=({ss['f0tr_mean']:.4f},{ss['f0tr_var']:.4f}) "
                      f"pool_n={ss['pool_n']}")
                print(f"    eng:   ctx={es.get('ctx')} sp={es.get('sp_target')} "
                      f"durt={es.get('durt')} f0tr={es.get('f0tr')} "
                      f"pool_n={es.get('pool_n')}")
        print("-" * 100)

    # ---- Aggregate report ----
    n_total      = len(reports)
    n_ok         = sum(1 for r in reports if not r.err)
    n_struct     = sum(1 for r in reports if r.structure_match)
    slot_total   = sum(r.slot_total for r in reports)
    slot_match   = sum(r.slot_match for r in reports)
    uid_total    = sum(r.uid_total  for r in reports)
    uid_match    = sum(r.uid_match  for r in reports)
    uid_lcs_match = sum(r.uid_lcs_match for r in reports)
    uid_lcs_denom = sum(r.uid_lcs_denom for r in reports)
    pos_total    = sum(r.pos_total  for r in reports)
    pos_match    = sum(r.pos_match  for r in reports)
    cat_total = {}
    for r in reports:
        for c, n in r.cat_counts.items():
            cat_total[c] = cat_total.get(c, 0) + n

    def pct(a, b):
        return (100.0 * a / b) if b else 0.0

    print()
    print("=" * 60)
    print(f"PHRASES:        {n_ok}/{n_total} ran clean   "
          f"({n_total - n_ok} failed)")
    print(f"STRUCTURE:      {n_struct}/{n_total} matched n_hp "
          f"({pct(n_struct, n_total):.1f}%)")
    if args.modes in ("slot", "both"):
        print(f"SLOT FIDELITY:  {slot_match}/{slot_total} "
              f"({pct(slot_match, slot_total):.1f}%) "
              f"[strict: structure-matching phrases only]")
        print(f"  positional:   {pos_match}/{pos_total} "
              f"({pct(pos_match, pos_total):.1f}%) "
              f"[legacy: includes structure-mismatched phrases]")
    if args.modes in ("uid", "both") and uid_total:
        print(f"PATH UID:       {uid_match}/{uid_total} "
              f"({pct(uid_match, uid_total):.1f}%) "
              f"[positional, structure-matching phrases only]")
        if uid_lcs_denom:
            print(f"  LCS:          {uid_lcs_match}/{uid_lcs_denom} "
                  f"({pct(uid_lcs_match, uid_lcs_denom):.1f}%) "
                  f"[max(eng_len, ours_len) denom; fair for DIFF_PL]")
    if cat_total:
        print(f"CATEGORIES:     " + "  ".join(
            f"{k}={v}" for k, v in
            sorted(cat_total.items(), key=lambda x: -x[1])))
    print(f"WALL:           {wall:.1f}s   ({wall/max(n_total,1):.2f}s/phrase "
          f"with {args.workers} workers)")
    print("=" * 60)

    # ---- JSON dump (for diffing runs) ----
    if args.json:
        out = {
            "exe": str(exe),
            "exe_mtime": mtime,
            "corpus": args.corpus,
            "n_phrases": n_total,
            "n_struct_match": n_struct,
            "slot_total": slot_total, "slot_match": slot_match,
            "uid_total":  uid_total,  "uid_match":  uid_match,
            "pos_total":  pos_total,  "pos_match":  pos_match,
            "categories": cat_total,
            "phrases": [asdict(r) for r in reports],
        }
        with open(args.json, "w") as f:
            json.dump(out, f, indent=2)
        print(f"# wrote {args.json}")


if __name__ == "__main__":
    # Windows multiprocessing requires the spawn guard.
    mp.freeze_support()
    main()
