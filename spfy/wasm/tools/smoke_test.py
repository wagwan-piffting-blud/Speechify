#!/usr/bin/env python3
"""Real-browser smoke test for the spfy WASM demo (Selenium + headless Chrome).

Serves ../build/web (the production bundle — run `npm run build` first) on
127.0.0.1 with Pages-like headers (no COEP, correct wasm MIME) and drives
headless Chrome through the lazy-voice path:

    Phase 1  Tom     — default auto-load + synth                 (core runtime)
    Phase 2  Felix   — switch to fr-CA: fresh module + fetch + synth
    Phase 3a Paulina — in-page cross-origin fetch of her .vcf    (external CORS)
    Phase 3b Paulina — full external load from her host + synth  (best effort; ~253 MB)

Exit 0 only if every REQUIRED phase passes (1, 2, 3a). 3b downloads the
whole Paulina corpus over the network and is reported but non-fatal; pass
--no-paulina-full to skip it, or --no-paulina to skip her entirely (e.g.
offline, or when her external host isn't up).

Requires: selenium>=4.6 (bundles Selenium Manager, which auto-provisions
chromedriver) and a Chrome/Chromium install.

    python tools/smoke_test.py            # full run
    python tools/smoke_test.py --no-paulina-full
    python tools/smoke_test.py --headed   # watch it

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import http.server
import socketserver
import sys
import threading
import time
from pathlib import Path

from selenium import webdriver
from selenium.webdriver.chrome.options import Options
from selenium.webdriver.common.by import By
from selenium.webdriver.support.ui import Select

WEB_DIR = (Path(__file__).resolve().parent.parent / "build" / "web")
PORT = 8099

MIME = {
    ".wasm": "application/wasm", ".js": "text/javascript",
    ".mjs": "text/javascript", ".json": "application/json",
    ".html": "text/html", ".css": "text/css",
    ".vin": "application/octet-stream", ".vdb": "application/octet-stream",
    ".vcf": "application/octet-stream",
}


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=str(WEB_DIR), **k)

    def guess_type(self, path):
        ext = Path(path).suffix.lower()
        if ext in MIME:
            return MIME[ext]
        if ".part" in Path(path).name:
            return "application/octet-stream"
        return super().guess_type(path)

    def log_message(self, *a):
        pass


def status_text(drv):
    try:
        return drv.find_element(By.ID, "status").text
    except Exception:
        return ""


def wait_status(drv, needle, timeout, label):
    t0, last = time.time(), ""
    while time.time() - t0 < timeout:
        txt = status_text(drv)
        if txt != last:
            print(f"    [{label}] status: {txt}")
            last = txt
        low = txt.lower()
        if needle.lower() in low:
            return
        if ("failed" in low or low.startswith("error")) and needle.lower() not in low:
            raise RuntimeError(f"error status waiting for '{needle}': {txt}")
        time.sleep(0.5)
    raise TimeoutError(f"timed out ({timeout}s) waiting for '{needle}' (last: {last})")


def do_synth(drv, text, label):
    ta = drv.find_element(By.ID, "text-input")
    ta.clear()
    ta.send_keys(text)
    drv.find_element(By.ID, "speak-btn").click()
    wait_status(drv, "Synthesized", 60, label)
    samples = drv.find_element(By.ID, "meta-samples").text
    print(f"    [{label}] {samples}")
    if "Duration:" not in samples:
        raise RuntimeError("meta-samples missing Duration after synth")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-paulina", action="store_true", help="skip Paulina entirely")
    ap.add_argument("--no-paulina-full", action="store_true",
                    help="skip the 253 MB Paulina download (keep the CORS check)")
    ap.add_argument("--headed", action="store_true", help="run a visible browser")
    args = ap.parse_args()

    if not (WEB_DIR / "index.html").is_file():
        print(f"FAIL: {WEB_DIR / 'index.html'} not found — run `npm run build` first.")
        return 2

    socketserver.ThreadingTCPServer.allow_reuse_address = True
    httpd = socketserver.ThreadingTCPServer(("127.0.0.1", PORT), Handler)
    httpd.daemon_threads = True
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{PORT}/"
    print(f"serving {WEB_DIR} at {base}")

    opts = Options()
    if not args.headed:
        opts.add_argument("--headless=new")
    opts.add_argument("--no-sandbox")
    opts.add_argument("--disable-dev-shm-usage")
    opts.add_argument("--autoplay-policy=no-user-gesture-required")
    opts.add_argument("--window-size=1280,900")
    opts.set_capability("goog:loggingPrefs", {"browser": "ALL"})

    results = {}
    drv = webdriver.Chrome(options=opts)
    try:
        drv.set_script_timeout(30)
        drv.get(base)

        print("[Phase 1] Tom — default load + synth")
        wait_status(drv, "Ready", 90, "Tom")
        ids = [o.get_attribute("value")
               for o in drv.find_elements(By.CSS_SELECTOR, "#voice-select option")]
        print(f"    voices in picker ({len(ids)}): {ids}")
        assert "Tom" in status_text(drv), "Tom not loaded"
        do_synth(drv, "The quick brown fox jumps over the lazy dog.", "Tom")
        results["1_tom"] = "PASS"

        print("[Phase 2] Felix — switch to fr-CA (fresh module) + synth")
        Select(drv.find_element(By.ID, "voice-select")).select_by_value("felix")
        wait_status(drv, "Ready", 120, "Felix")
        assert "Felix" in status_text(drv), "Felix not loaded"
        do_synth(drv, "Bonjour, je m'appelle Felix.", "Felix")
        results["2_felix"] = "PASS"

        if not args.no_paulina:
            print("[Phase 3a] Paulina — in-page cross-origin fetch (CORS)")
            cors = drv.execute_async_script("""
                const cb = arguments[arguments.length - 1];
                fetch('https://wagspuzzle.space/assets/paulina/paulina.vcf')
                  .then(r => cb({ok: r.ok, status: r.status,
                                 len: r.headers.get('content-length')}))
                  .catch(e => cb({error: String(e)}));
            """)
            print(f"    fetch result: {cors}")
            if not cors.get("ok"):
                raise RuntimeError(f"cross-origin fetch of Paulina failed: {cors}")
            results["3a_paulina_cors"] = "PASS"

            if not args.no_paulina_full:
                print("[Phase 3b] Paulina — full external load + synth (~253 MB)")
                try:
                    drv.execute_script("window.confirm = () => true;")
                    Select(drv.find_element(By.ID, "voice-select")).select_by_value("paulina")
                    wait_status(drv, "Ready", 300, "Paulina")
                    assert "Paulina" in status_text(drv), "Paulina not loaded"
                    do_synth(drv, "Hola, me llamo Paulina.", "Paulina")
                    results["3b_paulina_full"] = "PASS"
                except Exception as e:
                    results["3b_paulina_full"] = f"WARN ({e})"

        for l in drv.get_log("browser"):
            if l["level"] == "SEVERE" and "favicon" not in l["message"]:
                print("    console SEVERE:", l["message"][:200])
    except Exception as e:
        print(f"\nFAIL: {type(e).__name__}: {e}")
        try:
            for l in drv.get_log("browser")[-25:]:
                print("   console:", l["level"], l["message"][:200])
        except Exception:
            pass
        results.setdefault("_error", str(e))
    finally:
        try:
            drv.quit()
        except Exception:
            pass

    print("\n=== RESULTS ===")
    for k, v in results.items():
        print(f"  {k}: {v}")
    required = ["1_tom", "2_felix"] + ([] if args.no_paulina else ["3a_paulina_cors"])
    ok = all(results.get(k) == "PASS" for k in required)
    print("\nOVERALL:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
