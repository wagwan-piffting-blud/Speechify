#!/usr/bin/env python3
"""
tts_triage.py — Triage driver for a Ghidra JSONL export of a TTS DLL.

Reads the JSONL emitted by export_analysis_v2.java into SQLite, applies rule-
based tagging, finds anchor (seed) functions, expands a call-graph
neighborhood, bins everything into tiers 0-3, and emits reports/briefs/LLM
batches.

Typical first pass (run in order):

    python tts_triage.py ingest    export.jsonl analysis.db
    python tts_triage.py tag       analysis.db
    python tts_triage.py seed      analysis.db
    python tts_triage.py neighbors analysis.db --hops 2
    python tts_triage.py tier      analysis.db
    python tts_triage.py report    analysis.db --out ./reports
    python tts_triage.py brief     analysis.db --out ./reports/tier3
    python tts_triage.py batch     analysis.db --tier 2 --out tier2_batch.jsonl

DEFAULT_SEED_TERMS in this file is calibrated to the Speechify Front-end DLL.
Override with --terms if you want — but DO NOT pass terms shorter than 3 chars
(`f0`, `g2p`, `lts`) unless you know they're rare in your binary, or they will
substring-match against tons of `FUN_xxxxxxxx` auto-generated names. The
script filters those out by default, but very short terms can still match
real strings in noisy ways.

Ad-hoc inspection:

    python tts_triage.py stats   analysis.db
    python tts_triage.py grep    analysis.db cart
    python tts_triage.py show    analysis.db 180123abc

Re-running tag/seed/neighbors/tier is idempotent — they wipe their own tables.
Edit TAG_RULES below or pass --terms to seed to refine the analysis.
"""

import argparse
import json
import os
import re
import sqlite3
import sys
import time
from collections import defaultdict, deque
from pathlib import Path


# ─────────────────────────────────────────────────────────────────────────────
# Schema
# ─────────────────────────────────────────────────────────────────────────────

SCHEMA = """
CREATE TABLE IF NOT EXISTS program (
    program          TEXT,
    executable_path  TEXT,
    language         TEXT,
    compiler         TEXT,
    image_base       TEXT,
    pointer_size     INTEGER,
    memory_blocks    TEXT
);

CREATE TABLE IF NOT EXISTS strings (
    address  TEXT PRIMARY KEY,
    length   INTEGER,
    type     TEXT,
    xrefs    INTEGER,
    value    TEXT
);

CREATE TABLE IF NOT EXISTS functions (
    address              TEXT PRIMARY KEY,
    name                 TEXT,
    size                 INTEGER,
    instructions         INTEGER,
    basic_blocks         INTEGER,
    params               INTEGER,
    calling_convention   TEXT,
    signature            TEXT,
    frame_size           INTEGER,
    decompile_ok         INTEGER,
    decompiled           TEXT
);

CREATE TABLE IF NOT EXISTS callers (
    callee_addr  TEXT,
    caller_addr  TEXT,
    PRIMARY KEY (callee_addr, caller_addr)
);

CREATE TABLE IF NOT EXISTS callees (
    caller_addr  TEXT,
    callee_name  TEXT,
    callee_addr  TEXT,
    PRIMARY KEY (caller_addr, callee_addr, callee_name)
);

CREATE TABLE IF NOT EXISTS string_refs (
    func_addr    TEXT,
    instr_addr   TEXT,
    target_addr  TEXT,
    encoding     TEXT,
    value        TEXT
);

CREATE TABLE IF NOT EXISTS tags (
    func_addr  TEXT,
    tag        TEXT,
    PRIMARY KEY (func_addr, tag)
);

CREATE TABLE IF NOT EXISTS tiers (
    func_addr  TEXT PRIMARY KEY,
    tier       INTEGER,
    reason     TEXT
);

CREATE TABLE IF NOT EXISTS seeds (
    func_addr     TEXT PRIMARY KEY,
    matched_term  TEXT,
    matched_in    TEXT
);

CREATE TABLE IF NOT EXISTS neighborhood (
    func_addr   TEXT PRIMARY KEY,
    hops        INTEGER,
    seed_addr   TEXT
);

CREATE INDEX IF NOT EXISTS idx_strings_value   ON strings(value);
CREATE INDEX IF NOT EXISTS idx_strrefs_func    ON string_refs(func_addr);
CREATE INDEX IF NOT EXISTS idx_strrefs_value   ON string_refs(value);
CREATE INDEX IF NOT EXISTS idx_callers_caller  ON callers(caller_addr);
CREATE INDEX IF NOT EXISTS idx_callees_callee  ON callees(callee_addr);
CREATE INDEX IF NOT EXISTS idx_tags_tag        ON tags(tag);
CREATE INDEX IF NOT EXISTS idx_func_size       ON functions(size);
"""


# ─────────────────────────────────────────────────────────────────────────────
# Tagging rules
# ─────────────────────────────────────────────────────────────────────────────
#
# Each rule is (tag_name, predicates). Predicates are conjunctive within a
# rule, but `any_*` predicates match if ANY entry matches. All substring
# checks are case-insensitive. Edit freely.
#
# Predicate keys:
#   any_substr_in_strings     match if any string ref value contains substr
#   all_substr_in_strings     match if every substr is present somewhere
#   any_substr_in_callees     match if any callee name contains substr
#   any_substr_in_name        match if function name contains substr
#   any_re_in_name            match if function name matches any regex
#   any_re_in_decompile       match if decompiled C matches any regex
#   min_size, max_size        size in bytes
#   min_instructions, max_instructions
#   decompile_ok              bool

