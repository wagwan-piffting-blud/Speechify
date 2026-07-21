# spfy

Native C reimplementation of the SpeechWorks **Speechify 3.0.5** (2003)
TTS engine. The goal — **byte-exact 1:1 output** with the original
Windows engine on a fixed audit corpus — is **achieved**, with full
parity on both Windows and Linux.

```
PATH UID:       8532/8532 (100.0%)
SLOT FIDELITY:  8684/8684 (100.0%)
PHRASES:        226/226 ran clean   (0 failed)
```

The build is a 32-bit C99 project that loads SpeechWorks's original
Tom voice data (VIN/VDB/VCF) and a small in-process host for the
front-end DLL (`SWIttsFe-en-US.dll`). Both Windows and Linux produce
byte-identical WAVs from the same input.

---

## What ships

| Surface | Status |
|---|---|
| **CLI synth** (`spfy_synth` text → WAV) | ✅ 100% audit on Tom |
| **Inline phoneme escape** (`\![.1pa.0tx]` syntax) | ✅ |
| **Windows SAPI 5 voice DLL** (32-bit + 64-bit) | ✅ |
| **SSML `<prosody>`** (rate / pitch / volume) | ✅ |
| **SSML `<phoneme>` / SAPI XML `<pron>`** | ✅ |
| **Word / sentence / bookmark events** | ✅ |
| **Linux build** (`gcc-multilib`, bit-exact to Windows) | ✅ |

Tested SAPI consumers: Balabolka, Windows Narrator.

A Windows installer (Inno Setup) is built on every `v*` tag push via
GitHub Actions and attached to the corresponding release. End users
who just want to use the voice in SAPI clients can grab
`spfy-setup-X.Y.Z.exe` from the [Releases page](../../../releases) and
skip the build step entirely; the installer handles regsvr32 for both
bitnesses and lays out the FE data under
`%USERPROFILE%\Documents\Speechify\`. Voices (proprietary VIN/VDB/VCF
assets) are NOT bundled — drop them into
`%USERPROFILE%\Documents\Speechify\en-US\<voicename>\` after install
and re-run regsvr32 to register them.

---

## Quickstart

### Windows (msys2 mingw32)

```cmd
:: First time only — install mingw32 toolchain
pacman -S mingw-w64-i686-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja

:: Configure + build
spfy\build32.bat

:: Synth a phrase
C:\tmp\spfy_build32\src\cli\spfy_synth.exe ^
  en-US\tom\tom.vin en-US\tom\tom8.vdb en-US\tom\tom.vcf ^
  spfy\data\tom_hpclass.bin spfy\build\fe_symbol_table.json ^
  spfy\data\fe_tables_a spfy\data\fe_tables ^
  "The quick brown fox jumps over the lazy dog." C:\tmp\out.wav

:: Register the SAPI voice (elevated PowerShell, both bitnesses)
Start-Process 'C:\Windows\SysWOW64\regsvr32.exe' `
  -ArgumentList '/s','C:\tmp\spfy_build32\src\sapi\spfy_sapi.dll' `
  -Verb RunAs -Wait
```

After registration, "Speechify - tom" (plus any other voices in
`%USERPROFILE%\Documents\Speechify\en-US\*`) shows up in Balabolka,
Windows Narrator, etc.

### Linux (Ubuntu / Debian)

```sh
sudo apt install -y build-essential cmake ninja-build gcc-multilib python3

./build_linux.sh

# Synth a phrase
/tmp/spfy_build_linux32/src/cli/spfy_synth \
  en-US/tom/tom.vin en-US/tom/tom8.vdb en-US/tom/tom.vcf \
  spfy/data/tom_hpclass.bin spfy/build/fe_symbol_table.json \
  spfy/data/fe_tables_a spfy/data/fe_tables \
  "The quick brown fox jumps over the lazy dog." /tmp/out.wav
