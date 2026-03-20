# Voice Cloning Pipeline for SpeechWorks Speechify 3.0.5

## Overview

This document describes how to create a new voice for the SpeechWorks Speechify
3.0.5 TTS engine by "skinning" the existing Tom voice with a different voice's
timbre. The engine's unit selection, join costs, preselection cache, CART trees,
and all internal data structures remain identical to Tom's -- only the audio in
the VDB is replaced.

The pipeline uses three modern AI tools:
- **AudioSR** -- neural audio super-resolution (8kHz -> 48kHz)
- **RVC** -- retrieval-based voice conversion
- **Frida** -- optional runtime hook for cross-recording penalty

Total processing time: ~3-4 hours on an RTX 4070 Ti Super for ~6,800 recordings.

## Prerequisites

### Hardware
- NVIDIA GPU with 8+ GB VRAM (tested on RTX 4070 Ti Super, 16 GB)
- 16+ GB system RAM
- ~20 GB free disk space

### Software
- Python 3.10-3.12
- SpeechWorks Speechify 3.0.5 installed with Tom voice
- AudioSR (`pip install audiosr`)
- RVC (`pip install rvc-python`, plus rvc-no-gui for training)
- PyWorld, scipy, numpy, psutil, tqdm (see requirements.txt)
- Frida (`pip install frida-tools frida`) -- optional, for penalty hook

### Voice Reference
- 5-10 minutes of clean reference audio from the target voice
- If reference is limited, Qwen3-TTS or another zero-shot TTS can synthesize
  training data from as little as 18 seconds of reference

## Pipeline Summary

```
Tom 8kHz u-law (VDB) --> decode to PCM WAV
                           |
                           v
                     AudioSR upscale (8kHz -> 48kHz)
                           |
                           v
                     RVC voice conversion (Tom -> target voice)
                           |
                           v
                     build_voice_skin.py (swap audio in VDB, keep Tom's VIN)
                           |
                           v
                     <voice>.vin + <voice>8.vdb (ready to use)
```

## Step-by-Step Instructions

### Step 1: Extract Tom's Audio from VDB

Extract all recordings from Tom's VDB as individual WAV files:

```bash
python reveng/voice_cloning/extract_tom_wavs.py <voice>
```

This decodes the XOR-encrypted VDB, converts u-law to PCM16, and saves each
recording as an 8kHz mono WAV. Output: `en-US/<voice>/output/tom_all_for_rvc/`

Expected: ~6,849 WAV files.

### Step 2: Upscale with AudioSR

Tom's audio is 8kHz (phone quality). RVC needs higher quality input to produce
natural-sounding output. AudioSR uses a diffusion model to reconstruct the
missing high frequencies.

```bash
python reveng/voice_cloning/audiosr_batch.py <voice>
```

Key parameters:
- `model_name="speech"` -- use the speech-optimized model
- `ddim_steps=20` -- good balance of speed vs quality (50 is max quality)
- `guidance_scale=3.5` -- default works well
- Output is 48kHz WAV

Processing time: ~2-3 seconds per file, ~4-6 hours for 6,849 files.

The script trims trailing silence that AudioSR adds (threshold -40dB, 50ms pad).

