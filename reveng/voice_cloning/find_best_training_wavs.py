"""
Find the best reference recordings for RVC training.

Scores WAVs by: duration, RMS consistency, no clipping, signal-to-silence
ratio. Copies the top N files to a training directory.

Usage:
    python find_best_training_wavs.py [voice_name] [--wav-dir <dir>]
    python find_best_training_wavs.py mara
    python find_best_training_wavs.py craig --wav-dir resynth_rvc

If --wav-dir is not given, defaults to en-US/<voice>/output/resynth.
"""
import os
import sys
import wave
import math
import shutil

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


MIN_DURATION = 2.5  # seconds
TARGET_MINUTES = 8.0
MAX_FILES = 200


def main():
    voice_name = sys.argv[1] if len(sys.argv) > 1 else "mara"

    # Parse --wav-dir option
    wav_dir_name = "resynth"
    for i, arg in enumerate(sys.argv):
        if arg == '--wav-dir' and i + 1 < len(sys.argv):
            wav_dir_name = sys.argv[i + 1]

    PROJ = _detect_proj_root()
    WAV_DIR = os.path.join(PROJ, "en-US", voice_name, "output", wav_dir_name)
    OUT_DIR = os.path.join(PROJ, "en-US", voice_name, "output", "rvc_training")

    print("Scanning WAVs in %s ..." % WAV_DIR)

    results = []
    all_files = [f for f in os.listdir(WAV_DIR) if f.endswith('.wav')]
    for fname in all_files:
        fpath = os.path.join(WAV_DIR, fname)
        try:
            with wave.open(fpath, 'rb') as wf:
                sr = wf.getframerate()
                nframes = wf.getnframes()
                nch = wf.getnchannels()
                sw = wf.getsampwidth()
                duration = nframes / sr
                if duration < MIN_DURATION:
                    continue
                raw = wf.readframes(nframes)
        except Exception:
            continue

        # Convert to float samples
        if sw == 2:
            samples = np.frombuffer(raw, dtype='<i2').astype(np.float32)
        elif sw == 4:
            samples = np.frombuffer(raw, dtype='<i4').astype(np.float32)
        else:
            continue

        if nch > 1:
            samples = samples[::nch]  # take first channel

        if len(samples) < 100:
            continue

        # Normalize to -1..1
        peak = max(abs(samples.max()), abs(samples.min()), 1.0)
        norm = samples / peak

        # --- Scoring metrics ---

        # 1. RMS energy (prefer moderate, not too quiet)
        rms = math.sqrt(np.mean(norm ** 2))
        if rms < 0.01:
            continue  # basically silence

        # 2. RMS consistency: split into 200ms frames, compute RMS per frame
        frame_size = int(sr * 0.2)
        n_frames_energy = len(norm) // frame_size
        if n_frames_energy < 3:
            continue
        frame_rms = []
        for i in range(n_frames_energy):
            chunk = norm[i * frame_size:(i + 1) * frame_size]
            fr = math.sqrt(np.mean(chunk ** 2))
            frame_rms.append(fr)
        frame_rms = np.array(frame_rms)

        # Skip if too much silence (frames with rms < 0.02)
        speech_frames = (frame_rms > 0.02).sum()
        speech_ratio = speech_frames / len(frame_rms)
        if speech_ratio < 0.5:
            continue  # more than half silence

        # RMS consistency score (lower std relative to mean = better)
        speech_rms = frame_rms[frame_rms > 0.02]
        if len(speech_rms) < 2:
            continue
        rms_cv = float(np.std(speech_rms) / (np.mean(speech_rms) + 1e-8))

        # 3. Clipping check (samples near +/- 1.0)
        clip_ratio = float((np.abs(norm) > 0.98).sum() / len(norm))
        if clip_ratio > 0.01:
            continue  # clipped

        # 4. Duration score (prefer longer, up to 10s)
        dur_score = min(duration, 10.0) / 10.0

        # Combined score: higher = better
        score = dur_score * speech_ratio * (1.0 / (1.0 + rms_cv))

        results.append({
            'name': fname,
            'path': fpath,
            'duration': duration,
            'rms': rms,
            'rms_cv': rms_cv,
            'speech_ratio': speech_ratio,
            'clip_ratio': clip_ratio,
            'score': score,
            'sr': sr,
        })

    print("  %d WAVs scanned, %d passed filters (>%.1fs, speech>50%%, no clipping)"
          % (len(all_files), len(results), MIN_DURATION))

    # Sort by score descending
    results.sort(key=lambda x: x['score'], reverse=True)

    # Select top N until we hit target duration
    selected = []
    total_dur = 0.0
    for r in results:
        if total_dur >= TARGET_MINUTES * 60:
            break
        if len(selected) >= MAX_FILES:
            break
        selected.append(r)
        total_dur += r['duration']

    print("\n=== TOP %d RECORDINGS (%.1f minutes total) ===" % (len(selected), total_dur / 60))
    print("%3s %6s %5s %5s %5s %4s %s" % ('#', 'Score', 'Dur', 'RMS', 'CV', 'Sp%', 'Name'))
    print("-" * 65)
    for i, r in enumerate(selected[:50]):  # show top 50
        print("%3d %6.3f %5.1f %5.3f %5.2f %3.0f%% %s" % (
            i + 1, r['score'], r['duration'], r['rms'],
            r['rms_cv'], r['speech_ratio'] * 100, r['name']))
    if len(selected) > 50:
        print("  ... (%d more)" % (len(selected) - 50))

    # Copy selected to training directory
    os.makedirs(OUT_DIR, exist_ok=True)
    for r in selected:
        shutil.copy2(r['path'], os.path.join(OUT_DIR, r['name']))

    print("\nCopied %d files to %s (%.1f minutes)" % (len(selected), OUT_DIR, total_dur / 60))
    print("Use this directory as RVC training input.")


if __name__ == '__main__':
    main()
