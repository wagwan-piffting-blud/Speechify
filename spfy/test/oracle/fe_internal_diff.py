"""A/B diff harness: in-house pure-C FE (fe_internal) vs DLL FE.

For each text-mode phrase in corpus.jsonl, capture both tagged-text
outputs (DLL via spfy_fe_host_dump, in-house via spfy_fe_text2tagged),
parse them structurally, and bucket divergences by category.

Output:
  c:/tmp/fe_diff/raw/<phrase_id>.{dll,internal}.txt   raw tagged streams
  c:/tmp/fe_diff/categories.csv                       per-phrase pattern counts
  c:/tmp/fe_diff/top_patterns.txt                     ranked list of mismatch
                                                      patterns across the corpus

Usage:
  python spfy/test/oracle/fe_internal_diff.py [--limit N] [--ids id1,id2,...]
"""

from __future__ import annotations

import argparse
import collections
import csv
import json
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import List

REPO = Path(__file__).resolve().parents[3]
DEFAULT_CORPUS = REPO / "spfy" / "test" / "oracle" / "corpus.jsonl"
DEFAULT_VCF    = REPO.parent / "Speechify" / "en-US" / "tom" / "tom.vcf"
BUILD_DIR = Path(r"C:\tmp\spfy_build32")
INTERNAL_EXE = BUILD_DIR / "src" / "cli" / "spfy_fe_text2tagged.exe"
DLL_EXE      = BUILD_DIR / "src" / "cli" / "spfy_fe_host_dump.exe"

OUT_DIR = Path(r"c:\tmp\fe_diff")
RAW_DIR = OUT_DIR / "raw"


# --------------------------------------------------------------------
# Drive the two binaries, capture cleaned tagged streams.
# --------------------------------------------------------------------

def run_internal(text: str) -> str:
    r = subprocess.run([str(INTERNAL_EXE), text],
                       capture_output=True, text=True, encoding="utf-8",
                       errors="replace", timeout=30)
    if r.returncode != 0:
        raise RuntimeError(f"internal FE rc={r.returncode}: {r.stderr[:200]}")
    return r.stdout.strip()


# Match the fprintf format in fe_host.c: `[fe_host] tagged output (N bytes): TEXT`.
DLL_TAG_RE = re.compile(r"\[fe_host\] tagged output \(\d+ bytes\): (.+?)(?=\n\[host_dump\]|\Z)",
                        re.DOTALL)


def run_dll(text: str, vcf: Path) -> str:
    r = subprocess.run([str(DLL_EXE), str(vcf), text],
                       capture_output=True, text=True, encoding="utf-8",
                       errors="replace", timeout=60)
    # spfy_fe_host_dump emits the tagged stream on stderr (it's a debug log).
    haystack = (r.stderr or "") + "\n" + (r.stdout or "")
    m = DLL_TAG_RE.search(haystack)
    if not m:
        raise RuntimeError(f"DLL FE: no tagged output (rc={r.returncode}). "
                           f"stderr head: {haystack[:300]}")
    return m.group(1).strip()


# --------------------------------------------------------------------
# Lightweight parser. We share fe_parse.c's grammar but reimplement
# in Python so the harness has zero coupling to the C build state.
# Only the fields needed for pattern categorisation are extracted.
# --------------------------------------------------------------------

@dataclass
class Phoneme:
    name: str
    syl_idx: int          # 0-based index inside the word
    syl_stress: int       # 0/1/2 from `.X` marker preceding the syl chunk
    accent: str           # "" if no accent on this syl

@dataclass
class Word:
    text: str
    char_start: int
    char_len: int
    pos: str              # POS tag or "undef"
    stress_lvl: int       # 0/1/2 from word header
    phonemes: List[Phoneme] = field(default_factory=list)

@dataclass
class Utt:
    words: List[Word] = field(default_factory=list)
    end_boundary: str = ""    # "L-L%" / "H-H%" / "" — boundary tone

@dataclass
class Parsed:
    utts: List[Utt] = field(default_factory=list)


# Tokeniser
WS_RE       = re.compile(r"\s+")
# Word tokens in tagged output include apostrophe-bearing contractions
# (don't, we'll, today's) and the FE occasionally emits dotted abbreviations
# (e.g. "u.s."). Accept those characters inside an ident.
IDENT_RE    = re.compile(r"[A-Za-z][A-Za-z0-9_'.]*")
ACCENT_RE   = re.compile(r"H\*\+L|L\*\+H|L\+H\*|H\+L\*|H\*|L\*")
BOUNDARY_RE = re.compile(r"[LH]-[LH]%")