```

SAPI is Windows-only and is automatically skipped on Linux (top-level
CMakeLists gates the directory on `if(WIN32)`).

### Audit

```sh
SPFY_SYNTH_EXE=/tmp/spfy_build_linux32/src/cli/spfy_synth \
  python3 spfy/test/oracle/master_compare2.py \
    --multi-phrase --tmpdir /tmp --workers 4 --quiet
```

(On Windows: `python spfy\test\oracle\master_compare2.py --multi-phrase`
after `set SPFY_SYNTH_EXE=C:\tmp\spfy_build32\src\cli\spfy_synth.exe`.)

Expected output:

```
PATH UID:       8532/8532 (100.0%)  positional + LCS
SLOT FIDELITY:  8684/8684 (100.0%)
```

---

## Architecture

```
text ─┐
      ▼
   ┌──────────────────────────────────────┐
   │ FE host (in-process PE loader)       │
   │ • Loads SWIttsFe-en-US.dll (32-bit)  │
   │ • 80 import stubs (kernel32, msvcr71)│
   │ • Fake TIB on Linux                  │
   └─────┬────────────────────────────────┘
         ▼  (tagged FE output)
   ┌──────────────────────────────────────┐
   │ Slot builder (build_graph + link)    │
   │ • Phrase/Word/Syl/Halfphone tree     │
   │ • Predecessor lists                  │
   └─────┬────────────────────────────────┘
         ▼
   ┌──────────────────────────────────────┐
   │ Unit selection (Viterbi DP)          │
   │ • PRSL pool query                    │
   │ • Per-cand TC (D/F0/SP/S/FLAG)       │
   │ • Anchor scoring (multi-unit spans)  │
   │ • Same-recording adjacency join cost │
   │ • HP histogram prune                 │
   └─────┬────────────────────────────────┘
         ▼  (chosen UID path)
   ┌──────────────────────────────────────┐
   │ WSOLA streamer                       │
   │ • Engine UID-batching                │
   │ • Hann-windowed OLA                  │
   │ • Pair detection (same-rec / cross)  │
   │ • Optional pitch / rate post-process │
   └─────┬────────────────────────────────┘
         ▼
       WAV
```

The unit-selection scoring stack (D/F0/SP/S target costs, plus join
cost, anchor cost, HP prune) is bit-for-bit engine-faithful. The
WSOLA streamer matches the engine's "Plain WSOLA" mode (the path Tom-
family voices use); the PSOLA branch in the engine is dead code for
those voices.

---

## SSML / SAPI features

| Tag / state | Implementation |
|---|---|
| `<voice>` | SAPI CLSID switch (handled by SAPI runtime) |
| `<prosody rate="..." />` | WSOLA frame-based time-stretch (post-process) |
| `<prosody pitch="..." />` | Hybrid: corpus selection bias (clamp ±~1.5 / ±~2 st on Tom) + TD-PSOLA tail for the residual |
| `<prosody volume="..." />` | Per-sample scalar gain in the sink |
| `<break time="..." />` | Zero-sample silence emit |
| `<mark name="..." />` | `SPEI_TTS_BOOKMARK` event |
| `<phoneme alphabet="x-microsoft-sapi" ph="..." />` | SAPI phone IDs → ARPAbet → SPR → inline FE escape |
| `<pron sym="..." />` (SAPI XML) | Same path as `<phoneme>` |
| Word / sentence / bookmark boundaries | `SPEI_WORD_BOUNDARY` / `SPEI_SENTENCE_BOUNDARY` / `SPEI_TTS_BOOKMARK` with byte-accurate `ullAudioStreamOffset` |
| Host rate slider (`ISpVoice::SetRate`) | Pulled in via `ISpTTSEngineSite::GetRate()` and summed with `SPVSTATE.RateAdj` |

For pitch specifically: when the requested shift fits Tom's recorded
F0 range (~−2 to +1.5 semitones around 118 Hz median), it's handled
entirely by biasing the F0 target the Viterbi matches against — no
DSP. Past that range, the unit selector hits the corpus ceiling/floor
and a TD-PSOLA pass handles the residual. The net effect is fully
natural sound for moderate shifts, gracefully degrading toward
synthetic at extremes.

---

## Layout

```
spfy/
  CMakeLists.txt
  include/spfy/
    spfy.h, spfy_voice.h           public C API
  src/
    common/    obfuscation (XOR), riff, file_io, log
    voice/     VIN/VDB/VCF loaders, unit/feat tables, ccos, voice_runtime
    cart/      CART evaluator (durt + f0tr)
    usel/      hash, PRSL, costs (S/D/SP/F0/FLAG), build_graph, link_graph,
               slot_ctx, anchor_score, viterbi DP
    wsola/     ulaw, wav writer/sink, WSOLA streamer
    dsp/       pitch_shift (TD-PSOLA), time_stretch (WSOLA frame-based)
    fe/        hand-FE stages (retired but still linked for shared utilities)
    fe_host/   in-process PE loader for SWIttsFe-en-US.dll
    host/      generic PE loader, import stubs, Linux fake-TIB
    synth/     spfy_voice_t + per-call synth library
    sapi/      Windows SAPI 5 voice DLL (32-bit + 64-bit)
    cli/       spfy_synth, spfy_dump_voice, spfy_pitch_shift, etc.
  test/
    oracle/    230-phrase corpus + master_compare2.py harness
    diff/      WAV / per-cand-total diff utilities
    unit/      C unit tests
