"""master_compare2.py - audit using unified master JSONL traces.

Same audit semantics as master_compare.py, but loads engine ground
truth from a single unified JSONL per phrase
(spfy/test/oracle/traces_master/<tid>.jsonl, written by
run_frida_capture.py --hook master) instead of per-hook subdirs in
spfy/test/oracle/traces/.

The unified format is the canonical artifact going forward
(2026-05-13 evening). It carries cross-hook ordering via `master_seq`
and `sub_idx`, so sweep correlation is natural and per-event hook
origin is in-line. Per-hook `n` + `slot` is still the primary
sweep-detect signal (matching master_compare); master_seq is the
tiebreaker.

Reuses run_one + diff_slot + compare_phrase + the SynthResult /
PhraseReport dataclasses from master_compare. Only the engine loader
differs.

Defaults to sweep-1-only cart_walks (matches master_compare's new
default after the 2026-05-13 two-sweeps fix); set
SPFY_CART_WALKS_MIX_SWEEPS=1 to revert.

Usage:
    python spfy/test/oracle/master_compare2.py
    python spfy/test/oracle/master_compare2.py --filter '^text_(001|029)$'
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
from dataclasses import asdict
from pathlib import Path

# Reuse the heavy lifting -- run_one (subprocess driver), diff_slot
# (per-slot category compare), compare_phrase (rolls up per-phrase),
# defaults (paths to vin/vdb/vcf/etc).
sys.path.insert(0, str(Path(__file__).resolve().parent))
import master_compare as mc  # noqa: E402

THIS = Path(__file__).resolve()
REPO = THIS.parents[3]
ORACLE = THIS.parent
DEFAULT_TRACES_MASTER = ORACLE / "traces_master"


def load_engine_unified(tid: str, traces_master: Path,
                         multi_phrase: bool = False) -> dict:
    """Load engine ground truth from a unified master JSONL.

    Returns the same {slots, path_uids} shape as master_compare.load_engine.
    Sweep-1 default for cart_walks (env SPFY_CART_WALKS_MIX_SWEEPS=1
    reverts).

    When multi_phrase=True, namespace slots by utterance (utt_idx) using
    a global linear index: global_slot = utt_offset + local_slot, where
    utt_offset = sum of previous utts' n_hp. Path UIDs are concatenated
    across all viterbi_leaves. The synth side must also be running in
    multi-phrase mode and emit the same global ordering.
    """
    path = traces_master / f"{tid}.jsonl"
    if not path.exists():
        return {"slots": {}, "path_uids": []}

    with open(path) as f:
        events = [json.loads(ln) for ln in f if ln.strip()]

    sweep1_only = os.environ.get("SPFY_CART_WALKS_MIX_SWEEPS") != "1"
    by_slot: dict = {}

    # Determine per-utt slot ranges by scanning prsl_slot events. Each
    # utt resets slot to 0; track utt_idx and the max local slot seen
    # per utt to compute per-utt n_hp.
    utt_n_hp: list[int] = []
    cur_utt = -1
    cur_max = -1
    for e in events:
        if e.get("type") != "prsl_slot":
            continue
        s = e.get("slot")
        if s == 0:
            # Boundary: new utt (or first one)
            if cur_utt >= 0:
                utt_n_hp.append(cur_max + 1)
            cur_utt += 1
            cur_max = 0
        elif s is not None and s > cur_max:
            cur_max = s
    if cur_utt >= 0:
        utt_n_hp.append(cur_max + 1)

    def _utt_offset(uidx: int) -> int:
        return sum(utt_n_hp[:uidx]) if multi_phrase else 0

    # prsl_slot loop: emit per-utt (or utt-0 only) with global slot key
    cur_utt = -1
    seen_zero = False
    for e in events:
        if e.get("type") != "prsl_slot":
            continue
        s = e.get("slot")
        if s == 0:
            if seen_zero:
                cur_utt += 1
            else:
                seen_zero = True
                cur_utt = 0
        if not multi_phrase and cur_utt > 0:
            continue
        if s is None:
            continue
        gs = _utt_offset(cur_utt) + s
        if gs not in by_slot:
            by_slot[gs] = {
                "ctx": e.get("ctx"),
                "pool_uids": list(e.get("uids", [])),
                "pool_n": e.get("n_cands", 0),
            }

    # inner_scorer: first sp_target per (utt, slot)
    cur_utt = -1
    seen_zero = False
    for e in events:
        if e.get("type") != "inner_scorer":
            continue
        s = e.get("slot")
        if s == 0:
            if seen_zero:
                cur_utt += 1
            else:
                seen_zero = True
                cur_utt = 0
        if not multi_phrase and cur_utt > 0:
            continue
        if s is None:
            continue
        gs = _utt_offset(cur_utt) + s
        if gs in by_slot and "sp_target" not in by_slot[gs]:
            by_slot[gs]["sp_target"] = e.get("sp_target")

    # cart_walks: per-utt split by slot=0 boundary. Within each utt the
    # first walk per (slot, tree) is the slot-init walk (sweep 1); later
    # walks are anchor/sweep-2 and should be ignored.
    prev_slot = -1
    cur_utt = -1
    seen_zero = False
    in_sweep1 = True
    for e in events:
        if e.get("type") != "cart_walk":
            continue
        s = e.get("slot")
        if s == 0 and prev_slot != 0:
            if seen_zero:
                if multi_phrase:
                    cur_utt += 1
                    in_sweep1 = True   # reset per utt
                else:
                    in_sweep1 = False
            else:
                seen_zero = True
                cur_utt = 0
                in_sweep1 = True
        prev_slot = s
        if sweep1_only and not in_sweep1:
            continue
        if not multi_phrase and cur_utt > 0:
            continue
        if s is None:
            continue
        gs = _utt_offset(cur_utt) + s
        if gs not in by_slot:
            continue
        tree = e.get("tree")
        key = "durt" if tree == "durt" else "f0tr"
        if key not in by_slot[gs]:
            by_slot[gs][key] = (e.get("leaf_mean", 0.0),
                                e.get("leaf_var", 0.0))

    # viterbi_dp leave: walk predec backward from last-slot argmin to
    # produce engine's chosen path UID list (anchor cands expanded into
    # uid..join_key range). Same convention as
    # master_compare.engine_path_uids.
    #
    # GOTCHA (2026-05-14): engine fires viterbi DP MULTIPLE TIMES per
    # phrase (3 calls observed on nat_036, with slot counts 99/108/77
    # and resulting path lengths 74/78/54). Picking "first leave" gives
    # one of the preliminary DP runs (cand pruning or sub-utterance),
    # NOT the final chosen path. The FINAL path is the one whose HP-
    # expanded length matches n_hp_eng (= len(by_slot)) — that's the
    # full-corpus DP over the actual HP layout.
    #
    # Fix: compute the path for every viterbi_leave event; pick the one
    # whose path length equals n_hp_eng. Fall back to longest path if no
    # leaf matches exactly (DIFF_PL phrases). Picking the first call
    # gave Path UID = 95.4% on this corpus; picking by length gives
    # 96.4%.
    def _compute_path(leave: dict) -> list:
        slots = leave.get("slots", [])
        ptr_map: dict = {}
        for sl in slots:
            for ci, c in enumerate(sl.get("cands") or []):
                ptr_map[c["cand_ptr"]] = (sl["slot"], ci)
        last = None
        for i in range(len(slots) - 1, -1, -1):
            if slots[i].get("cands"):
                last = i
                break
        if last is None:
            return []
        cands = slots[last]["cands"]
        best = min(range(len(cands)),
                   key=lambda c: cands[c].get("dp_20", 1e30))
        cur_slot, cur_idx = last, best
        visited: set = set()
        stack: list = []
        for _ in range(len(slots) + 1):
            key = (cur_slot, cur_idx)
            if key in visited:
                break
            visited.add(key)
            cd = slots[cur_slot]["cands"][cur_idx]
            uid = cd["uid"]
            jk = cd.get("join_key", uid)
            if jk == uid:
                stack.append([uid])
            else:
                stack.append(list(range(uid, jk + 1)))
            predec = cd.get("predec", 0)
            if predec == 0 or predec not in ptr_map:
                break
            cur_slot, cur_idx = ptr_map[predec]
        stack.reverse()
        out: list = []
        for span in stack:
            out.extend(span)
        return out

    leaves = [e for e in events if e.get("type") == "viterbi_leave"]
    target_len = len(by_slot)
    path_uids: list[int] = []
    # Per-position mask: True iff engine ground truth covers this position.
    # Positions inside an utt that has no matching viterbi_leave are
    # marked False so the audit can exclude them from positional/LCS
    # counts (otherwise we compare synth UIDs against a fabricated
    # fallback path, which inflates the wrong-UID count — see
    # 2026-05-14 nat_035 trace-completeness finding).
    path_mask: list[bool] = []
    incomplete_utts: list[int] = []
    if leaves:
        candidates = [(L, _compute_path(L)) for L in leaves]
        if multi_phrase:
            # Multi-phrase: engine fires ONE viterbi DP per utterance.
            # Concatenate per-utt paths whose lengths match each utt's
            # n_hp. We have utt_n_hp from the prsl scan above. Map each
            # candidate path to its target utt by length.
            paths_by_len: dict[int, list] = {}
            for L, p in candidates:
                paths_by_len.setdefault(len(p), []).append(p)
            for uidx, n in enumerate(utt_n_hp):
                bucket = paths_by_len.get(n, [])
                if bucket:
                    path_uids.extend(bucket[0])
                    path_mask.extend([True] * n)
                    bucket.pop(0)
                else:
                    # No engine DP for this utt — likely a truncated
                    # master capture. Pad with sentinel UIDs and a
                    # False mask so the audit skips these positions.
                    incomplete_utts.append(uidx)
                    path_uids.extend([-1] * n)
                    path_mask.extend([False] * n)
        elif os.environ.get("SPFY_VITERBI_FIRST_LEAVE") == "1":
            path_uids = candidates[0][1]
            path_mask = [True] * len(path_uids)
        else:
            exact = [p for _, p in candidates if len(p) == target_len]
            if exact:
                path_uids = exact[0]
            else:
                # Fallback: longest path (least truncation against synth)
                path_uids = max(candidates, key=lambda x: len(x[1]))[1]
            path_mask = [True] * len(path_uids)

    return {"slots": by_slot, "path_uids": path_uids,
            "utt_n_hp": utt_n_hp,
            "path_mask": path_mask,
            "incomplete_utts": incomplete_utts}


# ---------------------------------------------------------------------------
# Multi-phrase synth runner (no SPFY_FIRST_PHRASE_ONLY)
# ---------------------------------------------------------------------------

_FE_BOUNDARY_RE = re.compile(r"^FE produced \d+ halfphone slots for text:")
_PHRASE_BOUNDARY_RE = re.compile(
    r"^spfy_phrase_boundary: phrase_idx=(\d+) n_hp=(\d+)")


def run_one_multi(args):
    """Worker: run spfy_synth WITHOUT SPFY_FIRST_PHRASE_ONLY=1 and
    namespace slot/hp indices globally across phrases.

    Each `FE produced N halfphone slots` stdout line marks a new phrase.
    Slot JSON dicts (`{"hp":N,...}`) and `hp N: uid=X` lines within a
    phrase use local N; we translate to global = phrase_offset + N.
    """
    (tid, text, exe, vin, vdb, vcf, hpc, voc, ta, tb, tmpdir) = args
    out_wav = os.path.join(tmpdir, f"_master2multi_{os.getpid()}_{tid}.wav")
    env = os.environ.copy()
    env["SPFY_SYNTH_DEBUG"] = "1"
    env["SPFY_DUMP_PATH"]   = "1"
    env.pop("SPFY_FIRST_PHRASE_ONLY", None)
    env.pop("SPFY_FE_HOST_PHRASE_MERGE", None)
    t0 = time.time()
    try:
        # Merge stderr into stdout so interleaved order is preserved:
        # `FE produced ...` goes to stdout, slot JSON `{"hp":...,"ctx":...}`
        # goes to stderr. Without merging, we'd see all FE-boundary lines
        # before any JSON dump, breaking phrase attribution.
        r = subprocess.run(
            [exe, vin, vdb, vcf, hpc, voc, ta, tb, text, out_wav],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, env=env, timeout=300)
    except subprocess.TimeoutExpired:
        return mc.SynthResult(tid=tid, ok=False, err="timeout",
                              elapsed_s=time.time() - t0)
    except Exception as e:
        return mc.SynthResult(tid=tid, ok=False, err=f"spawn: {e}",
                              elapsed_s=time.time() - t0)
    elapsed = time.time() - t0

    slots: list[dict] = []
    path_uids: list[int] = []
    phrase_offset = 0
    phrase_idx = -1
    last_local_hp = -1
    phrase_n_hp: list[int] = []
    phrase_n_uid: list[int] = []
    last_uid_count = 0

    for raw in r.stdout.splitlines():
        ln = mc.ANSI_RE.sub("", raw).strip()
        # Use the stderr-only marker `spfy_phrase_boundary:` for phrase
        # detection. The stdout `FE produced` line is intentionally
        # ignored here: under pipe buffering, stdout chunks flush at
        # arbitrary points relative to stderr, breaking interleaving.
        # The dedicated stderr marker shares buffer with JSON slot
        # dumps (also stderr) so order is guaranteed.
        m_phr = _PHRASE_BOUNDARY_RE.match(ln)
        if m_phr:
            if phrase_idx >= 0:
                phrase_n_hp.append(last_local_hp + 1)
                phrase_offset += last_local_hp + 1
                phrase_n_uid.append(len(path_uids) - last_uid_count)
                last_uid_count = len(path_uids)
            phrase_idx += 1
            last_local_hp = -1
            continue
        # Slot JSON
        if ln.startswith("{") and ln.endswith("}"):
            try:
                d = json.loads(ln)
            except Exception:
                continue
            if "hp" in d and "ctx" in d:
                local_hp = int(d["hp"])
                d["hp"] = phrase_offset + local_hp
                d["__phrase"] = phrase_idx
                slots.append(d)
                if local_hp > last_local_hp:
                    last_local_hp = local_hp
            continue
        # `  hp N: uid=X` path lines
        m = mc.PATH_LINE_RE.match(raw)
        if m:
            local_hp = int(m.group(1))
            uid = int(m.group(2))
            path_uids.append(uid)
            if local_hp > last_local_hp:
                last_local_hp = local_hp

    # Close final phrase
    if phrase_idx >= 0:
        phrase_n_hp.append(last_local_hp + 1)
        phrase_n_uid.append(len(path_uids) - last_uid_count)

    try:
        os.unlink(out_wav)
    except OSError:
        pass
    if r.returncode != 0 and not slots:
        return mc.SynthResult(tid=tid, ok=False,
                              err=f"rc={r.returncode}: {r.stderr[:200]}",
                              elapsed_s=elapsed)
    res = mc.SynthResult(tid=tid, ok=True, slots=slots,
                          path_uids=path_uids,
                          phrase_n_hp=phrase_n_hp,
                          elapsed_s=elapsed)
    # Attach per-phrase path_uid counts so compare_phrase can slice
    # the synth path_uids per-utt (HP count != path_uid count when
    # partial anchors win and overshoot HPs don't emit UIDs).
    res.phrase_n_uid = phrase_n_uid
    return res


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--exe", default=os.environ.get("SPFY_SYNTH_EXE"))
    ap.add_argument("--workers", type=int,
                    default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument("--corpus", default=str(mc.DEFAULT_CORPUS))
    ap.add_argument("--traces-master", default=str(DEFAULT_TRACES_MASTER))
    ap.add_argument("--vin", default=str(mc.DEFAULT_VIN))
    ap.add_argument("--vdb", default=str(mc.DEFAULT_VDB))
    ap.add_argument("--vcf", default=str(mc.DEFAULT_VCF))
    ap.add_argument("--hpc", default=str(mc.DEFAULT_HPC))
    ap.add_argument("--voc", default=str(mc.DEFAULT_VOC))
    ap.add_argument("--ta",  default=str(mc.DEFAULT_TA))
    ap.add_argument("--tb",  default=str(mc.DEFAULT_TB))
    ap.add_argument("--filter", default=None)
    ap.add_argument("--modes", default="both",
                    choices=["slot", "uid", "both"])
    ap.add_argument("--show-diff", action="store_true")
    ap.add_argument("--json", default=None)
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--tmpdir", default=os.environ.get("TEMP", "c:/tmp"))
    ap.add_argument("--multi-phrase", action="store_true",
                    help="Audit ALL phrases of each corpus entry "
                         "(default audits phrase 0 only).")
    args = ap.parse_args()

    if not args.exe:
        print("ERROR: --exe required (or set SPFY_SYNTH_EXE)",
              file=sys.stderr)
        sys.exit(2)
    exe = Path(args.exe)
    if not exe.exists():
        print(f"ERROR: exe not found: {exe}", file=sys.stderr)
        sys.exit(2)
    if "tom16" in args.vdb.lower():
        print(f"ERROR: refuse to audit with 16k VDB: {args.vdb}",
              file=sys.stderr)
        sys.exit(2)

    mtime = time.strftime("%Y-%m-%d %H:%M:%S",
                          time.localtime(exe.stat().st_mtime))
    print(f"# master_compare2.py (unified JSONL)")
    print(f"# exe:           {exe} (built {mtime})")
    print(f"# corpus:        {args.corpus}")
    print(f"# traces_master: {args.traces_master}")
    print(f"# vdb:           {args.vdb}")
    print(f"# workers:       {args.workers}   modes: {args.modes}")
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

    # Load engine ground truth from unified JSONL (main process, fast)
    traces_master = Path(args.traces_master)
    eng_by_tid = {}
    missing = []
    for it in items:
        eng = load_engine_unified(it["id"], traces_master,
                                   multi_phrase=args.multi_phrase)
        if not eng["slots"]:
            missing.append(it["id"])
        eng_by_tid[it["id"]] = eng
    if missing:
        print(f"# warning: {len(missing)} phrases lack unified traces "
              f"(skipping): {missing[:5]}{'...' if len(missing)>5 else ''}")
        items = [it for it in items if it["id"] not in missing]

    # Dispatch synth workers (same as master_compare)
    work = [
        (it["id"], it["text"], str(exe),
         args.vin, args.vdb, args.vcf, args.hpc, args.voc,
         args.ta, args.tb, args.tmpdir)
        for it in items
    ]

    t0 = time.time()
    results_by_tid = {}
    runner = run_one_multi if args.multi_phrase else mc.run_one
    with mp.Pool(args.workers) as pool:
        for i, res in enumerate(pool.imap_unordered(runner, work), 1):
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
        reports.append(mc.compare_phrase(it["id"], it["text"],
                                         res, eng_by_tid[it["id"]]))

    # Per-phrase rows (mirrors master_compare's format)
    if not args.quiet:
        print()
        print(f"{'tid':<10} {'n_hp':>9} {'struct':>6} "
              f"{'slot':>11} {'uid':>11} {'pos':>11}  cats")
        print("-" * 100)
        for r in reports:
            n_str = f"{r.n_hp_synth}/{r.n_hp_eng}"
            struct = "OK" if r.structure_match else "MISS"
            slot_s = (f"{r.slot_match}/{r.slot_total}"
                      if r.structure_match and args.modes in ("slot", "both")
                      else "-")
            uid_s = (f"{r.uid_match}/{r.uid_total}"
                     if r.structure_match and r.uid_total
                        and args.modes in ("uid", "both")
                     else "-")
            pos_s = (f"{r.pos_match}/{r.pos_total}"
                     if r.pos_total else "-")
            cats_s = ",".join(f"{k}={v}" for k, v in
                              sorted(r.cat_counts.items(),
                                     key=lambda x: -x[1])
                              ) if r.cat_counts else ""
            print(f"{r.tid:<10} {n_str:>9} {struct:>6} "
                  f"{slot_s:>11} {uid_s:>11} {pos_s:>11}  {cats_s}")
        print("-" * 100)

    # Aggregates
    n_total    = len(reports)
    n_ok       = sum(1 for r in reports if not r.err)
    n_struct   = sum(1 for r in reports if r.structure_match)
    slot_total = sum(r.slot_total for r in reports)
    slot_match = sum(r.slot_match for r in reports)
    uid_total  = sum(r.uid_total  for r in reports)
    uid_match  = sum(r.uid_match  for r in reports)
    uid_lcs_match = sum(r.uid_lcs_match for r in reports)
    uid_lcs_denom = sum(r.uid_lcs_denom for r in reports)
    pos_total  = sum(r.pos_total  for r in reports)
    pos_match  = sum(r.pos_match  for r in reports)
    cat_total: dict = {}
    for r in reports:
        for c, n in r.cat_counts.items():
            cat_total[c] = cat_total.get(c, 0) + n

    def pct(a: int, b: int) -> float:
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
    print(f"WALL:           {wall:.1f}s   "
          f"({wall/max(n_total,1):.2f}s/phrase with {args.workers} workers)")
    print("=" * 60)

    if args.json:
        out = {
            "exe": str(exe), "exe_mtime": mtime, "corpus": args.corpus,
            "traces_master": args.traces_master,
            "n_phrases": n_total, "n_struct_match": n_struct,
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
    mp.freeze_support()
    main()
