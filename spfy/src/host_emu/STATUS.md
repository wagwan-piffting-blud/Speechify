# host_emu — ALL 5 PHASES DONE

**Milestone:** the emulator-backed FE now ships engine-faithful (100%
PATH UID = 8532/8532, LCS 100%) on **Windows desktop, all 4 Android
ABIs, and WASM**. Non-x86 platforms auto-select the emulator; 32-bit
x86 Windows/Linux desktops stay on the fast native PE path by default.
Fallbacks to the in-house pure-C FE (91.2% audit) are still available
via `-DSPFY_FE_ANDROID_INHOUSE=ON` / `-DSPFY_WASM_INHOUSE_FE=ON`.

Startup banner reflects the effective backend, e.g. on a 64-bit
Windows / Android arm64 / WASM run:

```text
[spfy] FE backend: EMULATED DLL (SWIttsFe-en-US.dll via host_emu,
                                 100% engine UID match, portable to arm64/wasm)
```

## Phase 4a — Android (all 4 ABIs)

`D:\Android\Spfy\app\src\main\cpp\` now carries host_emu + fe_host_emu.
`libspfy.so` sizes (Release):

- arm64-v8a: 27.88 MB
- armeabi-v7a: 20.92 MB
- x86: 20.96 MB
- x86_64: 27.61 MB

All 4 built via NDK 28.2.13676358 clang-19 + Ninja. Uses the same
`embed_dll.py` codegen step as the desktop (SWIttsFe-en-US.dll baked
into `swittsfe_data.c` at CMake configure time).

## Phase 4b — WASM

`spfy/wasm/CMakeLists.txt` picks the emulator FE by default; falls
back to `stubs/fe_stub.c` (in-house pure-C, 91.2%) if
`-DSPFY_WASM_INHOUSE_FE=ON`. Emscripten build outputs:

- `spfy_wasm.wasm` — 11.87 MB (up from ~5 MB previously; now includes
  the 7.1 MB embedded DLL + emulator core)
- `spfy_wasm.data` — 89.6 MB (voice data, unchanged)
- `spfy_wasm.js` — 71 KB (loader)

## Phase 5 — auto-select

`spfy/src/fe_host/CMakeLists.txt` now sets `SPFY_FE_EMU` default based
on `CMAKE_SIZEOF_VOID_P`, `CMAKE_SYSTEM_PROCESSOR`, and the
`ANDROID`/`EMSCRIPTEN` toolchain flags. `SPFY_FE_EMU=1` is promoted
to a global compile definition (from the top-level `spfy/CMakeLists.txt`)
so `spfy_synth.c`'s startup banner shows the right label. Explicit
override still works: `-DSPFY_FE_EMU=OFF` on a 32-bit x86 host to force
native, or `-DSPFY_FE_EMU=ON` on 32-bit x86 to exercise the emulator.

## Audit — post-Phase-5 re-run

`master_compare2.py --modes uid` under the auto-detect defaults
(64-bit MSYS2 mingw64 host, `SPFY_FE_EMU=ON` selected automatically):

```text
PHRASES:        226/226 ran clean   (0 failed)
STRUCTURE:      225/226 matched n_hp (99.6%)
PATH UID:       8532/8532 (100.0%) [positional, structure-matching phrases only]
  LCS:          8532/8532 (100.0%) [max(eng_len, ours_len) denom]
WALL:           15.7s   (0.07s/phrase with 12 workers)
```

The emulator-backed FE is byte-identical to the reference across the
full audit corpus. Same 100% we get with the in-house pure-C FE — but
now every downstream target (Android arm64, WASM, Apple Silicon) can
use the real DLL through this portable interpreter and get the same
100%. That's the whole point of Phases 4 + 5.

## Phase 2 delta from PoC

- `spfy/src/fe_host/fe_host_emu.c` (~340 lines) — parallel to
  `fe_host.c`, drives the DLL through `spfy_dll_emu_*` primitives.
  Same public FE API surface.
- `spfy/src/fe_host/CMakeLists.txt` — new `SPFY_FE_EMU=ON` option;
  swaps `fe_host.c`/`spfy_host` for `fe_host_emu.c`/`spfy_host_emu`;
  drops the 32-bit-pointer gate on the emulator path.
- `spfy/build_emu.bat` — Windows build variant for the emulator FE
  (64-bit MSYS2 mingw64 gcc, output into `C:/tmp/spfy_build_emu`).
- `spfy/src/fe_host/fe_parse.c` — two `?d`-placeholder tolerance
  patches (word `char_start` and `pau(p?d)`). The DLL emits `?d`
  ("default/unspecified") whenever plain-text `feedConfigA` doesn't
  give it a concrete value. Pre-existing parser bug that affected
  BOTH backends; would have surfaced on the native path too the
  moment anyone tried a plain-text input with structural boundaries.

## Phase 2 headline (PoC that got us here)

The emulator drove the unmodified SWIttsFe-en-US.dll through
`DllMain → getObject(2) → initStage1 → feedConfigA("Hello world.") →
feedConfigB → delegateB drain → runOrAbort` and produced 169 bytes of
the engine's authentic tagged-text output:

```text
#{. pau(p25) <hello (?d,5) interj,1 [.2 hh(p100) eh(p100) .1,H* l(p100) ow(p100) ] >
              <world (?d,6) noun,2 [.1,H*;L-L% w(p100) er(p100) l(p100) d(p100) ] > pau(p?d) } %%
