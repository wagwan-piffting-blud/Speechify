"""Generate the Speechify demo set.

Three passes:

  1. every available voice through the REAL 3.0.5 engine (bin/spfy_dumpwav.exe
     against a running bin/Speechify.exe), which means rewriting
     config/SWIttsConfig.xml and restarting the server once per voice -- the
     server reads the voice config exactly once, at startup;
  2. the same demos through our reimplementation (spfy_synth.exe), which takes
     vin/vdb/vcf as arguments and so needs no server and can run multithreaded;
  3. Tom only, in Speechify 4 mode (spfy_synth -4), over a set of common
     phrases plus one long verbatim bulletin.

config/SWIttsConfig.xml is backed up before the first edit and restored on the
way out, including on Ctrl+C. Server output goes to a FILE, never a pipe: the
server is long-lived and a full pipe buffer deadlocks it.

  python demos/gen_demo.py                 everything
  python demos/gen_demo.py --only tom      one voice
  python demos/gen_demo.py --skip-engine   no server, our engine only
  python demos/gen_demo.py --list          show what would be rendered
"""
import argparse
import concurrent.futures as futures
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import wave

# Everything is relative to the repo, which is this script's parent. Hard-coding
# an absolute path bakes in whoever happened to run it first.
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "bin")
CONFIG = os.path.join(ROOT, "config", "SWIttsConfig.xml")
CONFIG_BAK = CONFIG + ".gen_demo.bak"
SERVER_EXE = os.path.join(BIN, "Speechify.exe")
DUMPWAV_EXE = os.path.join(BIN, "spfy_dumpwav.exe")

# spfy_synth is a build artefact, so it has no fixed home. Resolved from
# --synth, falling back to PATH; see resolve_synth().
SYNTH_EXE = None
DEMOS = os.path.join(ROOT, "demos")
OUT_ENGINE = os.path.join(DEMOS, "engine")
OUT_SPFY = os.path.join(DEMOS, "spfy")
OUT_S4 = os.path.join(DEMOS, "spfy4m")
TEXT_DIR = os.path.join(DEMOS, "text")
CEM_TXT = os.path.join(TEXT_DIR, "cem.txt")
MANIFEST = os.path.join(DEMOS, "manifest.json")
LOG_DIR = os.path.join(tempfile.gettempdir(), "gen_demo")


def resolve_synth(arg):
    """Locate spfy_synth: --synth, then PATH, then the usual build outputs.

    It is a build artefact, so there is no path that is right for everyone."""
    if arg:
        if not os.path.isfile(arg):
            sys.exit("--synth: no such file: %s" % arg)
        return os.path.abspath(arg)
    found = shutil.which("spfy_synth") or shutil.which("spfy_synth.exe")
    if found:
        return found
    for rel in (os.path.join("build", "src", "cli"),
                os.path.join("build32", "src", "cli"),
                os.path.join("spfy", "build", "src", "cli")):
        for exe in ("spfy_synth", "spfy_synth.exe"):
            cand = os.path.join(ROOT, rel, exe)
            if os.path.isfile(cand):
                return cand
    return None

SERVER_PORT = 5555
SERVER_TIMEOUT_S = 30.0

LANGS = ("en-US", "es-MX", "fr-CA")

# The clone voices share tom.vin byte for byte and their VDB audio is
# synthetic -- fine to demo, never admissible as evidence about SpeechWorks.
SYNTHETIC = {"aicraig", "crsmara", "crstom"}

DISPLAY = {
    "tom": "Tom",
    "jill": "Jill",
    "javier": "Javier",
    "paulina": "Paulina",
    "felix": "Félix",
    "aicraig": "Craig",
    "crsmara": "C R S Mara",
    "crstom": "C R S Tom",
}

