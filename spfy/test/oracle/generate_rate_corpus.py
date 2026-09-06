"""generate_rate_corpus.py -- build corpus_rate.jsonl.

The rate path has no answer key: every trace in traces_master, and every wav
in engine_ref/, was captured with NO rate tag, so the 221/221 byte-identical
headline says nothing at all about \\!rp. This builds the missing corpus.

Shape: a small set of texts crossed with the rate tags that matter, plus a
few spans and pause interactions that a whole-utterance tag cannot exercise.
Kept deliberately small -- each entry costs an engine render and a compare,
and the point is coverage of the RATE MECHANISM, not of the text.

Three things every set here must keep, because each one caught a real bug:

  - \\!rp100 and \\!rpr, which ask for NO change and still must not render
    like an untagged utterance (they arm target matching).
  - \\!vp100, the negative control: volume does NOT arm it, so this MUST be
    byte-identical to the untagged render. A gate where the control cannot
    fail is not a gate.
  - the 33 and 300 endpoints, which is where a clamp bug hides.

    python generate_rate_corpus.py [--out corpus_rate.jsonl]
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

# Short, phonetically varied, and all already exercised by the main corpus,
# so a failure here is about rate and not about some new text.
TEXTS = {
    "wx":   "The national weather service has issued a warning.",
    "pan":  "The quick brown fox jumps over the lazy dog.",
    "hi":   "Hello, world.",
    "num":  "It is seventy two degrees at four thirty.",
}

# Whole-utterance rate, the main sweep. 100 is deliberately absent: it is an
# ARMING case, not a sweep point, and lives in section 4 beside \!rpr.
RATES = [33, 50, 67, 75, 90, 110, 125, 150, 200, 250, 300]


def build():
    out = []

    # 1. Untagged baselines, so every ratio has a same-voice denominator.
    for tk, text in TEXTS.items():
        out.append({"id": f"rate_{tk}_base", "mode": "text", "text": text,
                    "tags": ["rate", "baseline"]})

    # 2. The sweep, on the one text with a known vendor curve.
    for n in RATES:
        out.append({"id": f"rate_wx_rp{n}", "mode": "text",
                    "text": f"\\!rp{n} {TEXTS['wx']}",
                    "tags": ["rate", "sweep", "rp"]})

    # 3. \!rd must behave identically to \!rp when no port rate was set.
    for n in (50, 150, 300):
        out.append({"id": f"rate_wx_rd{n}", "mode": "text",
                    "text": f"\\!rd{n} {TEXTS['wx']}",
                    "tags": ["rate", "rd"]})

    # 4. The arming cases: no-op rate tags that must still change the audio,
    #    and the volume control that must NOT.
    out.append({"id": "rate_wx_rp100", "mode": "text",
                "text": f"\\!rp100 {TEXTS['wx']}", "tags": ["rate", "arm"]})
    out.append({"id": "rate_wx_rpr", "mode": "text",
                "text": f"\\!rpr {TEXTS['wx']}", "tags": ["rate", "arm"]})
    out.append({"id": "rate_wx_rdr", "mode": "text",
                "text": f"\\!rdr {TEXTS['wx']}", "tags": ["rate", "arm"]})
    out.append({"id": "rate_wx_vp100", "mode": "text",
                "text": f"\\!vp100 {TEXTS['wx']}",
                "tags": ["rate", "control"]})

    # 5. Spans: a rate that starts and stops mid-utterance. This is what
    #    exercises the per-unit factor persisting across pad and pau slots.
    out.append({"id": "rate_span_mid", "mode": "text",
                "text": "The national weather \\!rp50 service has issued "
                        "\\!rpr a warning.",
                "tags": ["rate", "span"]})
    out.append({"id": "rate_span_two", "mode": "text",
                "text": "\\!rp150 The national weather \\!rp75 service has "
                        "issued a warning.",
                "tags": ["rate", "span"]})
    out.append({"id": "rate_span_tail", "mode": "text",
                "text": "The national weather service has \\!rp200 issued "
                        "a warning.",
                "tags": ["rate", "span"]})

    # 6. Pause interaction. The Guide says rate scales pauses too, and the
    #    plosive guard's "pau" exclusion is how that happens -- so a \!p at
    #    three rates is the direct test of it.
    for tag, pre in (("base", ""), ("rp50", "\\!rp50 "), ("rp200", "\\!rp200 ")):
        out.append({"id": f"rate_pau_{tag}", "mode": "text",
                    "text": pre + "The national weather service \\!p500 has "
                                  "issued a warning.",
                    "tags": ["rate", "pause"]})

    # 7. Plosive-heavy text at both extremes: the pass-through guard only
    #    shows up where there are stops to pass through.
    plos = "Pat kept a big packet of taped tickets."
    out.append({"id": "rate_plos_base", "mode": "text", "text": plos,
                "tags": ["rate", "plosive", "baseline"]})
    for n in (50, 200):
        out.append({"id": f"rate_plos_rp{n}", "mode": "text",
                    "text": f"\\!rp{n} {plos}", "tags": ["rate", "plosive"]})

    # 8. The other texts at one slow and one fast rate, for breadth.
    for tk in ("pan", "hi", "num"):
        for n in (50, 200):
            out.append({"id": f"rate_{tk}_rp{n}", "mode": "text",
                        "text": f"\\!rp{n} {TEXTS[tk]}",
                        "tags": ["rate", "breadth"]})
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(Path(__file__).with_name(
        "corpus_rate.jsonl")))
    a = ap.parse_args()
    rows = build()
    ids = [r["id"] for r in rows]
    assert len(ids) == len(set(ids)), "duplicate id"
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        for r in rows:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")
    print(f"wrote {len(rows)} entries to {a.out}")


if __name__ == "__main__":
    main()
