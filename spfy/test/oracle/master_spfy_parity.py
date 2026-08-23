"""master_spfy_parity.py - THE parity gate: selection AND audio, one run.

    Speechify output === spfy output, byte for byte, with no env vars set.

Reports four things, innermost to outermost:

    SLOT FIDELITY   do we build the same per-slot scoring inputs?
    PATH UID        do we SELECT the same units?
    EMITTED UNITS   do we hand SYNTHESIS the same units?   (from wsola_in)
    BYTE-IDENTICAL  do we produce the same SAMPLES?        <- the real goal

Each can pass while the next fails, and each has done so:

  - PATH UID read 100% while EMITTED UNITS failed on edge_042, because an
    anchor covers a UID span and selection != emission.
  - EMITTED UNITS tracked BYTE-IDENTICAL exactly once both were measured --
    which is why it is the line to read when asking "would this produce the
    engine's audio". `--g2p` and `prsl_slot` are pre-anchor views.

⚠ The audio stage renders SEPARATELY from the selection stage, and must.
The selection stage sets SPFY_SYNTH_DEBUG / SPFY_DUMP_PATH / _FIRST_PHRASE_ONLY
and inherits the caller's environment; the audio gate strips every SPFY_* so it
measures the SHIPPING configuration. Reusing one render for both would let a
diagnostic env var silently define the result. `--no-audio` skips it.

⚠ COVERAGE is printed every run. Percentages are over AUDITED entries only,
and this tool audits `mode: text` and `mode: spr`. A corpus entry in any other
mode has never been checked by it.

Engine ground truth is a unified JSONL per phrase
(spfy/test/oracle/traces_master/<tid>.jsonl, written by
run_frida_capture.py --hook master) instead of per-hook subdirs in
spfy/test/oracle/traces/.

Exit codes: 0 = parity, 1 = a mismatch, 2 = broken/missing answer key.

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

VOICES. `--voice NAME` audits any installed voice; the corpus follows the
voice's LANGUAGE (en-US -> corpus.jsonl, es-MX -> corpus_es_MX.jsonl,
fr-CA -> corpus_fr_CA.jsonl) and the traces follow the voice name
(traces_master_<voice>, except Tom's, which predate the convention and stayed
at traces_master). `--all-voices` walks every installed voice and prints a
summary table.

⚠ The selection stages need Frida master traces, which exist only for voices
that were captured. A voice without them is audited AUDIO-ONLY and the report
says so on its own line -- it never silently reports "0/0 matched" as success.

Usage:
    python spfy/test/oracle/master_spfy_parity.py
    python spfy/test/oracle/master_spfy_parity.py --voice felix
    python spfy/test/oracle/master_spfy_parity.py --all-voices
    python spfy/test/oracle/master_spfy_parity.py --filter '^text_(001|029)$'
"""
from __future__ import annotations

import argparse
import json
import multiprocessing as mp
import os
import re
import subprocess
import sys
import tempfile
import time
from dataclasses import asdict
from pathlib import Path

# Reuse the heavy lifting -- run_one (subprocess driver), diff_slot
# (per-slot category compare), compare_phrase (rolls up per-phrase),
# defaults (paths to vin/vdb/vcf/etc).
sys.path.insert(0, str(Path(__file__).resolve().parent))
import master_compare as mc  # noqa: E402
# The audio gate lives in audio_compare.py and is REUSED here rather than
# duplicated -- it stays runnable standalone for quick iteration on a filter.
import audio_compare as ac  # noqa: E402

THIS = Path(__file__).resolve()
REPO = THIS.parents[3]
ORACLE = THIS.parent
DEFAULT_TRACES_MASTER = ORACLE / "traces_master"

sys.path.insert(0, str(REPO / "bin"))
import server_ctl


def default_traces(vname: str) -> Path:
    """Tom's captures predate the per-voice naming and stayed put."""
    return (DEFAULT_TRACES_MASTER if vname == "tom"
            else ORACLE / f"traces_master_{vname}")


