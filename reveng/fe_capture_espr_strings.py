"""Capture full ESPR strings the engine receives, save samples."""
import frida, json, subprocess, sys, time
from pathlib import Path

ROOT    = Path(__file__).resolve().parents[1]
HOOK    = ROOT / "viz/frida_hooks/fe_espr_capture_hook.js"
DUMPWAV = ROOT / "bin/spfy_dumpwav.exe"
OUT     = ROOT / "spfy/build/fe_espr_samples.txt"

texts = [
    ("hello",     "Hello, world."),
    ("pangram",   "The quick brown fox jumps over the lazy dog."),
    ("single_a",  "A."),
    ("two_words", "Hi there."),
    ("emphasis",  "I love this."),
]

def main():
    sess = frida.attach("Speechify.exe")
    captured = []
    def on_msg(msg, data):
        if msg["type"] == "send":
            p = msg["payload"]
            if p.get("type") == "espr_input":
                captured.append(p)
            elif p.get("type") == "espr_error":
                print(f"  hook error: {p}")
    script = sess.create_script(HOOK.read_text(encoding="utf-8"))
    script.on("message", on_msg)
    script.load()
    time.sleep(0.5)
    out_lines = []
    for tid, text in texts:
        before = len(captured)
        subprocess.run([str(DUMPWAV), text, f"c:/tmp/espr_{tid}.wav"],
                       capture_output=True, timeout=10)
        time.sleep(0.5)
        new = captured[before:]
        out_lines.append(f"=== {tid}: {text!r} -> {len(new)} ESPR strings ===")
        for i, e in enumerate(new):
            out_lines.append(f"--- string {i+1} (len={e['len']}) ---")
            out_lines.append(e["text"])
            out_lines.append("")
        print(f"  {tid}: {len(new)} ESPR strings captured")
    sess.detach()
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(out_lines), encoding="utf-8")
    print(f"\nWrote {OUT}")

if __name__ == "__main__":
    sys.exit(main() or 0)
