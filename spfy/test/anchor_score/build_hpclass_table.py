"""Reconstruct full per-uid hp_class table from Frida chunks."""
import json
import os

OUT = "c:/tmp/unit_hpclass_table.bin"
hpclass = [-1] * 200000
n_units = 0
with open(os.path.expanduser("~/Documents/Speechify/spfy/test/oracle/traces/unit_hpclass_dump/text_002.jsonl"), encoding="utf-8") as f:
    for l in f:
        if not l.strip(): continue
        ev = json.loads(l)
        t = ev.get("type")
        if t == "unit_hpclass_chunk":
            start = ev["start"]
            for i, v in enumerate(ev["hpclass"]):
                hpclass[start + i] = v
        elif t == "unit_hpclass_done":
            n_units = ev["n_units"]
            print(f"n_units={n_units}, ok_count={ev['ok_count']}")

# Verify our 3 cands
for uid in [26214, 46287, 54549]:
    print(f"uid {uid}: hp_class = {hpclass[uid]}")

# Save table
with open(OUT, "wb") as f:
    f.write(bytes(b if 0 <= b <= 255 else 0xff for b in hpclass[:n_units]))
print(f"Saved to {OUT} ({n_units} bytes)")
