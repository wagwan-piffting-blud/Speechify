import json
from pathlib import Path

entries = {json.loads(l)["fn"] for l in open(r"C:\tmp\fe_recomp\ghidra_export.jsonl", encoding="utf-8")}
hot = set(json.load(open(r"C:\tmp\fe_recomp\hot_fns.json")))
traced = set(Path(r"C:\tmp\fe_recomp\trace.txt").read_text().split())
fnset = sorted((hot | traced) & entries)
print(f"hot {len(hot)}, traced {len(traced)} ({len(traced & entries)} are function entries), set {len(fnset)}")
json.dump(fnset, open(r"C:\tmp\fe_recomp\fnset.json", "w"))
