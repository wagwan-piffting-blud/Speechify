# Tom Speaks - Phase A audible synthesis demo

First audible output from the new native-C engine, 2026-05-06.

## Files

| File | Source text | Duration | Units |
|------|-------------|----------|-------|
| `text_002_tom_speaks.wav` | "The quick brown fox jumps over the lazy dog." | 2.23 s | 64 |
| `text_001.wav` | text_001 from corpus | 1.39 s | 20 |
| `text_004.wav` | text_004 from corpus | 0.60 s | 4 |
| `text_010.wav` | text_010 from corpus | 1.86 s | 58 |
| `text_016.wav` | text_016 from corpus | 2.62 s | 74 |
| `text_029.wav` | text_029 from corpus | 3.46 s | 86 |

All 8 kHz mono s16le WAVs. Choppy at unit boundaries (no WSOLA yet -- M4
will fix this).

## How they were produced

```powershell
& "spfy/build/win/src/cli/spfy_concat.exe" `
  "en-US/tom/tom.vin" `
  "en-US/tom/tom8.vdb" `
  "spfy/test/oracle/traces/wsola_buffer/text_002.jsonl" `
  "out.wav"
```

Pipeline (all in native C):
1. `spfy_voice_open` -- VIN+VDB voice loaders.
2. Read captured wsola_buffer trace (engine's chosen UID sequence).
3. Per UID: lookup unit table → file_idx → recording name → VDB byte range.
4. Decode u-law to s16le, concatenate, write WAV.

The captured trace bypasses FE+Viterbi+WSOLA; this demonstrates that
the unit-lookup + audio-decode + WAV-emit chain is bit-correct in C.

For text-only synthesis (no captured trace), see Phase B4.4 / Phase C.