DEMO_TEXT = {
    "en-US": (
        "Hello. My name is {name}. I am a concatenative text to speech voice "
        "for Speechify three point zero point five, by SpeechWorks International.\n"
        "The National Weather Service in Melburne, has issued \\![.1e] tornado warning "
        "for. Northern Lake County in east central Florida.\n"
    ),
    "es-MX": (
        "Hola. Me llamo {name}. Soy una voz sintética de Speechify tres punto "
        "cero punto cinco, de SpeechWorks International.\n"
        "El servicio meteorológico nacional ha emitido una advertencia de "
        "tormenta eléctrica severa.\n"
        "Las temperaturas máximas estarán en los veintiocho grados.\n"
    ),
    "fr-CA": (
        "Bonjour. Je m'appelle {name}. Je suis une voix de synthèse vocale de "
        "Speechify trois point zéro point cinq, de SpeechWorks International.\n"
        "Environnement Canada a émis un avertissement d'orage violent pour les "
        "régions suivantes.\n"
        "Les températures maximales atteindront vingt-huit degrés.\n"
    ),
}

# Speechify 4 mode, Tom only. Text is written verbatim -- the \! escapes are
# engine control codes, not Python escapes, so these are raw strings.
S4_PHRASES = [
    ("radar_threat", r"\![ToBI:H*]This is a radar indicated threat."),
    ("highs_60s", "Highs in the lower 60s."),
    ("basement", "Move to a basement or the most interior room on the lowest "
                 "floor of a building."),
    ("nws_tornado", "National Weather Service Doppler Radar indicated a severe "
                    "thunderstorm capable of producing a tornado."),
]
S4_FILE_PHRASES = [("cem_bulletin", CEM_TXT)]


def die(msg):
    print("ERROR: " + msg, file=sys.stderr)
    sys.exit(1)


def find_voices(fmt):
    """Every voice directory carrying a complete vin/vcf/vdb set at `fmt` kHz."""
    out = []
    for lang in LANGS:
        langdir = os.path.join(ROOT, lang)
        if not os.path.isdir(langdir):
            continue
        for name in sorted(os.listdir(langdir)):
            vdir = os.path.join(langdir, name)
            if not os.path.isdir(vdir):
                continue
            vin = os.path.join(vdir, name + ".vin")
            vcf = os.path.join(vdir, name + ".vcf")
            vdb = os.path.join(vdir, "%s%d.vdb" % (name, fmt))
            if os.path.isfile(vin) and os.path.isfile(vcf) and os.path.isfile(vdb):
                out.append({"name": name, "lang": lang, "dir": vdir,
                            "vin": vin, "vcf": vcf, "vdb": vdb,
                            "display": DISPLAY.get(name, name.title()),
                            "synthetic": name in SYNTHETIC})
    return out


def demo_text_for(voice):
    tmpl = DEMO_TEXT.get(voice["lang"])
    if tmpl is None:
        tmpl = DEMO_TEXT["en-US"]
    return tmpl.format(name=voice["display"])


def write_text(path, text):
    # UTF-8 for both engines: spfy_dumpwav documents UTF-8/UTF-16LE, and
    # spfy_synth was verified to produce byte-identical audio from UTF-8 and
    # ISO-8859-1 copies of the same accented French text.
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


# ---------------------------------------------------------------- config

def read_config():
    with open(CONFIG, "r", encoding="iso-8859-1", newline="") as f:
        return f.read()


def set_param(xml, name, value):
    pat = re.compile(r'(<param\s+name="' + re.escape(name) + r'"\s*>\s*<value>)'
                     r'([^<]*)(</value>)')
    new, n = pat.subn(lambda m: m.group(1) + " " + str(value) + " " + m.group(3),
                      xml)
    if n != 1:
        die("config: expected exactly 1 '%s' param, found %d" % (name, n))
    return new


def apply_voice_config(voice, fmt):
    xml = read_config()
    xml = set_param(xml, "tts.voice.name", voice["name"])
    xml = set_param(xml, "tts.voice.language", voice["lang"])
    xml = set_param(xml, "tts.voice.format", fmt)
    with open(CONFIG, "w", encoding="iso-8859-1", newline="") as f:
        f.write(xml)


def backup_config():
    if not os.path.isfile(CONFIG):
        die("no config at " + CONFIG)
    shutil.copy2(CONFIG, CONFIG_BAK)


def restore_config():
    if os.path.isfile(CONFIG_BAK):
        shutil.copy2(CONFIG_BAK, CONFIG)
        os.remove(CONFIG_BAK)