TAG_RULES = [
    # ─── Speechify-specific (PE id'd as SWIttsFe-en-US 3.0.5.5040) ───
    ("speechify.core", {
        "any_substr_in_strings": [
            "Speechify", "SpeechifyInput", "SWIttsFe",
            "salsa", "enu.syn", "enu.ddl", "audio.cdv",
        ],
    }),
    ("speechify.delta", {
        # Internal "Delta" scripting language + LFILE/LOGIO plumbing.
        # Looks like the toolchain that builds voice DBs.
        "any_substr_in_strings": [
            "delta insert", "delta project", "delta delete",
            "DeltaTools", "DELTIO", "LFILE", "LOGIO",
            "pgmin", "pgmout", "cmdin", "cmdout",
            "wordsin", "wordsout", "errorout", "execfile",
            "consprout", "sprout", "prmout", "prompt",
        ],
    }),
    ("eloquence.eci", {
        # Output-format compatibility, NOT Eloquence-the-engine.
        "any_substr_in_strings": [
            "ECIoutput", "Eloquence output", "Eloquence program output",
            "Concatenative ECI", "ECI Output",
        ],
    }),

    # ─── Synthesis backends ───
    ("tts.klatt", {
        "any_substr_in_strings": [
            "klatt", "KlattOpen", "KlattSetConstParms",
            "vocal_tract", "wind_size", "breathi",
            "gain_fac", "stretch_fac", "PSgain",
            "Midline", "Rangeval", "diaph_ghost", "diplo",
        ],
    }),
    ("tts.concatenative", {
        "any_substr_in_strings": [
            "Concatenative", "halfphone", "half_phone",
            "diphone", "triphone", "audio.cdv",
            "viterbi", "beam", "lattice", "candidate", "n_best", "n-best",
        ],
    }),
    ("tts.unit_db", {
        "any_substr_in_strings": [
            ".vdb", ".vin", ".vcf", ".pho", ".cart", ".idx",
            ".syn", ".ddl", ".cdv",
            "unit_db", "voice_db", "voice_data",
        ],
    }),

    # ─── Prosody (Speechify uses 2-tier A/B declination + ToBI tones) ───
    ("tts.declination", {
        "any_substr_in_strings": [
            "ADecln", "BDecln", "ADeclnScale", "BDeclnScale",
            "ADeclnLevel", "BDeclnLevel", "Midline", "Rangeval",
        ],
    }),
    ("tts.tone", {
        "any_substr_in_strings": [
            "bound_tone", "phr_tone", "nuc_tone",
            "inton_phr", "intonation", "tobi",
            "port_rate", "def_rate", "port_vol", "def_vol",
            "brk_priority", "acc_valu", "d_step", "stress_level",
        ],
    }),
    ("tts.f0", {
        "any_substr_in_strings": ["f0", "pitch", "fundamental"],
    }),
    ("tts.duration", {
        "any_substr_in_strings": [
            "duration", "dur_model", "syllable_dur",
        ],
    }),

    # ─── Phonetic features (= concatenation-cost dimensions) ───
    ("tts.phonetic_features", {
        "any_substr_in_strings": [
            "place_of_artic", "manner_of_artic", "sonority",
            "voicing", "tvoic", "~voic", "backness", "front", "tense",
            "glide", "stress", "syllable", "phone", "phoneme",
            "phonesAssigned", "transition", "after",
            "acute_acc", "eow_dlmtr",
        ],
    }),

    # ─── Cost / search (likely present, vocabulary may differ) ───
    ("tts.cost", {
        "any_substr_in_strings": [
            "target_cost", "concat_cost", "join_cost",
            "spectral_dist", "transition_cost",
        ],
    }),
    ("tts.cart", {
        "any_substr_in_strings": [
            "cart", "decision_tree", "decision tree", "cluster_unit",
            "tree_node", "leaf_node",
        ],
    }),

    # ─── Linguistic frontend ───
    ("tts.dictionary", {
        # Note: this DLL has at least 7 dict types. C++ class names visible:
        # UserDict, DictionarySet (with prioritizedLookup, activate, etc.)
        "any_substr_in_strings": [
            "userdict", "rootdict", "worddict", "hugedict",
            "abbrdict", "maindict", "disambigDict",
            "UserDict::", "DictionarySet::",
        ],
    }),
    ("tts.pos", {
        # Speechify has a rich POS/morphology system feeding disambigDict
        "any_substr_in_strings": [
            "noun_verb_adj", "noun_verb", "noun_adj", "verb_adj", "adj_adv",
            "noun_verb_s", "subj_obj", "poss_nom", "obj_poss",
            "be_poss_nom", "be_poss", "hav_modal",
            "quantif", "nomposs", "modal", "indef", "foreign",
            "coord", "subord", "postpos", "interj", "auxil",
            "contr", "negat", "proper", "letname",
            "category", "subcat", "pl_type", "noun_verb_s",
        ],
    }),
    ("tts.lexicon", {
        "any_substr_in_strings": [
            ".lex", "lexicon", "g2p", "letter_to_sound", "lts",
            "arpabet", "sampa", "ipa", "cmu",
            "letter_type", "character_type", "letcase",
        ],
    }),
    ("tts.text_norm", {
        "any_substr_in_strings": [
            "normalize", "tokeniz", "abbrev", "expand", "ssml",
            "vtml", "number_word", "currency", "ordinal", "cardinal",
            "fraction", "digit", "delimiter", "afterslash", "punct",
            "abbreviation",
        ],
    }),
    ("tts.signal", {
        # Generic signal processing (kept separate from tts.klatt)
        "any_substr_in_strings": [
            "lpc", "mfcc", "mel_cepstr", "mel-cepstr", "formant",
            "psola", "wsola", "hnm", "lsf", "lsp",
            "pitch_mark", "pitch mark", "epoch", "glottal",
        ],
    }),
    ("tts.engine", {
        "any_substr_in_strings": [
            "synthes", "frontend", "front_end", "front-end",
            "backend", "back_end", "back-end", "tts",
        ],
    }),

    # ─── Plumbing / runtime ───
    ("io.file", {
        "any_substr_in_callees": [
            "fopen", "_open", "CreateFileA", "CreateFileW",
            "ReadFile", "WriteFile", "fread", "fwrite", "fclose",
            "MapViewOfFile", "CreateFileMapping",
        ],
    }),
    ("io.registry", {
        "any_substr_in_callees": [
            "RegOpenKey", "RegQueryValue", "RegCloseKey",
            "RegEnumKey", "RegSetValue",
        ],
    }),
    ("io.string", {
        "any_substr_in_callees": [
            "strcpy", "strncpy", "strcat", "wcscpy", "wcsncpy", "wcscat",
            "sprintf", "wsprintf", "_snprintf", "_vsnprintf",
        ],
    }),
    ("math.heavy", {
        "any_substr_in_callees": [
            "exp", "log", "sqrt", "pow", "fabs", "atan", "sin", "cos",
        ],
    }),

    # ─── Structural heuristics (size/decompile) ───
    ("struct.tiny",     {"max_size": 16}),
    ("struct.thunk",    {"max_instructions": 3}),
    ("struct.small",    {"min_size": 17, "max_size": 199}),
    ("struct.medium",   {"min_size": 200, "max_size": 1999}),
    ("struct.large",    {"min_size": 2000, "max_size": 7999}),
    ("struct.huge",     {"min_size": 8000}),
    ("struct.no_decomp",{"decompile_ok": False}),

    # ─── Name-based runtime/CRT identification (Tier-0 fast path) ───
    ("name.runtime", {
        "any_re_in_name": [
            r"^_?_?(malloc|free|calloc|realloc|memcpy|memset|memmove|memcmp)\b",
            r"^_?_?(strlen|strcmp|strcpy|strcat|strncmp|wcslen|wcscmp)\b",
            r"^_?_?(operator new|operator delete)",
            r"^__security_",
            r"^__chkstk",
            r"^_CxxThrowException",
            r"^_?_except_handler",
            r"^__scrt_",
            r"^__GS",
            r"^__std_",
            r"^_?atexit\b",
        ],
    }),
    ("name.dllmain", {
        "any_re_in_name": [r"^_?DllMain", r"^DllEntryPoint"],
    }),
]