```

### CLIs

| CLI | Purpose |
|---|---|
| `spfy_synth` | text (or inline-SPR phonemes) → WAV |
| `spfy_dump_voice` | introspect a VIN/VDB/VCF (units, ccos, prsl, ...) |
| `spfy_dump_f0` | per-voice F0 byte distribution |
| `spfy_pitch_shift` | A/B test the TD-PSOLA pitch shifter on a WAV |
| `spfy_time_stretch` | A/B test the WSOLA time-stretch on a WAV |
| `spfy_concat` | (legacy) concat oracle-chosen units to WAV without WSOLA |
| `spfy_*_replay` | offline validation tools that replay captured engine traces through C |

---

## Cost stack reference

All scoring uses long-double accumulators with a final cast to `float`
(matches MSVC 7.1 / 2003 x87 80-bit semantics).

```
D-cost  = | (1/stddev) * (unit_mem[+0x12] - durt_mean) |^2 * DUR_WEIGHT
F0-cost = MISSING_F0_COST                                            if voicing[hp_class]==0
        = w_f0_miss                                                  if stored_f0 == 0
        = | (1/stddev) * (stored_f0 - f0tr_mean)      |^2 * ABS_F0_WEIGHT
SP-cost = sum(k=0..4) weight[k] * matrix[k][target_feat[k]][cand_byte[k]]
S-cost  = ccos_weight * sum(slot=0..3)
              ccos[hp_class*4+slot][s_remap[target.ctx[s]]][s_remap[cand.ctx[s]]]
FLAG    = cand.context_cost * 0.25 * 0.01
JOIN    = 0                                              if curr.uid == prev.uid + 1 && curr.flag_b
        = hash_value                                     if hash hit
        = JOIN_COST_OFFSET + smooth_curve(curve_idx)     if voiced-join precondition met
        = miss_offset                                    otherwise
