"""
Compare pitch between reference audio and RVC output.
Recommends optimal f0up_key for matching the target voice.

Usage:
    python diag_pitch.py [voice_name]
    python diag_pitch.py mara
    python diag_pitch.py craig
"""
import glob
import os
import sys
import wave
import numpy as np


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


def get_f0_from_wav(wav_path, sr_target=16000):
    """Extract F0 using pyworld (preferred) or autocorrelation fallback."""
    with wave.open(wav_path, 'rb') as wf:
        sr = wf.getframerate()
        nch = wf.getnchannels()
        sw = wf.getsampwidth()
        raw = wf.readframes(wf.getnframes())

    if sw == 2:
        samples = np.frombuffer(raw, dtype=np.int16).astype(np.float64)
    elif sw == 4:
        samples = np.frombuffer(raw, dtype=np.int32).astype(np.float64) / 65536.0
    else:
        samples = np.frombuffer(raw, dtype=np.float64)

    if nch > 1:
        samples = samples.reshape(-1, nch).mean(axis=1)

    # Resample if needed
    if sr != sr_target:
        from scipy.signal import resample_poly
        from math import gcd
        g = gcd(sr, sr_target)
        samples = resample_poly(samples, sr_target // g, sr // g)
        sr = sr_target

    # Use pyworld if available, else simple method
    try:
        import pyworld as pw
        samples_64 = samples.astype(np.float64)
        f0, _ = pw.harvest(samples_64, sr, f0_floor=60, f0_ceil=500)
        f0_voiced = f0[f0 > 0]
        return f0_voiced
    except ImportError:
        pass

    # Fallback: autocorrelation pitch detection
    frame_len = int(sr * 0.03)  # 30ms frames
    hop = int(sr * 0.01)        # 10ms hop
    f0_list = []

    for start in range(0, len(samples) - frame_len, hop):
        frame = samples[start:start + frame_len]
        frame = frame - np.mean(frame)
        if np.max(np.abs(frame)) < 100:  # silence
            continue

        # Autocorrelation
        corr = np.correlate(frame, frame, mode='full')
        corr = corr[len(corr)//2:]

        # Find first peak after minimum lag (60 Hz = sr/60 samples)
        min_lag = int(sr / 500)  # 500 Hz max
        max_lag = int(sr / 60)   # 60 Hz min

        if max_lag >= len(corr):
            continue

        segment = corr[min_lag:max_lag]
        if len(segment) == 0:
            continue

        peak_idx = np.argmax(segment) + min_lag
        if corr[peak_idx] > 0.3 * corr[0]:  # voiced threshold
            f0 = sr / peak_idx
            if 60 < f0 < 500:
                f0_list.append(f0)

    return np.array(f0_list)


def analyze_dir(directory, label, max_files=200):
    """Analyze pitch across a directory of WAVs."""
    wavs = sorted(glob.glob(os.path.join(directory, "*.wav")))
    if not wavs:
        print("  No WAVs found in %s" % directory)
        return None

    wavs = wavs[:max_files]
    all_f0 = []

    for wav_path in wavs:
        try:
            f0 = get_f0_from_wav(wav_path)
            if len(f0) > 0:
                all_f0.extend(f0.tolist())
        except Exception:
            continue

    if not all_f0:
        print("  No voiced frames found in %s" % directory)
        return None

    all_f0 = np.array(all_f0)
    median = np.median(all_f0)
    mean = np.mean(all_f0)
    p25 = np.percentile(all_f0, 25)
    p75 = np.percentile(all_f0, 75)

    print("  %s (%d files, %d voiced frames):" % (label, len(wavs), len(all_f0)))
    print("    Median F0: %.1f Hz" % median)
    print("    Mean F0:   %.1f Hz" % mean)
    print("    P25-P75:   %.1f - %.1f Hz" % (p25, p75))

    return median


def main():
    voice_name = sys.argv[1] if len(sys.argv) > 1 else "mara"

    PROJ = _detect_proj_root()
    voice_dir = os.path.join(PROJ, "en-US", voice_name, "output")

    # Directories to check for RVC output and reference
    RVC_DIR = os.path.join(voice_dir, "tom_all_rvc")
    REF_CANDIDATES = [
        os.path.join(voice_dir, "rvc_training"),   # curated training WAVs
        os.path.join(voice_dir, "resynth_rvc"),     # Qwen RVC'd
    ]

    print("=" * 60)
    print("PITCH DIAGNOSTIC -- voice: %s" % voice_name)
    print("=" * 60)

    # Analyze RVC output (current voice)
    print("\nCurrent RVC output (what the engine uses):")
    rvc_median = analyze_dir(RVC_DIR, "RVC output")

    # Analyze reference (what the voice should sound like)
    ref_median = None
    for ref_dir in REF_CANDIDATES:
        if os.path.isdir(ref_dir):
            print("\nReference audio (target voice):")
            ref_median = analyze_dir(ref_dir, os.path.basename(ref_dir))
            if ref_median is not None:
                break

    if rvc_median is None:
        print("\nERROR: Could not analyze RVC output in %s" % RVC_DIR)
        return

    if ref_median is None:
        print("\nNo reference found. Enter target pitch manually.")
        print("Typical female voice: 165-255 Hz")
        print("Typical male voice: 85-155 Hz")
        print("Tom (original): ~120 Hz")
        return

    # Calculate optimal f0up_key
    # Semitones = 12 * log2(target / source)
    ratio = ref_median / rvc_median
    semitone_correction = 12 * np.log2(ratio)

    print("\n" + "=" * 60)
    print("ANALYSIS")
    print("=" * 60)
    print("  RVC output median:   %.1f Hz" % rvc_median)
    print("  Reference median:    %.1f Hz" % ref_median)
    print("  Ratio:               %.3f" % ratio)
    print("  Correction needed:   %.1f semitones" % semitone_correction)
    print()

    # Assume current f0up_key based on voice characteristics
    current_key = 8  # default for female target from male source
    recommended = round(current_key + semitone_correction)

    print("  Assumed current f0up_key: %d" % current_key)
    print("  Recommended f0up_key:     %d" % recommended)
    print()

    if recommended != current_key:
        print("  ACTION: Change f0up_key to %d in rvc_batch.py" % recommended)
        print("  Then re-run the RVC batch conversion.")
    else:
        print("  Pitch is already matched. No change needed.")


if __name__ == '__main__':
    main()
