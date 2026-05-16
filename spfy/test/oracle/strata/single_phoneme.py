"""D-01 single-phoneme stratum (50 rows).

Walks every real entry of Tom's ARPAbet phoneset wrapped in a fixed
carrier sentence ("This is the X sound."), then pads to exactly 50 with
SPR stress-position variants of six representative vowels. The script
is the source of truth; corpus.jsonl is its output.

# SOURCE: spfy/src/fe/phoneset.h:19-24 (Tom's 46-entry ARPAbet phoneset)
# SPR conversion table mirrored from bin/spfy_dumpwav.c:38-47.
"""
from __future__ import annotations


# Tom's full ARPAbet phoneset, position == phone_id. Verbatim from
# spfy/src/fe/phoneset.h:19-24. 46 entries; 44 real phones (skip pau, xx).
TOM_PHONESET: list[str] = [
    "aa", "ae", "ah", "ao", "aw", "ax", "ay", "b", "ch", "dx",
    "d", "dh", "eh", "el", "er", "en", "ey", "f", "g", "hh",
    "ih", "ix", "iy", "jh", "k", "l", "m", "n", "ng", "ow",
    "oy", "p", "pau", "r", "s", "sh", "t", "th", "uh", "uw",
    "v", "w", "xx", "y", "z", "zh",
]

# ARPA -> SPR conversion (bin/spfy_dumpwav.c:38-47).  Used only for the
# six padding rows below; the 44 real-phone rows are mode="text" carriers.
_ARPA_TO_SPR: dict[str, str] = {
    "aa": "a", "ae": "A", "ah": "H", "ao": "c", "aw": "W", "ax": "x",
    "ay": "Y", "b":  "b", "ch": "C", "d":  "d", "dh": "D", "dx": "F",
    "eh": "E", "el": "l", "en": "N", "er": "R", "ey": "e", "f":  "f",
    "g":  "g", "hh": "h", "ih": "I", "ix": "X", "iy": "i", "jh": "J",
    "k":  "k", "l":  "l", "m":  "m", "n":  "n", "ng": "G", "ow": "o",
    "oy": "O", "p":  "p", "pau": "_", "r":  "r", "s":  "s", "sh": "S",
    "t":  "t", "th": "T", "uh": "U", "uw": "u", "v":  "v", "w":  "w",
    "xx": "x", "y":  "y", "z":  "z", "zh": "Z",
}

# Six stress-position padding rows: aa, ay, iy, ow, ah, er at stress 0/1/2.
# We pick three (vowel, stress) pairs deterministically to land at exactly
# 50 rows when combined with the 44 real-phone carriers.
_PAD_VOWEL_STRESS: list[tuple[str, str]] = [
    ("aa", "1"),
    ("ay", "2"),
    ("iy", "1"),
    ("ow", "0"),
    ("ah", "2"),
    ("er", "1"),
]


def generate() -> list[dict]:
    """Return list of 50 {id, mode, text, tags} rows for the phn stratum."""
    rows: list[dict] = []
    real_phones = [p for p in TOM_PHONESET if p not in ("pau", "xx")]
    # Sanity: phoneset.h:19-24 minus pau/xx = 44 real phones.
    assert len(real_phones) == 44, f"expected 44 real phones, got {len(real_phones)}"
    for i, phone in enumerate(real_phones, 1):
        rows.append({
            "id":   f"phn_{i:03d}",
            "mode": "text",
            "text": f"This is the {phone} sound.",
            "tags": ["phn", "stratum=single_phoneme", f"phone={phone}"],
        })
    # Pad to exactly 50 with SPR stress-position variants of six vowels.
    for j, (arpa, stress) in enumerate(_PAD_VOWEL_STRESS, 1):
        spr_sym = _ARPA_TO_SPR[arpa]
        text = f"\\![.{stress}{spr_sym}]"
        rows.append({
            "id":   f"phn_{44 + j:03d}",
            "mode": "spr",
            "text": text,
            "tags": [
                "phn", "stratum=single_phoneme",
                f"phone={arpa}", f"stress={stress}", "padding=stress_variant",
            ],
        })
    assert len(rows) == 50, f"single_phoneme must emit 50 rows, got {len(rows)}"
    return rows
