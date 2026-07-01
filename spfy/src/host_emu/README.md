# spfy host_emu — portable x86 PE32 emulator for SWIttsFe-en-US.dll

A 32-bit x86 interpreter + PE32 loader + Win32/CRT shim layer that runs
the unmodified `SWIttsFe-en-US.dll` natively on **any** platform Speechify
ships to (Windows x64, Linux ARM64, Android arm64-v8a/x86_64,
emscripten/WASM, Apple Silicon).

The point: our in-house pure-C FE reaches 91.2 % engine-UID match. The
real DLL run through this emulator should hit **100 %** because it _is_
the engine. The 4 critical x87 fixes that make math-heavy MSVC code
correct (FSCALE, DC reg-form FSUB/FSUBR/FDIV/FDIVR, FXAM, FIST rounding)
are already in `cpu.c` below.

See `Speechify/spfy/RESUME.md` for the full
multi-phase plan that this directory implements.

## File provenance

The CPU core, memory model, PE loader, and Win32 import shims are
direct copies of the donor codebase that already powers two adjacent
audio engines:

- **`_emu`** at `D:/Programs/Audacity242/Plug-Ins/_emu/` — 32-bit
  x86 VST/LADSPA host that runs ~326 of 340 plugin DLLs headless
  (in-browser and natively). The README there documents the four
  x87 bugs and how they were tracked down via the `sc4` compressor
  and `dblue_Crusher` bitcrusher. **`_emu`'s `cpu.c` is the version
  carrying all four fixes; AcuVoice Roger's is the older lineage.**

- **AcuVoice Roger** at `AcuVoiceRoger/web/emu/`
  — same core, hosting `AvCore_acu.dll` for the Roger TTS voice.
  Structurally identical to what we want here (TTS DLL → emulator),
  but its `cpu.c` predates the four x87 fixes, so we sourced from
  `_emu/` instead.

Donor files (copied from `_emu/`, SHA-256 captured at copy time so future
upstream changes are detectable with `sha256sum -c`):

- `cpu.c` — 1052 lines, source `_emu/cpu.c`, sha256
  `46e38afe8abf60cc8a0264e0e8460e87ea63d442075134027082007ad53b01eb`, no local edits.
- `loader.c` — 187 lines, source `_emu/loader.c`, sha256
  `f8e61f1f0fbaa153b440e074ff1a733b304810cbd7aad2e5d8733cd6cc589e9e`,
  local edit: added `pe_load_mem(bytes, len)` in Phase 1; `pe_load(path)`
  is now a thin wrapper around it.
- `mem.c` — 85 lines, source `_emu/mem.c`, sha256
  `2ff2df76feb6c985b26ff2a7ba51495bde75973cd28dc74af7d52ae16b081593`, no local edits.
- `emu.h` — 122 lines, source `_emu/emu.h`, sha256
  `f94d51f4a0962d13f5514289885f0545201d94b27dce574ae7c2b10621daf8a6`, no local edits.
- `win32_donor.c` — 694 lines, source `_emu/win32_vst.c` (renamed),
  sha256 `6c95ad2fef67dc463f2397acede5484f2cde4d4b7f5f016d9d7f5f542bd0fbc4`,
  local edit: 3 lines added inside `win32_register_import` to support
  `EMU_IATDUMP=1` printing.

Local-only files (authored for this project):

- `spfy_dll_boot.{c,h}` — boot wrapper that the rest of the spfy tree calls.
- `CMakeLists.txt` — builds `spfy_host_emu` as a portable static lib.

The win32 shim file was renamed to `win32_donor.c` (from VST-flavoured
`win32_vst.c`) so its filename doesn't suggest VST-only relevance. The
file contents are byte-identical to the donor; we will trim VST/GDI/
Delphi-VCL specifics in a later pass once `EMU_IATDUMP=1` confirms which
imports `SWIttsFe-en-US.dll` actually pulls in.

## The four x87 fixes (verified in `cpu.c` of this directory)

