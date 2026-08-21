"""Path anchors for the copied build tools. Import this; hardcode nothing.

This is a depth-corrected stand-in for `reveng/spfy4/tools/wayback/paths.py`.
The tools beside it are BYTE-IDENTICAL copies of the originals, and they all do
`from paths import REPO`; the only thing that differs in this location is how
far up the tree the repository root sits.

    reveng/spfy4/tools/wayback/paths.py   ->  up 4
    en-US/crsmara/build_tools/paths.py    ->  up 3

Every machine-specific location is an environment variable with a default, so
nothing here needs editing to run somewhere else:

    SPFY4_SCRATCH   working directory for build arms      (default C:\\tmp)
    SPFY4_BUILD     the built spfy CLI tree               (default <scratch>\\spfy_build32)
    SPFY4_CRS       the audio archive, if you have one    (default D:\\__crs)

`python paths.py` prints which anchors actually resolve on this machine. Run it
first if a tool cannot find something.
"""
import os
from pathlib import Path

HERE = Path(__file__).resolve().parent          # en-US/crsmara/build_tools
VOICE = HERE.parent                             # en-US/crsmara
REPO = HERE.parent.parent.parent                # repository root
DATA = HERE / "data"                            # the lists shipped beside these tools


def _env(name, default):
    v = os.environ.get(name)
    return Path(v) if v else Path(default)


SCRATCH = _env("SPFY4_SCRATCH", r"C:\tmp")
CRS = _env("SPFY4_CRS", r"D:\__crs")
BUILD = _env("SPFY4_BUILD", SCRATCH / "spfy_build32")

SYNTH_EXE = BUILD / "src" / "cli" / "spfy_synth.exe"
VB_BUILD_EXE = BUILD / "src" / "cli" / "spfy_vb_build.exe"
VB_VERIFY_EXE = BUILD / "src" / "cli" / "spfy_vb_verify.exe"

# ⚠ Names the originals export that have no meaning outside the wayback corpus
# tools. They are defined so an unmodified copied tool still imports; a tool
# that actually READS one of these is a tool that does not belong in this
# directory.
WAYBACK_DB = DATA / "wayback.sqlite"
IEM_CACHE = DATA / "iem_cache.sqlite"
WAYBACK_TEXT = DATA / "wayback_text"
BY_SPEAKER = CRS / "wayback_by_speaker"
AUDIO_ROOT = CRS / "wayback"
WHISPER = DATA / "wayback_whisper"
DECISIONS = DATA / "wayback_review_decisions.json"
REVIEW = SCRATCH / "wb_review"


def ensure(*paths):
    """mkdir -p for outputs; returns the first path for chaining."""
    for p in paths:
        Path(p).mkdir(parents=True, exist_ok=True)
    return Path(paths[0]) if paths else None


def check():
    """Report which anchors exist. Run when something cannot be found."""
    rows = [("REPO", REPO), ("VOICE", VOICE), ("DATA", DATA),
            ("SCRATCH", SCRATCH), ("BUILD", BUILD),
            ("VB_BUILD_EXE", VB_BUILD_EXE), ("VB_VERIFY_EXE", VB_VERIFY_EXE),
            ("SYNTH_EXE", SYNTH_EXE)]
    width = max(len(n) for n, _ in rows)
    for name, p in rows:
        print(f"  {name:<{width}}  {'ok     ' if p.exists() else 'MISSING'}  {p}")


if __name__ == "__main__":
    check()