# ─────────────────────────────────────────────────────────────────────────────
# Default seed terms (for `seed` if --terms not provided)
# ─────────────────────────────────────────────────────────────────────────────

DEFAULT_SEED_TERMS = [
    # Klatt-specific (confirmed present in this DLL)
    "klatt", "KlattOpen", "KlattSetConstParms",
    "vocal_tract", "PSgain", "Midline", "Rangeval", "breathi", "diaph_ghost",
    # Concatenative (suspected; vocabulary may differ — adjust as you find more)
    "halfphone", "diphone", "triphone", "Concatenative",
    "viterbi", "beam_search", "lattice", "candidate",
    "target_cost", "concat_cost", "join_cost",
    "cart", "decision_tree", "cluster_unit",
    # Prosody (confirmed: 2-tier declination + ToBI tones)
    "ADecln", "BDecln", "inton_phr",
    "bound_tone", "phr_tone", "nuc_tone", "brk_priority",
    # Phonetic features (confirmed — these are the cost dimensions)
    "place_of_artic", "manner_of_artic", "sonority", "voicing", "backness",
    # Dictionaries (confirmed — C++ classes UserDict, DictionarySet)
    "disambigDict", "UserDict", "DictionarySet",
    "abbrdict", "maindict", "hugedict", "rootdict",
    # POS (confirmed — rich inventory feeding disambigDict)
    "noun_verb", "subj_obj", "poss_nom", "be_poss", "hav_modal",
    # Speechify-specific
    "Speechify", "SpeechifyInput", "audio.cdv", "enu.syn", "enu.ddl",
    # DeltaTools internal scripting (toolchain leak — possibly built the voice DBs)
    "delta insert", "delta project", "DeltaTools", "DELTIO",
    # Generic anchors
    "f0", "pitch", "duration", "prosody",
    "g2p", "letter_to_sound",
    ".vdb", ".vin", ".vcf", ".pho",
]


# ─────────────────────────────────────────────────────────────────────────────
# DB helpers
# ─────────────────────────────────────────────────────────────────────────────

def open_db(path):
    db = sqlite3.connect(path)
    db.executescript(SCHEMA)
    db.execute("PRAGMA journal_mode = WAL")
    db.execute("PRAGMA synchronous = NORMAL")
    return db


def wipe(db, tables):
    for t in tables:
        db.execute(f"DELETE FROM {t}")
    db.commit()


def progress(label, n, every=1000, t0=None):
    if t0 is None:
        return
    if n % every == 0:
        dt = time.time() - t0
        rate = n / dt if dt > 0 else 0
        print(f"  {label}: {n:>8}  ({rate:,.0f}/s)", file=sys.stderr)


# ─────────────────────────────────────────────────────────────────────────────
# ingest
# ─────────────────────────────────────────────────────────────────────────────

