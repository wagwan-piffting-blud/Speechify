#!/usr/bin/env python3
"""<pron sym> must not change how the words AROUND it are phonemized.

Each case renders a sentence twice with SPFY_ETAGS_DUMP=1:
  pron  - with the <pron> construct
  ref   - the same sentence with the construct's text (or "thing") in its
          place and a `\\!p1` on the end, which routes it through the
          single-pass builder so a tagged line is printed. Words are what the
          FE gives the whole sentence.
Every word block OTHER than the pron's own must match exactly: POS, word
stress, syllable stress, accent, boundary tone, phones.

Before build_inline_pron_tagged, the word just before a <pron> came back
phrase-final (`say ... [.1,H*;L-L%`), and function words were read unreduced
(`it` as `ih t`, not `ix t`). SPFY_INLINE_PRON_LEGACY=1 restores that path;
`--expect-fail` runs this against it to prove the test can fail.

    python spfy/test/pron_context_test.py
    python spfy/test/pron_context_test.py --expect-fail
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

CASES = [
    'Say <pron sym="h eh 1 l ow"/> now.',
    'Is it <pron sym="h eh 1 l ow"/> or goodbye?',
    'The weather service in <pron sym="t ah 1 m ey 2 t ow"/> says rain.',
    'I said the <pron sym="t ah 1 m ey 2 t ow">tomato</pron>, the potato and the carrot are fine.',
    'Watch for <pron sym="b l ae 1 f n ih 0 k"/> \\!p300 near the coast tonight.',
    'Well. <pron sym="h eh 1 l ow"/> there.',
]

PRON_RE = re.compile(r'<pron\b[^>]*?/>|<pron\b[^>]*>(.*?)</pron>', re.S)
BLOCK_RE = re.compile(r'<[^<>]*>')


def tagged(exe, voice, text, wav, txt, legacy):
    with open(txt, "w", encoding="utf-8") as f:
        f.write(text)
    env = dict(os.environ)
    env["SPFY_ETAGS_DUMP"] = "1"
    env["SPFY_NO_UPDATE_CHECK"] = "1"
    env.pop("SPFY_INLINE_PRON_LEGACY", None)
    if legacy:
        env["SPFY_INLINE_PRON_LEGACY"] = "1"
    p = subprocess.run([exe, "-f", txt, voice, wav], capture_output=True,
                       text=True, env=env, encoding="utf-8", errors="replace")
    for line in (p.stderr or "").splitlines():
        m = re.match(r"^\[etags\] tagged: (.*)$", line)
        if m:
            return m.group(1)
    return None


def blocks(line):
    # `(?d,N)`'s N is a character count, which the reference's appended `\!p1`
    # shifts on the last word; it is not phonemization.
    out = []
    for b in BLOCK_RE.findall(line or ""):
        b = re.sub(r"\(p\d+\)", "", b)
        out.append(re.sub(r"\(\?d,\d+\)", "(?d)", b))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=r"C:\tmp\spfy_build32\src\cli\spfy_synth.exe")
    ap.add_argument("--voice", default="tom")
    ap.add_argument("--expect-fail", action="store_true",
                    help="run against SPFY_INLINE_PRON_LEGACY=1 and require failures")
    args = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix="pronctx_")
    wav = os.path.join(tmp, "o.wav")
    txt = os.path.join(tmp, "in.txt")
    failed = 0
    for text in CASES:
        ref_text = PRON_RE.sub(lambda m: (m.group(1) or "thing").strip(), text)
        ref_text = ref_text.rstrip() + " \\!p1"
        got = tagged(args.exe, args.voice, text, wav, txt, args.expect_fail)
        ref = tagged(args.exe, args.voice, ref_text, wav, txt, False)
        gb, rb = blocks(got), blocks(ref)
        bad = []
        if not got or not ref:
            bad.append("no tagged line")
        elif len(gb) != len(rb):
            bad.append(f"{len(gb)} word blocks vs {len(rb)}")
        else:
            names = {"<_pron_"} | {"<" + (m.group(1) or "").strip().lower()
                                   for m in PRON_RE.finditer(text) if m.group(1)}
            skipped = 0
            for g, r in zip(gb, rb):
                if g.split()[0] in names:
                    skipped += 1
                    continue
                if g != r:
                    bad.append(f"  pron: {g}\n    ref:  {r}")
            n_pron = len(PRON_RE.findall(text))
            if skipped != n_pron:
                bad.append(f"expected {n_pron} pron block(s) to skip, found {skipped}")
        status = "FAIL" if bad else "PASS"
        failed += bool(bad)
        print(f"{status}  {text}")
        for b in bad:
            print(b)

    print("-" * 70)
    print(f"{len(CASES) - failed} passed, {failed} failed")
    if args.expect_fail:
        return 0 if failed else 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
