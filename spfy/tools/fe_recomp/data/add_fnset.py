"""Add entry VAs (whitespace-separated, from a file) to fnset.json.

    python add_fnset.py <va-file>
"""
import json
import sys
from pathlib import Path

here = Path(__file__).resolve().parent
fnset = set(json.load(open(here / "fnset.json")))
new = set(Path(sys.argv[1]).read_text().split())
n0 = len(fnset)
fnset |= new
json.dump(sorted(fnset), open(here / "fnset.json", "w"))
print(f"fnset {n0} -> {len(fnset)}")