def cmd_ingest(args):
    db = open_db(args.db)
    wipe(db, ["program", "strings", "functions", "callers",
              "callees", "string_refs", "tags", "tiers",
              "seeds", "neighborhood"])

    n_str = n_fn = 0
    t0 = time.time()
    cur = db.cursor()

    func_buf, caller_buf, callee_buf, sref_buf = [], [], [], []
    BATCH = 500

    def flush():
        nonlocal func_buf, caller_buf, callee_buf, sref_buf
        if func_buf:
            cur.executemany(
                "INSERT OR REPLACE INTO functions VALUES (?,?,?,?,?,?,?,?,?,?,?)",
                func_buf)
        if caller_buf:
            cur.executemany(
                "INSERT OR IGNORE INTO callers VALUES (?,?)", caller_buf)
        if callee_buf:
            cur.executemany(
                "INSERT OR IGNORE INTO callees VALUES (?,?,?)", callee_buf)
        if sref_buf:
            cur.executemany(
                "INSERT INTO string_refs VALUES (?,?,?,?,?)", sref_buf)
        func_buf, caller_buf, callee_buf, sref_buf = [], [], [], []
        db.commit()

    with open(args.jsonl, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError as e:
                print(f"  WARN: bad JSON line skipped ({e})", file=sys.stderr)
                continue
            kind = rec.get("record")

            if kind == "header":
                cur.execute(
                    "INSERT INTO program VALUES (?,?,?,?,?,?,?)",
                    (rec.get("program"), rec.get("executable_path"),
                     rec.get("language"), rec.get("compiler"),
                     rec.get("image_base"), rec.get("pointer_size"),
                     json.dumps(rec.get("memory_blocks", []))))

            elif kind == "string":
                cur.execute(
                    "INSERT OR REPLACE INTO strings VALUES (?,?,?,?,?)",
                    (rec["address"], rec.get("length"), rec.get("type"),
                     rec.get("xrefs"), rec.get("value")))
                n_str += 1
                progress("strings", n_str, every=5000, t0=t0)

            elif kind == "function":
                addr = rec["address"]
                func_buf.append((
                    addr, rec.get("name"), rec.get("size", 0),
                    rec.get("instructions", 0), rec.get("basic_blocks", 0),
                    rec.get("params", 0), rec.get("calling_convention"),
                    rec.get("signature"), rec.get("frame_size", 0),
                    1 if rec.get("decompile_ok") else 0,
                    rec.get("decompiled", "")))
                for ca in rec.get("callers", []):
                    caller_buf.append((addr, ca))
                for ce in rec.get("callees", []):
                    callee_buf.append(
                        (addr, ce.get("name", "?"), ce.get("address")))
                for sr in rec.get("string_refs", []):
                    sref_buf.append((
                        addr, sr.get("instr"), sr.get("target"),
                        sr.get("encoding"), sr.get("value")))
                n_fn += 1
                if n_fn % BATCH == 0:
                    flush()
                progress("functions", n_fn, every=500, t0=t0)

    flush()
    print(f"\nIngested: {n_str} strings, {n_fn} functions in {time.time()-t0:.1f}s",
          file=sys.stderr)


# ─────────────────────────────────────────────────────────────────────────────
# tag
# ─────────────────────────────────────────────────────────────────────────────

def _matches(rule, fn_row, strings, callees, name, decomp):
    """Evaluate a rule's predicates against a function's data."""
    pred = rule
    for key, val in pred.items():
        if key == "any_substr_in_strings":
            hay = " ".join(strings).lower()
            if not any(s.lower() in hay for s in val):
                return False
        elif key == "all_substr_in_strings":
            hay = " ".join(strings).lower()
            if not all(s.lower() in hay for s in val):
                return False
        elif key == "any_substr_in_callees":
            hay = " ".join(callees).lower()
            if not any(s.lower() in hay for s in val):
                return False
        elif key == "any_substr_in_name":
            n = (name or "").lower()
            if not any(s.lower() in n for s in val):
                return False
        elif key == "any_re_in_name":
            n = name or ""
            if not any(re.search(p, n) for p in val):
                return False
        elif key == "any_re_in_decompile":
            d = decomp or ""
            if not any(re.search(p, d, re.IGNORECASE) for p in val):
                return False
        elif key == "min_size":
            if (fn_row["size"] or 0) < val: return False
        elif key == "max_size":
            if (fn_row["size"] or 0) > val: return False
        elif key == "min_instructions":
            if (fn_row["instructions"] or 0) < val: return False
        elif key == "max_instructions":
            if (fn_row["instructions"] or 0) > val: return False
        elif key == "decompile_ok":
            if bool(fn_row["decompile_ok"]) != bool(val): return False
    return True


def cmd_tag(args):
    db = open_db(args.db)
    wipe(db, ["tags"])

    cur = db.cursor()
    fns = cur.execute(
        "SELECT address, name, size, instructions, decompile_ok, decompiled "
        "FROM functions").fetchall()
    print(f"Tagging {len(fns)} functions…", file=sys.stderr)

    tag_count = defaultdict(int)
    out = []
    t0 = time.time()
    for i, row in enumerate(fns):
        addr, name, size, ins, dok, decomp = row
        fn = {"size": size, "instructions": ins, "decompile_ok": dok}
        # Pull strings + callees once per function
        strings = [r[0] for r in db.execute(
            "SELECT value FROM string_refs WHERE func_addr=?", (addr,)).fetchall()]
        callees = [r[0] for r in db.execute(
            "SELECT callee_name FROM callees WHERE caller_addr=?", (addr,)).fetchall()]

        for tag_name, pred in TAG_RULES:
            if _matches(pred, fn, strings, callees, name, decomp):
                out.append((addr, tag_name))
                tag_count[tag_name] += 1
        progress("tagged", i + 1, every=500, t0=t0)

    cur.executemany("INSERT OR IGNORE INTO tags VALUES (?,?)", out)
    db.commit()
    print(f"\nApplied {len(out)} tag assignments across {len(tag_count)} tags",
          file=sys.stderr)
    for t, n in sorted(tag_count.items(), key=lambda x: -x[1]):
        print(f"  {n:>6}  {t}", file=sys.stderr)


# ─────────────────────────────────────────────────────────────────────────────
# seed
# ─────────────────────────────────────────────────────────────────────────────

def _is_autoname(n):
    """Ghidra auto-generated names are just the address as hex, so substring-
    matching them is matching the address. Worthless for seed discovery."""
    if not n:
        return True
    return (n.startswith("FUN_") or n.startswith("thunk_FUN_")
            or n.startswith("LAB_") or n.startswith("SUB_")
            or n.startswith("UndefinedFunction_"))


def cmd_seed(args):
    db = open_db(args.db)
    wipe(db, ["seeds"])
    terms = (args.terms.split(",") if args.terms
             else list(DEFAULT_SEED_TERMS))
    terms = [t.strip().lower() for t in terms if t.strip()]

    # Warn on dangerously short / hex-like terms — these tend to false-match
    # against auto-generated FUN_<hex> names if those aren't filtered out.
    short = [t for t in terms if len(t) < 3]
    if short:
        print(f"  WARN: short terms may false-match: {short}", file=sys.stderr)

    seeds = {}  # addr -> (term, where)

    # Match by string ref (the most reliable signal)
    for term in terms:
        rows = db.execute(
            "SELECT DISTINCT func_addr, value FROM string_refs "
            "WHERE LOWER(value) LIKE ?", (f"%{term}%",)).fetchall()
        for addr, val in rows:
            seeds.setdefault(addr, (term, f"string:{val[:64]}"))

    # Match by function name — but skip Ghidra auto-names (FUN_xxxxxxxx etc.)
    for term in terms:
        rows = db.execute(
            "SELECT address, name FROM functions WHERE LOWER(name) LIKE ?",
            (f"%{term}%",)).fetchall()
        for addr, name in rows:
            if _is_autoname(name):
                continue
            seeds.setdefault(addr, (term, f"name:{name}"))

    # Match by callee name — same auto-name filter
    for term in terms:
        rows = db.execute(
            "SELECT DISTINCT caller_addr, callee_name FROM callees "
            "WHERE LOWER(callee_name) LIKE ?", (f"%{term}%",)).fetchall()
        for addr, cn in rows:
            if _is_autoname(cn):
                continue
            seeds.setdefault(addr, (term, f"callee:{cn}"))

    db.executemany("INSERT OR REPLACE INTO seeds VALUES (?,?,?)",
                   [(a, t, w) for a, (t, w) in seeds.items()])
    db.commit()

    # Diagnostics: which terms actually contributed?
    by_term = defaultdict(int)
    by_src  = defaultdict(int)
    for addr, (term, where) in seeds.items():
        by_term[term] += 1
        by_src[where.split(":", 1)[0]] += 1

    print(f"Seeds: {len(seeds)} (terms: {', '.join(terms)})", file=sys.stderr)
    print("  by source:", dict(by_src), file=sys.stderr)
    if by_term:
        print("  by term:", file=sys.stderr)
        for t, n in sorted(by_term.items(), key=lambda x: -x[1]):
            print(f"    {n:>4}  {t}", file=sys.stderr)
    zero_terms = [t for t in terms if t not in by_term]
    if zero_terms:
        print(f"  no matches: {zero_terms}", file=sys.stderr)
    for addr, (term, where) in list(seeds.items())[:30]:
        name = db.execute("SELECT name FROM functions WHERE address=?",
                          (addr,)).fetchone()
        print(f"  {addr}  [{term:<16}]  {name[0] if name else '?'}",
              file=sys.stderr)
    if len(seeds) > 30:
        print(f"  … +{len(seeds)-30} more", file=sys.stderr)


# ─────────────────────────────────────────────────────────────────────────────
# neighbors
# ─────────────────────────────────────────────────────────────────────────────

def cmd_neighbors(args):
    db = open_db(args.db)
    wipe(db, ["neighborhood"])

    seed_addrs = [r[0] for r in db.execute("SELECT func_addr FROM seeds")]
    if not seed_addrs:
        print("No seeds — run `seed` first.", file=sys.stderr)
        return

    # Build forward call graph (caller -> set of callees by ADDRESS only)
    fwd = defaultdict(set)
    for caller, _, callee in db.execute(
            "SELECT caller_addr, callee_name, callee_addr FROM callees"):
        if callee:
            fwd[caller].add(callee)

    # And the reverse (for `--reverse`)
    rev = defaultdict(set)
    for callee, caller in db.execute(
            "SELECT callee_addr, caller_addr FROM callers"):
        rev[callee].add(caller)

    graph = rev if args.reverse else fwd
    direction = "callers" if args.reverse else "callees"

    visited = {}  # addr -> (hops, seed_addr)
    for s in seed_addrs:
        visited[s] = (0, s)
        q = deque([(s, 0)])
        while q:
            cur, h = q.popleft()
            if h >= args.hops:
                continue
            for nxt in graph.get(cur, ()):
                if nxt in visited and visited[nxt][0] <= h + 1:
                    continue
                visited[nxt] = (h + 1, s)
                q.append((nxt, h + 1))

    db.executemany("INSERT OR REPLACE INTO neighborhood VALUES (?,?,?)",
                   [(a, h, s) for a, (h, s) in visited.items()])
    db.commit()

    by_hop = defaultdict(int)
    for _, (h, _) in visited.items():
        by_hop[h] += 1
    print(f"Neighborhood ({direction}, hops≤{args.hops}): "
          f"{len(visited)} functions", file=sys.stderr)
    for h in sorted(by_hop):
        print(f"  hop {h}: {by_hop[h]}", file=sys.stderr)


# ─────────────────────────────────────────────────────────────────────────────
# tier
# ─────────────────────────────────────────────────────────────────────────────

def cmd_tier(args):
    db = open_db(args.db)
    wipe(db, ["tiers"])

    cur = db.cursor()
    # Pre-fetch tag map
    tags_by_addr = defaultdict(set)
    for addr, tag in cur.execute("SELECT func_addr, tag FROM tags"):
        tags_by_addr[addr].add(tag)
    seed_set = set(r[0] for r in cur.execute("SELECT func_addr FROM seeds"))
    nbr = {a: h for a, h in cur.execute(
        "SELECT func_addr, hops FROM neighborhood")}

    out = []
    for row in cur.execute(
            "SELECT address, size, instructions, decompile_ok FROM functions"):
        addr, size, ins, dok = row
        tags = tags_by_addr.get(addr, set())
        tts_tags = {t for t in tags if t.startswith("tts.")
                                    or t.startswith("eloquence.")}

        tier, reason = 1, "default"

        # Tier 0: junk
        if "name.runtime" in tags:
            tier, reason = 0, "runtime"
        elif "struct.tiny" in tags or "struct.thunk" in tags:
            tier, reason = 0, "tiny/thunk"
        elif not dok and (size or 0) < 50:
            tier, reason = 0, "no-decomp+tiny"

        # Tier 3: deep dive
        elif addr in seed_set:
            tier, reason = 3, "seed"
        elif nbr.get(addr, 99) <= 1 and tts_tags:
            tier, reason = 3, "1-hop+tts"
        elif len(tts_tags) >= 3:
            tier, reason = 3, "3+ tts tags"
        elif "struct.huge" in tags and tts_tags:
            tier, reason = 3, "huge+tts"

        # Tier 2: paragraph
        elif tts_tags:
            tier, reason = 2, "tts tag"
        elif addr in nbr:
            tier, reason = 2, f"nbr hop {nbr[addr]}"
        elif "struct.large" in tags or "struct.huge" in tags:
            tier, reason = 2, "large fn"

        out.append((addr, tier, reason))

    cur.executemany("INSERT OR REPLACE INTO tiers VALUES (?,?,?)", out)
    db.commit()

    counts = defaultdict(int)
    for _, t, _ in out:
        counts[t] += 1
    print(f"Tiered {len(out)} functions:", file=sys.stderr)
    for t in (0, 1, 2, 3):
        print(f"  tier {t}: {counts[t]:>6}", file=sys.stderr)


# ─────────────────────────────────────────────────────────────────────────────
# report
# ─────────────────────────────────────────────────────────────────────────────

def cmd_report(args):
    db = open_db(args.db)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    prog = db.execute("SELECT * FROM program").fetchone()
    n_fn  = db.execute("SELECT COUNT(*) FROM functions").fetchone()[0]
    n_str = db.execute("SELECT COUNT(*) FROM strings").fetchone()[0]

    # Tier counts
    tier_counts = dict(db.execute(
        "SELECT tier, COUNT(*) FROM tiers GROUP BY tier").fetchall())
    # Tag counts
    tag_counts = db.execute(
        "SELECT tag, COUNT(*) FROM tags GROUP BY tag ORDER BY 2 DESC").fetchall()

    summary = out / "summary.md"
    with summary.open("w", encoding="utf-8") as f:
        f.write("# Triage Summary\n\n")
        if prog:
            f.write(f"**Program:** `{prog[0]}`  \n")
            f.write(f"**Path:** `{prog[1]}`  \n")
            f.write(f"**Language:** `{prog[2]}`  \n")
            f.write(f"**Compiler:** `{prog[3]}`  \n")
            f.write(f"**Image base:** `{prog[4]}`  \n")
            f.write(f"**Pointer size:** {prog[5]}  \n\n")
        f.write(f"**Functions:** {n_fn:,}  \n")
        f.write(f"**Strings:** {n_str:,}  \n\n")
        f.write("## Tier distribution\n\n")
        f.write("| Tier | Count | Meaning |\n|---|---|---|\n")
        meanings = {0: "skip (junk)", 1: "auto-tag only", 2: "paragraph (LLM)",
                    3: "deep dive"}
        for t in (0, 1, 2, 3):
            f.write(f"| {t} | {tier_counts.get(t, 0):,} | {meanings[t]} |\n")
        f.write("\n## Tag counts\n\n")
        f.write("| Tag | Count |\n|---|---|\n")
        for tag, n in tag_counts:
            f.write(f"| `{tag}` | {n:,} |\n")

        # Top seed candidates
        f.write("\n## Seed functions\n\n")
        seeds = db.execute(
            "SELECT s.func_addr, s.matched_term, s.matched_in, f.name, f.size "
            "FROM seeds s JOIN functions f ON s.func_addr=f.address "
            "ORDER BY f.size DESC LIMIT 100").fetchall()
        f.write("| Address | Name | Size | Term | Match |\n"
                "|---|---|---|---|---|\n")
        for addr, term, where, name, size in seeds:
            f.write(f"| `{addr}` | `{name}` | {size} | `{term}` | "
                    f"`{(where or '')[:60]}` |\n")

        # Tier-3 listing
        f.write("\n## Tier 3 (deep dive)\n\n")
        rows = db.execute(
            "SELECT t.func_addr, t.reason, f.name, f.size "
            "FROM tiers t JOIN functions f ON t.func_addr=f.address "
            "WHERE t.tier=3 ORDER BY f.size DESC").fetchall()
        f.write(f"_{len(rows)} functions_\n\n")
        f.write("| Address | Name | Size | Reason |\n|---|---|---|---|\n")
        for addr, reason, name, size in rows:
            f.write(f"| `{addr}` | `{name}` | {size} | {reason} |\n")

    print(f"Wrote {summary}", file=sys.stderr)


# ─────────────────────────────────────────────────────────────────────────────
# brief — per-function markdown for tier 3
# ─────────────────────────────────────────────────────────────────────────────

def cmd_brief(args):
    db = open_db(args.db)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    rows = db.execute(
        "SELECT f.address, f.name, f.size, f.instructions, f.basic_blocks, "
        "f.params, f.calling_convention, f.signature, f.decompiled, "
        "t.reason "
        "FROM functions f JOIN tiers t ON f.address=t.func_addr "
        f"WHERE t.tier={args.tier}").fetchall()
    print(f"Writing {len(rows)} tier-{args.tier} briefs to {out}…",
          file=sys.stderr)

    for r in rows:
        addr, name, size, ins, bb, params, cc, sig, decomp, reason = r
        safe_name = re.sub(r"[^A-Za-z0-9_.-]", "_", name or "anon")[:60]
        path = out / f"{addr}_{safe_name}.md"
        tags = [x[0] for x in db.execute(
            "SELECT tag FROM tags WHERE func_addr=?", (addr,)).fetchall()]
        callers = db.execute(
            "SELECT c.caller_addr, f.name FROM callers c "
            "LEFT JOIN functions f ON c.caller_addr=f.address "
            "WHERE c.callee_addr=?", (addr,)).fetchall()
        callees = db.execute(
            "SELECT callee_name, callee_addr FROM callees "
            "WHERE caller_addr=?", (addr,)).fetchall()
        srefs = db.execute(
            "SELECT target_addr, encoding, value FROM string_refs "
            "WHERE func_addr=?", (addr,)).fetchall()

        with path.open("w", encoding="utf-8") as f:
            f.write(f"# `{name}` @ `{addr}`\n\n")
            f.write(f"- **Tier reason:** {reason}\n")
            f.write(f"- **Size:** {size} bytes / {ins} instr / {bb} BBs\n")
            f.write(f"- **Params:** {params} / `{cc}`\n")
            f.write(f"- **Signature:** `{sig}`\n")
            if tags:
                f.write(f"- **Tags:** {', '.join(f'`{t}`' for t in tags)}\n")
            f.write("\n## Callers\n\n")
            if callers:
                for ca, cname in callers:
                    f.write(f"- `{ca}`  `{cname or '?'}`\n")
            else:
                f.write("_(none)_\n")
            f.write("\n## Callees\n\n")
            if callees:
                for cn, caddr in callees:
                    f.write(f"- `{caddr or '?'}`  `{cn}`\n")
            else:
                f.write("_(none)_\n")
            f.write("\n## String references\n\n")
            if srefs:
                for ta, enc, val in srefs:
                    f.write(f"- `{ta}` ({enc}): {val}\n")
            else:
                f.write("_(none)_\n")
            f.write("\n## Decompiled\n\n```c\n")
            f.write(decomp or "// (no decompilation)")
            f.write("\n```\n")
    print(f"Wrote {len(rows)} briefs", file=sys.stderr)


# ─────────────────────────────────────────────────────────────────────────────
# batch — emit JSONL ready to feed to an LLM
# ─────────────────────────────────────────────────────────────────────────────

PROMPT_TEMPLATE = """\
You are reverse-engineering a Text-to-Speech engine DLL. Summarise the function
below in this exact format (one line per field, no extra prose):

PURPOSE: <one-line summary; favour terms like target_cost, concat_cost,
          cart_walk, viterbi, beam_step, unit_load, lex_lookup, g2p, f0_predict,
          duration_predict, text_norm, prosody, formant, klatt, psola, wsola>
INPUTS:  <param meanings if discernible, else `?`>
OUTPUTS: <return value meaning + side effects, else `?`>
KEY_OPS: <comma-separated list of key operations: float math, table lookup,
          loop over array, recursion, error path, etc.>
CALLS:   <names of callees that matter, comma-separated>
NOTES:   <anything weird: magic numbers, hardcoded thresholds, file format
          parsing, vtable dispatch>

Function:
  Name: {name}
  Address: {address}
  Signature: {signature}
  Size: {size} bytes, {instructions} instructions, {basic_blocks} basic blocks
  Tags: {tags}
  Callers ({n_callers}): {callers}
  Callees ({n_callees}): {callees}
  String refs: {string_refs}

Decompiled C:
```c
{decompiled}
```
"""


def cmd_batch(args):
    db = open_db(args.db)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)

    rows = db.execute(
        "SELECT f.address, f.name, f.size, f.instructions, f.basic_blocks, "
        "f.signature, f.decompiled "
        "FROM functions f JOIN tiers t ON f.address=t.func_addr "
        f"WHERE t.tier={args.tier}").fetchall()

    n = 0
    with out.open("w", encoding="utf-8") as fh:
        for addr, name, size, ins, bb, sig, decomp in rows:
            tags = [r[0] for r in db.execute(
                "SELECT tag FROM tags WHERE func_addr=?", (addr,)).fetchall()]
            callers = [r[0] for r in db.execute(
                "SELECT caller_addr FROM callers WHERE callee_addr=?",
                (addr,)).fetchall()]
            callees = [r[0] for r in db.execute(
                "SELECT callee_name FROM callees WHERE caller_addr=?",
                (addr,)).fetchall()]
            srefs = [r[0] for r in db.execute(
                "SELECT value FROM string_refs WHERE func_addr=?",
                (addr,)).fetchall()]

            decomp_trunc = (decomp or "")
            if len(decomp_trunc) > args.max_chars:
                decomp_trunc = decomp_trunc[:args.max_chars] + "\n/* ...truncated... */"

            prompt = PROMPT_TEMPLATE.format(
                name=name, address=addr, signature=sig,
                size=size, instructions=ins, basic_blocks=bb,
                tags=", ".join(tags) or "(none)",
                n_callers=len(callers), n_callees=len(callees),
                callers=", ".join(callers[:30]) or "(none)",
                callees=", ".join(callees[:30]) or "(none)",
                string_refs=" | ".join(srefs[:20]) or "(none)",
                decompiled=decomp_trunc)

            rec = {"address": addr, "name": name, "tier": args.tier,
                   "prompt": prompt}
            fh.write(json.dumps(rec, ensure_ascii=False) + "\n")
            n += 1

    print(f"Wrote {n} prompts to {out}", file=sys.stderr)


