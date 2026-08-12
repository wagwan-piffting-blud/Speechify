"""slot_diff.py -- everything known about ONE diverging phrase, side by side.

The parity gate says WHICH phrases differ and rolls the reasons into category
counts. That is the right shape for a gate and the wrong shape for a diagnosis:
by the time `sp=572` is printed, the question "which slot, in which word, and
by how much" has already been thrown away.

This prints, per half-phone slot, the engine's value beside ours for every
scoring input at once -- ctx, the five sp targets, the durt/f0tr CART leaves,
the candidate pool, and the chosen UID -- anchored to the word and phone the
slot belongs to, so a run of mismatches can be read against the text that
produced it.

    ENGINE ROWS COME FROM THE TRACE, ours from a fresh SPFY_SYNTH_DEBUG run.
    A slot the trace does not cover is printed as `?`, never as agreement.

⚠ `pool_n` here is the TRUE pool size on both sides, but the debug dump caps
its `cands` LIST at 16 unless SPFY_FULL_POOL_DUMP=1. Only `uid0` is read from
that list, so this tool is unaffected -- but any probe that compares pool
MEMBERSHIP must set that variable, or a 54-candidate pool reads as 16 and our
pick looks drawn from a smaller pool than the engine's. That artifact very
nearly became a finding ("we prune too aggressively"); it was the printer.

Usage:
    python slot_diff.py --voice felix --tid fr_012
    python slot_diff.py --voice felix --tid fr_012 --all      (identical rows too)
    python slot_diff.py --voice felix --worst 5               (pick by delta)
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import wave
from pathlib import Path

THIS = Path(__file__).resolve()
ORACLE = THIS.parent
REPO = THIS.parents[3]
sys.path.insert(0, str(ORACLE))
import master_spfy_parity as msp        # noqa: E402
import audio_compare as ac              # noqa: E402

DEFAULT_EXE = Path(r"C:\tmp\spfy_build32\src\cli\spfy_synth.exe")
WORD_RE = re.compile(r"<(\S+)\s+\(([^)]*)\)\s+([^\s,]*),(\d+)\s+\[(.*?)\]\s*>")
PHONE_RE = re.compile(r"(\.\d+(?:,[^\s]+)?)|([A-Za-z~@][A-Za-z0-9~@]*)\(p\d+\)")
SP_NAMES = ["sp0", "sp1", "sp2", "sp3", "sp4"]


def read_wav_n(p):
    try:
        with wave.open(str(p), "rb") as w:
            return w.getnframes()
    except (OSError, wave.Error):
        return None


def fe_phone_sequence(tagged: str):
    """[(word, syl_index_in_word, phone, stress, accent)] in utterance order,
    with a leading and trailing `pau` to match the slot layout."""
    seq = [("_pau_", 0, "pau", 0, "")]
    for wtext, _d, _pos, _st, body in WORD_RE.findall(tagged):
        syl = -1
        stress = 0
        accent = ""
        for m in PHONE_RE.finditer(body):
            if m.group(1):
                mark = m.group(1)
                syl += 1
                stress = int(mark[1])
                accent = mark[2:].lstrip(",") if "," in mark else ""
                continue
            if syl < 0:            # liaison: continues previous word's syllable
                syl = 0
            seq.append((wtext, syl, m.group(2), stress, accent))
    seq.append(("_pau_", 0, "pau", 0, ""))
    return seq


def run_ours(voice, text):
    d = REPO / voice["lang"] / voice["name"]
    env = {k: v for k, v in os.environ.items() if not k.startswith("SPFY_")}
    env["SPFY_SYNTH_DEBUG"] = "1"
    env["SPFY_DUMP_PATH"] = "1"
    r = subprocess.run(
        [str(voice["exe"]), str(d / f"{voice['name']}.vin"),
         str(d / f"{voice['name']}8.vdb"), str(d / f"{voice['name']}.vcf"),
         text, str(Path(os.environ.get("TEMP", "/tmp")) / "_slotdiff.wav")],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        errors="replace", env=env, timeout=600, check=False)
    slots, uids, tagged = {}, [], ""
    off, last = 0, -1
    for raw in r.stdout.splitlines():
        ln = raw.strip()
        if "tagged output" in ln:
            m = re.search(r"tagged output \(\d+ bytes\): (.*)", ln)
            if m:
                tagged = m.group(1)
            continue
        if ln.startswith("spfy_phrase_boundary:"):
            if last >= 0:
                off += last + 1
            last = -1
            continue
        if ln.startswith("{") and '"hp"' in ln:
            try:
                j = json.loads(ln)
            except ValueError:
                continue
            h = int(j["hp"])
            j["hp"] = off + h
            slots[off + h] = j
            last = max(last, h)
            continue
        m = re.match(r"^\s*hp\s+(\d+):\s*uid=(\d+)", raw)
        if m:
            uids.append(int(m.group(2)))
            last = max(last, int(m.group(1)))
    return slots, uids, tagged


def fmt(v):
    if v is None:
        return "--"
    if isinstance(v, float):
        # 6 significant digits, not 4: at 4 the CART leaf means 94.244 and
        # 94.238 both render "94.24", so a flagged row showed "94.24>94.24"
        # and read as a false positive in the tool rather than a real delta.
        return f"{v:.6g}"
    return str(v)


def cell(e, o, width):
    """engine>ours pair; the bare value when they agree. A missing side is
    '?', never blank -- an absent engine value must not read as agreement."""
    if e is None and o is None:
        return "?".rjust(width)
    if e == o:
        return fmt(e).rjust(width)
    return f"{fmt(e) if e is not None else '?'}>" \
           f"{fmt(o) if o is not None else '?'}".rjust(width)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--voice", default="felix")
    ap.add_argument("--tid", default=None)
    ap.add_argument("--worst", type=int, default=0,
                    help="instead of --tid, show the N phrases with the "
                         "largest |sample delta|")
    ap.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    ap.add_argument("--all", action="store_true",
                    help="print every slot, not just mismatching ones")
    ap.add_argument("--context", type=int, default=2,
                    help="slots of context around each mismatch (default 2)")
    args = ap.parse_args()

    vp = ac.resolve_voice(args.voice)
    vp["exe"] = args.exe
    traces = msp.default_traces(args.voice)
    ref = ac.engine_ref_dir(args.voice)
    corpus = {json.loads(l)["id"]: json.loads(l)["text"]
              for l in open(vp["corpus"], encoding="utf-8") if l.strip()}

    if args.tid:
        tids = [args.tid]
    else:
        scored = []
        for tid, text in corpus.items():
            e = read_wav_n(ref / f"{tid}.wav")
            if e is None:
                continue
            slots, uids, tagged = run_ours(vp, text)
            o = read_wav_n(Path(os.environ.get("TEMP", "/tmp")) / "_slotdiff.wav")
            if o is None or o == e:
                continue
            scored.append((abs(o - e), tid))
        scored.sort(reverse=True)
        tids = [t for _d, t in scored[:max(args.worst, 1)]]

    for tid in tids:
        text = corpus.get(tid, "")
        eng = msp.load_engine_unified(tid, traces, multi_phrase=True)
        slots, uids, tagged = run_ours(vp, text)
        seq = fe_phone_sequence(tagged)
        n_e = read_wav_n(ref / f"{tid}.wav")
        n_o = read_wav_n(Path(os.environ.get("TEMP", "/tmp")) / "_slotdiff.wav")

        print("=" * 118)
        print(f"{args.voice} / {tid}   {text!r}")
        d = (n_o - n_e) if (n_e and n_o) else 0
        print(f"audio: engine {n_e} smp   ours {n_o} smp   delta {d:+d} "
              f"({d / 8000.0:+.3f} s)")
        e_emit = eng.get("emitted_uids") or []
        print(f"units: engine emits {len(e_emit)}, we emit {len(uids)}   "
              f"identical sequence: {e_emit == uids}")
        print(f"FE:    {tagged[:400]}")
        print("-" * 118)
        hdr = (f"{'hp':>4} {'word':<12} {'syl':>3} {'ph':<5} {'sd':>2} | "
               + " ".join(n.rjust(9) for n in SP_NAMES)
               + f" | {'pool_n':>9} {'uid':>13} | {'durt':>13} {'f0tr':>13}")
        print(hdr)
        print(f"{'':4} {'':12} {'':3} {'':5} {'':2} | "
              + " ".join("eng>our".rjust(9) for _ in SP_NAMES)
              + f" | {'eng>our':>9} {'eng>our':>13} | "
                f"{'eng>our':>13} {'eng>our':>13}")
        print("-" * 118)

        n_hp = max(len(slots), max(eng["slots"].keys(), default=-1) + 1)
        rows = []
        for hp in range(n_hp):
            e = eng["slots"].get(hp) or {}
            o = slots.get(hp) or {}
            et = e.get("sp_target")
            ot = o.get("sp")
            sp_pairs = [(et[i] if et else None, ot[i] if ot else None)
                        for i in range(5)]
            e_pool = e.get("pool_n")
            o_pool = o.get("pool_n")
            e_uid = (e.get("pool_uids") or [None])[0]
            o_uid = (o.get("cands") or [None])[0]
            e_durt = None
            if e.get("durt"):
                e_durt = round(float(e["durt"][0]), 3)
            o_durt = round(float(o["durt_mean"]), 3) if "durt_mean" in o else None
            e_f0 = round(float(e["f0tr"][0]), 3) if e.get("f0tr") else None
            o_f0 = round(float(o["f0tr_mean"]), 3) if "f0tr_mean" in o else None
            # ctx is a CART INPUT, so a durt/f0tr leaf difference with matching
            # sp is only interpretable next to it. Compared per element so the
            # `why` column can name which position moved.
            e_ctx, o_ctx = e.get("ctx"), o.get("ctx")

            why = []
            for i, (a, b) in enumerate(sp_pairs):
                if a is not None and b is not None and a != b:
                    why.append(SP_NAMES[i])
            if e_ctx and o_ctx:
                for i in range(min(len(e_ctx), len(o_ctx))):
                    if e_ctx[i] != o_ctx[i]:
                        why.append(f"ctx[{i}]")
            if e_pool is not None and o_pool is not None and e_pool != o_pool:
                why.append("pool_n")
            if e_uid is not None and o_uid is not None and e_uid != o_uid:
                why.append("uid0")
            if e_durt is not None and o_durt is not None \
                    and abs(e_durt - o_durt) > 1e-3:
                why.append("durt")
            if e_f0 is not None and o_f0 is not None \
                    and abs(e_f0 - o_f0) > 1e-3:
                why.append("f0tr")
            diff = bool(why)
            pi, side = hp // 2, hp % 2
            w, sy, ph, _st, _acc = seq[pi] if pi < len(seq) else ("?", 0, "?", 0, "")
            rows.append((hp, w, sy, ph, side, sp_pairs, (e_pool, o_pool),
                         (e_uid, o_uid), (e_durt, o_durt), (e_f0, o_f0), diff,
                         why, (e_ctx, o_ctx)))

        show = set()
        for i, r in enumerate(rows):
            if r[-1]:
                for k in range(max(0, i - args.context),
                               min(len(rows), i + args.context + 1)):
                    show.add(k)
        gap = False
        for i, (hp, w, sy, ph, side, sp_pairs, pool, uid, durt, f0, diff,
                why, ctxs) in enumerate(rows):
            if not args.all and i not in show:
                gap = True
                continue
            if gap:
                print(f"{'':>4} ...")
                gap = False
            mark = "*" if diff else " "
            print(f"{hp:>4}{mark}{w[:11]:<11} {sy:>3} {ph[:4]:<5} {side:>2} | "
                  + " ".join(cell(a, b, 9) for a, b in sp_pairs)
                  + f" | {cell(pool[0], pool[1], 9)} {cell(uid[0], uid[1], 13)}"
                  + f" | {cell(durt[0], durt[1], 13)} "
                    f"{cell(f0[0], f0[1], 13)} | {','.join(why)}")
            if diff and ctxs[0] and ctxs[1] and ctxs[0] != ctxs[1]:
                print(f"{'':>4} {'ctx':<11} {'':>3} {'':<5} {'':>2} |   "
                      f"eng {ctxs[0]}  ours {ctxs[1]}")
        n_diff = sum(1 for r in rows if r[10])
        tally = {}
        for r in rows:
            for k in r[11]:
                tally[k] = tally.get(k, 0) + 1
        print("-" * 118)
        print(f"{n_diff}/{len(rows)} slots differ in at least one field. "
              f"'*' marks them; 'a>b' is engine>ours; '?' = side absent.")
        if tally:
            print("fields:  " + "  ".join(
                f"{k}={v}" for k, v in sorted(tally.items(),
                                              key=lambda x: -x[1])))
        print()


if __name__ == "__main__":
    sys.exit(main())
