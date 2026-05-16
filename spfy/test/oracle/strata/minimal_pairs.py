"""D-02 minimal-pair voiced/unvoiced join stratum (50 rows).

Cross-product of the eight standard obstruent pairs with six contexts
(initial / medial / final / cluster_pre / cluster_post / across_boundary)
= 48 rows; padded to 50 with two geminate-back-to-back rows for s/z and
t/d. Output is deterministic in (PAIRS order x CONTEXTS order) iteration.
"""
from __future__ import annotations


# Eight standard obstruent voiced/unvoiced minimal pairs.
PAIRS: list[tuple[str, str]] = [
    ("p",  "b"),
    ("t",  "d"),
    ("k",  "g"),
    ("f",  "v"),
    ("s",  "z"),
    ("sh", "zh"),
    ("ch", "jh"),
    ("th", "dh"),
]

CONTEXTS: list[str] = [
    "initial",
    "medial",
    "final",
    "cluster_pre",
    "cluster_post",
    "across_boundary",
]


def _carrier(p1: str, p2: str, ctx: str) -> str:
    """Render one minimal-pair carrier sentence for (p1, p2, ctx).

    Carriers are deliberately simple lexical-shape templates -- they are
    NOT meant to be real English words. They exercise the FE -> SPR -> DP
    pipeline with the surface phone bigrams the engine should see.
    """
    if ctx == "initial":
        return f"A {p1}at and a {p2}at."
    if ctx == "medial":
        return f"A ra{p1}id ra{p2}id."
    if ctx == "final":
        return f"A ca{p1}, a ca{p2}."
    if ctx == "cluster_pre":
        # Special case: /sth/ and /sdh/ are impossible English clusters,
        # so the th/dh pair gets a final-position cluster carrier instead.
        if (p1, p2) == ("th", "dh"):
            return f"A wi{p1}, a wi{p2}."
        return f"A s{p1}ot, a s{p2}ot."
    if ctx == "cluster_post":
        return f"A {p1}lay, a {p2}lay."
    if ctx == "across_boundary":
        return f"a{p1} #a{p2}."
    raise ValueError(f"unknown context: {ctx}")


def generate() -> list[dict]:
    """Return list of 50 {id, mode, text, tags} rows for the mp stratum."""
    rows: list[dict] = []
    i = 1
    for (p1, p2) in PAIRS:
        for ctx in CONTEXTS:
            rows.append({
                "id":   f"mp_{i:03d}",
                "mode": "text",
                "text": _carrier(p1, p2, ctx),
                "tags": [
                    "mp", "stratum=minimal_pairs",
                    f"pair={p1}_{p2}", f"context={ctx}",
                ],
            })
            i += 1
    # 8 * 6 = 48; pad with two geminate-back-to-back rows.
    geminates: list[tuple[str, str]] = [("s", "z"), ("t", "d")]
    for (p1, p2) in geminates:
        rows.append({
            "id":   f"mp_{i:03d}",
            "mode": "text",
            "text": f"a{p1}{p1} a{p2}{p2}.",
            "tags": [
                "mp", "stratum=minimal_pairs",
                f"pair={p1}_{p2}", "context=geminate",
            ],
        })
        i += 1
    assert len(rows) == 50, f"minimal_pairs must emit 50 rows, got {len(rows)}"
    return rows