def installed_voices() -> list[str]:
    """Every voice with a complete 8 kHz vin/vdb/vcf, in language order."""
    out = []
    for lang in ac.LANG_DIRS:
        d = REPO / lang
        if not d.is_dir():
            continue
        for name in sorted(p.name for p in d.iterdir() if p.is_dir()):
            v = d / name
            if all((v / f).is_file() for f in
                   (f"{name}.vin", f"{name}8.vdb", f"{name}.vcf")):
                out.append(name)
    return out


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
    # expanded length matches n_hp_eng (= len(by_slot)) - that's the
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
    # fallback path, which inflates the wrong-UID count - see
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
                    # No engine DP for this utt - likely a truncated
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

    # ⭐ EMITTED units - the engine's post-selection ground truth.
    #
    # Everything above reconstructs what the engine SELECTED (prsl_slot +
    # viterbi). `wsola_in` records what it actually handed to synthesis, and
    # the two can differ: on edge_042 the engine selects 52 half-phones but
    # emits 48, because phrase 2 is realised by one 14-UID ANCHOR
    # (uid=98424 join_key=98437) covering the whole word from a single
    # recording. Our path matched the selection exactly, so the audit read
    # 52/52 while the audio gate failed outright.
    #
    # This list is what produces audio, so it is the view S4 work needs.
    emitted_uids: list[int] = []
    for e in events:
        if e.get("type") != "wsola_in":
            continue
        if not multi_phrase and e.get("sub_idx", 0) != 0:
            continue
        for u in (e.get("units") or []):
            if u.get("uid") is not None:
                emitted_uids.append(u["uid"])

    return {"slots": by_slot, "path_uids": path_uids,
            "utt_n_hp": utt_n_hp,
            "path_mask": path_mask,
            "emitted_uids": emitted_uids,
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
    (tid, text, exe, vin, vdb, vcf, tmpdir) = args
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
            [exe, vin, vdb, vcf, text, out_wav],
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
        # r.stderr is None when stderr was merged into stdout via
        # stderr=subprocess.STDOUT (the case here). Fall back to the
        # last lines of stdout for context.
        tail = "\n".join(r.stdout.splitlines()[-10:]) if r.stdout else ""
        if os.environ.get("SPFY_AUDIT_DEBUG"):
            print(f"=== full stdout for failed {tid} ===\n{r.stdout}\n=== end ===")
        return mc.SynthResult(tid=tid, ok=False,
                              err=f"rc={r.returncode}: ...{tail[-200:]}",
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


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--exe", default=os.environ.get("SPFY_SYNTH_EXE"))
    ap.add_argument("--workers", type=int,
                    default=max(1, (os.cpu_count() or 2) - 2))
    # Voice selection. Everything below that used to default to Tom now
    # defaults to None and is filled in from the voice, so --voice felix
    # picks up fr-CA's corpus and felix's traces without four more flags.
    ap.add_argument("--voice", default="tom",
                    help="which voice to audit (default tom)")
    ap.add_argument("--all-voices", action="store_true",
                    help="audit every installed voice in turn and print a "
                         "summary table. Voices with no captured traces are "
                         "run audio-only and SAID so.")
    ap.add_argument("--audio-only", action="store_true",
                    help="skip the selection stages (they need Frida master "
                         "traces) and measure BYTE-IDENTICAL alone")
    ap.add_argument("--corpus", default=None)
    ap.add_argument("--traces-master", default=None)
    ap.add_argument("--no-audio", action="store_true",
                    help="Skip the BYTE-IDENTICAL stage. The selection "
                         "metrics alone cannot tell you whether the audio "
                         "matches -- PATH UID read 100%% while the audio gate "
                         "failed. Use only when the engine is unavailable.")
    ap.add_argument("--allow-missing-traces", action="store_true",
                    help="Audit the phrases that DO have traces and only warn "
                         "about the rest. Default is to fail: a missing trace "
                         "is a broken answer key, and skipping it silently "
                         "inflates the percentage.")
    ap.add_argument("--vin", default=None)
    ap.add_argument("--vdb", default=None)
    ap.add_argument("--vcf", default=None)
    ap.add_argument("--filter", default=None)
    ap.add_argument("--modes", default="both",
                    choices=["slot", "uid", "both"])
    ap.add_argument("--show-diff", action="store_true")
    ap.add_argument("--json", default=None)
    ap.add_argument("--quiet", action="store_true")
    # Platform-agnostic scratch dir: gettempdir() honours TMPDIR (macOS /
    # Linux) and TEMP/TMP (Windows), falling back to the right OS default.
    # It used to read TEMP with a literal "c:/tmp" fallback, which silently
    # handed every worker an unwritable path on macOS/Linux -- the synth
    # loaded the voice, then failed writing its WAV and exited 1.
    ap.add_argument("--tmpdir", default=tempfile.gettempdir())
    # Multi-phrase comparison is the DEFAULT: the synth runner always
    # synthesizes every phrase (it pops SPFY_FIRST_PHRASE_ONLY), so the
    # engine-side comparison must align all phrases too. Single-phrase mode
    # clips to utt 0 and, on multi-phrase inputs (e.g. edge_042 "Apples,
    # oranges, etc." -> 3 comma-split phrases), mis-pairs the synth's utt-0
    # slots against a length-fallback engine path from a later utt -> a
    # false mismatch. Use --single-phrase to restore the legacy utt-0-only
    # behaviour.
    ap.add_argument("--multi-phrase", dest="multi_phrase",
                    action="store_true", default=True,
                    help="Audit ALL phrases of each corpus entry (default).")
    ap.add_argument("--single-phrase", dest="multi_phrase",
                    action="store_false",
                    help="Legacy: audit phrase 0 only (mis-handles "
                         "multi-phrase inputs).")
    args = ap.parse_args()

    if not args.exe:
        print("ERROR: --exe required (or set SPFY_SYNTH_EXE)",
              file=sys.stderr)
        return 2
    exe = Path(args.exe)
    if not exe.exists():
        print(f"ERROR: exe not found: {exe}", file=sys.stderr)
        return 2

    explicit = (args.corpus or args.traces_master or args.vin or args.vdb
                or args.vcf)
    if args.all_voices and explicit:
        print("ERROR: --all-voices overrides per-voice paths, so it cannot be "
              "combined with --corpus/--traces-master/--vin/--vdb/--vcf",
              file=sys.stderr)
        return 2

    voices = installed_voices() if args.all_voices else [args.voice]
    if not voices:
        print("ERROR: no installed voices found", file=sys.stderr)
        return 2

    mtime = time.strftime("%Y-%m-%d %H:%M:%S",
                          time.localtime(exe.stat().st_mtime))

    # Rendering a reference repoints the server, which rewrites
    # config/SWIttsConfig.xml. Put it back: leaving the config on whichever
    # voice happened to be last is a trap for the next tool that assumes it
    # is still serving Tom.
    orig_voice, _orig_lang = server_ctl.read_config()

    summaries = []
    rc = 0
    try:
        for vname in voices:
            s = audit_voice(args, vname, exe, mtime)
            summaries.append(s)
            rc = max(rc, s["rc"])
    finally:
        now_voice, _ = server_ctl.read_config()
        if orig_voice and now_voice != orig_voice:
            print(f"\n# restoring server config to voice={orig_voice}")
            if server_ctl.port_open():
                server_ctl.use(orig_voice)
            else:
                server_ctl.write_config(
                    orig_voice, server_ctl.find_voice_language(orig_voice))

    if len(voices) > 1:
        print()
        print("=" * 78)
        print("PER-VOICE SUMMARY")
        print(f"{'voice':<10} {'lang':<7} {'n':>4} {'slot':>8} {'uid':>8} "
              f"{'emit':>8} {'byte-identical':>16}")
        print("-" * 78)
        for s in summaries:
            print(f"{s['voice']:<10} {s['lang']:<7} {s['n']:>4} "
                  f"{s['slot']:>8} {s['uid']:>8} {s['emit']:>8} "
                  f"{s['audio']:>16}")
        print("=" * 78)
        noaudit = [s for s in summaries if s["selection_skipped"]]
        if noaudit:
            print("SELECTION NOT AUDITED (no master traces captured): "
                  + ", ".join(s["voice"] for s in noaudit))
            print("  Capture with capture_voice.ps1 <voice> <corpus> "
                  "<out_traces> <out_master>.")
    return rc


def audit_voice(args, vname: str, exe: Path, mtime: str) -> dict:
    """One voice, end to end. Returns a summary row; never raises for a
    mere mismatch -- `rc` carries the verdict (0 ok, 1 mismatch, 2 broken
    answer key)."""
    vp = ac.resolve_voice(vname)
    corpus = args.corpus or str(vp["corpus"])
    traces = args.traces_master or str(default_traces(vname))
    vin = args.vin or str(vp["vin"])
    vdb = args.vdb or str(vp["vdb"])
    vcf = args.vcf or str(vp["vcf"])

    # NEVER a 16 kHz VDB, for any voice. Tom's tom16.vdb is a local upsample
    # with nothing above 4 kHz; the rule generalises because any 16 kHz VDB
    # would be measuring a different thing than the shipped 8 kHz voice.
    if re.search(r"16\.vdb$", vdb.lower()):
        print(f"ERROR: refuse to audit with a 16k VDB: {vdb}", file=sys.stderr)
        return {"voice": vname, "lang": vp["lang"], "n": 0, "slot": "-",
                "uid": "-", "emit": "-", "audio": "REFUSED", "rc": 2,
                "selection_skipped": True}

    # A voice with no captured traces has no selection answer key. That is a
    # coverage fact, not a failure -- but it is never inferred silently.
    selection_skipped = args.audio_only or not Path(traces).is_dir()

    print()
    print("#" + "=" * 70)
    print(f"# VOICE:         {vname} ({vp['lang']})")
    print("# master_spfy_parity.py (unified JSONL)")
    print(f"# exe:           {exe} (built {mtime})")
    print(f"# corpus:        {corpus}")
    print(f"# traces_master: {traces}"
          f"{'   [ABSENT -- selection not audited]' if selection_skipped and not args.audio_only else ''}")
    print(f"# vdb:           {vdb}")
    print(f"# workers:       {args.workers}   modes: {args.modes}")
    print()

    # Load corpus
    # Modes this tool audits. `spr` entries carry the inline `\![...]` form in
    # their text and are handed to the synth exactly like `text` ones -- the
    # same way audio_compare.py renders them. They were excluded for as long
    # as they had no traces, which is how `spr_002` stayed unaudited while
    # failing the audio gate outright.
    AUDITED_MODES = {"text", "spr"}
    items = []
    excluded_mode = []          # in the corpus, NOT audited by this tool
    with open(corpus, encoding="utf-8") as f:
        for ln in f:
            if not ln.strip():
                continue
            d = json.loads(ln)
            if args.filter and not re.search(args.filter, d["id"]):
                continue
            if d.get("mode") not in AUDITED_MODES:
                excluded_mode.append((d["id"], d.get("mode")))
                continue
            items.append(d)
    if not items:
        print("no phrases matched filter", file=sys.stderr)
        return {"voice": vname, "lang": vp["lang"], "n": 0, "slot": "-",
                "uid": "-", "emit": "-", "audio": "NO PHRASES", "rc": 1,
                "selection_skipped": selection_skipped}
    n_corpus = len(items) + len(excluded_mode)

    # Load engine ground truth from unified JSONL (main process, fast)
    eng_by_tid = {}
    if not selection_skipped:
        traces_master = Path(traces)
        missing = []
        for it in items:
            eng = load_engine_unified(it["id"], traces_master,
                                      multi_phrase=args.multi_phrase)
            if not eng["slots"]:
                missing.append(it["id"])
            eng_by_tid[it["id"]] = eng
        if missing:
            # ⚠ A text-mode entry with no trace is a BROKEN ANSWER KEY, not a
            # phrase to drop. Silently skipping it lets the audit report 100%
            # over a denominator nobody is watching -- which is exactly how
            # `spr_002` went unaudited while failing the audio gate. Fail loud.
            print(f"# ERROR: {len(missing)} text-mode phrases have NO unified "
                  f"trace, so there is no answer key for them:")
            for tid in missing:
                print(f"#   {tid}")
            print("# Capture them with run_frida_capture.py --hook master, or "
                  "pass --allow-missing-traces to audit the rest anyway.")
            if not args.allow_missing_traces:
                return {"voice": vname, "lang": vp["lang"], "n": len(items),
                        "slot": "-", "uid": "-", "emit": "-",
                        "audio": "NO KEY", "rc": 2,
                        "selection_skipped": selection_skipped}
            items = [it for it in items if it["id"] not in missing]
    else:
        why = ("--audio-only" if args.audio_only
               else f"no traces at {traces}")
        print(f"# SELECTION STAGES SKIPPED ({why}). Only BYTE-IDENTICAL is "
              f"measured below.")
        print()

    # Dispatch synth workers (same as master_compare)
    work = [
        (it["id"], it["text"], str(exe),
         vin, vdb, vcf, args.tmpdir)
        for it in items
    ]

    t0 = time.time()
    results_by_tid = {}
    reports = []
    emit_ok, emit_bad = 0, []
    wall = 0.0
    if not selection_skipped:
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
        for it in items:
            res = results_by_tid.get(it["id"])
            if res is None:
                continue
            reports.append(mc.compare_phrase(it["id"], it["text"],
                                             res, eng_by_tid[it["id"]]))

        # ⭐ EMITTED-UNIT check: do we hand SYNTHESIS the same units the engine
        # did? Distinct from PATH UID, which only asks whether we SELECTED the
        # same ones -- and the two diverge exactly where an anchor covers a span
        # (edge_042). This is the check that tracks the audio gate.
        for it in items:
            res = results_by_tid.get(it["id"])
            if res is None or not res.ok:
                continue
            eng_emit = eng_by_tid[it["id"]].get("emitted_uids") or []
            if not eng_emit:
                continue                  # no wsola_in in this trace
            ours = [u for u in res.path_uids if u not in (-1, 0xFFFFFFFF)]
            if ours == eng_emit:
                emit_ok += 1
            else:
                emit_bad.append((it["id"], len(eng_emit), len(ours)))

    # Per-phrase rows (mirrors master_compare's format)
    if not args.quiet and not selection_skipped:
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
    if selection_skipped:
        print("SLOT/UID/EMITTED: NOT MEASURED -- "
              + ("--audio-only" if args.audio_only
                 else f"no master traces at {traces}"))
    else:
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
            print("CATEGORIES:     " + "  ".join(
                f"{k}={v}" for k, v in
                sorted(cat_total.items(), key=lambda x: -x[1])))
        print(f"WALL:           {wall:.1f}s   "
              f"({wall/max(n_total,1):.2f}s/phrase with {args.workers} "
              f"workers)")
    # ⚠ COVERAGE, stated every run. Every percentage above is over the
    # AUDITED entries only. This tool audits `mode: "text"` and nothing else,
    # so a corpus entry in another mode has never been checked by it -- and
    # `spr_002` sat in that gap while failing the audio gate outright. Read
    # the percentages together with this line or they mean less than they look.
    if emit_ok or emit_bad:
        tot = emit_ok + len(emit_bad)
        print(f"EMITTED UNITS:  {emit_ok}/{tot} "
              f"({pct(emit_ok, tot):.1f}%) [what the engine actually handed "
              f"to synthesis, from wsola_in]")
        for tid, n_eng, n_our in emit_bad:
            print(f"  DIFFERS:      {tid} -- engine emits {n_eng}, we emit "
                  f"{n_our}")
    # ⭐ THE GOAL. Rendered SEPARATELY from the selection stage above, with
    # every SPFY_* stripped, so this measures the SHIPPING configuration --
    # the selection stage runs with SPFY_SYNTH_DEBUG et al. set and inherits
    # the caller's environment, and reusing its renders here would let a
    # diagnostic env var define the answer.
    audio_ident = audio_total = 0
    audio_bad: list = []
    if not args.no_audio:
        pairs = [(it["id"], it["text"]) for it in items]
        # The engine reference is TRACKED in the repo, so this stage runs
        # without the proprietary engine installed. Ours still renders fresh
        # every time, in a cleaned environment - it is the thing under test.
        eng_dir = ac.engine_ref_dir(vname)
        our_dir = ac.SCRATCH / "ours" / vname
        ac.render_engine(pairs, eng_dir, vname)
        ac.render_ours(pairs, our_dir, exe, args.workers, vp)
        for tid, _t in pairs:
            ea = ac.read_wav(eng_dir / f"{tid}.wav")
            oa = ac.read_wav(our_dir / f"{tid}.wav")
            if ea is None or oa is None:
                audio_bad.append((tid, "missing render"))
                audio_total += 1
                continue
            audio_total += 1
            if len(ea) == len(oa) and bool((ea == oa).all()):
                audio_ident += 1
            else:
                audio_bad.append(
                    (tid, f"engine {len(ea)} smp, ours {len(oa)} smp"))
        print(f"BYTE-IDENTICAL: {audio_ident}/{audio_total} "
              f"({pct(audio_ident, audio_total):.1f}%)  <- THE GOAL "
              f"[clean env, samples compared]")
        # Say where the reference came from. "235/235 against a committed
        # reference that could not be re-verified" is a weaker claim than
        # "235/235 against audio the engine produced just now", and printing
        # the number without the provenance would blur the two.
        print(f"  REFERENCE:    {ac.ENGINE_PROVENANCE}")
        # Guard on the PROVENANCE, not on audio_total: a missing reference
        # still counts every pair (as "missing render"), so audio_total is
        # 235 either way and would never have caught this.
        if ac.ENGINE_PROVENANCE.startswith("UNAVAILABLE"):
            print("  >>> NO REFERENCE AUDIO. Install the engine, or restore "
                  "spfy/test/oracle/engine_ref/ - this stage proved nothing.")
        for tid, why in audio_bad[:12]:
            print(f"  DIFFERS:      {tid} -- {why}")
        if len(audio_bad) > 12:
            print(f"  ... and {len(audio_bad) - 12} more")
    else:
        print("BYTE-IDENTICAL: SKIPPED (--no-audio) -- the selection metrics "
              "above do NOT imply the audio matches")

    print(f"COVERAGE:       {len(items)}/{n_corpus} corpus entries audited")
    if excluded_mode:
        by_mode = {}
        for tid, m in excluded_mode:
            by_mode.setdefault(m, []).append(tid)
        for m, ids in sorted(by_mode.items()):
            print(f"  NOT AUDITED:  {len(ids)} entries with mode={m!r} "
                  f"-- {', '.join(ids[:6])}"
                  f"{'...' if len(ids) > 6 else ''}")
        print("  Not checked by ANY stage of this tool, audio included.")
    print("=" * 60)

    if args.json:
        out = {
            "exe": str(exe), "exe_mtime": mtime, "corpus": corpus,
            "traces_master": traces,
            "voice": vname, "lang": vp["lang"],
            "selection_skipped": selection_skipped,
            "n_phrases": n_total, "n_struct_match": n_struct,
            "slot_total": slot_total, "slot_match": slot_match,
            "uid_total":  uid_total,  "uid_match":  uid_match,
            "pos_total":  pos_total,  "pos_match":  pos_match,
            "categories": cat_total,
            "emit_ok": emit_ok, "emit_bad": [b[0] for b in emit_bad],
            "audio_ident": audio_ident, "audio_total": audio_total,
            "audio_bad": [b[0] for b in audio_bad],
            "coverage_audited": len(items), "coverage_corpus": n_corpus,
            "phrases": [asdict(r) for r in reports],
        }
        jpath = f"{args.json}.{vname}" if args.all_voices else args.json
        with open(jpath, "w") as f:
            json.dump(out, f, indent=2)
        print(f"# wrote {jpath}")

    # Gate semantics, inherited from audio_compare.py so folding it in did not
    # quietly discard them: 0 only when everything measured actually matched.
    # (A missing answer key already returned 2 further up.)
    ok = (n_ok == n_total
          and not emit_bad
          and (args.no_audio or audio_ident == audio_total))
    return {
        "voice": vname, "lang": vp["lang"], "n": len(items),
        "slot": ("-" if selection_skipped
                 else f"{slot_match}/{slot_total}"),
        "uid": ("-" if selection_skipped else f"{uid_match}/{uid_total}"),
        "emit": ("-" if selection_skipped
                 else f"{emit_ok}/{emit_ok + len(emit_bad)}"),
        "audio": ("SKIPPED" if args.no_audio
                  else f"{audio_ident}/{audio_total}"),
        "rc": 0 if ok else 1,
        "selection_skipped": selection_skipped,
    }


if __name__ == "__main__":
    mp.freeze_support()
    sys.exit(main())
