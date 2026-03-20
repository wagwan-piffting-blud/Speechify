"""
Batch-convert WAVs through RVC model with threading.

Processes AudioSR-upscaled WAVs through a trained RVC model to perform
voice conversion. Uses multiple threads sharing one GPU.

Usage:
    python rvc_batch.py [voice_name] [--model <path>] [--f0up-key <int>] [--workers <int>]
    python rvc_batch.py mara
    python rvc_batch.py craig --model path/to/craig.pth --f0up-key 4

Defaults:
    --model:     en-US/<voice>/output/rvc-no-gui/RVC/assets/weights/<voice>.pth
    --f0up-key:  8  (semitones; male->female typical)
    --workers:   8  (threads sharing one GPU)
"""
import os
import glob
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
import threading


def _detect_proj_root():
    """Find project root from cwd/script dir by looking for expected layout."""
    starts = (os.path.abspath(os.getcwd()), os.path.abspath(os.path.dirname(__file__)))
    seen = set()
    for start in starts:
        cur = start
        while cur and cur not in seen:
            seen.add(cur)
            if os.path.isfile(os.path.join(cur, "bin", "spfy_dumpwav32_8khz.exe")):
                return cur
            parent = os.path.dirname(cur)
            if parent == cur:
                break
            cur = parent
    return os.path.abspath(os.path.dirname(__file__))


def main():
    voice_name = sys.argv[1] if len(sys.argv) > 1 else "mara"

    # Parse optional arguments
    model_path = None
    f0up_key = 8
    n_workers = 8
    for i, arg in enumerate(sys.argv):
        if arg == '--model' and i + 1 < len(sys.argv):
            model_path = sys.argv[i + 1]
        elif arg == '--f0up-key' and i + 1 < len(sys.argv):
            f0up_key = int(sys.argv[i + 1])
        elif arg == '--workers' and i + 1 < len(sys.argv):
            n_workers = int(sys.argv[i + 1])

    PROJ = _detect_proj_root()
    voice_out = os.path.join(PROJ, "en-US", voice_name, "output")

    IN_DIR = os.path.join(PROJ, "reveng", "voice_cloning", "rvc_input_flashsr")
    OUT_DIR = os.path.join(voice_out, "%s_rvc_pure_crepe" % voice_name)
    if model_path is None:
        model_path = os.path.join(
            voice_out, "rvc-no-gui", "RVC", "assets", "weights",
            "%s.pth" % voice_name)

    print("=" * 70)
    print("  RVC Batch Conversion")
    print("  Voice:     %s" % voice_name)
    print("  Input:     %s" % IN_DIR)
    print("  Output:    %s" % OUT_DIR)
    print("  Model:     %s" % model_path)
    print("  f0up_key:  %d" % f0up_key)
    print("  Workers:   %d" % n_workers)
    print("=" * 70)

    os.makedirs(OUT_DIR, exist_ok=True)

    wavs = sorted(glob.glob(os.path.join(IN_DIR, "*.wav")))
    print("Found %d WAVs to convert" % len(wavs))

    # Filter out already-done
    todo = []
    skipped = 0
    for wav_path in wavs:
        name = os.path.basename(wav_path)
        out_path = os.path.join(OUT_DIR, name)
        if os.path.exists(out_path):
            skipped += 1
        else:
            todo.append((wav_path, out_path))

    print("  %d already done, %d to convert, %d workers" % (skipped, len(todo), n_workers))

    if not todo:
        print("Nothing to do!")
        return

    # Create one RVC instance per worker to avoid lock contention
    from rvc_python.infer import RVCInference

    print("Loading RVC models...")
    models = []
    for i in range(n_workers):
        rvc = RVCInference(device="cuda:0")
        rvc.load_model(model_path)
        rvc.set_params(f0up_key=f0up_key, f0method="crepe", protect=0.1)
        models.append(rvc)
    print("  %d models loaded" % n_workers)

    lock = threading.Lock()
    done = 0
    failed = 0
    t0 = time.time()

    def convert(args):
        nonlocal done, failed
        wav_path, out_path = args
        worker_id = threading.current_thread()._ident % n_workers
        rvc = models[worker_id % len(models)]
        try:
            rvc.infer_file(wav_path, out_path)
            with lock:
                done += 1
                if done % 100 == 0:
                    elapsed = time.time() - t0
                    rate = done / elapsed
                    remaining = (len(todo) - done - failed) / max(rate, 0.01)
                    print("  [%d/%d] %d done, %d failed (%.1f/s, ~%.0fm remaining)" % (
                        done + skipped, len(wavs), done, failed,
                        rate, remaining / 60))
        except Exception as e:
            with lock:
                failed += 1
                if failed <= 5:
                    print("  FAILED: %s: %s" % (os.path.basename(wav_path), e))

    with ThreadPoolExecutor(max_workers=n_workers) as pool:
        futures = [pool.submit(convert, args) for args in todo]
        for f in as_completed(futures):
            pass  # errors handled inside convert()

    elapsed = time.time() - t0
    rate = done / max(elapsed, 1)
    print("\nDone in %.0fs (%.1f/s). %d converted, %d failed, %d skipped." % (
        elapsed, rate, done, failed, skipped))


if __name__ == '__main__':
    main()
