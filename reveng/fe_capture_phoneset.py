"""fe_capture_phoneset.py -- attach Frida to running Speechify.exe,
hook ESPRparser::parsePhoneme, then drive synth via spfy_dumpwav.exe
subprocess calls. Capture every phoneme NAME the engine sees, dedup,
save the inventory.

Speechify.exe MUST already be running (it's the SAPI server that
loads SWIttsEngine.dll; spfy_dumpwav is a thin SAPI client that
delegates synth to it).

Usage:
    python reveng/fe_capture_phoneset.py
    -> spfy/build/fe_phoneset.json
"""

import frida
import json
import subprocess
import sys
import threading
import time
from collections import Counter
from pathlib import Path

ROOT       = Path(__file__).resolve().parents[1]
HOOK       = ROOT / "viz/frida_hooks/fe_parsephoneme_hook.js"
DUMPWAV    = ROOT / "bin/spfy_dumpwav.exe"
CORPUS     = ROOT / "spfy/test/oracle/corpus.jsonl"
OUT_JSON   = ROOT / "spfy/build/fe_phoneset.json"
TMP_WAV    = "c:/tmp/fe_phoneset_probe.wav"
TARGET_PROC = "Speechify.exe"


class CaptureSession:
    """Persistent attach to Speechify.exe; receives events as we drive
    synth via dumpwav subprocesses."""
    def __init__(self, hook_src: str):
        self.events: list[dict] = []
        self.captured: list[str] = []
        self.lock = threading.Lock()
        # Attach by name.
        self.sess = frida.attach(TARGET_PROC)
        self.script = self.sess.create_script(hook_src)
        self.script.on("message", self._on_msg)
        self.script.load()

    def _on_msg(self, msg, data):
        if msg["type"] == "send":
            p = msg["payload"]
            t = p.get("type")
            with self.lock:
                if t == "parsephon_batch":
                    for it in p.get("items", []):
                        self.captured.append(it["name"])
                elif t == "espr_input":
                    self.events.append(p)
                else:
                    self.events.append(p)
        elif msg["type"] == "error":
            with self.lock:
                self.events.append({"error": msg.get("description", str(msg))})

    def synth(self, text: str, timeout: float = 8.0) -> list[str]:
        """Drive a synth via dumpwav and return phonemes captured during."""
        with self.lock:
            mark = len(self.captured)
        try:
            subprocess.run([str(DUMPWAV), text, TMP_WAV],
                           timeout=timeout, capture_output=True)
        except subprocess.TimeoutExpired:
            pass
        # Give events a moment to arrive.
        time.sleep(0.3)
        try:
            self.script.exports_sync.flush()
        except Exception:
            pass
        time.sleep(0.1)
        with self.lock:
            return list(self.captured[mark:])

    def close(self):
        try:
            self.script.exports_sync.flush()
        except Exception:
            pass
        try:
            self.sess.detach()
        except Exception:
            pass


def run_one(text: str, sess: CaptureSession):
    return sess.synth(text), sess.events


def main():
    hook_src = HOOK.read_text(encoding="utf-8")
    corpus = [
        json.loads(l) for l in CORPUS.read_text(encoding="utf-8").splitlines()
        if l.strip()
    ]
    text_entries = [e for e in corpus if e.get("mode") == "text"]
    print(f"corpus: {len(text_entries)} texts; "
          f"attaching to {TARGET_PROC} and driving via dumpwav...")
    try:
        sess = CaptureSession(hook_src)
    except frida.ProcessNotFoundError:
        print(f"ERROR: {TARGET_PROC} not running. Start it first.")
        return 1

    name_freq: Counter = Counter()
    per_text: dict[str, list[str]] = {}
    # First text: print full diagnostic so we can debug.
    first = text_entries[0]
    names = sess.synth(first["text"])
    print(f"\n[diagnostic from {first['id']}]:")
    for d in sess.events:
        if "initial_modules" in d:
            mods = d["initial_modules"]
            tts_mods = [m for m in mods if "tts" in m.lower() or
                                            "swi" in m.lower()]
            print(f"  boot: target_loaded={d.get('initial_target_loaded')}, "
                  f"{len(mods)} modules, tts-related: {tts_mods}")
        elif "modules_now" in d or "modules_at_timeout" in d:
            mods = d.get("modules_now") or d.get("modules_at_timeout") or []
            tts_mods = [m for m in mods if "tts" in m.lower() or
                                            "swi" in m.lower()]
            print(f"  {d.get('type')}: tts-related: {tts_mods}")
        else:
            print(f"  {d}")
    print(f"  (got {len(names)} phonemes from first text)\n")

    if len(names) == 0:
        print("ZERO PHONEMES: hook didn't fire on first text. Aborting.")
        sess.close()
        return 0

    per_text[first["id"]] = names
    for n in names: name_freq[n] += 1
    n_done = 1
    for e in text_entries[1:]:
        names = sess.synth(e["text"])
        per_text[e["id"]] = names
        for n in names:
            name_freq[n] += 1
        n_done += 1
        if n_done <= 6 or n_done % 5 == 0:
            print(f"  [{n_done:>2}/{len(text_entries)}] {e['id']}: "
                  f"got {len(names)} phonemes "
                  f"(running total: {len(name_freq)} unique)")
    sess.close()

    inv = sorted(name_freq.items(), key=lambda kv: -kv[1])
    print(f"\n=== Engine phoneme inventory ({len(inv)} unique) ===")
    for nm, cnt in inv:
        print(f"  {nm:<6}  {cnt}")

    OUT_JSON.parent.mkdir(parents=True, exist_ok=True)
    with OUT_JSON.open("w", encoding="utf-8") as fp:
        json.dump({
            "inventory": [{"name": n, "count": c} for n, c in inv],
            "n_unique": len(inv),
            "per_text": per_text,
        }, fp, indent=2)
    print(f"\nWrote {OUT_JSON}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