```

Engine-truth values for Tom (from VCF / runtime capture):

| Constant | Value |
|---|---|
| `DUR_WEIGHT` | `0.3` |
| `ABS_F0_WEIGHT` | `0.2` |
| `MISSING_F0_COST` (`w_f0_miss`) | `5.0` |
| `JOIN_COST_OFFSET` | `0.2` |
| `gate_weight` (smooth-curve) | `0.6` |
| `miss_offset` | `1000.0` |
| SP weights | `[0.05, 0.05, 0.05, 0.05, 0]` |

Notable: the VCF param `DUR_WEIGHT` scores `unit_mem[+0x12]` —
**that's `f0_context`, not duration** — despite the name. This was a
multi-day red herring; the engine's "duration" cost is actually scoring
the unit's contextual-pitch byte against the f0tr CART prediction. See
the `M3.4g` commit message for the full disasm walk.

---

## Linux build: how it works

The Linux build runs a 32-bit Windows PE (the FE DLL) inside a 32-bit
Linux process. The two non-obvious enablers:

1. **Fake Thread Information Block via `set_thread_area(2)`.**
   MSVC-compiled code reads `fs:[0x00]` (SEH chain head) and
   `fs:[0x18]` (TIB self pointer) in nearly every function prologue.
   Linux 32-bit processes have `FS = 0` and reading `fs:[*]` segfaults.
   We install a single 4 KB fake TIB struct via `set_thread_area(2)`
   and point FS at it. SWIttsFe is single-threaded (calls
   `DisableThreadLibraryCalls` in DllMain), so per-thread TIBs are
   unnecessary. See `src/host/tib_linux.c`.

2. **MSVCRT `_iob` ABI.** MSVCR71 exports `_iob` as an inline array of
   32-byte FILE structs, NOT as an array of pointers. The DLL's
   `stdin` / `stdout` / `stderr` macros expand to `&_iob[0..2]` —
   pointers INTO that struct array. We replicate the layout in
   `src/host/imports.c` (`msvcrt_FILE`) and back each slot's
   `_tmpfname` slot with a glibc `FILE *` that all stdio stubs
   wrap/unwrap around.

3. **`getObject` is `__cdecl`, not `__stdcall`.** The DLL's exported
   `getObject` ends with a plain `ret` (no operand), so the caller has
   to clean up the stack. Misdeclaring it as `__stdcall` worked on a
   smaller test binary (gcc emitted EBP-relative local addressing,
   which is invariant to post-call ESP) but corrupted the full synth's
   local stack frame (gcc went ESP-relative there). The fix is one
   character: `__cdecl getObject_fn` in `src/fe_host/fe_host.c`.

The Linux audit passes 8532/8532 with byte-identical WAVs to Windows
(`cmp` confirmed on the Tom pangram).

---

## Frida hook policy

After repeated server crashes during reverse-engineering, the policy
is **function-entry hooks ONLY**. Mid-instruction `Interceptor.attach`
inside `SWIttsUSelUnitSelection`'s x87 FP loops destabilises the
engine stochastically — accumulated trampoline trips perturb x87
stack state and the engine eventually access-violates. Same hook can
run cleanly on 1735 probes one day and crash at 512 the next.

If hot-path data is needed, use Frida **Stalker** (transcoded
execution) instead — slower but robust. Retired hot-path hooks are
still in `viz/frida_hooks/` with DANGER banners; do not re-add them
to `run_frida_capture.py`'s `HOOK_JS` map without a Stalker rewrite.

---

## Reverse-engineering lessons

The bugs that took the longest:

1. **`feat.filename` is keyed by `stored_id`, not position.** Sort the
   `feat` chunk's filename entries by `stored_id` at load time; then
   `entries[stored_id]` is canonical. Naive positional lookup passes
   boundary tests (first and last entries match by coincidence) and
   silently misroutes mid-corpus units to wrong recordings. This was
   the dominant cause of "Tom but garbled" output before M0b.

2. **`unit_mem[+0x12]` is `f0_context`, not `dur_like`.** The VCF
   param `DUR_WEIGHT` scores `f0_context` against the durt CART
   prediction. Replacing `cand.dur_like` with `cand.f0_context` in
   the D-cost doubled the aggregate match overnight.

3. **The four ccos cand bytes are signed (`MOVSX`); SP/D/FLAG are
   unsigned (`MOVZX`).** Disasm at `0x08e891a8` etc. proves the
   distinction. Tom's silence sentinel `0xff` is `-1` in the ccos
   path — interpreted as unsigned, our score was +18.85 off; as
   signed, bit-exact.

4. **Engine `_iob` is FILE-structs-inline, not FILE-pointers-array.**
   Our short pointer array ran off the end into BSS on Linux, and
   `getObject(2)` returned random pointers (sometimes glibc stderr's
   address itself, recognisable by `_IO_magic = 0xfbad2887`).

5. **`getObject` is `__cdecl`, not `__stdcall`.** The Win32
   convention is `__stdcall` for exports, but compilers occasionally
   produce `__cdecl` exports anyway. Single `ret` vs `ret N` byte
   tells the truth — verify against disassembly, don't trust the
   convention.

Memory notes in `~/.claude/projects/.../memory/MEMORY.md` cover the
full RE history; each major finding has a side memory.

---

## Env knobs

```
# Synth diagnostics
SPFY_TRACE_UNITS=1            per-push unit info to stderr
SPFY_WSOLA_VERBOSE=1          per-push lag + NCC diagnostic
SPFY_INTERWORD_MS=N           inter-word silence (default 0)
SPFY_DEBUG_MISMATCH=1         per-slot chosen-vs-best diff dump
SPFY_WORD_EVENTS_FILE=<path>  word-event sidecar TSV (used by 64-bit SAPI shim)
SPFY_PITCH_SEMITONES=N        pitch shift via unit selection (CLI)