# ─────────────────────────────────────────────────────────────────────────────
# stats / grep / show
# ─────────────────────────────────────────────────────────────────────────────

def cmd_stats(args):
    db = open_db(args.db)
    print("== program ==")
    p = db.execute("SELECT * FROM program").fetchone()
    if p:
        print(f"  program: {p[0]}")
        print(f"  language: {p[2]}    pointer_size: {p[5]}")
    for tbl in ("strings", "functions", "callers", "callees",
                "string_refs", "tags", "tiers", "seeds", "neighborhood"):
        n = db.execute(f"SELECT COUNT(*) FROM {tbl}").fetchone()[0]
        print(f"  {tbl:<14} {n:>10,}")

    print("\n== top tags ==")
    for tag, n in db.execute(
            "SELECT tag, COUNT(*) FROM tags GROUP BY tag "
            "ORDER BY 2 DESC LIMIT 30"):
        print(f"  {n:>6}  {tag}")

    print("\n== tier distribution ==")
    for t, n in db.execute(
            "SELECT tier, COUNT(*) FROM tiers GROUP BY tier ORDER BY 1"):
        print(f"  tier {t}: {n}")

    print("\n== size histogram ==")
    buckets = [(0, 16), (17, 99), (100, 499), (500, 1999),
               (2000, 7999), (8000, 10**9)]
    for lo, hi in buckets:
        n = db.execute(
            "SELECT COUNT(*) FROM functions WHERE size BETWEEN ? AND ?",
            (lo, hi)).fetchone()[0]
        print(f"  {lo:>6}-{hi:<6}: {n}")