class _P:
    __slots__ = ("s", "i")
    def __init__(self, s: str):
        self.s = s
        self.i = 0
    def eof(self) -> bool: return self.i >= len(self.s)
    def peek(self, n=1) -> str: return self.s[self.i:self.i+n]
    def skip_ws(self):
        m = WS_RE.match(self.s, self.i)
        if m: self.i = m.end()
    def match(self, lit: str) -> bool:
        self.skip_ws()
        if self.s.startswith(lit, self.i):
            self.i += len(lit); return True
        return False
    def expect(self, lit: str):
        if not self.match(lit):
            raise ValueError(f"expected {lit!r} at offset {self.i}: "
                             f"{self.s[max(0,self.i-20):self.i+20]!r}")
    def ident(self) -> str:
        self.skip_ws()
        m = IDENT_RE.match(self.s, self.i)
        if not m: raise ValueError(f"expected ident at {self.i}")
        self.i = m.end()
        return m.group(0)
    def integer(self) -> int:
        self.skip_ws()
        j = self.i
        if j < len(self.s) and self.s[j] == "-": j += 1
        k = j
        while k < len(self.s) and self.s[k].isdigit(): k += 1
        if k == j: raise ValueError(f"expected int at {self.i}")
        n = int(self.s[j:k]); self.i = k
        return n


def parse(stream: str) -> Parsed:
    p = _P(stream)
    out = Parsed()
    # Optional leading %%  -- desktop captures wrap streams as %% utt %%;
    # the spfy_fe_text2tagged form omits the outer %%.
    while not p.eof():
        p.skip_ws()
        if p.eof(): break
        if p.match("%%"): continue
        if not p.peek().startswith("#"): break
        # utterance
        utt = Utt()
        out.utts.append(utt)
        p.expect("#")
        p.expect("{")
        # Optional terminator marker inside the opener (`.`, `,`, `?`, `!`).
        p.skip_ws()
        if p.peek() and p.peek() in ".,?!;:":
            p.i += 1
        # body
        while not p.eof():
            p.skip_ws()
            if p.peek() == "}":
                p.expect("}"); break
            if p.s.startswith("pau", p.i):
                # pause token: pau(p<N>)
                p.i += 3
                if p.match("("):
                    while not p.eof() and p.s[p.i] != ")":
                        p.i += 1
                    p.expect(")")
                continue
            if p.peek() == "<":
                w = _parse_word(p)
                utt.words.append(w)
            else:
                # Skip unknown token
                p.i += 1
        # End-of-utt boundary tone (sometimes appears outside the last
        # word's syl chunk). We capture boundary as the last syl's
        # accent if it has the ";X-Y%" suffix.
    # Post-process: pull boundary tone from last word's last syl accent.
    for utt in out.utts:
        if utt.words and utt.words[-1].phonemes:
            last_acc = utt.words[-1].phonemes[-1].accent
            bm = BOUNDARY_RE.search(last_acc)
            if bm:
                utt.end_boundary = bm.group(0)
    return out


def _parse_word(p: _P) -> Word:
    p.expect("<")
    text = p.ident()
    # (char_start, char_len) — DLL emits "()" when the source span is
    # unknown (synthesized fragments like the `em` in alphabet spellings).
    p.expect("(")
    p.skip_ws()
    if p.peek() == ")":
        cs = -1; cl = 0
        p.expect(")")
    else:
        cs = p.integer(); p.expect(","); cl = p.integer(); p.expect(")")
    # POS,stress_level
    pos = p.ident(); p.expect(","); stress = p.integer()
    p.expect("[")
    word = Word(text=text, char_start=cs, char_len=cl, pos=pos, stress_lvl=stress)
    syl_idx = -1
    syl_stress = 0
    syl_accent = ""
    while not p.eof():
        p.skip_ws()
        if p.peek() == "]":
            p.expect("]"); break
        if p.peek() == ".":
            # New syllable header: `.<stress>[,<accent>][;<boundary>]`.
            # The DLL sometimes emits the boundary suffix without a
            # preceding accent (e.g. `.2;L-L%`), and other times bundles
            # both (e.g. `.1,H*;L-L%`). Consume either form into accent.
            p.i += 1
            syl_idx += 1
            syl_stress = p.integer()
            syl_accent = ""
            p.skip_ws()
            if p.peek() == ",":
                p.i += 1
                j = p.i
                while j < len(p.s) and p.s[j] not in " \t\n]":
                    j += 1
                syl_accent = p.s[p.i:j]; p.i = j
            elif p.peek() == ";":
                # Boundary-only suffix (no pitch accent). Capture the
                # whole "X-Y%" tail into syl_accent so accent_pos
                # detection still works against it.
                p.i += 1
                j = p.i
                while j < len(p.s) and p.s[j] not in " \t\n]":
                    j += 1
                syl_accent = p.s[p.i:j]; p.i = j
            continue
        # Phoneme: <name>(p<int>)
        name = p.ident()
        # optional ( pNNN )
        if p.match("("):
            while not p.eof() and p.s[p.i] != ")":
                p.i += 1
            p.expect(")")
        word.phonemes.append(Phoneme(
            name=name, syl_idx=max(0, syl_idx),
            syl_stress=syl_stress, accent=syl_accent))
        syl_accent = ""    # accent is per-syl on the first phoneme it appears
    p.expect(">")
    return word


