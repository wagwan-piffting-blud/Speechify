"""fe_vtable_capture.py -- run fe_vtable_hook.js against spfy_dumpwav and
capture send() events to JSONL.

Frida CLI's -o flag only redirects console.log; send() events go to the
host script. This wrapper subscribes to the host channel and dumps every
event to JSONL. Auto-detaches when the spawned process dies.

Usage:
    python reveng/fe_vtable_capture.py \\
        --hook viz/frida_hooks/fe_vtable_hook.js \\
        --target bin/spfy_dumpwav.exe \\
        --out spfy/build/fe_vtable_trace.jsonl \\
        -- "Hello from the front end test." c:/tmp/fe_probe.wav
"""

import argparse
import json
import sys
import time
import frida
from pathlib import Path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hook", required=True)
    ap.add_argument("--target", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--timeout", type=float, default=20.0)
    ap.add_argument("rest", nargs=argparse.REMAINDER,
                    help="-- target args (after --)")
    args = ap.parse_args()

    target_args = args.rest
    if target_args and target_args[0] == "--":
        target_args = target_args[1:]

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out = out_path.open("w", encoding="utf-8", buffering=1)
    n_msgs = [0]
    process_done = [False]

    def on_message(message, data):
        n_msgs[0] += 1
        if message["type"] == "send":
            out.write(json.dumps(message["payload"]) + "\n")
            payload = message["payload"]
            ptype = payload.get("type", "?")
            if ptype == "fe_vtable_dump":
                print(f"  [host] vtable_dump: {len(payload.get('entries', []))} slots")
            elif ptype == "fe_vt_batch":
                print(f"  [host] batch: {len(payload.get('items', []))} calls")
            elif ptype == "process_exit":
                print(f"  [host] process_exit: stats={payload.get('stats')}")
                process_done[0] = True
            else:
                print(f"  [host] {ptype}")
        elif message["type"] == "error":
            print(f"  [host] ERROR: {message.get('description', message)}")
        else:
            print(f"  [host] msg: {message}")

    print(f"[harness] spawning {args.target} {' '.join(target_args)}")
    pid = frida.spawn([args.target] + target_args)
    sess = frida.attach(pid)

    sess_done = [False]
    def on_detached(reason, *_a):
        print(f"[harness] session detached: {reason}")
        sess_done[0] = True
    sess.on("detached", on_detached)

    hook_js = Path(args.hook).read_text(encoding="utf-8")
    script = sess.create_script(hook_js)
    script.on("message", on_message)
    script.load()
    print(f"[harness] script loaded, resuming pid={pid}")
    frida.resume(pid)

    deadline = time.time() + args.timeout
    while time.time() < deadline:
        if sess_done[0] or process_done[0]:
            break
        time.sleep(0.1)

    # Final flush via rpc.
    try:
        st = script.exports_sync.flush()
        print(f"[harness] flushed; final stats={st}")
    except Exception as e:
        print(f"[harness] flush error: {e}")

    try:
        sess.detach()
    except Exception:
        pass
    out.close()
    print(f"[harness] {n_msgs[0]} messages written to {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