def cmd_grep(args):
    db = open_db(args.db)
    term = args.term.lower()
    if args.field in ("strings", "all"):
        print("== strings ==")
        for addr, val in db.execute(
                "SELECT address, value FROM strings WHERE LOWER(value) LIKE ? "
                "LIMIT ?", (f"%{term}%", args.limit)):
            print(f"  {addr}  {val}")
    if args.field in ("strrefs", "all"):
        print("== string refs (function -> value) ==")
        for fa, val in db.execute(
                "SELECT DISTINCT s.func_addr, s.value FROM string_refs s "
                "WHERE LOWER(s.value) LIKE ? LIMIT ?",
                (f"%{term}%", args.limit)):
            name = db.execute(
                "SELECT name FROM functions WHERE address=?",
                (fa,)).fetchone()
            print(f"  {fa}  {(name[0] if name else '?'):<40}  {val}")
    if args.field in ("names", "all"):
        print("== function names ==")
        for addr, name in db.execute(
                "SELECT address, name FROM functions WHERE LOWER(name) LIKE ? "
                "LIMIT ?", (f"%{term}%", args.limit)):
            print(f"  {addr}  {name}")
    if args.field in ("callees", "all"):
        print("== callees ==")
        for fa, cn in db.execute(
                "SELECT DISTINCT caller_addr, callee_name FROM callees "
                "WHERE LOWER(callee_name) LIKE ? LIMIT ?",
                (f"%{term}%", args.limit)):
            print(f"  {fa}  -> {cn}")


