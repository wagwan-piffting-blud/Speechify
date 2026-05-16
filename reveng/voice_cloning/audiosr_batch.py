"""
Batch-upscale 8kHz WAVs to 48kHz using AudioSR.

Tom's VDB audio is 8kHz (phone quality). RVC needs higher quality input
to produce natural-sounding output. AudioSR uses a diffusion model to
reconstruct the missing high frequencies.

Usage:
    python audiosr_batch.py [voice_name]
    python audiosr_batch.py mara
    python audiosr_batch.py craig
"""
import os
import glob
import sys
import time


def _detect_proj_root():
    """Find project root from cwd/script dir by looking for expected layout."""
    starts = (os.path.abspath(os.getcwd()), os.path.abspath(os.path.dirname(__file__)))
    seen = set()
    for start in starts:
        cur = start
        while cur and cur not in seen:
            seen.add(cur)
            if os.path.isfile(os.path.join(cur, "bin", "spfy_dumpwav.exe")):
                return cur
            parent = os.path.dirname(cur)
            if parent == cur:
                break
            cur = parent
    return os.path.abspath(os.path.dirname(__file__))


def main():
    voice_name = sys.argv[1] if len(sys.argv) > 1 else "mara"

    PROJ = _detect_proj_root()
    IN_DIR = os.path.join(PROJ, "en-US", voice_name, "output", "tom_all_for_rvc")
    OUT_DIR = os.path.join(PROJ, "en-US", voice_name, "output", "tom_all_upscaled")

    os.makedirs(OUT_DIR, exist_ok=True)

    wavs = sorted(glob.glob(os.path.join(IN_DIR, "*.wav")))
    print("Found %d WAVs to upscale in %s" % (len(wavs), IN_DIR))

    # Filter already done
    todo = []
    skipped = 0
    for wav_path in wavs:
        name = os.path.basename(wav_path)
        out_path = os.path.join(OUT_DIR, name)
        if os.path.exists(out_path):
            skipped += 1
        else:
            todo.append((wav_path, out_path))

    print("  %d already done, %d to upscale" % (skipped, len(todo)))

    if not todo:
        print("Nothing to do!")
        return

    # Import AudioSR
    import audiosr
    import soundfile as sf
    import numpy as np

    print("Loading AudioSR model...")
    model = audiosr.build_model(model_name="speech", device="cuda")
    print("  Model loaded")

    done = 0
    failed = 0
    t0 = time.time()

    for wav_path, out_path in todo:
        name = os.path.basename(wav_path)
        try:
            # AudioSR expects file path input
            waveform = audiosr.super_resolution(
                model,
                wav_path,
                seed=42,
                guidance_scale=3.5,
                ddim_steps=20,
            )
            # waveform may be tensor or numpy array
            audio = waveform[0]
            if hasattr(audio, 'cpu'):
                audio = audio.cpu().numpy()
            if audio.ndim == 2:
                audio = audio[0]  # mono
            # Trim trailing silence (threshold -40dB)
            abs_audio = np.abs(audio)
            threshold = 0.01  # ~-40dB
            # Find last sample above threshold
            above = np.where(abs_audio > threshold)[0]
            if len(above) > 0:
                # Keep 50ms of padding after last sound
                end = min(len(audio), above[-1] + int(48000 * 0.05))
                audio = audio[:end]
            # Normalize to int16 range
            audio = np.clip(audio, -1.0, 1.0)
            sf.write(out_path, audio, 48000)
            done += 1
        except Exception as e:
            failed += 1
            if failed <= 10:
                print("  FAILED: %s: %s" % (name, e))

        if (done + failed) % 50 == 0:
            elapsed = time.time() - t0
            rate = (done + failed) / max(elapsed, 1)
            remaining = (len(todo) - done - failed) / max(rate, 0.01)
            print("  [%d/%d] %d done, %d failed (%.1f/s, ~%.0fm remaining)" % (
                done + failed + skipped, len(wavs), done, failed,
                rate, remaining / 60))

    elapsed = time.time() - t0
    rate = done / max(elapsed, 1)
    print("\nDone in %.0fs (%.1f/s). %d upscaled, %d failed, %d skipped." % (
        elapsed, rate, done, failed, skipped))
    print("Output: %s" % OUT_DIR)
    print("\nNext: run RVC on the upscaled WAVs:")
    print("  python rvc_batch.py %s" % voice_name)


if __name__ == '__main__':
    main()