These are the difference between "TTS produces correct audio" and "TTS
produces NaN / silence / crunch":

1. **FSCALE (D9 FD)** — line 351: `*st(0) = ldexp(*st(0), (int)*st(1))`,
   no pop. Roger's older `cpu.c` had a spurious `fpop()`. Without this,
   `pow`/`exp` are broken and the FPU stack drifts.

2. **DC reg-form FSUB/FSUBR/FDIV/FDIVR** — lines 305-308: dest=ST(i)
   reversal matches the DE pop-forms (verified `dc e1 = fsubr st(1),st(0)`
   via capstone). Roger's `cpu.c` had this swapped; symptom in `sc4`
   was envelope coefficients corrupted → silent compressor.

3. **FXAM (D9 E5)** — line 326+: classifies ST(0) into C0–C3 per Intel
   (zero / NaN / inf / denormal / normal + sign). Roger's `cpu.c` made
   this a no-op; symptom in MSVC `exp`/`pow` dispatch was "audible
   crunch" because `fxam; fnstsw; xlatb; jmp [tbl]` mis-dispatched.

4. **FIST/FISTP/FRNDINT rounding** — `fpu_round_rc()` helper at line 35,
   used at all five integer-store sites (lines 255, 256, 267, 268, 278,
   350). Roger's `cpu.c` truncated via C cast; symptom was a 2× residual
   on bitcrushed audio. For TTS-CART output (engines floor/round
   quantize duration and pitch frames), the wrong rounding mode is the
   difference between "engine-bit-exact" and "drift by ~1 sample/frame".

## API the rest of the project sees (`emu.h`)

The 1:1 entry points needed by `spfy/src/fe_host/fe_host.c` after this
work lands:

The minimal `spfy_dll_emu_*` boot wrapper (`spfy_dll_boot.h`) is the
public surface for the rest of the project. It is a thin shim over the
emulator core's `emu.h`:

- `spfy_dll_emu_boot(bytes, len)` — calls `mem_init` → `cpu_reset` →
  `win32_reset` → `pe_load_mem` → `win32_init` → `pe_init_tls` →
  `pe_run_dllmain`; returns 0 only if DllMain doesn't fault.
- `spfy_dll_emu_get_export(name)` — wraps `pe_get_export`, the
  emulator-side analogue of `GetProcAddress(hMod, name)`.
- `spfy_dll_emu_call(fn, args, n)` — wraps `call_guest`; drives the
  guest CPU until the function returns to the `RET_SENTINEL`.
- `spfy_dll_emu_alloc(n, zero)` — wraps `guest_alloc`, the analogue of
  `malloc` returning a guest VA suitable as a DLL argument.
- `spfy_dll_emu_read(va, dst, n)` / `spfy_dll_emu_write(va, src, n)` —
  copy bytes across the host/guest boundary.

Direct callers needing vtable hooks or callback marshalling still pull
in `emu.h` for `host_register_callback`, `mem_read`/`mem_write`, etc.

## Diagnostics (env-gated, inherited from donor)

- `EMU_VERBOSE=1` — verbose import resolution + faults
- `EMU_IMPLOG=1` — log every import call with ESP/cleanup info
- `EMU_IATDUMP=1` — dump the IAT at load (use this in Phase 1 to
  enumerate `SWIttsFe-en-US.dll`'s ~80 imports)
- `EMU_FPUTRACE=lo,hi,max` — dump x87 state per insn in EIP range
- `EMU_FT8=1` — also dump all 8 ST registers
- `EMU_TESTFN=hexaddr,d1,d2` — invoke a guest function with two args
  (debugging tool)

## Build

```cmake
add_subdirectory(src/host_emu)         # from spfy/CMakeLists.txt
target_link_libraries(spfy_fe_host PRIVATE spfy_host_emu)
```

The library exports two CMake targets:
- `spfy_host_emu` — the full emulator + Win32 shims as a static lib
- `spfy_host_emu_include` — INTERFACE library for the `emu.h` header