def cmd_show(args):
    db = open_db(args.db)
    addr = args.address
    if not addr.startswith("0x") and not re.match(r"^[0-9a-fA-F]+$", addr):
        print("address looks malformed", file=sys.stderr)
        return
    # Try multiple formats
    candidates = [addr, addr.lower(), addr.upper(),
                  addr if addr.startswith("0x") else f"0x{addr}"]
    row = None
    for c in candidates:
        row = db.execute(
            "SELECT * FROM functions WHERE address=?", (c,)).fetchone()
        if row:
            addr = c
            break
    if not row:
        print("Not found", file=sys.stderr)
        return
    cols = [d[0] for d in db.execute("PRAGMA table_info(functions)").fetchall()]
    rec = dict(zip(cols, row))
    print(f"=== {rec['name']} @ {addr} ===")
    print(f"size={rec['size']}  instr={rec['instructions']}  "
          f"BB={rec['basic_blocks']}  params={rec['params']}")
    print(f"sig: {rec['signature']}")
    tags = [t[0] for t in db.execute(
        "SELECT tag FROM tags WHERE func_addr=?", (addr,)).fetchall()]
    print(f"tags: {', '.join(tags) or '(none)'}")
    tier = db.execute(
        "SELECT tier, reason FROM tiers WHERE func_addr=?",
        (addr,)).fetchone()
    if tier:
        print(f"tier: {tier[0]} ({tier[1]})")
    print("\n--- callers ---")
    for ca, in db.execute(
            "SELECT caller_addr FROM callers WHERE callee_addr=?", (addr,)):
        print(f"  {ca}")
    print("\n--- callees ---")
    for cn, ca in db.execute(
            "SELECT callee_name, callee_addr FROM callees WHERE caller_addr=?",
            (addr,)):
        print(f"  {ca}  {cn}")
    print("\n--- string refs ---")
    for ia, ta, enc, v in db.execute(
            "SELECT instr_addr, target_addr, encoding, value "
            "FROM string_refs WHERE func_addr=?", (addr,)):
        print(f"  {ia} -> {ta} ({enc}): {v}")
    print("\n--- decompiled ---")
    print(rec["decompiled"] or "(none)")


