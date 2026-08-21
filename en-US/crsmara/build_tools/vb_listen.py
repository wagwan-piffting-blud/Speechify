#!/usr/bin/env python3
"""Render held-out text through vendor Jill and our round-trip, for LISTENING.

`vb_gate.py` answers "did we recover the speaker" with an F0 statistic. It
cannot hear a bad join. This produces the material for ears:

    NN_a_jill.wav     vendor
    NN_b_jillrt.wav   ours
    NN_ab.wav         the two spliced with 500 ms between   <- play this one
    reel_ab.wav       every pair end to end                 <- or this one
    index.txt         which number is which sentence

⚠ Text is HELD OUT by default: corpus lines that were NOT in the selection the
voice was built from. Scoring or listening on the build set measures
memorisation. `--hard` adds the cases most likely to expose a bad inventory:
words whose phone pairs are thin, and the pau/boundary behaviour.

    python vb_listen.py --n 20
    python vb_listen.py --n 12 --hard
    python vb_listen.py --text "Whatever you want to hear."
"""
import argparse
import json
import os
import subprocess
import sys
import wave
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "wayback"))
sys.path.insert(0, HERE)

from paths import BUILD, DATA, REPO, SCRATCH, ensure  # noqa: E402

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

SYNTH = BUILD / "src" / "cli" / "spfy_synth.exe"
# ⭐ VOICES ARE DISCOVERED, NOT LISTED. Every new arm used to need three edits
# -- this dict, `build_voice.ps1`'s ValidateSet, and the `$Voice -in @(...)`
# that picks the corpus branch -- and updating only some of them is a real bug
# that has already happened twice in one session: once the build resolved its
# corpus to a non-existent `render/` dir, once `vb_lpclamp` died with a bare
# KeyError on a voice that was otherwise fine.
#
# A directory IS a voice if it contains `<name>.vin`. That also keeps the
# pristine trees out by construction: `en-US/mara/` and `en-US/craig/` hold
# only a README, so they can never be discovered.
_ROOTS = (REPO / "en-US", REPO / "reveng" / "en-US")


def discover():
    """{name: dir} for every buildable voice on disk, vendors first."""
    out = {}
    for root in _ROOTS:
        if not root.is_dir():
            continue
        for d in sorted(root.iterdir()):
            if d.is_dir() and (d / f"{d.name}.vin").is_file():
                out[d.name] = d
    return out


VOICES = discover()


def voice_dir(name):
    """Resolve a name, with a useful message instead of a bare KeyError."""
    if name in VOICES:
        return VOICES[name]
    raise SystemExit(f"no such voice: {name!r}\navailable ({len(VOICES)}): "
                     + ", ".join(sorted(VOICES)))


# Documentation only. Nothing below gates anything and a name missing from it
# is not an error; the full account of each arm is in reveng/spfy4/RESUME.md.
NOTES = {
    "jill": "vendor",
    "tom": "vendor",
    "jillrt": "our round-trip of jill, engine-segmented",
    # Built from CRS recordings, NOT from vendor material -- outside the
    # pristine en-US/mara and en-US/craig trees on purpose.
    "donnart": "built from CRS recordings, not vendor material",
    "donnarvc": "first RVC gap fill; 14.3% -> 19.0%, superseded",
    "donnacrop": "vb_recrop re-placed 10 misaligned recordings",
    "donnacut": "donnacrop plus the 8 it could not repair, removed",
    "donnadrop": "control: all 18 flagged recordings simply deleted",
    "donnasyl": "unit +0x15 as a SYLLABLE START, not first-half-of-phone",
    "donnadur": "per-phone duration floor from jill p2",
    "donnasfix": "/s/ /z/ boundary repair v1",
    "donnasfix2": "repair retuned: file reference, 2-frame bridge, gap split",
    "donnasfix3": "vendor-p25 gate; /sh/ /ch/ promoted from control to target",
    "donnagap": "converted units for zh/uh/en/jh and no other phone",
    "donnawd": "plus WORD coverage; prefer-real, no phone allow-list",
    "donnadg": "duration gate jill p5-p99, emptied groups dropped",
}

# Cases that stress an inventory rather than a statistic: thin phone pairs
# (zh, dh, ng clusters), numbers and abbreviations the FE expands, long
# clause chains where the join count is high, and a bare short utterance
# where one bad join is the whole file.
HARD = [
    "The measure of a treasure is usually a pleasure.",
    "Beige garage visions of camouflage and sabotage.",
    "Winds north northwest at 15 to 25 mph with gusts to 40.",
    "Temperatures will range from 32 degrees to 78 degrees on Wednesday.",
    "This is the National Weather Service in Melbourne, Florida.",
    "A tornado watch remains in effect until 9 PM Eastern Daylight Time.",
    "Rough.",
    "Thanks.",
    "The rhythm of the algorithm was smooth although the sixth month was cold.",
    "She sells seashells, and the shells she sells are surely seashells.",
    "Isolated thunderstorms are possible this afternoon and evening.",
    "Boaters should exercise caution in and near thunderstorms.",
]


