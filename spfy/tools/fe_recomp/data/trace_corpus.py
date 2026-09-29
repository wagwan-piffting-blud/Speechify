import os
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import psutil
from tqdm import tqdm

EXE = r"C:\tmp\spfy_build32\src\cli\spfy_synth.exe"
TRACE = Path(r"C:\tmp\fe_recomp\trace.txt")
OUT = Path(r"C:\tmp\fe_recomp\trace_wav")


def run(txt):
    env = dict(os.environ, SPFY_FE_RECOMP_TRACE=str(TRACE), SPFY_NO_UPDATE_CHECK="1")
    p = subprocess.Popen([EXE, "-f", str(txt), "tom", str(OUT / ("o_%d.wav" % (hash(txt) % 64)))],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    try:
        psutil.Process(p.pid).nice(psutil.BELOW_NORMAL_PRIORITY_CLASS)
    except psutil.Error:
        pass
    return p.wait()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    TRACE.unlink(missing_ok=True)
    texts = sorted(Path(r"D:\__crs\wayback_by_speaker\tom").glob("*.txt"))
    with ThreadPoolExecutor(max_workers=12) as ex:
        rcs = list(tqdm(ex.map(run, texts), total=len(texts), desc="trace"))
    lines = set(TRACE.read_text().split()) if TRACE.exists() else set()
    print(f"{len(texts)} texts, {sum(1 for r in rcs if r)} failed, {len(lines)} distinct missed call targets")


if __name__ == "__main__":
    main()
