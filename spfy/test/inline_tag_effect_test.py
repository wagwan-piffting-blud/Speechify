"""Do the inline `\\!` tags actually reach the audio?

`ssml_effect_test.py` covers the SSML surface. Nothing covered the inline tag
surface, and four defects lived there undetected because every test that
existed used a well-formed tag with a mid-range value:

  * `\\!vp0` rendered at FULL volume -- 0 was the volume map's own "no tag
    here" sentinel, so asking for silence read as asking for nothing.
  * `\\!vp50Hello` DELETED "Hello" -- the tag matcher rejected digits fused to
    a letter and the catch-all `\\!` swallow then ate the word with it.
  * `\\!vp 50` SPOKE "fifty" at full volume, same catch-all.
  * The leading and trailing `pau` played at full gain whatever the tag said,
    because they belong to no word and the map is keyed by word.

Every case here is a paired measurement against a control that goes through
the identical binary and code path with no tag on it.

Measured on the 2026-09-04 binary, BEFORE the fix, so the cases are known to
be able to fail (`Hello world this is a test.`, tom, 11322-sample control):

    \\!vp0       rms 8567.1, ratio 1.000 vs untagged  -- no effect at all
    \\!vp50Hello n=8882   -- "Hello" gone
    \\!vp 50     n=14298  -- "fifty" spoken
    \\!vp10      last 545 samples at ratio 1.0, not 0.1

Usage:  python inline_tag_effect_test.py [--exe PATH] [--voice tom]
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


def synth(exe, voice, text, out):
    r = subprocess.run([exe, voice, text, out], capture_output=True,
                       text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise SystemExit("synth failed (%d) on %r\n%s"
                         % (r.returncode, text, r.stderr[-800:]))
    return out


def pcm(path):
    with wave.open(path, "rb") as w:
        raw = w.readframes(w.getnframes())
    return struct.unpack("<%dh" % (len(raw) // 2), raw)


def rms_dbfs(s):
    if not s:
        return -120.0
    m = sum(float(x) * float(x) for x in s) / len(s)
    return 20.0 * math.log10(math.sqrt(m) / 32768.0) if m > 0 else -120.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe",
                    default=r"C:\tmp\spfy_build32\src\cli\spfy_synth.exe")
    ap.add_argument("--voice", default="tom")
    args = ap.parse_args()
    tmp = tempfile.gettempdir()

    def render(name, text):
        return pcm(synth(args.exe, args.voice, text,
                         os.path.join(tmp, "inline_%s.wav" % name)))

    ctl = render("ctl", BASE)
    ctl_db = rms_dbfs(ctl)
    print("control: %d samples, %.1f dBFS" % (len(ctl), ctl_db))
    print("-" * 70)

    n_pass = n_fail = 0

    def check(name, ok, detail):
        nonlocal n_pass, n_fail
        print("%s %-28s %s" % ("PASS" if ok else "FAIL", name, detail))
        if ok:
            n_pass += 1
        else:
            n_fail += 1

    # 1. \!vp0 is silence, not "no tag".
    s = render("vp0", "\\!vp0 " + BASE)
    peak = max(abs(x) for x in s) if s else 0
    check("vp0 -> silence", peak == 0, "peak=%d (expect 0)" % peak)

    # 2. The gain reaches the whole utterance, pauses included. A per-word map
    #    leaves the lead-in and tail pau at full gain; they carry room tone, so
    #    the check is on the QUIETEST tag, where any unscaled region dominates.
    s = render("vp10", "\\!vp10 " + BASE)
    got = rms_dbfs(s) - ctl_db
    check("vp10 -> -20 dB everywhere", abs(got - (-20.0)) < 1.0,
          "%.2f dB (expect -20.0 +/- 1.0)" % got)

    # 3. A tag fused to the next word must not eat the word.
    s = render("nospace", "\\!vp50" + BASE)
    ref = render("spaced", "\\!vp50 " + BASE)
    check("vp50<word> keeps the word",
          abs(len(s) - len(ref)) <= 2,
          "n=%d vs %d with a space" % (len(s), len(ref)))

    # 4. A space before the value must not turn the value into a spoken word.
    s = render("spacedval", "\\!vp 50 " + BASE)
    check("vp<sp>50 is not spoken",
          abs(len(s) - len(ref)) <= 2,
          "n=%d vs %d for \\!vp50" % (len(s), len(ref)))

    # 5. \!vp100 must be byte-identical to no tag at all -- volume is a gain,
    #    it may not perturb selection or timing. (The engine's own behaviour:
    #    \!rp100 DOES change the audio, \!vp100 does not.)
    s = render("vp100", "\\!vp100 " + BASE)
    check("vp100 == untagged", list(s) == list(ctl),
          "%d/%d samples equal"
          % (sum(1 for a, b in zip(s, ctl) if a == b), len(ctl)))

    # 6. The reset returns to full volume for the rest of the utterance.
    s = render("vpr", "\\!vp10 The national weather service \\!vpr "
                      "has issued a warning.")
    got = rms_dbfs(s) - ctl_db
    check("vpr restores volume", got > -12.0,
          "%.2f dB (a stuck \\!vp10 reads about -20)" % got)

    print("-" * 70)
    print("%d passed, %d failed" % (n_pass, n_fail))
    return 1 if n_fail else 0


sys.exit(main())
