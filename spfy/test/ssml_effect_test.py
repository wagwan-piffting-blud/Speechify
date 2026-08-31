#!/usr/bin/env python3
"""SSML effect sizes -- does each tag actually move the audio, and by how much?

The translation gate (ssml_translate_test.py) proves the right embedded tag
comes out. It cannot prove the tag reaches the synth: `\\!rp` spent months
parsed into a map that nothing read (see the comment at the etag_rate consumer
in spfy_synth.c). This measures the RENDERED WAV instead.

Every arm renders the SAME sentence through the SAME binary; only the tag
differs. Each case declares the direction and a minimum effect size, so a tag
that is parsed but inert fails here instead of passing quietly.

    python spfy/test/ssml_effect_test.py

Needs praat-parselmouth for the pitch arm; that arm is SKIPPED (not passed)
when it is missing.
"""

import argparse
import math
import os
import struct
import subprocess
import sys
import tempfile
import wave

BASE = "The national weather service has issued a warning."


def synth(exe, voice, text, wav, txt):
    with open(txt, "w", encoding="utf-8") as f:
        f.write(text)
    env = dict(os.environ)
    env["SPFY_NO_UPDATE_CHECK"] = "1"
    p = subprocess.run([exe, "-f", txt, voice, wav],
                       capture_output=True, text=True, env=env,
                       encoding="utf-8", errors="replace")
    if p.returncode != 0:
        raise RuntimeError("synth failed (%d): %s" % (p.returncode, p.stderr[-400:]))
    return wav


def read_wav(path):
    with wave.open(path, "rb") as w:
        n, rate = w.getnframes(), w.getframerate()
        raw = w.readframes(n)
    pcm = struct.unpack("<%dh" % (len(raw) // 2), raw)
    return pcm, rate


def duration_s(path):
    pcm, rate = read_wav(path)
    return len(pcm) / float(rate)


def rms_dbfs(path):
    pcm, _ = read_wav(path)
    if not pcm:
        return -120.0
    s = sum(float(x) * float(x) for x in pcm) / len(pcm)
    return 20.0 * math.log10(math.sqrt(s) / 32768.0) if s > 0 else -120.0


def median_f0(path, f0_min=60.0, f0_max=300.0):
    """Median of the VOICED frames only.

    ⚠ f0_min/f0_max are passed explicitly and never left to a heuristic --
    an auto-range ceiling once clipped a 114 Hz-median voice at 123 Hz and
    made a real pitch shift look like no shift at all.
    """
    import parselmouth
    snd = parselmouth.Sound(path)
    pitch = snd.to_pitch(pitch_floor=f0_min, pitch_ceiling=f0_max)
    vals = [v for v in pitch.selected_array["frequency"] if v > 0]
    if not vals:
        return None
    vals.sort()
    return vals[len(vals) // 2]


# (name, ssml, metric, direction, minimum effect vs the control)
CASES = [
    ("rate x-slow  -> longer",  '<prosody rate="x-slow">%s</prosody>' % BASE,
     "dur", "up", 1.15),
    ("rate x-fast  -> shorter", '<prosody rate="x-fast">%s</prosody>' % BASE,
     "dur", "down", 1.10),
    ("volume soft  -> quieter", '<prosody volume="soft">%s</prosody>' % BASE,
     "rms", "down", 3.0),
    ("volume loud  -> louder",  '<prosody volume="loud">%s</prosody>' % BASE,
     "rms", "up", 2.0),
    ("pitch +6st   -> higher",  '<prosody pitch="+6st">%s</prosody>' % BASE,
     "f0", "up", 1.5),
    ("pitch -6st   -> lower",   '<prosody pitch="-6st">%s</prosody>' % BASE,
     "f0", "down", 1.5),
    ("break 1s     -> longer",  'The national weather service<break time="1s"/> '
                                'has issued a warning.',
     "dur", "up", 0.80),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=r"C:\tmp\spfy_build32\src\cli\spfy_synth.exe")
    ap.add_argument("--voice", default="tom")
    args = ap.parse_args()

    tmp = tempfile.gettempdir()
    txt = os.path.join(tmp, "spfy_ssml_eff.txt")

    # The control goes through the identical code path -- same binary, same
    # -f file input, same sentence -- with no tag on it.
    ctl = synth(args.exe, args.voice, BASE, os.path.join(tmp, "eff_ctl.wav"), txt)
    ctl_dur, ctl_rms = duration_s(ctl), rms_dbfs(ctl)
    try:
        ctl_f0 = median_f0(ctl)
        have_pm = ctl_f0 is not None
    except ImportError:
        ctl_f0, have_pm = None, False

    print("control: %.3f s, %.1f dBFS, f0 %s" %
          (ctl_dur, ctl_rms, ("%.1f Hz" % ctl_f0) if ctl_f0 else "n/a"))
    print("-" * 68)

    n_pass = n_fail = n_skip = 0
    for i, (name, ssml, metric, direction, need) in enumerate(CASES):
        if metric == "f0" and not have_pm:
            print("SKIP %-26s (praat-parselmouth not installed)" % name)
            n_skip += 1
            continue
        wav = synth(args.exe, args.voice, ssml,
                    os.path.join(tmp, "eff_%d.wav" % i), txt)
        if metric == "dur":
            got, base, unit = duration_s(wav), ctl_dur, "s"
            eff = got - base if "break" in name else got / base
            shown = ("%+.2f %s" % (eff, unit)) if "break" in name else ("x%.2f" % eff)
            ok = (eff >= need) if direction == "up" else (eff <= 1.0 / need)
        elif metric == "rms":
            got, base, unit = rms_dbfs(wav), ctl_rms, "dB"
            eff = got - base
            shown = "%+.1f %s" % (eff, unit)
            ok = (eff >= need) if direction == "up" else (eff <= -need)
        else:
            got = median_f0(wav)
            if got is None:
                print("FAIL %-26s no voiced frames" % name)
                n_fail += 1
                continue
            eff = 12.0 * math.log(got / ctl_f0, 2.0)
            shown = "%+.2f st (%.1f Hz)" % (eff, got)
            ok = (eff >= need) if direction == "up" else (eff <= -need)

        print("%s %-26s %s" % ("PASS" if ok else "FAIL", name, shown))
        if ok:
            n_pass += 1
        else:
            n_fail += 1

    print("-" * 68)
    print("%d passed, %d failed, %d skipped" % (n_pass, n_fail, n_skip))
    return 1 if n_fail else 0


if __name__ == "__main__":
    sys.exit(main())