# ---------------------------------------------------------------- server

def port_open(port, timeout=0.25):
    s = socket.socket()
    s.settimeout(timeout)
    try:
        s.connect(("127.0.0.1", port))
        return True
    except OSError:
        return False
    finally:
        s.close()


def stop_server(proc=None):
    if proc is not None and proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    subprocess.run(["taskkill", "/F", "/IM", "Speechify.exe"],
                   capture_output=True)
    t0 = time.time()
    while port_open(SERVER_PORT) and time.time() - t0 < 10.0:
        time.sleep(0.1)


def start_server(tag):
    log = os.path.join(LOG_DIR, "server_%s.log" % tag)
    fh = open(log, "wb")
    proc = subprocess.Popen([SERVER_EXE], cwd=BIN, stdout=fh, stderr=fh)
    t0 = time.time()
    while time.time() - t0 < SERVER_TIMEOUT_S:
        if port_open(SERVER_PORT):
            return proc, fh, log, time.time() - t0
        if proc.poll() is not None:
            fh.close()
            return None, None, log, time.time() - t0
        time.sleep(0.1)
    fh.close()
    stop_server(proc)
    return None, None, log, time.time() - t0


# ---------------------------------------------------------------- render

def wav_info(path):
    try:
        with wave.open(path, "rb") as w:
            return {"samples": w.getnframes(), "rate": w.getframerate(),
                    "seconds": round(w.getnframes() / float(w.getframerate()), 3)}
    except Exception:
        return None


def run_dumpwav(text_path, out_wav, fmt):
    cmd = [DUMPWAV_EXE]
    if fmt == 16:
        cmd.append("--16k")
    cmd += ["-f", text_path, out_wav]
    p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                       errors="replace")
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def run_synth(voice, text_path, out_wav, s4):
    cmd = [SYNTH_EXE]
    if s4:
        cmd.append("-4")
    cmd += [voice["vin"], voice["vdb"], voice["vcf"], "-f", text_path, out_wav]
    p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                       errors="replace")
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def write_manifest(results, fmt):
    """Merge this run into the manifest -- a --only run must not erase the
    record of renders whose wavs are still sitting on disk."""
    kept = []
    if os.path.isfile(MANIFEST):
        try:
            with open(MANIFEST, "r", encoding="utf-8") as f:
                old = json.load(f).get("renders", [])
            fresh = {(r["kind"], r["id"]) for r in results}
            kept = [r for r in old if (r.get("kind"), r.get("id")) not in fresh
                    and r.get("wav")
                    and os.path.isfile(os.path.join(ROOT, r["wav"]))]
        except (ValueError, KeyError):
            kept = []
    rows = sorted(kept + results, key=lambda r: (r["kind"], r["id"]))
    with open(MANIFEST, "w", encoding="utf-8") as f:
        json.dump({"generated": time.strftime("%Y-%m-%dT%H:%M:%S"),
                   "format_khz": fmt, "renders": rows}, f, indent=2)


def record(results, kind, key, voice, text_path, out_wav, rc, log):
    ok = rc == 0 and os.path.isfile(out_wav)
    row = {"kind": kind, "id": key, "voice": voice["name"],
           "lang": voice["lang"], "synthetic": voice["synthetic"],
           "text_file": os.path.relpath(text_path, ROOT).replace("\\", "/"),
           "wav": os.path.relpath(out_wav, ROOT).replace("\\", "/") if ok else None,
           "rc": rc, "ok": ok}
    if ok:
        row["audio"] = wav_info(out_wav)
    else:
        row["error"] = log.strip().splitlines()[-3:]
    results.append(row)
    return ok


# ---------------------------------------------------------------- passes

