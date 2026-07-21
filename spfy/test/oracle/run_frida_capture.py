"""Drive a Frida hook over the oracle corpus and emit JSONL traces.

Attaches to the running Speechify.exe server, loads the named hook JS,
runs every corpus entry through bin/spfy_dumpwav.exe, drains the hook's
sample ring after each, and writes one JSONL file per hook + entry.

Output: spfy/test/oracle/traces/<hook>/<entry_id>.jsonl

Prerequisite: bin/Speechify.exe (the server) must be running. The host
should also have frida-tools installed:  pip install frida frida-tools

  python run_frida_capture.py --hook hash_lookup [--filter REGEX]
                              [--corpus PATH] [--out DIR]

Available hooks:
  hash_lookup    closes plan gap #1 (M3 blocker)
  prsl_lookup    once-per-utterance USelNetwork::BuildGraph capture (legacy)
  prsl_slot      per-slot PRSL preselection (M3.4c, captures context tuple
                 + candidate pool from FUN_08e91dc0 = USelNetwork::AddUnit)
  ulaw_lut       closes plan gap #7
  wsola_buffer   closes plan gap #3
  fe_token       supports FE track gap #10
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

try:
    import frida
except ImportError:
    frida = None

PROJECT_ROOT  = Path(__file__).resolve().parents[3]
DEFAULT_CORPUS = Path(__file__).resolve().with_name("corpus.jsonl")
DEFAULT_OUT    = Path(__file__).resolve().parent.parent / "oracle" / "traces"
DEFAULT_DUMPWAV = PROJECT_ROOT / "bin" / "spfy_dumpwav.exe"
HOOK_DIR = PROJECT_ROOT / "viz" / "frida_hooks"
TARGET = "Speechify.exe"

# SAFETY POLICY (2026-05-05, after 4 server kills):
# Only function-entry hooks. Mid-instruction hooks inside hot loops
# (especially under x87 FP-heavy code in SWIttsUSelUnitSelection) are
# unstable -- they accumulate FP/EFLAGS perturbation across thousands of
# trampoline trips per phrase. The kills today were all in
# SWIttsUSelUnitSelection's hot path.
#
# RETIRED hooks (do NOT re-add unless you have a fix for the mid-
# instruction hot-path instability):
#   hash_lookup_hook.js  -- cmp at 0x8E8B7E6 inside Viterbi inner loop.
#                           Already validated 1735/1735 probes; we have
#                           enough data, the C lookup is bit-exact.
#   cart_walks_hook.js   -- compare/dispatch fns called per-question
#                           inside the scoring loop. Already validated
#                           3366/3366 walks; C CART evaluator matches.
#   fe_stalker_calltree.js -- Stalker.follow over FE runtime is mid-
#                           instruction by Frida design (run_frida_capture.py
#                           policy violation). Removed per Phase 3 D-06
#                           plan-zero audit (.planning/phases/03-fe-
#                           convergence/03-PLAN-ZERO-AUDIT.{jsonl,md}).
#                           Discovery purpose fulfilled: the 12 hot FE RVAs
#                           it identified are now captured by
#                           fe_lts_hot_hook.js function-entry attaches.
#
# KEEP (function entries, low-frequency or per-utterance):
#   prsl_lookup, wsola_buffer (v2), fe_token (v2), ulaw_lut
HOOK_JS = {
    "prsl_lookup":  HOOK_DIR / "prsl_lookup_hook.js",
    "prsl_slot":    HOOK_DIR / "prsl_slot_hook.js",      # M3.4c per-slot preselect
    "inner_scorer": HOOK_DIR / "inner_scorer_hook.js",   # M3.4e per-slot SP target
    "inner_scorer_durt": HOOK_DIR / "inner_scorer_durt_hook.js",  # plan 02-05 D-17 Path R: preselect-time durt CART output
    "viterbi_consts": HOOK_DIR / "viterbi_consts_hook.js",  # M3.4o miss_offset etc
    "viterbi_dp":     HOOK_DIR / "viterbi_dp_hook.js",      # M3.4r per-utt DP entry/leave dump
    "fe_tree":        HOOK_DIR / "fe_tree_hook.js",         # M3.4r Phase B1 utterance tree dump
    "cart_walker_args": HOOK_DIR / "cart_walker_args_hook.js",  # M3.4r Phase B4.3 per-slot CART feature inputs
    # cart_walks: the original hash_lookup-class mid-loop hook is retired (kills the engine).
    # cart_walks_safe is a function-entry-only replacement that captures slot/tree/leaf_mean/leaf_var
    # — the only fields spfy_viterbi_replay.c::parse_cart_walk_line consumes. The "questions"
    # array stays empty in new captures; old captures (text_001..030, spr_001..002) keep theirs.
    # We register it under BOTH keys so callers can invoke either name; output goes to whichever
    # subdir the --hook key names. For 02-06 corpus-wide capture, use --hook cart_walks so files
    # land in spfy/test/oracle/traces/cart_walks/ alongside the 32 existing ones.
    "cart_walks":       HOOK_DIR / "cart_walks_safe_hook.js",
    "cart_walks_safe":  HOOK_DIR / "cart_walks_safe_hook.js",
    "cart_walks_surgical": HOOK_DIR / "cart_walks_surgical_hook.js",  # 2026-05-14 nat_036 residual: full call-stack-aware walk capture
    "anchor_slot_dump": HOOK_DIR / "anchor_slot_dump_hook.js",  # 2026-05-14 evening: per-anchor net workspace arrays
    "post_scoring":   HOOK_DIR / "post_scoring_hook.js",     # M3.4p before/after PostScoringAdj
    "anchor_score":   HOOK_DIR / "anchor_score_hook.js",     # M3.4r B4.4 anchor cost unknowns
    "anchor_cost4":   HOOK_DIR / "anchor_cost4_hook.js",     # B4.4 engine per-cand cost4 + survivor dump
    "anchor_components": HOOK_DIR / "anchor_components_hook.js",  # 2026-05-13 final-6.2%-UID-gap: per-cand FLAG/SP/D/F0 sync-point dump on FUN_08e8ce60
    "durt_walk":         HOOK_DIR / "durt_walk_hook.js",          # 2026-05-13 anchor-D-bug: per-call (is_first_half, leaf_mean, leaf_var) on FUN_08e87d90
    "fe_lts_access":  HOOK_DIR / "fe_lts_access_trace.js",   # LTS rule index access trace (MemoryAccessMonitor)
    "ccos_cell_probe": HOOK_DIR / "ccos_cell_probe_hook.js",  # M3.4r B4.4 4-cell ccos cell probe
    "ccos_apply":      HOOK_DIR / "ccos_apply_hook.js",       # plan 02-02 D-05 CCOS context-cost reduction
    "unit_hpclass_dump": HOOK_DIR / "unit_hpclass_dump_hook.js",  # M3.4r B4.4 per-uid mem+0x13 dump
    "ulaw_lut":     HOOK_DIR / "ulaw_lut_dump.js",
    "wsola_buffer": HOOK_DIR / "wsola_buffer_hook.js",   # v2 hardened 2026-05-05
    "fe_token":     HOOK_DIR / "fe_token_hook.js",       # v2 hardened 2026-05-05
    "accent_decision": HOOK_DIR / "accent_decision_hook.js",  # FUN_07e09337 entry/exit
    "fe_vtable_trace": HOOK_DIR / "fe_vtable_trace.js",      # 2026-05-10 DLL-hosting: SWIttsFe vtable + getObject + FUN_0836c420 delegate dispatch
    "fe_utt_hunt":     HOOK_DIR / "fe_utt_hunt_hook.js",     # 2026-05-11 DLL-hosting: locate utterance pointer in iobj->state for direct-IR read
    "fe_state_dump":   HOOK_DIR / "fe_state_dump_hook.js",   # 2026-05-11 DLL-hosting: dump iobj->state + ctrl right before slot42 drain (for diff vs hosted)
    "fe_addr_lookup":  HOOK_DIR / "fe_addr_lookup_hook.js",  # 2026-05-11 DLL-hosting: identify what module/function captured addresses live in
    "fe_callback_probe": HOOK_DIR / "fe_callback_probe_hook.js",  # 2026-05-11 DLL-hosting: probe args[1] vtable methods called by setPair_E/F/G
    "fe_ctrl_watch":     HOOK_DIR / "fe_ctrl_watch_hook.js",      # 2026-05-11 DLL-hosting: per-vtable-call ctrl-block before/after diff to find verbose-mode trigger
    "fe_setpair_probe":  HOOK_DIR / "fe_setpair_probe_hook.js",   # 2026-05-12 K2-B: probe engine-side setPair callbacks to find utt_ptr offset in user_arg
    "hp_prune_trace":    HOOK_DIR / "hp_prune_trace_hook.js",     # 2026-05-14 final-residual: trace FUN_08e88830 inputs/threshold/n_kept for prune-precision diagnosis
    "viterbi_c7c":       HOOK_DIR / "viterbi_c7c_hook.js",        # 2026-05-14 evening: per-cand c7c/c80 run-length state dump on FUN_08e8b620 leave (for 14-UID residual diag)
    "wsola_unit_probe":  HOOK_DIR / "wsola_unit_probe_hook.js",   # 2026-05-14: WsolaUnit + sub-unit + pmark struct dump at FUN_08EE2960 entry (PSOLA port scoping)
    "userdict_lookup":   HOOK_DIR / "userdict_lookup_hook.js",    # 2026-05-20: dump (dict, key, value) at UserDict::lookup. Used to extract engine's disambigDict contents byte-exact for fe_internal POS port.
    "probe_fe_module":   HOOK_DIR / "probe_fe_module.js",         # 2026-05-20: one-shot diag — list FE/engine module bases in target process.
    "fe_feedconfig":     HOOK_DIR / "fe_feedconfig_hook.js",      # 2026-07-20 ESPR: dump exact string engine feeds FE via feedConfigA (TTSFrontEnd::speak) — recovers the \!SWIespr1 header + real values + escaping.
}

# ---------------------------------------------------------------------------
# "master" mode: load multiple child hooks in a single Frida session so the
# 4 capture passes spfy_viterbi_replay needs (wsola_buffer + prsl_slot +
# cart_walks + inner_scorer) become 1 pass. Wall-clock cost goes from
# ~4 × 17 min to ~1 × 17 min on the 202-phrase tail. Each child file is
# untouched; we wrap its body in an IIFE that shadows Frida's `rpc` so the
# child's `rpc.exports = {...}` writes to a local object, then a master
# rpc aggregates flush/reset/stats across all children. Multiple
# `send({type:'ready'})` calls from children are idempotent on the python
# side. Per-event-type routing to per-hook output subdirs happens in
# write_master_jsonl() below.
# ---------------------------------------------------------------------------
MASTER_CHILDREN: list[Path] = [
    HOOK_DIR / "wsola_buffer_hook.js",
    HOOK_DIR / "prsl_slot_hook.js",
    HOOK_DIR / "cart_walks_safe_hook.js",
    HOOK_DIR / "inner_scorer_hook.js",
    HOOK_DIR / "viterbi_dp_hook.js",         # 2026-05-13 evening: master2 — per-utt DP entry/leave
    HOOK_DIR / "anchor_components_hook.js",  # 2026-05-13 evening: master2 — per-cand anchor cost sync points
]
HOOK_JS["master"] = MASTER_CHILDREN  # type: ignore[assignment]

# Map send() event type → output subdir under traces/. Both batch and
# inner-flattened types are listed because the python-side write_jsonl
# splits batches into per-line records; events arriving at the message
# handler still carry the batch type. ready events are silently dropped.
MASTER_TYPE_TO_HOOK: dict[str, str] = {
    "wsola_in_batch":           "wsola_buffer",
    "wsola_in":                 "wsola_buffer",
    "prsl_slot_batch":          "prsl_slot",
    "prsl_slot":                "prsl_slot",
    "cart_walk_batch":          "cart_walks",
    "cart_walk":                "cart_walks",
    "inner_scorer_batch":       "inner_scorer",
    "inner_scorer":             "inner_scorer",
    # inner_scorer-emitted side channels — co-located in inner_scorer/
    "flag_const":               "inner_scorer",
    "join_consts":              "inner_scorer",
    "hp_class_remap":           "inner_scorer",
    "s_ctx_remap":              "inner_scorer",
    "unit_layout_probe":        "inner_scorer",
    "cand_ctx_probe":           "inner_scorer",
    "voice_field_probe":        "inner_scorer",
    "hash_cell_probe":          "inner_scorer",
    "ccos_meta_probe":          "inner_scorer",
    "ccos_cell_probe":          "inner_scorer",
    "proscost_matrix_probe":    "inner_scorer",
    "comp_capture_enabled":     "inner_scorer",
    "comp_capture_failed":      "inner_scorer",
    # viterbi_dp emits per-utterance enter/leave events directly (no _batch)
    "viterbi_enter":            "viterbi_dp",
    "viterbi_leave":            "viterbi_dp",
    # anchor_components emits per-anchor-call frames directly (no _batch)
    "anchor_components":        "anchor_components",
    # userdict_lookup emits per-call (dict, key, value) batches
    "userdict_lookup_batch":    "userdict_lookup",
    "userdict_lookup":          "userdict_lookup",
}


def build_master_script(child_paths: list[Path]) -> str:
    """Concatenate child hooks into one script, each in an IIFE.

    Each IIFE shadows Frida's global `rpc` object with a local accessor
    so the child's `rpc.exports = {...}` populates a per-child exports
    object instead of the global. The master then aggregates them.

    Additionally we shadow Frida's global `send()` with a wrapper that
    stamps every outgoing payload with a monotonic `master_seq` (across
    ALL children) -- the global timeline used for cross-hook ordering
    and (downstream) for sweep correlation in master_compare2. The
    `type:'ready'` events bypass the stamp.
    """
    parts: list[str] = [
        "'use strict';\n",
        "var __MASTER_CHILDREN__ = [];\n",
        "var __MASTER_NAMES__ = [];\n",
        "/* Global cross-hook send sequence stamp. Per-child IIFEs\n"
        " * shadow `send` with a wrapper that increments this counter and\n"
        " * stamps every outgoing payload (`type:'ready'` excepted). The\n"
        " * stamp is the cross-hook timeline used by master_compare2 for\n"
        " * sweep correlation. We can't reassign Frida's global `send`\n"
        " * (read-only), so we shadow inside each IIFE. */\n"
        "var __MASTER_SEQ = 0;\n"
        "var __ORIG_SEND = send;\n"
        "function __master_send_wrap(payload) {\n"
        "    if (payload && typeof payload === 'object' &&\n"
        "        payload.type !== 'ready') {\n"
        "        payload.master_seq = ++__MASTER_SEQ;\n"
        "    }\n"
        "    return __ORIG_SEND.call(null, payload);\n"
        "}\n",
    ]
    for p in child_paths:
        body = p.read_text(encoding="utf-8")
        parts.append(
            "__MASTER_NAMES__.push(" + json.dumps(p.name) + ");\n"
            "__MASTER_CHILDREN__.push((function () {\n"
            "  var __child_exports = {};\n"
            "  /* Shadow Frida's `rpc`: child's `rpc.exports = {...}`\n"
            "   * writes to __child_exports instead of the global. */\n"
            "  var rpc = { get exports() { return __child_exports; },\n"
            "              set exports(v) { __child_exports = v; } };\n"
            "  /* Shadow Frida's `send` so child calls go through the\n"
            "   * master_seq stamper. Child's `send(...)` resolves to\n"
            "   * this local in the IIFE scope. */\n"
            "  var send = __master_send_wrap;\n"
            f"  /* ----- begin {p.name} ----- */\n"
            f"{body}\n"
            f"  /* ----- end {p.name} ----- */\n"
            "  return __child_exports;\n"
            "})());\n"
        )
    parts.append(
        "rpc.exports = {\n"
        "  stats: function () {\n"
        "    var out = {};\n"
        "    for (var i = 0; i < __MASTER_CHILDREN__.length; ++i) {\n"
        "      try { out[__MASTER_NAMES__[i]] = "
        "                __MASTER_CHILDREN__[i].stats(); }\n"
        "      catch (e) { out[__MASTER_NAMES__[i]] = 'err:' + e; }\n"
        "    }\n"
        "    return out;\n"
        "  },\n"
        "  flush: function () {\n"
        "    for (var i = 0; i < __MASTER_CHILDREN__.length; ++i) {\n"
        "      try { __MASTER_CHILDREN__[i].flush(); } catch (e) {}\n"
        "    }\n"
        "  },\n"
        "  reset: function () {\n"
        "    for (var i = 0; i < __MASTER_CHILDREN__.length; ++i) {\n"
        "      try { __MASTER_CHILDREN__[i].reset(); } catch (e) {}\n"
        "    }\n"
        "  }\n"
        "};\n"
        "send({ type: 'ready', hook: 'master',\n"
        "       children_count: __MASTER_CHILDREN__.length });\n"
    )
    return "".join(parts)


def write_master_jsonl(out_root: Path, entry_id: str,
                       events: list[dict]) -> dict[str, int]:
    """Route events by type to per-hook output files. Returns per-hook
    line counts. Unmapped events go to traces/_unmapped/."""
    bins: dict[str, list[dict]] = {}
    for ev in events:
        t = ev.get("type", "")
        if t == "ready":
            continue
        # Try the batch type first, then the flattened-inner type.
        hook = MASTER_TYPE_TO_HOOK.get(t)
        if hook is None and t.endswith("_batch"):
            hook = MASTER_TYPE_TO_HOOK.get(t[: -len("_batch")])
        if hook is None:
            hook = "_unmapped"
        bins.setdefault(hook, []).append(ev)
    counts: dict[str, int] = {}
    for hook, evs in bins.items():
        out_dir = out_root / hook
        out_dir.mkdir(parents=True, exist_ok=True)
        path = out_dir / f"{entry_id}.jsonl"
        counts[hook] = write_jsonl(path, evs)
    return counts


def write_unified_master_jsonl(out_path: Path,
                               events: list[dict]) -> int:
    """Write one unified JSONL per phrase, ordered by `master_seq`. Each
    line carries the original event payload PLUS a `hook_origin` field
    (derived via MASTER_TYPE_TO_HOOK) so master_compare2 can route
    events without re-deriving the source. Batched events are flattened
    into per-sample lines (each sample inherits the batch's master_seq
    plus a sub-index for ordering within the batch).

    The unified format is the canonical input for master_compare2.py;
    legacy per-hook JSONL still gets written by write_master_jsonl()
    in parallel for backward compatibility.
    """
    out_path.parent.mkdir(parents=True, exist_ok=True)
    flat: list[tuple[int, int, dict]] = []  # (master_seq, sub_idx, line)
    for ev in events:
        t = ev.get("type", "")
        if t == "ready":
            continue
        seq = ev.get("master_seq", 0)
        # Determine hook origin for this event type
        hook = MASTER_TYPE_TO_HOOK.get(t)
        if hook is None and t.endswith("_batch"):
            hook = MASTER_TYPE_TO_HOOK.get(t[: -len("_batch")])
        if hook is None:
            hook = "_unmapped"
        samples = ev.get("samples")
        if t.endswith("_batch") and isinstance(samples, list):
            inner_type = t[: -len("_batch")]
            for i, s in enumerate(samples):
                line = {"master_seq": seq, "sub_idx": i,
                        "hook_origin": hook, "type": inner_type, **s}
                flat.append((seq, i, line))
        else:
            line = {"hook_origin": hook, **ev}
            line["master_seq"] = seq  # ensure present even if None
            flat.append((seq, 0, line))
    flat.sort(key=lambda x: (x[0], x[1]))
    with out_path.open("w", encoding="utf-8") as fp:
        for _seq, _sub, line in flat:
            fp.write(json.dumps(line, separators=(",", ":")) + "\n")
    return len(flat)


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--hook", required=True, choices=sorted(HOOK_JS.keys()))
    ap.add_argument("--corpus",  type=Path, default=DEFAULT_CORPUS)
    ap.add_argument("--out",     type=Path, default=DEFAULT_OUT)
    ap.add_argument("--dumpwav", type=Path, default=DEFAULT_DUMPWAV)
    ap.add_argument("--filter",  default=None,
                    help="regex applied to entry id; only matching entries are run")
    ap.add_argument("--scratch", type=Path, default=Path("C:/tmp/spfy_frida_wavs"),
                    help="throwaway dir for synthesis output WAVs")
    ap.add_argument("--quiet-rpc", action="store_true",
                    help="suppress RPC failure warnings during reset/flush")
    ap.add_argument("--batch-synth", action="store_true",
                    help="keep ONE spfy_dumpwav.exe alive and feed it "
                         "utterances, instead of spawning one per phrase. "
                         "OFF by default: measured 2789 vs 2804 ms/entry, "
                         "i.e. no gain, because under Frida instrumentation "
                         "the hooked synthesis (~2.5 s) dominates and the "
                         "per-process teardown it saves was already fixed in "
                         "spfy_dumpwav itself. Batch is worth using for "
                         "UNinstrumented bulk synthesis (34 vs 92 ms).")
    ap.add_argument("--timing", action="store_true",
                    help="print per-entry reset/synth/drain timings")
    ap.add_argument("--settle", type=float, default=0.3,
                    help="seconds to wait for in-flight Frida send() messages "
                         "after each utterance (default: 0.3). Dominates "
                         "wall-clock once batch synthesis is enabled.")
    ap.add_argument("--master-unified-out", type=Path,
                    default=PROJECT_ROOT / "spfy" / "test" / "oracle" /
                            "traces_master",
                    help="(master mode) unified JSONL output dir, one file per "
                         "phrase ordered by master_seq. Written in parallel to "
                         "legacy per-hook subdirs. Default: traces_master/")
    ap.add_argument("--no-master-unified", action="store_true",
                    help="(master mode) skip the unified-JSONL output")
    return ap.parse_args()


def load_corpus(path: Path) -> list[dict]:
    entries: list[dict] = []
    with path.open("r", encoding="utf-8") as fp:
        for ln, line in enumerate(fp, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            try:
                entries.append(json.loads(line))
            except json.JSONDecodeError as exc:
                raise SystemExit(f"{path}:{ln}: {exc}") from exc
    return entries


class HookSession:
    """Streams events from a Frida hook via send() messages.

    Hooks are designed to push small batched messages rather than expose a
    big synchronous drain RPC -- the latter stalls under back-pressure when
    the buffer is megabytes.
    """

    def __init__(self, hook_name: str, quiet_rpc: bool = False) -> None:
        self.hook_name = hook_name
        self.quiet_rpc = quiet_rpc
        self.events: list[dict] = []
        self._lock = threading.Lock()
        self._ready = threading.Event()
        self.session = frida.attach(TARGET)
        spec = HOOK_JS[hook_name]
        if isinstance(spec, list):
            js = build_master_script(spec)
        else:
            js = spec.read_text(encoding="utf-8")
        self.script = self.session.create_script(js)
        self.script.on("message", self._on_message)
        self.script.load()
        if not self._ready.wait(timeout=5):
            raise SystemExit(f"{hook_name}: hook did not signal ready in 5s")

    def _on_message(self, message, _data) -> None:
        if message["type"] == "send":
            payload = message["payload"]
            if payload.get("type") == "ready":
                self._ready.set()
                return
            with self._lock:
                self.events.append(payload)
        elif message["type"] == "error":
            print(f"  [frida error] {message.get('stack', message)}",
                  file=sys.stderr)

    def reset_events(self) -> None:
        with self._lock:
            self.events = []
        try:
            self.script.exports_sync.reset()
        except Exception as exc:
            if not self.quiet_rpc:
                print(f"  [warn] reset RPC failed: {exc}", file=sys.stderr)

    def flush_and_collect(self, settle_s: float = 0.3) -> list[dict]:
        """Ask the hook to flush any pending batch, then drain in-flight
        send() messages from the channel."""
        try:
            self.script.exports_sync.flush()
        except Exception as exc:
            if not self.quiet_rpc:
                print(f"  [warn] flush RPC failed: {exc}", file=sys.stderr)
        time.sleep(settle_s)
        with self._lock:
            return list(self.events)

    def detach(self, timeout_s: float = 5.0) -> None:
        """Detach with a hard timeout so a stuck session can't hang the run."""
        done = threading.Event()
        def _do() -> None:
            try:
                self.script.unload()
            except Exception:
                pass
            try:
                self.session.detach()
            except Exception:
                pass
            done.set()
        t = threading.Thread(target=_do, daemon=True)
        t.start()
        if not done.wait(timeout=timeout_s):
            print(f"  [warn] frida detach timed out after {timeout_s}s; "
                  f"continuing anyway", file=sys.stderr)