# --------------------------------------------------------------------
# Diff engine. Aligns words pairwise and reports categorised mismatches.
# --------------------------------------------------------------------

@dataclass
class Mismatch:
    category: str           # "syl_count", "phoneme", "stress", "accent_pos",
                            # "pos_tag", "word_stress", "boundary", "word_count"
    pattern: str            # short human-readable instance string
    detail: str = ""

def diff_phrase(internal: Parsed, dll: Parsed) -> List[Mismatch]:
    out: List[Mismatch] = []
    # Compare per-utterance, then per-word.
    if len(internal.utts) != len(dll.utts):
        out.append(Mismatch("utt_count",
                            f"int={len(internal.utts)} dll={len(dll.utts)}"))
    for ui in range(min(len(internal.utts), len(dll.utts))):
        ui_int = internal.utts[ui]
        ui_dll = dll.utts[ui]
        if ui_int.end_boundary != ui_dll.end_boundary:
            out.append(Mismatch("boundary",
                                f"int={ui_int.end_boundary or 'none'} "
                                f"dll={ui_dll.end_boundary or 'none'}"))
        if len(ui_int.words) != len(ui_dll.words):
            out.append(Mismatch("word_count",
                                f"int={len(ui_int.words)} "
                                f"dll={len(ui_dll.words)}"))
        for wi in range(min(len(ui_int.words), len(ui_dll.words))):
            w_int = ui_int.words[wi]
            w_dll = ui_dll.words[wi]
            _diff_word(w_int, w_dll, out)
    return out


def _phon_syl(word: Word) -> List[List[str]]:
    """Group word.phonemes into list-of-syl-of-phon-names."""
    syls: List[List[str]] = []
    cur = -1
    for ph in word.phonemes:
        while cur < ph.syl_idx:
            syls.append([]); cur += 1
        syls[-1].append(ph.name)
    return syls


def _accent_syl_idx(word: Word) -> int:
    """Return syl index that carries the H*/L* pitch accent, or -1."""
    seen = {}
    for ph in word.phonemes:
        # First non-empty accent on each syl-idx; ignore boundary-only.
        if ph.syl_idx not in seen and ph.accent:
            acc = ACCENT_RE.match(ph.accent)
            seen[ph.syl_idx] = acc.group(0) if acc else ""
    for idx, acc in sorted(seen.items()):
        if acc:
            return idx
    return -1


def _diff_word(w_int: Word, w_dll: Word, out: List[Mismatch]):
    # Sanity: same word? POS may differ even when text matches.
    if w_int.text.lower() != w_dll.text.lower():
        # Words misaligned — skip detailed diff for this pair.
        return
    if w_int.pos != w_dll.pos:
        out.append(Mismatch("pos_tag",
                            f"{w_int.text}: int={w_int.pos} dll={w_dll.pos}"))
    if w_int.stress_lvl != w_dll.stress_lvl:
        out.append(Mismatch("word_stress",
                            f"{w_int.text}: int={w_int.stress_lvl} "
                            f"dll={w_dll.stress_lvl}"))
    syls_i = _phon_syl(w_int)
    syls_d = _phon_syl(w_dll)
    if len(syls_i) != len(syls_d):
        out.append(Mismatch("syl_count",
                            f"{w_int.text}: int={len(syls_i)} dll={len(syls_d)} "
                            f"(int={'-'.join('_'.join(s) for s in syls_i)} "
                            f"dll={'-'.join('_'.join(s) for s in syls_d)})"))
        return
    # Same syl count — check syl-by-syl
    for si in range(len(syls_i)):
        si_i = "_".join(syls_i[si])
        si_d = "_".join(syls_d[si])
        if si_i != si_d:
            out.append(Mismatch("phoneme",
                                f"{w_int.text}[syl{si}]: int={si_i} dll={si_d}"))
    # Accent placement
    ai_int = _accent_syl_idx(w_int)
    ai_dll = _accent_syl_idx(w_dll)
    if ai_int != ai_dll and (ai_int >= 0 or ai_dll >= 0):
        out.append(Mismatch("accent_pos",
                            f"{w_int.text}: int_syl={ai_int} dll_syl={ai_dll}"))


