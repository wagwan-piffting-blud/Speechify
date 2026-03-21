# AI Mara ("aimara") - Voice Clone for SpeechWorks Speechify 3.0.5

A female voice clone for the SpeechWorks Speechify 3.0.5 TTS engine, built by
replacing Tom's original audio with voice-converted recordings while preserving
his entire unit selection infrastructure.

**Voice:** AI Mara (based on the real NWR CRS voice "Mara", used 2003-2016)
**Pitch:** ~176 Hz median (matched to real Mara's 175.7 Hz)
**Recording switches:** 28 raw / 24 with Frida penalty hook

## How It Works

This is a "voice skin" approach. Tom's VIN file contains all the linguistic
intelligence -- unit table, hash join costs, PRSL preselection cache, CART
trees, checklist indexes. We keep ALL of that intact. We only replace the
audio bytes in the VDB with voice-converted versions of Tom's own recordings.

The engine doesn't know the difference. It selects units, computes join costs,
and reads audio at the same byte offsets as always. You just hear a different voice. The engine has no concept of "Tom" or "Mara" -- it's just playing audio from the VDB based on the VIN's unit table and the VCF's weights.

## Pipeline Overview

```
Tom's VDB (8kHz u-law)
  |
  v
1. Extract recordings     -> 6,849 individual WAVs (8kHz PCM)
  |
  v
2. Upscale (FlashSR/AudioSR) -> 48kHz WAVs (~6,600, short files may fail)
  |
  v
3. Train RVC model        -> <voice>.pth (from reference recordings)
  |
  v
4. Batch RVC conversion   -> 6,600+ voice-converted WAVs
  |                          (failed upscales: direct RVC on 8kHz as fallback)
  v
5. Build voice skin       -> aimara.vin + aimara8.vdb
  |
  v
6. Patch f0tr trees       -> Expand pitch range for expressiveness
  |
  v
7. Smooth VDB boundaries  -> Reduce spectral discontinuities at unit joins
  |
  v
8. (Optional) Patch VCF   -> Tune prune threshold, max units
  |
  v
9. Test!
```

## Prerequisites

### Python
- Python 3.10+ (3.12 recommended)
- No conda required for the build pipeline itself

### Pip packages (build pipeline, required if building yourself)
```bash
pip install numpy scipy pyworld tqdm psutil
```

### Pip packages (Frida diagnostics, optional)
```bash
pip install frida frida-tools
```

### External tools

**Applio** (RVC voice conversion with GUI):
- https://applio.org/
- Used for: training RVC models and batch voice conversion
- Alternative: rvc-python (pip install rvc-python) for CLI-only workflows
- Key params: f0up_key=7-8, f0method=rmvpe, protect=0.172, index_rate=1

**FlashSR** (faster alternative to AudioSR, 22x speed):
- https://github.com/jakeoneijk/FlashSR_Inference
- One-step diffusion distillation
- Use GPU version (student_ldm + sr_vocoder + vae), NOT ONNX (CPU-only, low quality)
- Set lowpass_input=False for 8kHz source
- Requires 16kHz min input: resample 8kHz -> 16kHz with librosa or ffmpeg first
- Normalize output to -1dB headroom (clips at peak=1.0)

**Speechify engine** (for testing):
- `bin/Speechify.exe` (server) + `bin/spfy_dumpwav32_8khz.exe` (client)
- Must be running for synthesis tests

## Step-by-Step Guide

Most of these steps have already been completed by me personally, but if you want to make a totally custom voice, they're all here for you to follow. The key insight is that you don't need to start from scratch with Qwen3 synthesis or anything of the sort -- you can leverage the existing VIN structure and just replace the audio in the VDB, which is what this pipeline does.

### Step 1: Extract Tom's audio from VDB

```
cd <project_root>
python reveng/voice_cloning/extract_tom_wavs.py mara
```

Output: `en-US/mara/output/tom_all_for_rvc/` (~6,849 WAV files)

### Step 2: Upscale to 48kHz

Using FlashSR:
```
python <flashsr_dir>/inference.py --input en-US/mara/output/tom_all_for_rvc --output en-US/mara/output/tom_all_upscaled
```

Output: `en-US/mara/output/tom_all_upscaled/` (~6,600 WAVs at 48kHz)

Processing time: ~2-3 sec/file with AudioSR (ddim=20), much faster with FlashSR

### Step 3: Prepare RVC training data

Option A -- Clone from existing reference recordings (recommended):
- Collect 3-10 minutes of clean reference audio of the target voice
- For Mara: used 4 minutes of direct source recordings from the NWR CRS website (archived at [https://web.archive.org/web/*/http://weather.gov/ops2/](https://web.archive.org/web/*/http://weather.gov/ops2/))
- Key: source purity matters MORE than quantity (4 min clean is infinitely better than 30 min mixed quality)

Option B -- Clone from TTS output:
- Use find_best_training_wavs.py to select the best synthetic WAVs
- Good when no real reference exists

### Step 4: Train RVC model

Using Applio (recommended):
1. Open Applio web UI
2. Upload training audio
3. Settings: ContentVec embedder, 200-500 epochs, batch size 8
4. Train and export .pth file

Using CLI (rvc-no-gui):
```
python pipeline.py train -m "mara" -e 500 -a path/to/training/*.wav
```

Key insight: ContentVec embedder >> Spin-v2 for this task.

### Step 5: Calibrate pitch (f0up_key)

```
python reveng/voice_cloning/diag_pitch.py mara --ref en-US/mara/mara.wav --test <single_rvc_output.wav>
```

- Male to female: typically f0up_key = 7-12
- Mara: f0up_key=7 (with Applio) or f0up_key=8 (with rvc-python)
- Target: within 0.5 semitones of reference pitch

### Step 6: Batch convert all recordings

```
python reveng/voice_cloning/rvc_batch.py mara --model path/to/mara.pth --f0up-key 7 --workers 8
```

Output: `en-US/mara/output/tom_all_rvc/` (~6,600 converted WAVs)

For the ~243 files that failed AudioSR (too short), run RVC directly on the
raw 8kHz Tom WAVs as a fallback. Slightly lower quality but eliminates Tom
voice leaks.

Processing time: ~1-2 hours with 8 GPU workers

### Step 7: Build voice skin

```
python build_voice_skin.py mara --wav-dir en-US/mara/output/tom_all_rvc
```

This copies Tom's VIN byte-for-byte and rebuilds the VDB with Mara's audio.
Each recording is truncated or padded to match Tom's exact byte count.

Output: `en-US/mara/mara.vin` + `en-US/mara/mara8.vdb`

Build time: ~4 seconds

### Step 8: Patch f0tr trees (pitch expressiveness)

```
python patch_f0tr.py --voice mara --scale 1.0 --expand 6.0
```

- scale=1.0: keep median at Tom's natural pitch (RVC already shifted it)
- expand=6.0: widen the pitch variation range for broadcast cadence
- WARNING: scale != 1.0 causes DOUBLE CORRECTION (RVC + WSOLA both shift pitch)

### Step 9: Smooth VDB boundaries

```
python wsola_boundary_smooth.py --voice mara --strength 0.5
```

Applies Gaussian smoothing at unit boundary positions. Reduces spectral
discontinuities at join points. strength=0.5 and 0.7 produce identical
results through the engine (8kHz u-law is the bottleneck).

### Step 10: (Optional) Patch VCF weights

```
python reveng/vcf_edit.py --in en-US/mara/mara.vcf --out en-US/mara/mara.vcf
```

Recommended settings for voice skin builds:
- HALFPHONE_CAND_PRUNE_THRESH = 3.0 (default 0.8 is too aggressive)
- HALFPHONE_CAND_MAX_UNITS = 200 (default 50)

### Step 11: Test

Start Speechify server, then:

```
bin\spfy_dumpwav32_8khz.exe "The weather today will be partly cloudy."
```

For detailed diagnostics:
```
python diag_ground_truth.py "The weather today will be partly cloudy."
```

For penalty hook (reduces recording switches from 28 to 24):
```
python reveng/voice_cloning/frida_viterbi_penalty.py "The weather today will be partly cloudy." 50
```

## Voice Registration

To use a new voice name, edit `config/SWIttsConfig.xml`:

```xml
<tts.voice.name>aimara</tts.voice.name>
```

The engine looks for files at:
- `en-US/<voice>/<voice>.vin`
- `en-US/<voice>/<voice>8.vdb`
- `en-US/<voice>/<voice>.vcf`

## Applio files
> `aimara.pth` -- Applio HiFi-GAN model checkpoint (epoch 150, version 4, sounds most accurate to True Mara)
> `aimara.index` -- FAISS index for RVC retrieval (built from training data)

Key findings:
- Source recording purity matters more than quantity
- ContentVec embedder is far superior to Spin-v2 for this task
- Pure-source model (4 min clean) converges faster than mixed-quality (30 min), convergence score ~19 vs ~30, sounds more accurate to the real Mara voice

## Current Best Configuration

| Component | Setting |
|-----------|---------|
| Upscaler | FlashSR GPU (lowpass=False, -1dB norm) or AudioSR (ddim=20) |
| RVC Model | Pure-source ContentVec e100 or original ContentVec e500 |
| Applio params | pitch=7, index_rate=0.5, protect=0.1, f0method=rmvpe |
| VDB | WSOLA boundary smoothing (strength=0.5) |
| VIN | f0tr patched (scale=1.0, expand=6.0) |
| VCF | HALFPHONE_CAND_PRUNE_THRESH=3.0, MAX_UNITS=200 |
| Runtime | Frida penalty hook p=50 at 0x8E8B854 (optional) |

## Troubleshooting

**Engine crashes on startup:**
- Check diags.log for "database is corrupt" -- lp+dl exceeds recording bounds
- Rebuild with build_voice_skin.py (has safety clamping built in)

**Tom's voice leaks through on some words:**
- Some recordings failed AudioSR (too short)
- Run RVC directly on those 8kHz WAVs as fallback
- Check: all 6,849 recordings should have Mara WAVs

**Voice sounds robotic/metallic:**
- Reduce protect parameter in Applio/RVC (try 0.1)
- Check f0up_key calibration with diag_pitch.py
- Real Mara has warm spectral tilt (-63 dB/decade) -- RVC outputs are brighter

**Voice sounds "drunk" or pitch is wrong:**
- f0tr scale != 1.0 causes double correction (RVC + WSOLA both shift pitch)
- Use scale=1.0 with expand for range, NOT scale for pitch shifting

**Synthesis sounds choppy/stuttery:**
- Set VCF: HALFPHONE_CAND_PRUNE_THRESH=3.0, MAX_UNITS=200
- Use Frida penalty hook (p=50) for fewer recording switches
- Check that all recordings are present (no coverage gaps)

## Files in This Directory

- `aimara.vin` -- Voice index (Tom's structure + Mara metadata)
- `orig_aimara.vin` -- Backup of original VIN before F0 patching
- `aimara8.vdb` -- Voice database (Mara's audio, XOR-encrypted RIFF WAVE)
- `aimara.vcf` -- Voice config (nibble-cipher encrypted XML)
- `aimara8.xml` -- Alternate voice config for testing
- `aimara.pth` -- RVC model checkpoint (ContentVec, epoch 150, version 4)
- `aimara.index` -- FAISS index for RVC retrieval (built from training data)
- `OLD/` -- Previous iteration of the voice files (for reference)

## Links

- Applio: https://applio.org/
- FlashSR: https://github.com/jakeoneijk/FlashSR_Inference
- RVC Project: https://github.com/RVC-Project/Retrieval-based-Voice-Conversion-WebUI