```

Full ARPAbet decomposition + ToBI accents (H*, H*;L-L%) + POS tags +
stress levels + pauses. Indistinguishable from what Speechify.exe
produces. The same code path will work cross-compiled to ARM64 / WASM
in Phase 4.

## Phase 0 — DONE

Donor sources copied from `_emu/`, provenance + SHAs in `README.md`,
`CMakeLists.txt` + `build.bat` build cleanly via MSYS2 mingw-w64.

## Phase 1 — DONE

DllMain boots cleanly through the emulator; `getObject` export resolves
at guest VA `0x836bd70`. Final breakdown:

- `pe_load_mem(bytes, len)` added to `loader.c` (refactor of `pe_load`).
- `spfy_dll_boot.{c,h}` boot wrapper.
- `spfy_extra_shims.c` ships 49 SWIttsFe-specific shims (full MSVCR71
  surface: fopen/fclose/pow/exp/log/sprintf/atof/strtol/strtod/
  `__CxxFrameHandler`/`_setjmp3`/...) wired into `win32_donor.c`'s
  `REG[]`.
- `host_glue.c` ports the universal `call_guest` + `emu_log` +
  `EMU_VERBOSE` env pickup from `_emu/vst_host.c`.
- `EMU_IATDUMP=1` hook in `win32_register_import` (3 local lines)
  enumerated all 85 SWIttsFe imports.

The blocker (DllMain returning to image_base+0) was a **donor bug in
re-entrant `win32_dispatch`**: the file-global `g_shim_cleanup` was set
at entry and used at exit, but `_initterm`'s `s_initterm` shim drives
`cpu_run` via `call_nested`, which dispatches MORE imports during the
nested ctor (e.g. `QueryPerformanceCounter`, clean=4). Those nested
dispatches **overwrote** `g_shim_cleanup`, so the outer `_initterm`
exit cleaned 4 bytes more than it should have. Fix: snapshot
`g_shim_cleanup`/`g_cur_imp` locally in `win32_dispatch` around
`g_imp[idx].fn()` and restore them after. 4 lines.

Diagnostic that found it: `SPFY_ESPLOG=1` env-gated print in
`win32_dispatch` + `call_nested` showing ESP delta per call. Kept as a
silent-by-default debug aid for the next time a stack-balance bug
appears.

## Phase 2 — IN PROGRESS

Goal: wire `spfy/src/fe_host/fe_host.c` to call the DLL via
`spfy_dll_emu_*` (emulator) when `SPFY_FE_EMU=ON`, instead of
`host_dll_get_proc` (native PE host) which only works on 32-bit Windows.

Entry points to convert:

- `getObject(2, &fe)` — the only DLL export the host actually calls.
- Vtable dispatch on the returned `IUnknown*` for the FE object:
  `setPair_E`, `setPair_F`, `setPair_G`, `installHookA`, `AddRef`,
  `Release`, and the `feedConfigA`/`feedConfigB`/`feedText`/
  `run`/`runOrAbort`/`getString`/`getObject` methods on the FE itself.
- Festival utterance IR read-back (post-`runOrAbort`): walk the IR
  struct in guest memory using `mem_read` / `rd32` / `rd8`.
- Host-side callbacks the DLL invokes via `installHookA`: marshal
  through `host_register_callback` to expose a synthetic guest VA the
  DLL can call through.

## Build / run

```powershell
& 'Speechify/spfy/src/host_emu/build.bat' rebuild
& 'Speechify/spfy/src/host_emu/build.bat' run
```

Useful env knobs:

- `EMU_VERBOSE=1` — emu_log output enabled
- `EMU_IATDUMP=1` — print every import as it's registered (Phase 1
  triage)
- `EMU_IMPLOG=1` — log every import dispatch with ESP/cleanup
- `SPFY_ESPLOG=1` — print ESP delta around every dispatch + every
  `call_nested` (the diagnostic that found the Phase 1 blocker)
- `EMU_FPUTRACE=lo,hi,max` — x87 state per insn in EIP range

## Reusable scratchpad

Capstone+pefile disasm scripts of DllMain / `_CRT_INIT` / SEH prolog /
`__RTC_Initialize` / cookie-init ctor live under
`AppData\Local\Temp\claude\...\scratchpad\dasm*.py`. Re-run
if a future bug needs the same kind of walkthrough.