# --------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS)
    ap.add_argument("--vcf",    type=Path, default=DEFAULT_VCF)
    ap.add_argument("--limit",  type=int, default=0,
                    help="If >0, process only the first N text-mode entries.")
    ap.add_argument("--ids", type=str, default="",
                    help="Comma-separated phrase ids to run (default: all text-mode).")
    args = ap.parse_args()

    if not INTERNAL_EXE.exists():
        sys.exit(f"missing: {INTERNAL_EXE}")
    if not DLL_EXE.exists():
        sys.exit(f"missing: {DLL_EXE}")

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    RAW_DIR.mkdir(parents=True, exist_ok=True)

    rows = []
    with args.corpus.open() as f:
        for line in f:
            line = line.strip()
            if not line: continue
            rows.append(json.loads(line))

    if args.ids:
        keep = set(args.ids.split(","))
        rows = [r for r in rows if r["id"] in keep]
    rows = [r for r in rows if r.get("mode", "text") == "text"]
    if args.limit > 0:
        rows = rows[:args.limit]
    print(f"# corpus={args.corpus.name}  rows={len(rows)}")

    category_total = collections.Counter()
    pattern_total  = collections.Counter()
    per_phrase = []
    failed = 0

    for r in rows:
        phrase_id = r["id"]
        text      = r["text"]
        raw_int   = RAW_DIR / f"{phrase_id}.internal.txt"
        raw_dll   = RAW_DIR / f"{phrase_id}.dll.txt"
        try:
            t_int = run_internal(text)
            t_dll = run_dll(text, args.vcf)
        except Exception as e:
            print(f"{phrase_id}: capture failed: {e}", file=sys.stderr)
            failed += 1
            continue
        raw_int.write_text(t_int, encoding="utf-8")
        raw_dll.write_text(t_dll, encoding="utf-8")
        try:
            p_int = parse(t_int)
            p_dll = parse(t_dll)
        except Exception as e:
            print(f"{phrase_id}: parse failed: {e}", file=sys.stderr)
            failed += 1
            continue
        mismatches = diff_phrase(p_int, p_dll)
        per_phrase.append((phrase_id, text, mismatches))
        cat_counts = collections.Counter(m.category for m in mismatches)
        category_total.update(cat_counts)
        for m in mismatches:
            pattern_total[(m.category, m.pattern)] += 1

    # Emit categories.csv
    cats_csv = OUT_DIR / "categories.csv"
    with cats_csv.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["phrase_id", "text", "n_total",
                    "utt_count", "word_count", "syl_count", "phoneme",
                    "accent_pos", "pos_tag", "word_stress", "boundary"])
        for phrase_id, text, ms in per_phrase:
            cc = collections.Counter(m.category for m in ms)
            w.writerow([phrase_id, text, sum(cc.values()),
                        cc["utt_count"], cc["word_count"], cc["syl_count"],
                        cc["phoneme"], cc["accent_pos"], cc["pos_tag"],
                        cc["word_stress"], cc["boundary"]])

    # Summary
    print()
    print(f"# phrases ok={len(per_phrase)}  failed={failed}")
    print("# Category totals (#mismatches across corpus):")
    for cat, n in category_total.most_common():
        print(f"  {cat:<14} {n}")
    print()
    print("# Top 30 individual mismatch patterns:")
    top_path = OUT_DIR / "top_patterns.txt"
    with top_path.open("w", encoding="utf-8") as f:
        for (cat, pat), n in pattern_total.most_common():
            line = f"{n:>5}  [{cat:<12}] {pat}"
            f.write(line + "\n")
        f.write("\n# end\n")
    for (cat, pat), n in pattern_total.most_common(30):
        print(f"  {n:>4}  [{cat:<12}] {pat}")

    print()
    print(f"# wrote: {cats_csv}")
    print(f"# wrote: {top_path}")
    print(f"# raw streams: {RAW_DIR}")


if __name__ == "__main__":
    main()
