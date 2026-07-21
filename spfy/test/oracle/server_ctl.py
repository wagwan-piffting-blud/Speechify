"""server_ctl.py -- start/stop/point the Speechify 3.0.5 server at a voice.

Speechify reads the voice .vin/.vdb only at startup, so switching voices
means: rewrite config/SWIttsConfig.xml, restart bin/Speechify.exe, wait
for port 5555 to accept. This wraps that so oracle capture runs can swap
voices unattended.

Unlike viz/app.py's /api/voices/select, this writes BOTH tts.voice.name
and tts.voice.language. The language is mandatory for the non-en-US
voices -- the config builds the .vcf path as
${tts.voice.dir}/${tts.voice.language}/${name}/${name}.vcf, so leaving
the language at en-US while selecting `felix` points at a path that does
not exist.

Usage:
    python spfy/test/oracle/server_ctl.py status
    python spfy/test/oracle/server_ctl.py start
    python spfy/test/oracle/server_ctl.py stop
    python spfy/test/oracle/server_ctl.py use felix     # swap + restart
"""
from __future__ import annotations

import argparse
import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[3]
CONFIG       = PROJECT_ROOT / "config" / "SWIttsConfig.xml"
SERVER_EXE   = PROJECT_ROOT / "bin" / "Speechify.exe"
SERVER_CWD   = PROJECT_ROOT / "bin"
PORT         = 5555
LANG_DIRS    = ("en-US", "es-MX", "fr-CA")

_NAME_RE = re.compile(
    r'(<param\s+name="tts\.voice\.name">\s*<value>\s*)([^<\s]+)(\s*</value>)')
_LANG_RE = re.compile(
    r'(<param\s+name="tts\.voice\.language">\s*<value>\s*)([^<\s]+)(\s*</value>)')


def find_voice_language(name: str) -> str | None:
    """Which language dir holds this voice? Derived from disk, not a map,
    so adding a voice needs no code change."""
    for lang in LANG_DIRS:
        if (PROJECT_ROOT / lang / name / f"{name}.vin").exists():
            return lang
    return None


def read_config() -> tuple[str | None, str | None]:
    if not CONFIG.exists():
        return None, None
    text = CONFIG.read_text(encoding="utf-8", errors="replace")
    n = _NAME_RE.search(text)
    l = _LANG_RE.search(text)
    return (n.group(2) if n else None), (l.group(2) if l else None)


def write_config(name: str, lang: str) -> None:
    text = CONFIG.read_text(encoding="utf-8", errors="replace")
    text, n1 = _NAME_RE.subn(rf'\g<1>{name}\g<3>', text)
    text, n2 = _LANG_RE.subn(rf'\g<1>{lang}\g<3>', text)
    if n1 == 0:
        raise SystemExit("error: tts.voice.name not found in config")
    if n2 == 0:
        raise SystemExit("error: tts.voice.language not found in config")
    CONFIG.write_text(text, encoding="utf-8")


def port_open(timeout: float = 0.25) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", PORT), timeout=timeout):
            return True
    except OSError:
        return False


def server_pids() -> list[int]:
    """PIDs of running Speechify.exe, via tasklist (no psutil dependency)."""
    try:
        out = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq Speechify.exe", "/FO", "CSV", "/NH"],
            capture_output=True, text=True, timeout=20).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    pids = []
    for line in out.splitlines():
        parts = [p.strip('" ') for p in line.split('","')]
        if len(parts) >= 2 and parts[0].lower() == "speechify.exe":
            try:
                pids.append(int(parts[1]))
            except ValueError:
                pass
    return pids


def stop(quiet: bool = False) -> None:
    pids = server_pids()
    if not pids:
        if not quiet:
            print("server: not running")
        return
    for pid in pids:
        subprocess.run(["taskkill", "/PID", str(pid), "/F"],
                       capture_output=True, timeout=20)
    # Wait for the listener to actually disappear before returning.
    for _ in range(40):
        if not port_open() and not server_pids():
            break
        time.sleep(0.25)
    if not quiet:
        print(f"server: stopped {pids}")


def start(wait_s: float = 30.0) -> bool:
    if port_open():
        print("server: already listening on %d" % PORT)
        return True
    if not SERVER_EXE.exists():
        raise SystemExit(f"error: {SERVER_EXE} not found")

    # DETACHED_PROCESS so the server outlives this script; cwd must be
    # bin/ so it resolves its config and voice data relative to itself.
    subprocess.Popen([str(SERVER_EXE)], cwd=str(SERVER_CWD),
                     creationflags=subprocess.DETACHED_PROCESS
                     | subprocess.CREATE_NEW_PROCESS_GROUP,
                     stdin=subprocess.DEVNULL,
                     stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL)

    deadline = time.time() + wait_s
    while time.time() < deadline:
        if port_open():
            name, lang = read_config()
            print(f"server: ready on {PORT} (voice={name} lang={lang})")
            return True
        time.sleep(0.25)
    print(f"server: FAILED to open port {PORT} within {wait_s:.0f}s",
          file=sys.stderr)
    return False


def status() -> None:
    name, lang = read_config()
    pids = server_pids()
    print(f"config:   voice={name} language={lang}")
    print(f"process:  {pids if pids else 'not running'}")
    print(f"port {PORT}:  {'listening' if port_open() else 'closed'}")


def use(name: str) -> bool:
    lang = find_voice_language(name)
    if not lang:
        raise SystemExit(
            f"error: no voice '{name}' under {'/'.join(LANG_DIRS)}")
    cur_name, cur_lang = read_config()
    if (cur_name, cur_lang) == (name, lang) and port_open():
        print(f"server: already serving {name} ({lang})")
        return True
    write_config(name, lang)
    print(f"config:   voice={name} language={lang}")
    stop(quiet=True)
    return start()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("command", choices=["status", "start", "stop",
                                        "restart", "use"])
    ap.add_argument("voice", nargs="?", help="voice name for `use`")
    args = ap.parse_args()

    if args.command == "status":
        status()
    elif args.command == "start":
        return 0 if start() else 1
    elif args.command == "stop":
        stop()
    elif args.command == "restart":
        stop(quiet=True)
        return 0 if start() else 1
    elif args.command == "use":
        if not args.voice:
            raise SystemExit("error: `use` needs a voice name")
        return 0 if use(args.voice) else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