Expected: ~6,600 upscaled WAVs (some very short files may fail, that's OK).

### Step 3: Train RVC Model

Select the best reference audio for RVC training. If using Qwen-synthesized
reference audio:

```bash
python reveng/voice_cloning/find_best_training_wavs.py <voice>
```

This selects the top ~100 WAVs by quality metrics (duration >2.5s, speech ratio
>50%, no clipping, stable RMS energy). Output: `en-US/<voice>/output/rvc_training/`

Train the RVC model:

```bash
python pipeline.py train -m "<voice>" -e 500 -a <training_wavs>/*.wav
```

If using rvc-no-gui, ensure the mute file exists:
```
RVC/logs/mute/0_gt_wavs/mute32k.wav  (1 second of silence at 32kHz)
```

Training takes ~90 minutes on RTX 4070 Ti Super (500 epochs, batch size 8).

Model output: `RVC/assets/weights/<voice>.pth`

### Step 4: Determine Pitch Shift (f0up_key)

The most critical RVC parameter. Tom's voice is male (~120 Hz). The target
voice's pitch determines the shift:

```bash
python reveng/voice_cloning/diag_pitch.py <voice>
```

This compares the RVC output pitch against the reference audio and recommends
an f0up_key value. Typical values:
- Male -> Female: +8 to +12 semitones
- Male -> Male (different speaker): -2 to +2 semitones
- Male -> Low female: +6 to +9 semitones

For Mara (182 Hz target): **f0up_key=8**

Test on a single file first:
```python
from rvc_python.infer import RVCInference
rvc = RVCInference(device="cuda:0")
rvc.load_model("rvc-no-gui/RVC/assets/weights/truest_mara_hq_e50_s4250.pth")
rvc.set_params(
    f0up_key=7,
    f0method="crepe",
    protect=0.2,
    index_rate=0.3,
    filter_radius=5
)
rvc.infer_file("tom_4am.wav", "truest_mara_4am.wav")
```

Listen and adjust f0up_key until the pitch matches the reference.

### Step 5: Batch RVC Conversion

```bash
python reveng/voice_cloning/rvc_batch.py <voice>
```

Key parameters (command-line or edit script defaults):
- `--model` -- path to trained RVC .pth file (default: auto-detected)
- `--workers` -- number of parallel threads (8 works on 16GB VRAM)
- `--f0up-key` -- pitch shift in semitones (from Step 4)
- `f0method="rmvpe"` -- best quality pitch detection (hardcoded default)
- `protect=0.33` -- protects voiceless consonants (hardcoded default)

Processing time: ~1-2 hours with 8 workers on RTX 4070 Ti Super.

Expected: ~6,600 converted WAVs with zero failures.

### Step 6: Build Voice Skin

```bash
python build_voice_skin.py <voice> --wav-dir tom_all_rvc
```

This script:
1. Copies Tom's VIN byte-for-byte (all unit data, hash, prsl, trees preserved)
2. Copies Tom's VDB structure
3. For each recording, replaces Tom's audio with the RVC-converted version
4. Truncates or pads each recording to match Tom's exact byte count
5. XOR-encodes both files

Output:
- `en-US/<voice>/<voice>.vin` (Tom's structure, unchanged)
- `en-US/<voice>/<voice>8.vdb` (Mara's voice in Tom's audio slots)

Processing time: ~5 seconds.

### Step 7: Configure VCF

Copy Tom's VCF and edit the voice name:
```bash
python reveng/vcf_edit.py --in en-US/<voice>/<voice>.vcf --out en-US/<voice>/<voice>.vcf
```

Key parameters (Tom's defaults work well):
- `HALFPHONE_CAND_PRUNE_THRESH = 0.8` (or 3.0 for more candidates)
- `HALFPHONE_CAND_MAX_UNITS = 50` (or 200 for more candidates)
- `JOIN_COST_WEIGHT = 0.7`
- `DUR_WEIGHT = 0.3`
- `CONTEXT_COST_WEIGHT = 1.0`

### Step 8: Test

Start Speechify server and synthesize:

```bash
bin\spfy_dumpwav32_8khz.exe "The weather today will be partly cloudy."
```

For detailed diagnostics:
```bash
python diag_ground_truth.py "The weather today will be partly cloudy."
```

### Step 9 (Optional): Frida Penalty Hook

The penalty hook adds a cost to cross-recording transitions, encouraging the
Viterbi to stay within the same recording longer. This reduces audible switches.

```bash
python reveng/voice_cloning/frida_viterbi_penalty.py "Text to speak." 50
```

The penalty value (50) is additive to the join cost for every cross-recording
transition. Higher values = fewer switches but potentially worse phone selection.

Results with Mara:
- Without penalty: 28 recording switches per 100 halfphones
- With penalty=50: 24 recording switches per 100 halfphones

The penalty hook requires Frida attached to the Speechify.exe server process.
It patches the Viterbi inner loop at 0x8E8B854 with a code cave that checks
file_idx for each candidate pair.

## Results

### Mara Voice (from NWR 2003-2016 recordings)

| Metric                    | Value       |
|---------------------------|-------------|
| Recording switches (raw)  | 28 / 100 HP |
| Recording switches (hook) | 24 / 100 HP |
| Mean run length           | 3.7         |
| Max run length            | 13          |
| Pitch (median F0)         | 180.2 Hz    |
| Target pitch              | 182.9 Hz    |
| Pitch error               | 0.3 semitones |
| RVC f0up_key              | 8           |
| AudioSR failures          | 243 / 6849  |
| RVC failures              | 0 / 6606    |
| Build time                | ~5 seconds  |

## File Inventory

### Scripts (project root)
- `build_voice_skin.py` -- voice skin builder (swap audio, keep structure)
- `build_voice_pipeline.py` -- full pipeline builder (for Qwen-based voices)
- `reveng/vcf_edit.py` -- VCF editor (nibble-cipher encrypted XML config)

### Scripts (reveng/voice_cloning/ -- pipeline and diagnostic tools)
- `extract_tom_wavs.py` -- extract Tom's VDB audio as WAVs
- `audiosr_batch.py` -- batch AudioSR upscaling
- `rvc_batch.py` -- batch RVC conversion (threaded)
- `find_best_training_wavs.py` -- select best WAVs for RVC training
- `diag_pitch.py` -- pitch comparison diagnostic
- `frida_viterbi_penalty.py` -- Frida penalty hook + ground truth diagnostic

### Voice Files (en-US/<voice>/)
- `<voice>.vin` -- voice index (RIFF, XOR 0xCE encrypted)
- `<voice>8.vdb` -- voice database (RIFF WAVE, XOR 0xCE encrypted, u-law 8kHz)
- `<voice>.vcf` -- voice config (nibble-cipher encrypted XML)
- `<voice>.wav` -- reference audio for pitch calibration

### Intermediate Files (en-US/<voice>/output/)
- `tom_all_for_rvc/` -- extracted Tom WAVs (8kHz PCM)
- `tom_all_upscaled/` -- AudioSR output (48kHz PCM)
- `tom_all_rvc/` -- RVC output (target voice)
- `rvc_training/` -- curated WAVs for RVC model training
- `rvc-no-gui/RVC/assets/weights/<voice>.pth` -- trained RVC model

## Troubleshooting

### RVC sounds robotic
- **Cause**: Input audio quality too low (8kHz) for voice conversion
- **Fix**: Run AudioSR upscaling first. Never feed raw 8kHz to RVC.

### Pitch too high/low
- **Cause**: Wrong f0up_key value
- **Fix**: Run diag_pitch.py, adjust f0up_key, re-run RVC batch

### AudioSR fails on some files
- **Cause**: Very short recordings (<100ms) don't align to AudioSR's block size
- **Fix**: Ignore. These are silence boundaries that rarely get selected.

### Engine crashes on load
- **Cause**: VDB audio length mismatch (lp+dl exceeds recording bounds)
- **Fix**: build_voice_skin.py truncates/pads to exact Tom length. If using
  build_voice_pipeline.py, ensure safety clamp is enabled.

### Tom's voice leaks through
- **Cause**: Some recordings have no converted WAV (RVC/AudioSR failed)
- **Fix**: Tom's original audio is kept for those slots. The engine may select
  units from them. To prevent this, increase the failed recording count and
  ensure AudioSR processes all files.

### "mute32k.wav not found" during RVC training
- **Cause**: RVC training requires a silence file that rvc-no-gui doesn't create
- **Fix**: Create the directory and generate a 1-second silent WAV at 32kHz:
  ```
  mkdir RVC/logs/mute/0_gt_wavs
  python -c "import numpy as np, wave; f=wave.open('mute32k.wav','wb'); \
    f.setnchannels(1); f.setsampwidth(2); f.setframerate(32000); \
    f.writeframes(np.zeros(32000,dtype=np.int16).tobytes()); f.close()"
  ```

## Architecture Notes

The voice skin approach works because SpeechWorks designed a clean separation
between the unit selection logic (VIN) and the audio data (VDB). The engine:

1. Reads the VIN to determine which units to select (phone context, costs, trees)
2. Reads the VDB at positions specified by lp/dl to get audio
3. Concatenates audio segments using WSOLA overlap-add

By keeping Tom's VIN and only replacing the VDB audio, we preserve:
- Perfect phone alignment (Tom's original lp/dl values)
- Optimal join costs (Tom's hash, computed from Tom's spectral boundaries)
- Preselection cache (Tom's prsl, curated for Tom's recordings)
- CART trees (Tom's f0 and duration prediction models)
- All other voice metadata (ckls, ccos, mean, vers, cnts, feat)

The only requirement is that the replacement audio be the same length as Tom's
original for each recording (build_voice_skin.py handles this via truncation
and padding).

This approach was discovered after extensive investigation of alternative
methods including Qwen TTS synthesis, MFA forced alignment, spectral EQ,
PRSL candidate injection, prune threshold tuning, spectral join cost code
caves, and audio trimming. The voice skin approach outperformed all of them
because it preserves the engine's entire optimization stack intact.

See README_TECHNICAL.md for detailed engine internals and DLL_ANALYSIS.md for
disassembly notes. See EXPERIMENTS.md for the full history of approaches tried.