def pass_engine(voices, fmt, results):
    print("\n=== pass 1: real Speechify 3.0.5 (server + spfy_dumpwav) ===")
    was_running = port_open(SERVER_PORT)
    if was_running:
        print("  note: a Speechify server was already running; it will be "
              "stopped and the original config restored at the end")
    backup_config()
    proc = fh = None
    try:
        for i, v in enumerate(voices, 1):
            tag = "%s_%s" % (v["lang"], v["name"])
            print("  [%d/%d] %-9s %s" % (i, len(voices), v["name"], v["lang"]),
                  end="", flush=True)
            stop_server(proc)
            if fh is not None:
                fh.close()
            apply_voice_config(v, fmt)
            proc, fh, log, dt = start_server(tag)
            if proc is None:
                print("   SERVER FAILED TO START (%.1fs) -- see %s" % (dt, log))
                results.append({"kind": "engine", "id": v["name"],
                                "voice": v["name"], "lang": v["lang"],
                                "ok": False, "rc": None,
                                "error": ["server did not come up", log]})
                continue
            text = demo_text_for(v)
            tp = os.path.join(TEXT_DIR, "demo_%s.txt" % v["name"])
            write_text(tp, text)
            out = os.path.join(OUT_ENGINE, v["name"] + ".wav")
            rc, out_log = run_dumpwav(tp, out, fmt)
            ok = record(results, "engine", v["name"], v, tp, out, rc, out_log)
            info = wav_info(out) if ok else None
            print("   up %.2fs  %s%s" % (
                dt, "ok" if ok else "FAILED rc=%s" % rc,
                "  %.2fs audio" % info["seconds"] if info else ""))
    finally:
        stop_server(proc)
        if fh is not None:
            fh.close()
        restore_config()
        print("  config restored, server stopped")


def pass_spfy(voices, results, workers):
    print("\n=== pass 2: spfy reimplementation (spfy_synth) ===")
    jobs = []
    for v in voices:
        tp = os.path.join(TEXT_DIR, "demo_%s.txt" % v["name"])
        if not os.path.isfile(tp):
            write_text(tp, demo_text_for(v))
        jobs.append((v, tp, os.path.join(OUT_SPFY, v["name"] + ".wav")))

    with futures.ThreadPoolExecutor(max_workers=workers) as ex:
        fut = {ex.submit(run_synth, v, tp, out, False): (v, tp, out)
               for v, tp, out in jobs}
        done = 0
        for f in futures.as_completed(fut):
            v, tp, out = fut[f]
            rc, log = f.result()
            ok = record(results, "spfy", v["name"], v, tp, out, rc, log)
            done += 1
            info = wav_info(out) if ok else None
            print("  [%d/%d] %-9s %s%s" % (
                done, len(jobs), v["name"], "ok" if ok else "FAILED rc=%s" % rc,
                "  %.2fs audio" % info["seconds"] if info else ""))


