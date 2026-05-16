"""D-03 natural-phrases stratum (50 rows).

Hand-mined English sentences in three length buckets (short <=5 words,
medium 6-12, long >=13). Sampled with random.Random(NAT_SEED) for
reproducibility; the seed is recorded in every row's tags so Phase 6
can reproduce the exact picks.

CONTEXT.md D-03: "Pull all candidate sentences from demos/ and doc/ ...
bucket by length ... random-sample with random.Random(seed) to fill 50
with a balanced length distribution."

RESEARCH.md notes that demos/ + doc/ mining produced thin yields, so
the candidate pool is enumerated explicitly in this source file; the
script is self-contained.
"""
from __future__ import annotations

import random


NAT_SEED: int = 42


# Candidate sentence pool. Each bucket has at least 17 entries. Sourced
# from demos/, doc/, MEMORY.md regression history, and standard-issue
# TTS demo/IVR-style utterances. Kept as a literal so the script needs
# no external file at run time.
CANDIDATES: list[str] = [
    # ----- short bucket: <=5 words -----
    "Hello there.",
    "Good morning.",
    "Welcome back.",
    "Goodbye for now.",
    "Yes please.",
    "No thanks.",
    "Right now.",
    "Not yet.",
    "Try again later.",
    "All systems go.",
    "Wait one moment.",
    "Speak after the tone.",
    "Press any key.",
    "Hold the line please.",
    "Open the door.",
    "Close the window.",
    "Turn on the lights.",
    "Lights off please.",
    "It is raining.",
    "Time to go.",
    # ----- medium bucket: 6-12 words -----
    "The weather today is partly cloudy with light winds.",
    "Please leave your name and number after the beep.",
    "Your package will arrive between three and five today.",
    "All circuits are busy, please try your call later.",
    "The next train to the airport leaves in ten minutes.",
    "Press one for sales, two for support, three for billing.",
    "The current temperature is sixty eight degrees.",
    "Your appointment is confirmed for Tuesday at two o'clock.",
    "The library will be closed on Monday for the holiday.",
    "Office hours are from nine in the morning until five.",
    "Please stay on the line for the next available agent.",
    "This message is being recorded for quality assurance.",
    "The road ahead is closed due to ongoing construction.",
    "Mind the gap between the train and the platform.",
    "All passengers must remain seated until we land.",
    "The fire alarm test will run for thirty seconds.",
    "Please return your tray tables to the upright position.",
    "Your battery is low, please connect to power soon.",
    # ----- long bucket: >=13 words -----
    "Now is the time for all good men to come to the aid of their country.",
    "The quick brown fox jumps over the lazy dog while the cat watches with mild interest.",
    "If you would like to speak to a customer service representative, please remain on the line.",
    "Today's forecast calls for sunny skies in the morning followed by scattered thunderstorms in the afternoon.",
    "Thank you for calling, your business is important to us and we will be with you shortly.",
    "Please make sure all electronic devices are switched to airplane mode for the duration of the flight.",
    "The library is open from nine in the morning until eight at night Monday through Saturday.",
    "Due to the heavy snowfall overnight, all schools in the district will be closed today.",
    "We are experiencing a higher than usual call volume and apologize for the longer wait time.",
    "If this is a medical emergency, please hang up and dial nine one one immediately.",
    "Tickets for the show go on sale tomorrow morning at ten o'clock through the official website.",
    "Visitors are reminded that flash photography is not permitted anywhere inside the museum galleries.",
    "The annual general meeting will be held in the main conference room on the third floor.",
    "Once upon a time, in a land far far away, there lived a clever little fox.",
    "She walked along the seashore at sunset, listening to the gentle sound of waves on the sand.",
    "All employees must complete the mandatory training course before the end of the calendar quarter.",
    "Our records indicate that your subscription expired last month, please renew to continue uninterrupted service.",
]


def generate(seed: int = NAT_SEED) -> list[dict]:
    """Return 50 deterministic rows from CANDIDATES at the given seed.

    Sampling: shuffle each length bucket independently with
    random.Random(seed), then take 17 short + 17 medium + 16 long.
    """
    rng = random.Random(seed)
    short  = [c for c in CANDIDATES if 1 <= len(c.split()) <= 5]
    medium = [c for c in CANDIDATES if 6 <= len(c.split()) <= 12]
    long_  = [c for c in CANDIDATES if len(c.split()) >= 13]
    assert len(short)  >= 17, f"need >=17 short candidates, have {len(short)}"
    assert len(medium) >= 17, f"need >=17 medium candidates, have {len(medium)}"
    assert len(long_)  >= 16, f"need >=16 long candidates, have {len(long_)}"
    rng.shuffle(short)
    rng.shuffle(medium)
    rng.shuffle(long_)
    picked = short[:17] + medium[:17] + long_[:16]
    rows: list[dict] = []
    for i, text in enumerate(picked, 1):
        n = len(text.split())
        bucket = "short" if n <= 5 else ("medium" if n <= 12 else "long")
        rows.append({
            "id":   f"nat_{i:03d}",
            "mode": "text",
            "text": text,
            "tags": [
                "nat", "stratum=natural",
                f"length_bucket={bucket}", f"seed={seed}",
            ],
        })
    assert len(rows) == 50, f"natural_phrases must emit 50 rows, got {len(rows)}"
    return rows
