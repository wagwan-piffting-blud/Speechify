"""Render every wayback Tom transcript with SPFY_FE_RECOMP=0 and =1 and require
byte-identical WAVs. The recompiler's correctness gate beyond the 221-phrase
parity corpus."""
import hashlib
import os
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import psutil
from tqdm import tqdm

EXE = sys.argv[1] if len(sys.argv) > 1 else r"C:\tmp\spfy_build32\src\cli\spfy_synth.exe"
TMP = Path(tempfile.mkdtemp(prefix="rcequiv_"))


def render(txt, on):
    out = TMP / f"{txt.stem}_{on}.wav"
    env = dict(os.environ, SPFY_FE_RECOMP=str(on), SPFY_NO_UPDATE_CHECK="1")
    p = subprocess.Popen([EXE, "-f", str(txt), "tom", str(out)], stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, env=env)
    try:
        psutil.Process(p.pid).nice(psutil.BELOW_NORMAL_PRIORITY_CLASS)
    except psutil.Error:
        pass
    rc = p.wait()
    h = hashlib.md5(out.read_bytes()).hexdigest() if out.exists() else None
    out.unlink(missing_ok=True)
    return rc, h


def job(txt):
    return txt.name, render(txt, 0), render(txt, 1)


def main():
    texts = sorted(Path(r"D:\__crs\wayback_by_speaker\tom").glob("*.txt"))
    with ThreadPoolExecutor(max_workers=12) as ex:
        res = list(tqdm(ex.map(job, texts), total=len(texts), desc="equiv", mininterval=5))
    bad = [(n, a, b) for n, a, b in res if a != b or a[0] != 0 or a[1] is None]
    print(f"{len(res)} texts: {len(res) - len(bad)} identical, {len(bad)} differ/fail")
    for n, a, b in bad[:10]:
        print(f"  {n}: off={a} on={b}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