# ─────────────────────────────────────────────────────────────────────────────
# CLI
# ─────────────────────────────────────────────────────────────────────────────

def main():
    p = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=__doc__)
    sub = p.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("ingest", help="Parse JSONL into SQLite")
    sp.add_argument("jsonl"); sp.add_argument("db")
    sp.set_defaults(fn=cmd_ingest)

    sp = sub.add_parser("tag", help="Apply rule-based tags")
    sp.add_argument("db"); sp.set_defaults(fn=cmd_tag)

    sp = sub.add_parser("seed", help="Find anchor functions")
    sp.add_argument("db")
    sp.add_argument("--terms", default="",
                    help="comma-separated; defaults to TTS bank")
    sp.set_defaults(fn=cmd_seed)

    sp = sub.add_parser("neighbors", help="Expand call-graph neighborhood")
    sp.add_argument("db")
    sp.add_argument("--hops", type=int, default=2)
    sp.add_argument("--reverse", action="store_true",
                    help="walk callers instead of callees")
    sp.set_defaults(fn=cmd_neighbors)

    sp = sub.add_parser("tier", help="Bin every function into tiers 0-3")
    sp.add_argument("db"); sp.set_defaults(fn=cmd_tier)

    sp = sub.add_parser("report", help="Markdown summary + tier 3 listing")
    sp.add_argument("db"); sp.add_argument("--out", required=True)
    sp.set_defaults(fn=cmd_report)

    sp = sub.add_parser("brief", help="Per-function markdown briefs")
    sp.add_argument("db"); sp.add_argument("--out", required=True)
    sp.add_argument("--tier", type=int, default=3)
    sp.set_defaults(fn=cmd_brief)

    sp = sub.add_parser("batch", help="Emit JSONL prompts for LLM batch run")
    sp.add_argument("db"); sp.add_argument("--out", required=True)
    sp.add_argument("--tier", type=int, default=2)
    sp.add_argument("--max-chars", type=int, default=12000,
                    help="truncate decompiled C to this many chars")
    sp.set_defaults(fn=cmd_batch)

    sp = sub.add_parser("stats", help="Print DB stats")
    sp.add_argument("db"); sp.set_defaults(fn=cmd_stats)

    sp = sub.add_parser("grep", help="Search strings/names/callees")
    sp.add_argument("db"); sp.add_argument("term")
    sp.add_argument("--field",
                    choices=["all", "strings", "strrefs", "names", "callees"],
                    default="all")
    sp.add_argument("--limit", type=int, default=200)
    sp.set_defaults(fn=cmd_grep)

    sp = sub.add_parser("show", help="Print one function's full record")
    sp.add_argument("db"); sp.add_argument("address")
    sp.set_defaults(fn=cmd_show)

    args = p.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