def pass_s4(voices, results, workers, baseline):
    tom = next((v for v in voices if v["name"] == "tom"), None)
    if tom is None:
        print("\n=== pass 3: skipped, tom not available ===")
        return
    print("\n=== pass 3: Tom in Speechify 4 mode (spfy_synth -4) ===")

    jobs = []
    for slug, text in S4_PHRASES:
        tp = os.path.join(TEXT_DIR, "s4_%s.txt" % slug)
        write_text(tp, text + "\n")
        jobs.append((slug, tp))
    for slug, src in S4_FILE_PHRASES:
        if not os.path.isfile(src):
            print("  skipping %s: %s not found" % (slug, src))
            continue
        jobs.append((slug, src))

    work = [(slug, tp, os.path.join(OUT_S4, slug + ".wav"), True)
            for slug, tp in jobs]
    if baseline:
        work += [(slug, tp, os.path.join(OUT_S4, slug + "_no_s4.wav"), False)
                 for slug, tp in jobs]

    with futures.ThreadPoolExecutor(max_workers=workers) as ex:
        fut = {ex.submit(run_synth, tom, tp, out, s4): (slug, tp, out, s4)
               for slug, tp, out, s4 in work}
        done = 0
        for f in futures.as_completed(fut):
            slug, tp, out, s4 = fut[f]
            rc, log = f.result()
            key = slug if s4 else slug + " (baseline)"
            ok = record(results, "spfy4m" if s4 else "spfy4m_baseline", key,
                        tom, tp, out, rc, log)
            done += 1
            info = wav_info(out) if ok else None
            print("  [%d/%d] %-16s %s%s" % (
                done, len(work), key, "ok" if ok else "FAILED rc=%s" % rc,
                "  %.2fs audio" % info["seconds"] if info else ""))


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--synth", metavar="PATH", default=None,
                    help="path to the spfy_synth executable; a build artefact, "
                         "so it is looked for on PATH and in the usual build "
                         "directories when this is omitted. Required unless "
                         "both --skip-spfy and --skip-s4 are given.")
    ap.add_argument("--format", type=int, default=8, choices=(8, 16),
                    help="voice sample rate in kHz (default 8)")
    ap.add_argument("--only", action="append", default=[], metavar="VOICE",
                    help="restrict to this voice (repeatable)")
    ap.add_argument("--skip", action="append", default=[], metavar="VOICE",
                    help="exclude this voice (repeatable)")
    ap.add_argument("--no-synthetic", action="store_true",
                    help="exclude the cloned voices (%s)"
                         % ", ".join(sorted(SYNTHETIC)))
    ap.add_argument("--skip-engine", action="store_true",
                    help="skip pass 1 -- no server, no config edits")
    ap.add_argument("--skip-spfy", action="store_true", help="skip pass 2")
    ap.add_argument("--skip-s4", action="store_true", help="skip pass 3")
    ap.add_argument("--s4-baseline", action="store_true",
                    help="also render the S4 phrases with the mode OFF, for A/B")
    ap.add_argument("--workers", type=int, default=min(24, (os.cpu_count() or 4)),
                    help="parallel spfy_synth processes (default %(default)s)")
    ap.add_argument("--list", action="store_true",
                    help="show the voices found and exit")
    args = ap.parse_args()

    global SYNTH_EXE
    SYNTH_EXE = resolve_synth(args.synth)
    if (SYNTH_EXE is None and not args.list
            and not (args.skip_spfy and args.skip_s4)):
        sys.exit("spfy_synth not found. Pass --synth <path>, put it on PATH, "
                 "or skip the passes that need it with "
                 "--skip-spfy --skip-s4.")

    voices = find_voices(args.format)
    if args.only:
        want = {v.lower() for v in args.only}
        voices = [v for v in voices if v["name"].lower() in want]
    if args.skip:
        drop = {v.lower() for v in args.skip}
        voices = [v for v in voices if v["name"].lower() not in drop]
    if args.no_synthetic:
        voices = [v for v in voices if not v["synthetic"]]
    if not voices:
        die("no voices matched")

    if args.list:
        print("%-10s %-7s %-10s %s" % ("voice", "lang", "display", "vdb"))
        for v in voices:
            print("%-10s %-7s %-10s %s%s" % (
                v["name"], v["lang"], v["display"],
                os.path.relpath(v["vdb"], ROOT),
                "   [cloned voice, demo only]" if v["synthetic"] else ""))
        return 0

    for d in (OUT_ENGINE, OUT_SPFY, OUT_S4, TEXT_DIR, LOG_DIR):
        os.makedirs(d, exist_ok=True)

    for exe, need in ((SERVER_EXE, not args.skip_engine),
                      (DUMPWAV_EXE, not args.skip_engine),
                      (SYNTH_EXE, not (args.skip_spfy and args.skip_s4))):
        if need and not (exe and os.path.isfile(exe)):
            die("missing " + (exe or "spfy_synth (pass --synth)"))

    print("%d voice(s): %s" % (len(voices), ", ".join(v["name"] for v in voices)))
    results = []
    t0 = time.time()
    try:
        if not args.skip_engine:
            pass_engine(voices, args.format, results)
        if not args.skip_spfy:
            pass_spfy(voices, results, args.workers)
        if not args.skip_s4:
            pass_s4(voices, results, args.workers, args.s4_baseline)
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
    finally:
        restore_config()

    write_manifest(results, args.format)

    bad = [r for r in results if not r["ok"]]
    print("\n%d render(s) in %.1fs, %d failed" % (len(results), time.time() - t0,
                                                  len(bad)))
    for r in bad:
        print("  FAILED %-8s %-16s %s" % (r["kind"], r["id"],
                                          " | ".join(r.get("error") or [])))
    print("manifest: " + os.path.relpath(MANIFEST, ROOT))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