def render(voice, text, out):
    d = VOICES[voice]
    vin, vdb, vcf = (d / f"{voice}.vin", d / f"{voice}8.vdb", d / f"{voice}.vcf")
    env = {k: v for k, v in os.environ.items() if not k.startswith("SPFY_")}
    p = subprocess.run([str(SYNTH), str(vin), str(vdb), str(vcf), text, str(out)],
                       capture_output=True, text=True, env=env, timeout=300)
    if p.returncode != 0 or not os.path.isfile(out):
        return None, (p.stderr or p.stdout or "").strip()[:200]
    return out, None


def read_wav(p):
    with wave.open(str(p)) as w:
        return w.getframerate(), w.getnchannels(), w.getsampwidth(), \
            w.readframes(w.getnframes())


def write_wav(p, sr, ch, sw, frames):
    with wave.open(str(p), "wb") as w:
        w.setnchannels(ch)
        w.setsampwidth(sw)
        w.setframerate(sr)
        w.writeframes(frames)


def silence(sr, ch, sw, ms):
    return b"\0" * (int(sr * ms / 1000.0) * ch * sw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=20)
    ap.add_argument("--hard", action="store_true",
                    help="use the stress sentences instead of held-out corpus")
    ap.add_argument("--text", action="append", default=[],
                    help="render exactly this; repeatable")
    ap.add_argument("--voices", default="jill,jillrt",
                    help="comma list; first is the A arm")
    ap.add_argument("--gap-ms", type=int, default=500)
    ap.add_argument("--out", default=str(SCRATCH / "jillrt_listen"))
    ap.add_argument("--workers", type=int, default=min(20, os.cpu_count() or 4))
    a = ap.parse_args()

    voices = [v.strip() for v in a.voices.split(",") if v.strip()]
    for v in voices:
        d = VOICES.get(v)
        if not d or not (d / f"{v}.vin").is_file():
            print(f"no such voice: {v}", file=sys.stderr)
            return 1
    if not SYNTH.is_file():
        print(f"missing {SYNTH}", file=sys.stderr)
        return 1

    if a.text:
        picks = list(a.text)
        why = "command line"
    elif a.hard:
        picks = HARD[:a.n] if a.n < len(HARD) else HARD
        why = "stress set"
    else:
        rows = json.loads(
            (DATA / "vb_corpus.json").read_text(encoding="utf-8"))["rows"]
        sel = set(json.loads(
            (DATA / "vb_selection.json").read_text(encoding="utf-8"))["ids"])
        held = [r for r in rows
                if r["source"] == "existing" and r["id"] not in sel
                and 6 <= len(r["text"].split()) <= 22]
        if not held:
            print("no held-out lines", file=sys.stderr)
            return 1
        rnd = __import__("random").Random(4242)
        picks = [r["text"] for r in rnd.sample(held, min(a.n, len(held)))]
        why = f"held-out corpus ({len(held):,} candidates)"

    out = ensure(a.out)
    for f in out.glob("*.wav"):
        f.unlink()
    print(f"{len(picks)} lines from {why}")
    print(f"  voices  {' vs '.join(voices)}")
    print(f"  out     {out}")

    jobs = []
    for i, t in enumerate(picks, 1):
        for k, v in enumerate(voices):
            tag = chr(ord("a") + k)
            jobs.append((v, t, out / f"{i:02d}_{tag}_{v}.wav"))
    with ThreadPoolExecutor(max_workers=a.workers) as ex:
        res = list(ex.map(lambda j: render(*j), jobs))

    bad = [(j, e) for j, (p, e) in zip(jobs, res) if p is None]
    for (v, t, p), e in bad:
        print(f"  ⛔ {v} FAILED: {t[:60]!r} :: {e}", file=sys.stderr)

    # Per-line A/B splices, then one reel of everything.
    reel = []
    meta, sr, ch, sw = None, 8000, 1, 2
    lines = []
    for i, t in enumerate(picks, 1):
        parts = []
        durs = []
        for k, v in enumerate(voices):
            p = out / f"{i:02d}_{chr(ord('a')+k)}_{v}.wav"
            if not p.is_file():
                durs.append(None)
                continue
            sr, ch, sw, fr = read_wav(p)
            meta = (sr, ch, sw)
            parts.append(fr)
            durs.append(len(fr) / float(sr * ch * sw))
        if not parts:
            continue
        gap = silence(sr, ch, sw, a.gap_ms)
        ab = gap.join(parts)
        write_wav(out / f"{i:02d}_ab.wav", sr, ch, sw, ab)
        reel.append(ab)
        ds = "  ".join("----" if d is None else f"{d:.2f}s" for d in durs)
        lines.append(f"{i:02d}  {ds}  {t}")
        print(f"  {i:02d}  {ds}  {t[:64]}")

    if reel and meta:
        sr, ch, sw = meta
        write_wav(out / "reel_ab.wav", sr, ch, sw,
                  silence(sr, ch, sw, 900).join(reel))
    hdr = ["# " + "  ".join(f"{chr(ord('a')+k)}={v}"
                            for k, v in enumerate(voices)),
           f"# {len(picks)} lines from {why}",
           "# columns: id  " + "  ".join(f"dur_{v}" for v in voices) + "  text",
           ""]
    (out / "index.txt").write_text("\n".join(hdr + lines) + "\n",
                                   encoding="utf-8")
    print(f"\n  {len(reel)} pairs written; play {out / 'reel_ab.wav'}")
    if bad:
        print(f"  ⛔ {len(bad)} renders failed", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
