"""AUDIO-level oracle: is spfy's output the same SAMPLES as Speechify's?

WHY THIS EXISTS
---------------
`master_spfy_parity.py` compares **path UIDs against captured traces**. It has
read 226/226 / 100% path UID for a long time, and it is not wrong -- a fresh
Frida capture confirms both that the engine still picks those units and that
we still reproduce them exactly.

But it never looks at a single audio sample. So an audio-path divergence is
invisible to it, and one was there: on `text_002` the chosen units sum to a
nominal 2.681 s, the engine emits 2.392 s and we emit 2.590 s. Same units,
different audio. The engine's overlap-add consumes ~75 samples of duration per
join; ours consumes ~23.

This is the gate that would have caught it, and the gate that says when the
re-implementation is actually done:

    Speechify output === spfy output, no env vars set, byte for byte.

HOW TO READ THE COLUMNS
-----------------------
    ident     byte-identical WAV payload. This is the only column that is
              really the goal.
    len       sample-count ratio ours/theirs. 1.000 without `ident` means the
              timing is right and the samples are not.
    ncc       best-shift NCC of ours against theirs. HIGH ncc with a length
              mismatch = same units, different join arithmetic. LOW ncc = a
              selection or decode difference, which would be far worse.
    div       first sample index where the two differ, or "-" if identical.

⚠ CONFOUND -- `spfy_dumpwav.exe` is a CLIENT of the running `Speechify.exe`
server, which loads a config XML that points at the voice folder. If that
server is not running, or points somewhere else, every row here is meaningless
rather than failing loudly. The header prints what it found; check it.

⚠ Run with NO SPFY_* variables set. They are stripped from the child
environment here, but a wrapper that re-adds them would invalidate the run.

    python audio_compare.py                    # whole corpus
    python audio_compare.py --filter "^text_00" --verbose
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import wave
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
CORPUS = HERE / "corpus.jsonl"
DUMPWAV = ROOT / "bin" / "spfy_dumpwav.exe"
TOM = ROOT / "en-US" / "tom"
DEFAULT_EXE = Path(r"C:\tmp\spfy_build32\src\cli\spfy_synth.exe")
SCRATCH = Path(r"C:\tmp\spfy_audio_cmp")
# The engine reference lives IN THE REPO and is tracked, so the audio gate --
# the project's headline claim -- can be run by someone who does not have the
# proprietary engine. 6.0 MB of 8 kHz mono wavs. See render_engine().
ENGINE_REF = HERE / "engine_ref"
# Set by render_engine(): how the reference was obtained on this run.
ENGINE_PROVENANCE = "not rendered"
# The encoding the batch payload is written in. Recorded in .engine_key so a
# change here invalidates references rendered under the old one.
STDIN_ENCODING = "utf-8"

LANG_DIRS = ("en-US", "es-MX", "fr-CA")
# One corpus per LANGUAGE, not per voice: two en-US voices are asked the same
# 235 questions, which is the whole point of a cross-voice gate. The foreign
# corpora are smaller (100 each) because they were written by hand.
LANG_CORPUS = {
    "en-US": HERE / "corpus.jsonl",
    "es-MX": HERE / "corpus_es_MX.jsonl",
    "fr-CA": HERE / "corpus_fr_CA.jsonl",
}


def voice_language(name):
    """Which language dir holds this voice? Read from disk, so adding a
    voice needs no code change here."""
    for lang in LANG_DIRS:
        if (ROOT / lang / name / f"{name}.vin").is_file():
            return lang
    return None


def resolve_voice(name):
    """vin/vdb/vcf + language for a voice name. Always the 8 kHz VDB."""
    lang = voice_language(name)
    if lang is None:
        raise SystemExit(f"error: no voice {name!r} under {'/'.join(LANG_DIRS)}")
    d = ROOT / lang / name
    v = {"name": name, "lang": lang, "dir": d,
         "vin": d / f"{name}.vin",
         "vdb": d / f"{name}8.vdb",
         "vcf": d / f"{name}.vcf",
         "corpus": LANG_CORPUS[lang]}
    for k in ("vin", "vdb", "vcf"):
        if not v[k].is_file():
            raise SystemExit(f"error: {name}: missing {v[k]}")
    return v


def engine_ref_dir(voice_name):
    """Tom's reference stays FLAT in engine_ref/ because those 235 wavs are
    tracked and their .engine_key already matches; other voices nest one
    level down. Renaming the committed set for symmetry would be churn with
    no functional gain."""
    return ENGINE_REF if voice_name == "tom" else ENGINE_REF / voice_name


def read_wav(path):
    """int16 samples, or None. Raw payload, no resampling -- this is an
    exactness check, so nothing may touch the samples on the way in."""
    try:
        with wave.open(str(path), "rb") as w:
            n = w.getnframes()
            raw = w.readframes(n)
            ch = w.getnchannels()
    except (OSError, wave.Error):
        return None
    a = np.frombuffer(raw, dtype="<i2")
    return a[::ch] if ch > 1 else a


def best_ncc(a, b, max_shift=2048):
    """Best NCC of the shorter against the longer, over integer shifts."""
    a = np.asarray(a, np.float64)
    b = np.asarray(b, np.float64)
    if len(a) < 256 or len(b) < 256:
        return 0.0
    if len(a) > len(b):
        a, b = b, a
    n = min(len(a), len(b) - 1, 16000)          # cap: 2 s is plenty
    a = a[:n] - a[:n].mean()
    na = float(np.linalg.norm(a))
    if na < 1e-9:
        return 0.0
    lim = min(len(b) - n, max_shift)
    best = -1.0
    for s in range(0, lim + 1, 8):
        seg = b[s:s + n]
        seg = seg - seg.mean()
        d = float(np.linalg.norm(seg))
        if d > 1e-9:
            v = float(np.dot(a, seg) / (na * d))
            if v > best:
                best = v
    return best


def _text_hash(t):
    """Identity of ONE utterance's text.

    Per-utterance, not one hash over the whole set. A whole-set key looked
    tidier and was wrong: master_spfy_parity audits 235 corpus entries while
    audio_compare's own main filters to 226, so the two tools produced
    different keys for the same reference audio and neither could use the
    other's cache. On a machine without the engine that is not a slow path,
    it is a hard failure. Keyed per utterance, any subset is a hit."""
    return hashlib.sha256(t.encode("utf-8")).hexdigest()[:16]


def _engine_bin_key():
    """Identity of the engine binary, or None when it is not installed."""
    try:
        return hashlib.sha256(DUMPWAV.read_bytes()).hexdigest()[:16]
    except OSError:
        return None


def render_engine(items, out_dir, voice="tom"):
    """dumpwav --batch: one fresh port per utterance, ~50x faster than
    re-invoking. Fresh ports matter -- a shared port lets engine session
    state leak between utterances.

    CACHED, because this is an ANSWER KEY, not a measurement. The same
    engine binary over the same corpus text produces the same bytes every
    time, yet this was re-rendering all 226 utterances on every gate run:
    18.3 s of the 24.1 s audio stage, serially, for a result already on disk.
    Caching does not weaken the gate one bit -- our audio is still compared
    against identical reference bytes; they are simply not regenerated.

    ⚠ ON A KEY MISMATCH THE DIRECTORY IS WIPED, not overwritten in place. If
    a re-render failed for one id, an overwrite would leave the PREVIOUS
    corpus's wav sitting there under the right filename and the gate would
    happily compare against a stale answer. The key file is written LAST, so
    its presence means the whole set landed.

    THE KEY HAS THREE PARTS, and they are checked differently:

      corpus  the (id, text) pairs. Always checked. A corpus edit invalidates.
      engine  the engine binary's own hash. Checked ONLY when that binary is
              present. This is what lets the committed reference be used on a
              machine that has no spfy_dumpwav.exe -- the whole point of
              tracking it -- while still catching a swapped engine on a
              machine that does have one.
      voice   which voice produced these wavs. Always checked, and a mismatch
              invalidates every byte: the same text in two voices lands under
              the same filename, so without this the gate would happily read
              Jill's audio as Tom's answer key. Keys written before this
              existed are read as "tom", which is what they are.

    ⚠ THE SERVER IS REPOINTED when a render is actually needed -- Speechify
    fixes its voice at startup, so a reference for `felix` cannot be rendered
    by a server serving `tom`. On a cache hit the server is left alone, which
    keeps the common case (committed reference, no engine) untouched.

    Sets ENGINE_PROVENANCE so callers can report which of those happened. A
    reference that could not be re-verified against a local engine is a
    weaker claim than one that was, and the gate says so rather than
    printing an unqualified 235/235."""
    global ENGINE_PROVENANCE
    ekey = _engine_bin_key()
    keyfile = out_dir / ".engine_key"

    man = {}
    if keyfile.is_file():
        try:
            man = json.loads(keyfile.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            man = {}
    texts = man.get("texts", {}) if isinstance(man, dict) else {}

    # Absent from keys written before the reference went multi-voice, and
    # every one of those is Tom's.
    key_voice = man.get("voice", "tom")

    need = [(i, t) for i, t in items
            if texts.get(i) != _text_hash(t)
            or not (out_dir / f"{i}.wav").is_file()]
    # A different engine build makes EVERY reference byte suspect, not just
    # the ones whose text moved. So does a different stdin encoding: the
    # reference is only as good as the bytes the engine was handed, and the
    # cp1252 era produced silently wrong audio for every accented entry.
    # Both are checked ONLY where the engine binary exists, so a machine
    # without it can still use the committed reference -- which is the whole
    # point of tracking it.
    if ekey is not None and (man.get("engine") != ekey
                             or man.get("stdin_encoding") != STDIN_ENCODING):
        need = list(items)
    if key_voice != voice:
        need = list(items)

    if not need:
        ENGINE_PROVENANCE = (
            "cached, engine verified" if ekey is not None
            else "committed reference (engine not installed, NOT re-verified)")
        return {i for i, _ in items}

    if ekey is None:
        ENGINE_PROVENANCE = "UNAVAILABLE (no engine, and no matching reference)"
        return set()

    # Speechify fixes its voice at startup, so the server must be serving
    # THIS voice before a single reference byte is rendered.
    import server_ctl
    if not server_ctl.use(voice):
        ENGINE_PROVENANCE = f"UNAVAILABLE (server would not serve {voice})"
        return set()

    out_dir.mkdir(parents=True, exist_ok=True)
    # Delete the stale wavs BEFORE re-rendering. If a render then fails for
    # one id, the gate finds nothing rather than silently comparing against
    # the previous corpus's audio under the right filename.
    for i, _t in need:
        try:
            (out_dir / f"{i}.wav").unlink()
        except OSError:
            pass
    # ⚠ UTF-8 BYTES, EXPLICITLY. spfy_dumpwav's --batch reads stdin with fgets
    # and hands the bytes to Speak() declaring `charset=utf-8`, so stdin IS a
    # UTF-8 channel. This used to pass `input=payload, text=True`, which lets
    # Python encode with the locale's preferred encoding -- cp1252 on this
    # machine. Pure-ASCII corpora are identical either way, which is why Tom
    # and Jill never showed it; every accented es-MX/fr-CA entry was rendered
    # from mojibake, and the resulting reference audio was then used to fail
    # our own renders. 0/72 accented felix entries "passed" against it.
    payload = "".join(f"{i}\t{t}\n" for i, t in need)
    r = subprocess.run([str(DUMPWAV), "--batch", str(out_dir)],
                       input=payload.encode("utf-8"), capture_output=True,
                       check=False, timeout=3600)
    done = set(re.findall(r"^DONE (\S+)",
                          r.stdout.decode("utf-8", "replace"), re.M))

    if (man.get("engine") != ekey or key_voice != voice
            or man.get("stdin_encoding") != STDIN_ENCODING):
        texts = {}     # engine, voice or transport changed: prior hashes void
    for i, t in need:
        if (out_dir / f"{i}.wav").is_file():
            texts[i] = _text_hash(t)
    keyfile.write_text(json.dumps({"engine": ekey, "voice": voice,
                                   "stdin_encoding": STDIN_ENCODING,
                                   "texts": texts},
                                  indent=1, sort_keys=True), encoding="utf-8")
    ENGINE_PROVENANCE = f"rendered by the engine ({len(need)} utterance(s))"
    return {i for i, _ in items
            if (out_dir / f"{i}.wav").is_file()} | done


def render_ours(items, out_dir, exe, workers, voice=None):
    """Our side, rendered fresh every run in a cleaned environment -- it is
    the thing under test. `voice` is a resolve_voice() dict; None means Tom."""
    v = voice or resolve_voice("tom")
    out_dir.mkdir(parents=True, exist_ok=True)
    env = {k: val for k, val in os.environ.items() if not k.startswith("SPFY_")}

    def one(it):
        i, t = it
        p = out_dir / f"{i}.wav"
        subprocess.run([str(exe), str(v["vin"]), str(v["vdb"]), str(v["vcf"]),
                        t, str(p)], capture_output=True, env=env, check=False,
                       timeout=600)
        return i

    with ThreadPoolExecutor(max_workers=workers) as ex:
        list(ex.map(one, items))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    ap.add_argument("--voice", default="tom",
                    help="voice to compare (default tom). The corpus follows "
                         "the voice's LANGUAGE.")
    ap.add_argument("--filter", default=None)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--workers", type=int,
                    default=min(24, (os.cpu_count() or 8)))
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--json", type=Path, default=None)
    a = ap.parse_args()

    voice = resolve_voice(a.voice)
    items = []
    for line in voice["corpus"].read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        d = json.loads(line)
        if a.filter and not re.search(a.filter, d["id"]):
            continue
        items.append((d["id"], d["text"]))
    if a.limit:
        items = items[:a.limit]

    print(f"voice : {voice['name']} ({voice['lang']})  vdb {voice['vdb'].name}")
    print(f"corpus: {voice['corpus'].name}, {len(items)} texts   "
          f"workers: {a.workers}")
    print(f"engine: {DUMPWAV}")
    print(f"ours  : {a.exe}")
    if not DUMPWAV.exists() or not a.exe.exists():
        print("missing binary -- cannot compare")
        return 2

    eng_dir = engine_ref_dir(voice["name"])
    our_dir = SCRATCH / "ours" / voice["name"]
    # Rendering a reference repoints the server, which rewrites
    # config/SWIttsConfig.xml. Put it back afterwards -- leaving the config on
    # whichever voice ran last is a trap for the next tool.
    import server_ctl
    orig_voice, _ = server_ctl.read_config()
    print("rendering (engine, batched)...")
    try:
        done = render_engine(items, eng_dir, voice["name"])
    finally:
        now_voice, _ = server_ctl.read_config()
        if orig_voice and now_voice != orig_voice:
            print(f"  restoring server config to voice={orig_voice}")
            if server_ctl.port_open():
                server_ctl.use(orig_voice)
            else:
                server_ctl.write_config(
                    orig_voice, server_ctl.find_voice_language(orig_voice))
    print(f"  engine reported DONE for {len(done)}/{len(items)}")
    print(f"  reference: {ENGINE_PROVENANCE}")
    print("rendering (ours, parallel)...")
    render_ours(items, our_dir, a.exe, a.workers, voice)

    rows, n_ident, n_len = [], 0, 0
    for i, _t in items:
        ea, oa = read_wav(eng_dir / f"{i}.wav"), read_wav(our_dir / f"{i}.wav")
        if ea is None or oa is None:
            rows.append({"id": i, "err": "missing render"})
            continue
        ident = len(ea) == len(oa) and bool(np.array_equal(ea, oa))
        same_len = len(ea) == len(oa)
        n_ident += ident
        n_len += same_len
        div = "-"
        if not ident:
            m = min(len(ea), len(oa))
            ne = np.nonzero(ea[:m] != oa[:m])[0]
            div = int(ne[0]) if len(ne) else m
        rows.append({"id": i, "eng": len(ea), "ours": len(oa),
                     "ratio": len(oa) / max(len(ea), 1),
                     "ident": ident, "div": div,
                     "ncc": round(best_ncc(oa, ea), 4) if not ident else 1.0})

    ok = [r for r in rows if "err" not in r]
    print("\n" + "=" * 74)
    print(f"BYTE-IDENTICAL : {n_ident}/{len(ok)} "
          f"({n_ident/max(len(ok),1):.1%})   <- the goal")
    print(f"SAME LENGTH    : {n_len}/{len(ok)} "
          f"({n_len/max(len(ok),1):.1%})")
    if ok:
        rt = np.array([r["ratio"] for r in ok])
        nc = np.array([r["ncc"] for r in ok])
        print(f"LENGTH RATIO   : median {np.median(rt):.4f}  "
              f"min {rt.min():.4f}  max {rt.max():.4f}")
        print(f"NCC            : median {np.median(nc):.4f}  "
              f"p10 {np.percentile(nc, 10):.4f}")
        print("\n  A high NCC with a length ratio above 1 is the JOIN")
        print("  ARITHMETIC: same units decoded the same way, glued with a")
        print("  different overlap. A low NCC would mean something worse --")
        print("  a decode or selection difference.")
    if a.verbose:
        print(f"\n{'id':16s} {'engine':>8s} {'ours':>8s} {'ratio':>7s} "
              f"{'ncc':>7s} {'firstdiff':>10s}")
        for r in sorted(ok, key=lambda r: -r["ratio"])[:40]:
            print(f"{r['id']:16s} {r['eng']:8d} {r['ours']:8d} "
                  f"{r['ratio']:7.4f} {r['ncc']:7.4f} {str(r['div']):>10s}")
    if a.json:
        a.json.write_text(json.dumps(rows, indent=1), encoding="utf-8")
        print(f"\nwrote {a.json}")
    print("=" * 74)
    return 0 if n_ident == len(ok) else 1


if __name__ == "__main__":
    sys.exit(main())
