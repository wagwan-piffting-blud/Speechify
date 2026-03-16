from sys import argv
import wave, struct, numpy as np
import matplotlib
from pathlib import Path
matplotlib.use('Agg')
import matplotlib.pyplot as plt

WAV = Path(argv[1]).expanduser().resolve() if len(argv) > 1 else Path('c:/tmp/test.wav').resolve()

wav_filename_only = WAV.stem
print(f"Analyzing WAV file: {WAV}  (name only: {wav_filename_only})")

with wave.open(str(WAV)) as w:
    rate = w.getframerate()
    nch  = w.getnchannels()
    sw   = w.getsampwidth()
    nf   = w.getnframes()
    raw  = w.readframes(nf)
    print(f"Rate={rate} Hz  ch={nch}  width={sw}  frames={nf}  duration={nf/rate:.3f}s")

samples = np.frombuffer(raw, dtype='<i2').astype(np.float32) / 32768.0
t = np.arange(len(samples)) / rate

fig, axes = plt.subplots(3, 1, figsize=(14, 10))

# 1. Waveform
axes[0].plot(t, samples, linewidth=0.3)
axes[0].set_title('Waveform')
axes[0].set_xlabel('Time (s)')
axes[0].set_ylabel('Amplitude')
axes[0].set_ylim(-1.1, 1.1)

# 2. Spectrogram
axes[1].specgram(samples, Fs=rate, NFFT=256, noverlap=128, cmap='inferno')
axes[1].set_title('Spectrogram (NFFT=256, 8kHz audio)')
axes[1].set_xlabel('Time (s)')
axes[1].set_ylabel('Frequency (Hz)')

# 3. Short-time RMS energy (every 20ms frame = 160 samples)
frame = 160
n_frames = len(samples) // frame
rms = np.array([np.sqrt(np.mean(samples[i*frame:(i+1)*frame]**2))
                for i in range(n_frames)])
rms_t = np.arange(n_frames) * frame / rate
axes[2].plot(rms_t, rms)
axes[2].set_title('Short-time RMS energy (20ms frames)')
axes[2].set_xlabel('Time (s)')
axes[2].set_ylabel('RMS')

plt.tight_layout()
OUT = Path.cwd() / f'wav_analysis_{wav_filename_only}.png'
plt.savefig(OUT, dpi=120)
print(f"Saved: {OUT}")

# Stats
print(f"\nAmplitude stats:")
print(f"  max abs: {np.max(np.abs(samples)):.4f}")
print(f"  mean abs: {np.mean(np.abs(samples)):.4f}")
print(f"  std: {np.std(samples):.4f}")
print(f"  fraction clipped (>0.99): {np.mean(np.abs(samples) > 0.99):.4f}")

# Dominant frequency content
from scipy.fft import rfft, rfftfreq
fft_mag = np.abs(rfft(samples))
freqs = rfftfreq(len(samples), 1/rate)
# Top 5 frequency bins by energy
top_idx = np.argsort(fft_mag)[-10:]
print(f"\nTop 10 frequency bins by energy:")
for idx in sorted(top_idx, key=lambda i: -fft_mag[i]):
    print(f"  {freqs[idx]:.1f} Hz  (mag={fft_mag[idx]:.1f})")