def synth_one(dumpwav: Path, entry: dict, scratch: Path) -> tuple[bool, str]:
    scratch.mkdir(parents=True, exist_ok=True)
    out_wav = scratch / f"{entry['id']}.wav"
    if entry["mode"] == "text":
        argv = [str(dumpwav), entry["text"], str(out_wav)]
    elif entry["mode"] == "spr":
        argv = [str(dumpwav), "--pron", entry["text"], str(out_wav)]
    else:
        return False, f"unknown mode {entry['mode']!r}"
    try:
        cp = subprocess.run(argv, capture_output=True, text=True,
                            timeout=int(os.environ.get('SPFY_DUMPWAV_TIMEOUT','30')),
                            check=False)
    except subprocess.TimeoutExpired:
        return False, "timeout"
    if cp.returncode != 0:
        return False, f"rc={cp.returncode}: {cp.stderr[:200]!r}"
    return True, str(out_wav)


class BatchSynth:
    """Keep ONE spfy_dumpwav.exe alive and feed it utterances lock-step.

    Spawning a process per phrase costs ~4.7 s of SWIttsTerm/DLL-detach
    teardown for ~90 ms of actual synthesis (see reveng/SPEED_FIX.md,
    "Teardown, not IPC"). Batch mode amortizes that to ~34 ms per phrase.

    Lock-step matters for capture: we write one line, block until the
    client reports `DONE <id>`, and only then drain the Frida hook ring,
    so events cannot bleed across entries. Output is byte-identical to
    the per-process path -- verified by SHA-256 over the corpus.
    """

    def __init__(self, dumpwav: Path, scratch: Path) -> None:
        scratch.mkdir(parents=True, exist_ok=True)
        self.scratch = scratch
        self.proc = subprocess.Popen(
            [str(dumpwav), "--batch", str(scratch)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, encoding="utf-8",
            bufsize=1)

    def synth(self, entry: dict) -> tuple[bool, str]:
        if self.proc.poll() is not None:
            return False, f"batch client exited rc={self.proc.returncode}"

        if entry["mode"] == "text":
            text = entry["text"]
        elif entry["mode"] == "spr":
            # Same inline-SPR wrapper that --pron applies internally.
            text = "\\![" + entry["text"] + "]"
        else:
            return False, f"unknown mode {entry['mode']!r}"

        # A literal tab or newline would desync the line protocol.
        text = text.replace("\t", " ").replace("\r", " ").replace("\n", " ")

        try:
            self.proc.stdin.write(f"{entry['id']}\t{text}\n")
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError) as exc:
            return False, f"batch write failed: {exc}"

        want = f"DONE {entry['id']}"
        while True:
            line = self.proc.stdout.readline()
            if not line:
                return False, "batch client closed stdout"
            line = line.strip()
            if line == want:
                return True, str(self.scratch / f"{entry['id']}.wav")
            if line.startswith("DONE "):
                return False, f"out-of-order ack {line!r} (wanted {want!r})"

    def close(self) -> None:
        try:
            if self.proc.poll() is None:
                self.proc.stdin.close()
                self.proc.wait(timeout=30)
        except (OSError, subprocess.SubprocessError):
            try:
                self.proc.kill()
            except OSError:
                pass