# Host / loader diagnostics (rare)
SPFY_HOST_TRACE=1             PE loader phase markers + TIB install info

# SAPI diagnostics
SPFY_SAPI_DEBUG=1             SAPI DLL log to C:/tmp/_sapi_dbg.log
                              (must be set in the consumer process env)
SPFY_SAPI_PHONE_DEBUG=1       append raw pPhoneIds to C:/tmp/_sapi_phone_log.txt

# Engine-faithfulness reverts (kept for regression diagnosis)
SPFY_NO_RUN_BATCH=1           revert engine UID-batching to pair-only
SPFY_NO_HP_PRUNE=1            disable HP histogram prune
SPFY_PSA_SYL_FROM_RESYL=1     revert to local syllabifier (vs tree walk)
SPFY_NO_SYL_INITIAL_VOWEL=1   revert syllabifier initial-vowel fix
SPFY_NO_ANCHOR_HEAD_C6C=1     revert c6c head-vs-tail anchor fix
SPFY_HP_BIN_LROUND=1          revert HP_PRUNE truncation-binning fix
SPFY_NO_HP_EARLY_EXIT=1       disable FUN_08e88de0 running-min early exit
SPFY_NO_HP_PRUNE_BIN39_GUARD=1  always filter, ignoring the break-bin<39 gate
SPFY_NO_HP_SORT_UID_TIE=1     sort survivors by cost only (no uid tie-break)
SPFY_D_IDX_TARGET=1           revert D-span indexing to target_idx
SPFY_FE_HOST_NO_LEXICAL_OVERRIDE=1   disable no-refine lexicon
SPFY_ANCHOR_VOICING_GATE=1    re-enable voicing gate in anchor init
```

---

## See also

- [`docs/`](docs/) — deeper architecture notes
- [`test/oracle/README.md`](test/oracle/README.md) — oracle harness usage
- [`test/oracle/TRACE_SCHEMA.md`](test/oracle/TRACE_SCHEMA.md) — JSONL trace format
- [`../reveng/README_TECHNICAL.md`](../reveng/README_TECHNICAL.md) — master format spec for VIN/VDB/VCF
- [`../reveng/DLL_ANALYSIS.md`](../reveng/DLL_ANALYSIS.md) — engine pipeline + function maps
- [`../RESUME.md`](../RESUME.md) — running session log (each milestone closed)
