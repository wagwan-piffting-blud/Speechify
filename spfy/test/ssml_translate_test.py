#!/usr/bin/env python3
"""SSML -> embedded-tag translation gate.

Drives spfy_synth with SPFY_SSML_DUMP=1 and checks the line the translator
prints, not the audio. That is deliberate: the translation is the only part
this feature owns. Everything after it -- spfy_etags_resolve, the \\!pN pause
splice, <pron sym=...> -- was already tested by whatever tests those tags.

    python spfy/test/ssml_translate_test.py
    python spfy/test/ssml_translate_test.py --exe C:\\tmp\\spfy_build32\\src\\cli\\spfy_synth.exe

Exit 0 when every case passes, 1 otherwise.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

# (name, ssml in, expected translated text)
#
# Expectations are EXACT, including spacing. A translator that drifts by a
# space is still a translator that changed, and a fuzzy match here would hide
# the day it starts emitting the tag on the wrong side of a word.
CASES = [
    ("plain-passthrough-not-detected",
     "Hello there.",
     None),                                  # not SSML: no dump line at all

    ("pron-must-survive",
     '<pron sym="hh ax 0 l ow 1">hello</pron>',
     None),                                  # not ours; must not be detected

    ("speak-stripped",
     "<speak>Hello there.</speak>",
     "Hello there."),

    ("prosody-rate-named",
     '<speak>Hello <prosody rate="x-slow">slow world</prosody>. Done.</speak>',
     "Hello \\!rp50 slow world\\!rp100 . Done."),

    ("prosody-rate-percent",
     '<prosody rate="150%">fast</prosody>',
     "\\!rp150 fast\\!rp100 "),

    ("prosody-rate-relative",
     '<prosody rate="+20%">a</prosody>',
     "\\!rp120 a\\!rp100 "),

    ("prosody-rate-multiplier",
     '<prosody rate="1.5">a</prosody>',
     "\\!rp150 a\\!rp100 "),

    ("prosody-volume-named",
     '<prosody volume="loud">shout</prosody>',
     "\\!vp150 shout\\!vp100 "),

    ("prosody-volume-db",
     '<prosody volume="+6dB">a</prosody>',
     "\\!vp200 a\\!vp100 "),

    ("prosody-pitch-semitones",
     '<prosody pitch="+4st">high</prosody>',
     "\\!pp126 high\\!pp100 "),

    ("prosody-pitch-named",
     '<prosody pitch="x-high">a</prosody>',
     "\\!pp159 a\\!pp100 "),

    ("prosody-pitch-hz-ignored",
     '<prosody pitch="220Hz">a</prosody>',
     "a"),

    ("prosody-nested-composes",
     '<prosody rate="200%">a<prosody rate="50%">b</prosody>c</prosody>',
     "\\!rp200 a\\!rp50 b\\!rp200 c\\!rp100 "),

    ("prosody-multi-attr",
     '<prosody rate="slow" pitch="low" volume="soft">x</prosody>',
     "\\!vp50 \\!rp70 \\!pp79 x\\!vp100 \\!rp100 \\!pp100 "),

    ("break-time-ms",
     "a<break time=\"800ms\"/>b",
     "a \\!p800 b"),

    ("break-time-seconds",
     'a<break time="1.5s"/>b',
     "a \\!p1500 b"),

    ("break-strength",
     'a<break strength="strong"/>b',
     "a \\!p500 b"),

    ("break-bare",
     "a<break/>b",
     "a \\!p250 b"),

    ("break-strength-none-is-silent",
     'a<break strength="none"/>b',
     "ab"),

    ("emphasis-per-word",
     "<emphasis level=\"strong\">two words</emphasis> after",
     "\\![ToBI:L+H*]two \\![ToBI:L+H*]words after"),

    ("emphasis-reduced",
     '<emphasis level="reduced">quiet</emphasis>',
     "\\![ToBI:NONE]quiet"),

    ("phoneme-arpabet",
     '<phoneme alphabet="x-arpabet" ph="HH AH0 L OW1">hello</phoneme>',
     '<pron sym="hh ah 0 l ow 1">hello</pron>'),

    ("phoneme-ipa",
     '<phoneme alphabet="ipa" ph="h\u0259\u02c8lo\u028a">hello</phoneme>',
     '<pron sym="hh ax 0 l ow 1">hello</pron>'),

    ("phoneme-alphabet-sniffed-ipa",
     '<phoneme ph="\u02c8k\u0251t">cot</phoneme>',
     '<pron sym="k aa 1 t">cot</pron>'),

    ("say-as-characters",
     '<say-as interpret-as="characters">abc</say-as> then',
     "\\!tsc abc\\!ts0  then"),

    ("say-as-digits",
     '<say-as interpret-as="digits">123</say-as>',
     "\\!tsa 123\\!ts0 "),

    ("say-as-date-year",
     '<say-as interpret-as="date">1985</say-as>',
     "\\!ny0 1985\\!ny1 "),

    ("sub-alias-replaces",
     '<sub alias="World Wide Web Consortium">W3C</sub> rules',
     "World Wide Web Consortium rules"),

    ("entities-decoded",
     "<speak>Tom &amp; Jerry &lt;3</speak>",
     "Tom & Jerry <3"),

    ("numeric-entity",
     "<speak>caf&#233;</speak>",
     "caf\u00e9"),

    ("comment-dropped",
     "<speak>a<!-- not spoken -->b</speak>",
     "ab"),

    ("cdata-kept",
     "<speak><![CDATA[a < b]]></speak>",
     "a < b"),

    ("metadata-content-dropped",
     "<speak><metadata>secret</metadata>said</speak>",
     "said"),

    ("mark-dropped",
     '<speak>a<mark name="m1"/>b</speak>',
     "ab"),

    ("p-and-s-punctuate",
     "<speak><p><s>one</s><s>two</s></p></speak>",
     "one. two. "),

    ("unknown-tag-content-kept",
     "<speak><w>hello</w></speak>",
     "<w>hello</w>"),

    ("unbalanced-close-ignored",
     "<speak>a</prosody>b</speak>",
     "ab"),

    ("unclosed-prosody-still-terminates",
     '<speak><prosody rate="fast">a',
     "\\!rp130 a"),

    ("voice-content-kept",
     '<speak><voice name="other">hello</voice></speak>',
     "hello"),

    ("xml-declaration-dropped",
     '<?xml version="1.0"?><speak>hi</speak>',
     "hi"),

    ("namespaced-attr",
     '<speak xml:lang="en-US">hi</speak>',
     "hi"),
]


def run(exe, voice, text, wav, txt):
    """Feed the case through -f, never as an argv string.

    ⚠ NOT A TEST-HARNESS CONVENIENCE. On Windows argv arrives in the ANSI
    code page, so a UTF-8 IPA `ph` attribute loses every non-ASCII byte
    before main() ever sees it -- `<phoneme ph="həˈloʊ">` reaches the
    translator as `h'lo` and silently produces the wrong phonemes. It looked
    exactly like a broken IPA table until the same input went through a file.
    -f reads UTF-8 (and UTF-16 LE) properly, which is also how a GUI or any
    other front end should hand text to this binary.
    """
    with open(txt, "w", encoding="utf-8") as f:
        f.write(text)
    env = dict(os.environ)
    env["SPFY_SSML_DUMP"] = "1"
    env["SPFY_NO_UPDATE_CHECK"] = "1"
    p = subprocess.run([exe, "-f", txt, voice, wav],
                       capture_output=True, text=True, env=env,
                       encoding="utf-8", errors="replace")
    for line in (p.stderr or "").splitlines():
        m = re.match(r"^\[ssml\] -> (.*)$", line)
        if m:
            return m.group(1), p.returncode
    return None, p.returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=r"C:\tmp\spfy_build32\src\cli\spfy_synth.exe")
    ap.add_argument("--voice", default="tom")
    args = ap.parse_args()

    if not os.path.exists(args.exe):
        print("no such exe: %s" % args.exe, file=sys.stderr)
        return 2

    wav = os.path.join(tempfile.gettempdir(), "spfy_ssml_test.wav")
    txt = os.path.join(tempfile.gettempdir(), "spfy_ssml_test.txt")
    n_pass = n_fail = 0
    for name, ssml, want in CASES:
        got, rc = run(args.exe, args.voice, ssml, wav, txt)
        ok = (got == want)
        if ok:
            n_pass += 1
        else:
            n_fail += 1
            print("FAIL %s" % name)
            print("   in   %r" % ssml)
            print("   want %r" % want)
            print("   got  %r  (rc=%d)" % (got, rc))
    print("%d/%d passed" % (n_pass, n_pass + n_fail))
    return 1 if n_fail else 0


if __name__ == "__main__":
    sys.exit(main())
