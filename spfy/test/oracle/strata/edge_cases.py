"""D-04 edge-case stratum (>=50 rows, hand-curated).

The first eight entries are the non-negotiable MEMORY.md regression
anchors (D-04). Every subsequent row carries a one-line `# ` comment
immediately above it explaining why it is in the corpus.
"""
from __future__ import annotations


# CONTEXT.md D-04: hand-curated; MEMORY-listed regression anchors mandatory.
# Tuple shape: (id, mode, text, tags). Each row's purpose is documented in
# the `# ` comment immediately above it.
EDGE_CASES: list[tuple[str, str, str, list[str]]] = [
    # D-04 mandatory regression anchor: compound "seashells" (text_008 in seed)
    ("edge_001", "text", "She sells seashells by the seashore.",
        ["edge", "seed", "compound", "regression"]),
    # D-04 mandatory regression anchor: long sentence without explicit punctuation (text_029 in seed)
    ("edge_002", "text", "Now is the time for all good men to come to the aid of their country.",
        ["edge", "seed", "long-no-punct", "regression"]),
    # D-04 mandatory regression anchor: fricative-only utterance
    ("edge_003", "text", "Sssss.",
        ["edge", "seed", "fricative-only", "regression"]),
    # D-04 mandatory regression anchor: hyphen-suffix prefix-only token
    ("edge_004", "text", "syn-",
        ["edge", "seed", "prefix", "regression"]),
    # D-04 mandatory regression anchor: stress-edge LTS exception word
    ("edge_005", "text", "important",
        ["edge", "seed", "stress-edge", "regression"]),
    # D-04 mandatory regression anchor: compound decomposition (single-word)
    ("edge_006", "text", "seashore",
        ["edge", "seed", "compound", "regression"]),
    # D-04 mandatory regression anchor: compound decomposition (single-word)
    ("edge_007", "text", "lighthouse",
        ["edge", "seed", "compound", "regression"]),
    # D-04 mandatory regression anchor: long monomorpheme stress placement
    ("edge_008", "text", "synthesizing",
        ["edge", "seed", "long-monomorph", "regression"]),

    # --- pause-density variants ---
    # comma-separated pause variant
    ("edge_009", "text", "Hello, world.",
        ["edge", "comma", "pause-density"]),
    # semicolon variant (engine-specific pause weight)
    ("edge_010", "text", "Hello; world.",
        ["edge", "semicolon", "pause-density"]),
    # ellipsis: long inter-word pause
    ("edge_011", "text", "Hello... world.",
        ["edge", "ellipsis", "pause-density"]),
    # multiple short pauses inside a single utterance
    ("edge_012", "text", "Wait, listen, then speak.",
        ["edge", "multi-comma", "pause-density"]),
    # em-dash style break
    ("edge_013", "text", "Stop -- and look.",
        ["edge", "em-dash", "pause-density"]),

    # --- all-vowel utterances ---
    # diphthong-only word
    ("edge_014", "text", "Aye.",
        ["edge", "all-vowel"]),
    # back-rounded vowel exclamation
    ("edge_015", "text", "Ooh.",
        ["edge", "all-vowel"]),
    # front-vowel exclamation
    ("edge_016", "text", "Eee.",
        ["edge", "all-vowel"]),

    # --- consonant-cluster torture ---
    # final cluster /Nths/ stress test
    ("edge_017", "text", "Strengths.",
        ["edge", "consonant-cluster"]),
    # final cluster /ksTs/ -- iy + xths
    ("edge_018", "text", "Sixths.",
        ["edge", "consonant-cluster"]),
    # tongue-twister with sibilants and clusters
    ("edge_019", "text", "Stretched twelfths.",
        ["edge", "consonant-cluster"]),

    # --- very-short utterances ---
    # single-letter utterance "I."
    ("edge_020", "text", "I.",
        ["edge", "very-short", "vowel-only"]),
    # single-letter utterance "A."
    ("edge_021", "text", "A.",
        ["edge", "very-short", "vowel-only"]),
    # two-letter exclamation
    ("edge_022", "text", "Hi.",
        ["edge", "very-short"]),
    # single-letter spelt name
    ("edge_023", "text", "O.",
        ["edge", "very-short", "vowel-only"]),

    # --- very-long utterance (>=30 words) ---
    # paragraph-length sentence stresses utterance-segmentation pipeline
    ("edge_024", "text",
        "It was the best of times, it was the worst of times, it was the age of wisdom, it was the age of foolishness, it was the epoch of belief, it was the epoch of incredulity.",
        ["edge", "very-long"]),
    # second very-long stress test (>=30 words)
    ("edge_025", "text",
        "If you would like to speak with a representative, please stay on the line, otherwise press one for English, two for Spanish, three for French, or remain on the line for our next available agent.",
        ["edge", "very-long"]),

    # --- digits-as-words ---
    # individual digit words
    ("edge_026", "text", "One two three.",
        ["edge", "digits-as-words"]),
    # spelled count-down
    ("edge_027", "text", "Ten nine eight seven.",
        ["edge", "digits-as-words"]),

    # --- numbers-as-text (LTS digit normalisation) ---
    # bare three-digit integer
    ("edge_028", "text", "123",
        ["edge", "numbers-as-text"]),
    # year-like four-digit integer
    ("edge_029", "text", "2026",
        ["edge", "numbers-as-text"]),
    # decimal number
    ("edge_030", "text", "3.14",
        ["edge", "numbers-as-text"]),

    # --- proper nouns ---
    # bisyllabic personal name
    ("edge_031", "text", "Alice met Bob.",
        ["edge", "proper-noun"]),
    # placename ending in -ia
    ("edge_032", "text", "California.",
        ["edge", "proper-noun"]),
    # multi-word place name
    ("edge_033", "text", "New York City.",
        ["edge", "proper-noun"]),

    # --- contractions ---
    # negative contraction
    ("edge_034", "text", "I don't know.",
        ["edge", "contraction"]),
    # 'll future contraction
    ("edge_035", "text", "We'll see.",
        ["edge", "contraction"]),
    # 're contraction
    ("edge_036", "text", "They're here.",
        ["edge", "contraction"]),
    # 've perfect contraction
    ("edge_037", "text", "I've been there.",
        ["edge", "contraction"]),

    # --- hyphenated words ---
    # routine compound modifier
    ("edge_038", "text", "Well-known.",
        ["edge", "hyphenated"]),
    # multi-hyphen modifier
    ("edge_039", "text", "Up-to-date status.",
        ["edge", "hyphenated"]),

    # --- abbreviations ---
    # title abbreviation Dr. (NOT a sentence-end period)
    ("edge_040", "text", "Dr. Smith called.",
        ["edge", "abbreviation"]),
    # title abbreviation Mr. (NOT a sentence-end period)
    ("edge_041", "text", "Mr. Jones is here.",
        ["edge", "abbreviation"]),
    # latin abbreviation 'etc.'
    ("edge_042", "text", "Apples, oranges, etc.",
        ["edge", "abbreviation"]),

    # --- question forms ---
    # short wh-question
    ("edge_043", "text", "Why?",
        ["edge", "question"]),
    # yes-no question
    ("edge_044", "text", "Are you ready?",
        ["edge", "question"]),
    # tag question
    ("edge_045", "text", "It's working, isn't it?",
        ["edge", "question"]),

    # --- exclamations ---
    # single-word exclamation
    ("edge_046", "text", "Wow!",
        ["edge", "exclamation"]),
    # short exclamation phrase
    ("edge_047", "text", "Watch out!",
        ["edge", "exclamation"]),
    # double-punctuation exclamation
    ("edge_048", "text", "Stop!!",
        ["edge", "exclamation"]),

    # --- nasal-only utterance ---
    # bilabial-nasal hum (text_022 seed sibling)
    ("edge_049", "text", "Mmm.",
        ["edge", "nasal-only"]),

    # --- spr-mode edge case ---
    # explicit SPR phoneme string forces oracle to bypass LTS
    ("edge_050", "spr", "\\![.1Sa.0kIG]",
        ["edge", "spr", "lts-bypass"]),

    # --- additional misc ---
    # mixed-case acronym
    ("edge_051", "text", "NASA confirmed it.",
        ["edge", "acronym"]),
    # currency-as-text (no $ symbol normalisation)
    ("edge_052", "text", "Five dollars exactly.",
        ["edge", "currency-words"]),
]


def generate() -> list[dict]:
    """Return list of >=50 {id, mode, text, tags} rows for the edge stratum."""
    rows = [
        {"id": id_, "mode": mode, "text": text, "tags": tags}
        for (id_, mode, text, tags) in EDGE_CASES
    ]
    assert len(rows) >= 50, f"edge_cases must emit >=50 rows, got {len(rows)}"
    return rows