def write_jsonl(path: Path, events: list[dict]) -> int:
    """Flatten any '<X>_batch' event with a 'samples' array into per-sample
    lines of type '<X>'; pass other events through verbatim. Returns number
    of lines written."""
    n = 0
    with path.open("w", encoding="utf-8") as fp:
        for ev in events:
            t = ev.get("type", "")
            samples = ev.get("samples")
            if t.endswith("_batch") and isinstance(samples, list):
                inner_type = t[: -len("_batch")]
                for s in samples:
                    fp.write(json.dumps({"type": inner_type, **s},
                                        separators=(",", ":")) + "\n")
                    n += 1
            else:
                fp.write(json.dumps(ev, separators=(",", ":")) + "\n")
                n += 1
    return n


def main() -> int:
    if frida is None:
        print("frida not installed: pip install frida frida-tools",
              file=sys.stderr)
        return 1
    args = parse_args()

    if not args.dumpwav.exists():
        print(f"oracle binary not found: {args.dumpwav}", file=sys.stderr)
        return 1

    entries = load_corpus(args.corpus)
    flt = re.compile(args.filter) if args.filter else None

    is_master = args.hook == "master"
    if is_master:
        # Master writes per-hook subdirs under args.out; no single hook
        # subdir of its own. Children land in their respective subdirs.
        out_root = args.out
        out_root.mkdir(parents=True, exist_ok=True)
    else:
        out_dir = args.out / args.hook
        out_dir.mkdir(parents=True, exist_ok=True)

    print(f"attaching frida to {TARGET} with hook '{args.hook}' ...")
    sess = HookSession(args.hook, quiet_rpc=args.quiet_rpc)

    batch = None
    if args.batch_synth:
        batch = BatchSynth(args.dumpwav, args.scratch)
        print("synth: batch mode (one client process, fresh port per phrase)")

    n_run = n_ok = 0
    try:
        for entry in entries:
            if flt and not flt.search(entry["id"]):
                continue
            n_run += 1
            _t0 = time.time()
            sess.reset_events()
            _t1 = time.time()
            if batch is not None:
                ok, msg = batch.synth(entry)
            else:
                ok, msg = synth_one(args.dumpwav, entry, args.scratch)
            _t2 = time.time()
            events = sess.flush_and_collect(settle_s=args.settle)
            _t3 = time.time()
            if args.timing:
                print(f"    [timing] reset={_t1-_t0:.3f}s "
                      f"synth={_t2-_t1:.3f}s drain={_t3-_t2:.3f}s "
                      f"total={_t3-_t0:.3f}s")
            status = "ok " if ok else "FAIL"
            tail = "" if ok else f"  ({msg})"
            if is_master:
                counts = write_master_jsonl(args.out, entry["id"], events)
                summary = " ".join(f"{h}={n}" for h, n in sorted(counts.items()))
                if not args.no_master_unified:
                    uni_path = args.master_unified_out / f"{entry['id']}.jsonl"
                    n_uni = write_unified_master_jsonl(uni_path, events)
                    summary += f" uni={n_uni}"
                print(f"  {status} {entry['id']:12s} {summary}{tail}")
            else:
                out_path = out_dir / f"{entry['id']}.jsonl"
                n_lines = write_jsonl(out_path, events)
                print(f"  {status} {entry['id']:12s} lines={n_lines:7d} "
                      f"-> {out_path.name}{tail}")
            if ok:
                n_ok += 1
    finally:
        if batch is not None:
            batch.close()
        sess.detach(timeout_s=5.0)

    if is_master:
        print(f"\n{n_ok}/{n_run} entries captured -> {args.out}/<hook>/")
    else:
        print(f"\n{n_ok}/{n_run} entries captured -> {out_dir}")
    return 0 if n_ok == n_run else 1


if __name__ == "__main__":
    sys.exit(main())
