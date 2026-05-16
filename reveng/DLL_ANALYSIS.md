# SWItts DLL Analysis

## Overview / Call Graph

```
SWIttsWsolaConcat (0x8EE65E0)
  -> 0x8EE3310  (allocate segment unit table: N * 0x2c bytes)
  -> 0x8EE6010  (configure: set [esi+0x28] pitch flag, etc.)
  -> 0x8EE2680  (WSOLA object init/constructor -- sets window params based on sample rate)
  -> 0x8EE10F0, 0x8EE1100, 0x8EE1160, 0x8EE1110, 0x8EE1140  (configure synthesis state)
  -> 0x8EE8100/8110/8090  (read VCF config: speechDBSegmentSizeMB etc.)
  -> 0x8EE5880  (prosody / amplitude modulation prep)
  -> 0x8EE3AA0  (synthesis loop: iterates over units, calls 0x8EE2960 per unit)
      -> 0x8EE2960  (process single unit: reads VDB audio into output buffer)
          -> vtable[2] = 0x8EE5240  (audio read: bounds check + call copy)
              -> 0x8EE4130  (CRASH SITE: rep movsd copy from mapped VDB)
  -> 0x8EE1150  (finalize/flush)
  -> vtable[5] = call [edx+0x14]  (post-process?)
  -> vtable[0x1c/4] = call [eax+0x1c]  (output callback?)
```

## SWIttsWsola.dll

### Base Address
0x8EE0000 (no ASLR; confirmed by IMAGE_BASE)

### Export Table
- `SWIttsWsolaConcat` = 0x8EE65E0 (main entry: synthesizes one unit sequence)
- Others: WsolaVoiceDatabase open/close, WsolaReader, WsolaLocal::readopen

### Source File Tags (from .rdata strings)
- `wsola.cpp v1.1.2.19 2003/07/02`
- `wsola_db.cpp v1.1.2.10 2003/05/30`
- `wsola_concat.cpp v1.1.2.34 2003/07/02`
- `wsola_join.cpp v1.1.2.10 2003/04/30`
- Build path: `C:\Speechify_3.0.5\Build_5046\i386-win32\...\release\SWIttsWsola.pdb`

### VDB Loading Mechanism
- `CreateFileA` + `CreateFileMappingA` + `MapViewOfFile` (memory-mapped, NOT malloc)
- Mapping is SEGMENTED: `tts.engine.speechDBSegmentSizeMB` controls segment size
- Error string: `"MapViewOfFile failed: file %s segment=%u, offset=%ld, sizeBytes=%u"`
- MapViewOfFile failure logging is in fn 0x8EE7050 (large FFT/WSOLA signal processing function)
- `GetFileSize` used to determine total VDB size

### Audio Object Structure (ecx passed to vtable functions)

Based on 0x8EE5240 analysis:
- `[+0x04]`: page_ptr_array (array of segment pointers)
- `[+0x08]`: inner page object (has `[+0x30]` = page_array, `[+0x08]` = segment_size, `[+0x0c]` = format byte: 7 = u-law)
- `[+0x2c]`: some field (passed to 0x8EE4130 as arg)
- `[+0x30]`: max_offset (VDB data size in bytes; used for bounds checking)
- `[+0x34]`: current segment index (integer: 0, 1, 2...; multiplied by 3 for page_table indexing)

### WSOLA Object Structure (esi in 0x8EE2960; ecx in vtable calls)

Initialized by constructor 0x8EE2680 based on sample rate (CONFIRMED by disassembly):

**8kHz parameters** (samplerate = 0x1f40 = 8000):
- `[+0x04]` = 0x50 **(80)** = window_size  ← comparison value at 0x8EE297A; CONFIRMED
- `[+0x08]` = 0x28 (40) = window_size/2
- `[+0x0c]` = 0xa0 (160) = 2*window_size = step size
- `[+0x10]` = 0xf0 (240) = 3*window_size
- `[+0x14]` = 0x1f40 (8000) = sample rate
- `[+0x1c]` = 0x41000000 = 8.0f (pitch rate multiplier)
- `[+0x20]` = 8
- `[+0x24]` = **3** (shift_bits: dur * 2^3 = dur * 8 bytes per WSOLA tick)  ← CONFIRMED
- `[+0x28]` = 2
- `[+0x2c]` = bool: pitch_enabled flag
- `[+0x2d]` = bool: another flag
- `[+0x30]` = ptr to audio reader object (has vtable at 0x8EE9F14 for segment reads)
- `[+0x34]` = ptr: output temp buffer (allocated/reallocated per synthesis call)
- `[+0x38]` = capacity of output temp buffer
- `[+0x44]` = ptr to per-unit processing data struct (set each call to 0x8EE2960)
- `[+0x360c]` = 0xa0 (160) = frame buffer bound
- `[+0x35c4]` = **running read-cursor**: byte offset into VDB audio data (accumulates across units)
- `[+0x35cc]` = snapshot of 0x35c4 at start of each synthesis step
- `[+0x35d0]` = overlap amount from previous unit (0 or window_size*8)
- `[+0x35d4]` = computed absolute start position for current frame
- `[+0x361c]` = pitch scale factor (1.0f initially)

**16kHz parameters** (samplerate = 0x3e80 = 16000):
- `[+0x04]` = 0xa0 (160) = window_size
- `[+0x08]` = 0x50 (80) = window_size/2
- `[+0x0c]` = 0x140 (320) = 2*window_size
- `[+0x1c]` = 0x41800000 = 16.0f
- `[+0x24]` = 4 (shift_bits: dur * 2^4 = dur * 16 bytes/tick)
- `[+0x28]` = 4
- `[+0x360c]` = 0x140 (320)

### Key Functions

#### 0x8EE2680 -- WSOLA Object Init/Constructor
- `__thiscall`: ecx = WSOLA_obj, arg1 = log_obj, arg2 = another_struct, arg3 = samplerate_obj, arg4 = something
- Allocates 3 internal buffers (window_size*8, window_size*2, samplerate*2 words)
- Initializes large struct (size ~0x3620 bytes) with WSOLA parameters
- Calls 0x8EE11E0 (likely builds cosine window table)

#### 0x8EE2960 -- Process Single Unit (calls vtable[2])
- Non-standard prologue: `push ecx` (saves this), then reads arg2 via `mov eax, [esp+0xc]`
- **TWO stack args** (CONFIRMED by disassembly 0x8EE2961):
  ```asm
  08ee2960: push ecx                  ; saves this (ecx = WSOLA obj)
  08ee2961: mov  eax, [esp+0xc]       ; after 1 push: [esp+0xc] = original [esp+8] = ARG2
  08ee2970: mov  ebp, [eax+0x0c]      ; dur = WsolaUnit[+0x0c]  (arg2, NOT arg1!)
  ```
  At Frida onEnter (before any push): arg1=[esp+4], arg2=[esp+8]=WsolaUnit ptr
- Synthesis loop call (0x8EE3ACC): `push edx (WsolaUnit); push ebp (ctx); call 0x8EE2960`
- `ecx = this` (WSOLA object), `arg2 = WsolaUnit ptr` (a WsolaUnit, 0x2c bytes)
- WsolaUnit fields (`[+0x08]` = unit_id confirmed via configure `mov [esi+8], unit_id`):
  - `[+0x00]`, `[+0x04]`: pushed as args to vtable[4] (get_max_pos call)
  - `[+0x08]`: unit_id
  - `[+0x0c]`: **duration** in WSOLA ticks (signed int32; CRITICAL)
  - `[+0x10]`: start position in WSOLA ticks
  - `[+0x18]`, `[+0x1c]`: float pitch modifier / scale
  - `[+0x24]`: sub-unit count
  - `[+0x28]`: ptr to sub-unit array (0x30 bytes each; `[+0x08]`=samples, `[+0x18]`/`[+0x1c]`=pitch floats)
- **Full computation (CONFIRMED by disassembly):**
  ```
  ebp = [arg2+0x0c] << [esi+0x24]    (= dur * 8 for 8kHz)
  edx = [arg2+0x10] << [esi+0x24]    (= start_pos * 8 for 8kHz)

  cmp ebp, [esi+0x04]                (= cmp dur*8, 80)
  JL 0x8EE29A2 (SIGNED < branch):
      [esi+0x35c4] += ebp            !! cursor DECREMENTED for negative dur !!
      [esi+0x35d0]  = ebp            (save signed delta)
      xor ebp, ebp                   (ebp = 0 for vtable call)
  JGE path (0x8EE2990):
      ebp -= [esi+0x04]              (= dur*8 - 80)
      edx += [esi+0x04]              (shift start forward by window)
      [esi+0x35c4] = edx
      [esi+0x35d0] = [esi+0x04]     (= 80)

  (sub-unit loop: ebx accumulates int pitch corrections)
  (vtable[4] call: returns VDB capacity for this unit)

  [esi+0x35c4] += [esi+0x0c]        (add step=160 to cursor)
  if cursor+ebp > capacity:
      cursor = capacity - ebp        (clamp to VDB end)

  (output buffer realloc if cursor+ebx > [esi+0x38])

  call vtable[2](output_buf=[esi+0x34], start_offset=[esi+0x35c4], n_bytes=ebp)
  ```
- **Branch at 0x8EE298E: opcode `0x7C` = JL (SIGNED less-than) CONFIRMED**
  - Raw byte read confirms 0x7C, NOT 0x72 (JB unsigned)
  - For negative dur (e.g., dur=-82): -82*8 = -656 < 80 signed -> JL TAKEN -> ebp=0 -> vtable[2] called with n_bytes=0
  - **IMPLICATION**: process_unit itself NEVER passes n_bytes=-736 to vtable[2]
  - **CURSOR ACCUMULATION BUG**: In JL-taken path, `[esi+0x35c4] += dur*8` with negative dur
    decrements the cursor. Over many units, cursor can become negative (large unsigned).
    This corrupts the VDB position index passed to the audio reader.
  - The ACTUAL crash mechanism is likely cursor overflow -> wrong page_table lookup in 0x8EE4130 -> invalid source pointer -> rep movsd AV
  - Frida hook `c:/tmp/frida_wsola_5240.py` will capture vtable[2] args (cursor + n_bytes) at crash time

#### 0x8EE5240 -- vtable[2]: Audio Read+Copy wrapper (CONFIRMED)
- `__thiscall`, no standard prologue (vtable function): `ecx = audio_obj`
- Args: `arg1 = output_buf`, `arg2 = start_offset (cursor)`, `arg3 = n_bytes`
- `ret 0xc` (callee pops 3 args)
- Confirmed as vtable[2] by .rdata scan: address 0x8EE5240 at `[0x8EE9F1C]` = vtable_base+8
- Bounds checks (both unsigned, error logged but NOT abort):
  - `n_bytes <= [ecx+0x30]` (VDB capacity); if fail: error log line 0x1f55
  - `start_offset + n_bytes <= [ecx+0x30]`; if fail: error log line 0x1f56
- After bounds checks, calls 0x8EE4130 with:
  - `this = [ecx+8]` (inner page object)
  - `arg1 = [ecx+4]`, `arg2 = [ecx+0x2c]`, `arg3 = [ecx+0x34]` (segment_idx)
  - `arg4 = cursor` (arg2 of this fn), `arg5 = output_buf` (arg1), `arg6 = n_bytes` (arg3)
- **The "File end is beyond speech DB end" message comes from here (line 0x1f56) -- no abort**

#### 0x8EE4130 -- VDB Page Copy (CRASH SITE; only direct caller: 0x8EE52C2 inside 0x8EE5240)
- `__thiscall`, ecx = inner page object (`[audio_obj+8]`), 6 stack args, `ret 0x18`
- Arg layout (confirmed by tracking push order from 0x8EE5240):
  - arg3 = `[audio_obj+0x34]` = segment_idx (VDB chunk index; used as `page_table[idx*3+1]`)
  - arg5 = output_buf
  - arg6 = n_bytes
- Format check: `cmp word ptr [ecx+0xc], 7` -- if 7 (u-law), different path from PCM16
- For u-law path: n_bytes passed as-is; for PCM16: n_bytes *= 2 (bytes vs samples)
- Early exit: `jbe` on n_bytes (unsigned) -- exits if n_bytes == 0, but NOT if negative (large unsigned)
- Page table lookup: `ebp = [page_table + segment_idx*12 + 4]` (base ptr of chunk in mapped VDB)
- `ebp += n_bytes` (advance to end of copy region)
- Inner loop: `div chunk_size` -> chunk_index and offset_in_chunk -> `esi = chunk_ptr[chunk_index]`
- `shr ecx, 2` then `rep movsd` from `esi` to `edi`
- **CRASH MECHANISM**: if n_bytes is 0xFFFFFD20 (-736 unsigned-treated), `ebp += 0xFFFFFD20`
  overflows to garbage. `div chunk_size` with garbage `eax` -> wrong chunk_index out of page_table
  bounds -> `esi = garbage pointer` -> `rep movsd` AV

#### 0x8EE4C90 -- VDB File Open (WsolaVoiceDatabase::pitchdbfileopen)
- Opens VDB file (via 0x8EE3DD0 which calls `fopen` or CreateFileA)
- Allocates segment pointer array: `n_segments = total_size / segment_size`; each entry = 4 bytes
- Calls 0x8EE3F10 (likely mmap setup)
- Also allocates second array (n_segments * 2 bytes) via 0x8EE3F50
- Sets `[this+0x28]` = segment pointer array, `[this+0x20]` = second array

#### 0x8EE3310 -- Allocate Unit Processing Table
- Allocates `(N+1) * 0x2c` bytes via malloc (0x8EE87A2)
- Sets up a struct array where each entry is 0x2c bytes
- N comes from WsolaConcat arg
- Returns ptr to first entry (offset +4 from malloc base)

### Audio Reader Vtable at 0x8EE9F14

```
[0] = 0x8EE5DD0  (destructor or open?)
[1] = 0x8EE5120  (?)
[2] = 0x8EE5240  <- write(output_buf, cursor, n_bytes) -- called from process_unit
[3] = 0x8EE52D0  (?)
[4] = 0x8EE51C0  <- get_capacity(arg0, arg1) -- called from process_unit for bounds check
[5] = 0x8EE52F0  (?) -- called from process_unit after vtable[2] (call [edx+0x14] at 0x8EE2B7F)
[6] = 0x8EE5300  (?)
```

### Crash Analysis (0x8EE4130)

**ROOT CAUSE CONFIRMED (2026-03-13, Frida session)**:

Frida output during long-text Mara synthesis:
```
[5240 #140] UNUSUAL: output_buf=0x5c6f9f0 cursor=-824 n_bytes=1704 (0x6a8) this=0x37738d8
```

**Hypothesis A -- cursor overflow: CONFIRMED**
- cursor = -824 at call #140 (negative, exactly as predicted)
- n_bytes = 1704 (completely normal -- hypothesis B is ruled out)
- Negative-duration units take the JL path; each applies net `cursor += dur*8 + 160`
- For a unit with dur < -20, net contribution is negative; cursor accumulates below 0 after many units
- cursor=-824 passed to 0x8EE4130 as unsigned = 0xFFFFFCCC -> `ebp` overflows -> garbage chunk_index -> AV

**Hypothesis B -- n_bytes out of range: RULED OUT**
- n_bytes was 1704 (normal) at the crash-triggering call; not -736 or any negative value

**WsolaUnit duration source (confirmed)**:
- `[unit+0x0c]` = `feature_table[unit_id * 24 + 4]`
- `feature_table` = `WSOLA_voice[4][4][0x20]` (loaded from VIN at startup)
- `unit_id` comes from USel output; stride 24 bytes, signed int32 at [+4]
- Negative `dur` units exist in the feature table; JL branch handles them by zeroing n_bytes
  but ALSO decrements the cursor, which is the suspected bug

---

## Synthesis Call Chain (confirmed)

```
0x8EE65E0 SWIttsWsolaConcat(log_obj, unit_sequence, something, output_state, samplerate_info)
  |
  +-> 0x8EE66E9: call 0x8EE2680  (init WSOLA object at [ebp-0x3634])
  |   args: ([edi], [ebp-0x3648], esi, [esi+8][+0x10])
  |
  +-> 0x8EE67D6: call 0x8EE3AA0  (synthesis loop; ecx=[ebp-0x3634]=WSOLA_obj)
      |
      +-> calls 0x8EE2960 at 0x8EE3ACC (for first pass)
      +-> calls 0x8EE2960 at 0x8EE3B1C (for subsequent passes)
          |
          +-> vtable[2] = 0x8EE5240 (bounds check + dispatch)
              |
              +-> 0x8EE4130 (CRASH: rep movsd with cursor=-824 as unsigned overflow)
```

---

## SWIttsEngine.dll

### Key Facts (confirmed 2026-03-13)
- ImageBase: 0x06B00000 (no ASLR)
- WsolaConcat JMP thunk: 0x06B1B212 -> `jmp [0x6b1f2e8]` (IAT for SWIttsWsolaConcat)
- **SINGLE call site**: `call 0x06B1B212` at **0x06B190F0** inside fn starting at 0x06B15720
- `add esp, 0x10` after call = 4 args cleaned (cdecl, but 5 pushes -> WsolaConcat is __stdcall or ret-cleans-one? need to verify)

### WsolaConcat Call Args (at 0x06B190EF-0x06B190F0)
```asm
push edx    ; arg1 = [esi+8]               -- output context / voice state
push ecx    ; arg2 = [0x6b2f36c]           -- WSOLA global voice resource (loaded at init)
push edi    ; arg3 = ???                    -- from earlier computation
push eax    ; arg4 = [esp+0x18]            -- USel result (filled by call at 0x06B1908A)
push esi    ; arg5 = Engine synthesis obj
call 0x06B1B212
```

### USel Call (at 0x06B1908A)
- Thunk `0x06B1B248` -> `jmp [0x6b1f2bc]` (IAT for SWIttsUSel function, DLL TBD)
- Args: `[esi+4]`, `[0x6b2f368]`, `edi`, ptr-to-`[esp+0x18]`
- OUTPUT: fills `[esp+0x18]` struct (WsolaConcat arg4)

### WsolaConcat arg4 struct (from USel output, used in configure 0x8EE6010)
- Accessed in configure as `arg2[8]` = source unit array start
- Each source unit entry: stride 24 bytes
  - `[+0]` = unit_id (used as index: `feature_table[unit_id*24 + 4]` -> WsolaUnit[+0x0c] = duration)
  - `[+0xc]` = sub-unit duration (written to WsolaSubUnit[+8])
  - `[+0x10]` = another sub-unit field
- `arg2[0xc]` = unit count

### WsolaUnit[+0x0c] Duration Derivation (from configure 0x8EE6010 at 0x8EE60EE-0x8EE60F5)
```
duration = feature_table[unit_id * 24 + 4]
where feature_table = WSOLA_voice[4][4][0x20]
      WSOLA_voice   = [0x6b2f36c] (Engine global)
      unit_id       = USel_output[unit_index * 24 + 0]
```
- This is the STATIC feature table -- does not change per synthesis call
- If a unit_id has feature_table[unit_id*24+4] = negative, it will always be negative
- Whether this causes -736 depends on [esi+4] (window_size) and the branch type (JL=signed)

### TODO
- Find which unit_ids have negative dur in feature_table (write Frida script to log unit_id+dur at process_unit entry)
- Find how feature_table is populated from VIN data (SWIttsWsolaCreateVoice 0x8EE53A0 or SWIttsWsolaCreateResource 0x8EE6410)
- Fix: ensure all units in Mara VIN produce non-negative dur in feature_table (see fix plan in Open Questions)
- Disassemble vtable[1,3,5] of audio reader to complete interface picture

---

## SWIttsUSel.dll

### Base Address
0x08E80000 (no ASLR; .text at 0x08E81000)

### Key export
`SWIttsUSelUnitSelection` at `0x08E819E0`

### Previously confirmed
- context_key = left_hp*10000 + center_hp*100 + right_hp
- prsl/hash tables work as documented

### Hash Loader (load_join_cost_hash) -- confirmed 2026-03-16, updated with Stalker trace

Function at `0x8E854A8`. Loads the `hash` chunk from VIN into memory.

**Code flow:**
1. readBytes at `0x8E87930` reads raw sub-chunk data from RIFF
2. Buffer allocation at `0x8E855F3`:
   ```asm
   lea edx, [ebx*8]        ; edx = n_cells * 8 (size of AoS buffer)
   call 0x8E94E73           ; allocate combined Cell[] buffer
   ```
3. Buffer pointer stored at `[esi+0x80]` (interleaved `Cell[n_cells]` array)
4. Rows pointer stored at `[esi+0x84]` (the `u32[n_rows]` chain-start array)

**Allocation trace (Frida Stalker, Exp 47):**
- `readBytes(692,190)` -> rows (malloc 2,768,760 at 0x8E87954)
- `readBytes(2,416,481)` -> cells_A (malloc 9,665,924 at 0x8E87954)
- `readBytes(2,416,481)` -> cells_B (malloc 9,665,924 at 0x8E87954)
- `malloc(n_cells * 8)` at 0x8E85606 -> interleaved runtime AoS buffer
- All mallocs go through 0x8E94E73
- Allocations scale dynamically from head's n_cells value

**Earlier "allocation mystery" resolved:** The initial Frida hook missed calls because
it was hooking the wrong process or timing. Stalker tracing confirmed all allocations
do flow through 0x8E94E73.

### Viterbi Hash Lookup -- compressed perfect hash (CORRECTED 2026-03-16)

During Viterbi forward pass, join cost lookup is a **single indexed access**, NOT a
chain walk. Full disassembly of the critical path:

```asm
0x8e8b7bc:  mov eax, [edx + 0x10]       ; eax = uid_left (from candidate struct +0x10)
0x8e8b7e2:  mov esi, [esp + 0x40]        ; esi = hashBase + rows[uid_right] * 8
0x8e8b7e6:  cmp [esi + eax*8], ebx       ; ONE comparison: cell.key vs uid_left
0x8e8b7e9:  jne 0x8e8b7f5               ; miss -> fallback (NO loop back)
0x8e8b7eb:  fld [esi + eax*8 + 4]       ; HIT -> load f32 join cost
```

**Register assignments:**
- `esi` = `hashBase + rows[uid_right] * 8` (pre-computed for this uid_right)
- `eax` = uid_left, used as DIRECT INDEX into the cell array (NOT a scan variable)
- `ebx` = uid_left (same value, for comparison)
- **NO bounds check** on `eax` -- relies on sentinel (0xFFFFFFFF) at empty slots
- **NO loop** -- `jne` goes to miss fallback, not back to retry

**Lookup formula:** `cell[rows[uid_right] + uid_left]`
- If `.key == uid_left`: HIT, return `.cost` (f32)
- If `.key == 0xFFFFFFFF` (empty slot): MISS
- If `.key == other_uid`: also MISS (compressed layout, slot occupied by different pair)

**Hash miss fallback** at `0x8E8B7F5`:
- Loads `0.0f` as default join cost
- Checks `[ecx+0x6C]` against `20` (threshold/counter)
- If condition met: optionally computes ccos spectral distance at runtime
- If not: returns 0.0 (effectively free join cost -- but MISSING_JOIN_COST=10000 is
  applied elsewhere when the hash has no entry at all)

**Indexing direction CONFIRMED:** `rows[uid_right]` is the base offset; `uid_left` is
the direct index. Confirmed by:
- Frida exception handler (Exp 48): ESI = hashBase + rows[uid_right]*8, not raw hashBase
- In-memory verification (Exp 49): sentinels at expected positions in interleaved buffer
- Disassembly (Exp 50): single `cmp`/`jne`, no loop instruction anywhere nearby

**Why naive appending crashed:** `cell[rows[extra_uid_right] + uid_left]` goes OOB when
uid_left is larger than the appended region. The engine has NO bounds check on the index.

### use_edgeframes Config Logic -- confirmed 2026-03-16

At `0x8E86E67`:
- `use_joincache=1` overrides `use_edgeframes=2` (joincache takes priority)
- Switch on `[ebp+0x78]` selects join cost mode
- `"ccos"` chunk opened at `0x8E86831` regardless of mode
- `ccos` is phone-indexed (47 phones x 722 entries x 12 f32), NOT unit-indexed

### Viterbi Forward Pass (NoJoin) -- disassembled 2026-03-17

Function `0x8E8B620` -- the active Viterbi path for Mara (hash misses -> no join cost).

**Structure:**
```
Init loop (0x8E8B662): for each candidate at position 0:
  [cand+0x20] = [cand+0x2c]   // cum_score = initial target cost
  [cand+0x24] = 0              // no predecessor

Forward pass (0x8E8B6E8): for each position i = 1..N-1:
  esi = HP[i] from [edi+0x18][i*4]
  candidate count = [esi+0x2c]
  candidate ptrs = [esi+0x34]

  Inner loop: for each candidate c at position i:
    ecx = [esi+0x34][j*4]
    ebx = [ecx+0x0c]            // candidate uid

    Predecessor loop: for each predecessor p at position i-1:
      edx = predecessor_ptr
      eax = [edx+0x10]          // predecessor uid_alt

      // Hash lookup
      cell_idx = rows[ebx] + eax
      if cell[cell_idx].key == eax: HIT (use cell cost)
      else: MISS -> join_cost = 0.0

      // Adjacency check at 0x8E8B854
      if ebx == eax + 1:
        join_cost = 0, context_cost = 0  (FREE same-unit transition)

      new_cum = [edx+0x20] + join_cost + context_cost
      if new_cum < [ecx+0x20]:
        [ecx+0x20] = new_cum
        [ecx+0x24] = edx        // predecessor pointer
```

**Hookable points for recording-switch penalty:**
- `0x8E8B854`: adjacency check (`cmp ebx,eax; jne`) -- 7 bytes, patchable to `jmp cave`
- Cave writes penalty via `fadd` on FPU stack before the cmp, only when file_idx differs
- Penalty saturates at p=50 (40->32 switches) due to candidate pool limitation

### Candidate Pipeline (scoring -> pruning -> Viterbi)

The full pipeline from PRSL to Viterbi:
1. **Per-utterance setup** (`0x8E89A70` = `USelNetwork::BuildGraph`): builds
   the phrase/word/syllable/segment tree from the FE-emitted utterance.
   (Earlier docs labeled this as "PRSL lookup" -- incorrect; corrected
   2026-05-05 by Ghidra decompile and confirmed by hook fire-rate.)
1a. **Per-slot PRSL preselection** (`0x8E91DC0` = `USelNetwork::AddUnit`):
   the actual per-target preselection. Reads 5 contexts at positions
   slot-4..slot+4, builds key = `ctx[1]*10000 + ctx[2]*100 + ctx[3]`,
   returns candidate UIDs into a 0x18-byte-stride buffer. Called per slot
   from `USelNetwork::AddUnits` (`0x8E920F0`). Has a 1/2/3/5 fallback
   chain (gap #6).
2. **BuildCandidateList**: creates flat 0x18-byte candidate entries
3. **InnerScorer** (`0x8E88DE0`): scores all candidates (target cost from unit properties)
4. **Prune** (`0x8E88830`): removes candidates with total_score >= threshold (VCF param)
   - Object: `[ecx+0x14]` = pre-prune count, `[ecx+0x18]` = flat array, `[ecx+0x00]` = post-prune count
5. **PostScoringAdj** (`0x8E8D210`): copies survivors to HP candidate objects
6. **Viterbi** (`0x8E8B620`): reads from `[hp+0x34]` pointer array with count `[hp+0x2c]`

**Key insight (Exp 58-59):** Runtime injection after prune step does NOT propagate to
the Viterbi's pointer-array structure. Only candidates that go through the full pipeline
(steps 1-5) appear in the Viterbi. This is why PRSL build-time injection (Exp 59) works
but Frida runtime injection (Exp 58) doesn't.

### Extra Recording Evaluation -- confirmed 2026-03-16

Frida diagnostic (`diag_extra_selection`) confirmed:
- **0 extra units** evaluated by the candidate cost function during synthesis
- **1 extra unit** appeared in WSOLA output (final pau/silence only)
- Extra recordings in prsl (~1.56M candidates) are never reached by Viterbi because
  they lack hash entries and receive MISSING_JOIN_COST=10000

---

## SWIttsEngineUtil.dll

- RIFF I/O layer (XOR decode, chunk reading)
- Has `SWIttsAudioCvtInPlaceUlawToLin16` -- converts u-law to 16-bit PCM in-place
  (This is called by WSOLA after copying from VDB)

---

## Full Synthesis Pipeline Architecture (confirmed 2026-04-03, Ghidra MCP)

Decompiled via Ghidra MCP server across all three DLLs. The complete synthesis flow:

```
Text input
  |
  v
SWIttsEngineMsft.dll (orchestrator)
  |-- SWIttsInitEx -> SWIttsOpenPortEx -> SWIttsResourceAllocate
  |-- SWIttsSpeakEx -> ConcatTTSEngine::enhancedSPRCallback (0x06B18F70)
  |     |
  |     |-- FUN_06b0c460: Parse ESPR into utterance (Festival-derived)
  |     |     Relations: Segment, Syllable, SylStructure, Intonation, IntEvent,
  |     |     Phrase, Target, Control, Notification, WordStructure
  |     |
  |     |-- SWIttsUSelUnitSelection(uselHandle, voiceHandle, utterance, &result)
  |     |     [SWIttsUSel.dll -- unit selection + Viterbi]
  |     |
  |     |-- SWIttsWsolaConcat(wsolaHandle, voiceHandle, utterance, result)
  |     |     [SWIttsWsola.dll -- overlap-add synthesis]
  |     |
  |     |-- FUN_06b18b00: staticAudioCallback (deliver audio to user)
  |
  v
Audio output (u-law 8kHz or L16 8/16kHz)
```

### Initialization sequence (ConcatTTSEngine::initialize at 0x06B1ACA0)

```
SWIttsSSMLInit(strictValidation, failOnAudioFetchError)
SWIttsLexInit()
SWIttsUSelInit(0, configParams)
SWIttsUSelCreateVoice(&DAT_06b2f368, 0, configParams)     <- loads VIN for unit selection
SWIttsWsolaInit(0, configParams)
SWIttsWsolaCreateVoice(&DAT_06b2f36c, 0, configParams, DAT_06b2f368)  <- gets USel voice!
```

Note: WSOLA voice creation receives the USel voice handle -- WSOLA has access to VIN data.

### Sub-DLL architecture

| DLL | Role | Key Exports |
|-----|------|-------------|
| SWIttsEngineMsft.dll | Orchestrator, ESPR parsing, callbacks | SWIttsSpeakEx, SWIttsSetParameter |
| SWIttsUSel.dll | Unit selection, Viterbi, CART trees | SWIttsUSelUnitSelection |
| SWIttsWsola.dll | Audio concat, pitch smoothing, VDB I/O | SWIttsWsolaConcat |
| SWIttsFe-en-US.dll | English text-to-ESPR frontend | (not yet analyzed) |
| SWIttsEngineUtil.dll | RIFF I/O, XOR decode, audio conversion | SWIttsAudioCvtUlawToL16 |
| SWIttsConfig.dll | Configuration management | getInstance, init, shutdown |
| SWIttsSSML.dll | SSML parsing | SWIttsSSMLParse |
| SWIttsLex.dll | Lexicon | SWIttsLexInit |

### Festival/Flite heritage

String evidence confirms SpeechWorks built on Festival/Flite:
- Value types: `dur_stats`, `vit_cand`, `clunit_db`, `diphone_db`, `sts_list`, `lpcres`
- Utterance system: relations, items, features, ffunctions
- CART tree features: `lisp_mod_tobi_accent`, `lisp_stress_and_accent`, `syllfoot`, etc.

---

## USel Scoring Components (confirmed 2026-04-03, Ghidra decompile)

The `TOTAL PATH` debug string reveals 6 scoring components:

```
TOTAL PATH %d units scores (S %f D %f DU %f SP %f J %f F0 %f)
```

| Component | Config Weight | Mechanism |
|-----------|--------------|-----------|
| **S** (Static) | `CONTEXT_COST_WEIGHT` (1.0) | 4-component context tables from VIN ccos |
| **D** (Duration) | `DUR_WEIGHT` (0.3) | `\|scale * (actual_dur - durt_prediction)\|^2` |
| **DU** (Duration2) | same | Log-domain alternative duration metric |
| **SP** (Position) | `*_MISMATCH_COST` (0.05 each) | Prosodic position tables from VCF |
| **J** (Join) | `JOIN_COST_WEIGHT` (0.7) | Hash table lookup (precomputed spectral) |
| **F0** (Pitch) | `ABS_F0_WEIGHT` (0.05-0.2) | `\|scale * (actual_f0 - f0tr_prediction)\|^2` |

### Duration scoring (FUN_08e8d550 decompiled)

For each candidate unit:
1. Evaluate durt CART tree for this phone in context -> `predicted_mean`, `stddev`
2. Read candidate's stored duration byte (unit+0x12 in memory)
3. If emphasis enabled and emphasis_level > 0:
   `predicted_mean += (1/stddev) * emph_dur_offsets[emphasis_level]`
4. Score = `|stddev * (stored_dur - predicted_mean)|^2`

### F0 scoring (same function)

1. Evaluate f0tr CART tree -> `predicted_f0`, `f0_stddev`
2. Read candidate's stored f0_start byte (unit+0x0F in memory)
3. If emphasis enabled and emphasis_level > 0:
   `predicted_f0 += (1/f0_stddev) * emph_f0_offsets[emphasis_level]`
4. If f0_start == 0 (unvoiced): add `MISSING_F0_COST` (default 1000.0)
5. Else: Score = `|f0_stddev * (f0_start - predicted_f0)|^2`

### Per-score logging format strings

```
durcomp target_index %d syl_type %d syl_context %d phones (%d %d %d) phone_count %d phone_in_syl %d node_index %d
DUR %d %f -> %f diff %f scaled %f tot %f
DUR2 %d %f (%f) -> %f diff %f
ABS_F0 %d %f -> %f diff %f scaled %f tot %f
Phrase_pos unit %d %d -> %d : %f
Syl_type unit %d %d -> %d : %f
Word_pos unit %d %d -> %d : %f
Phone_pos unit %d %d -> %d : %f
```

---

## USel VCF Config Struct (confirmed 2026-04-03, FUN_08e90dc0 disassembly)

Complete mapping of the config struct (ESI base) from assembly analysis:

| Offset | Type | Default | VCF Parameter |
|--------|------|---------|---------------|
| 0x00 | int | 0 | `SKIP_WORDS` |
| 0x04 | int | 0 | `SKIP_SYLS` |
| 0x08 | str | NULL | `STATS_LOG_FILE` |
| 0x0C | byte | 0 | `LOG_COMPONENT_SCORES` |
| 0x10 | float | 0.1 | `PHRASE_POS_MISMATCH_COST` |
| 0x14 | float | 0.1 | `STRESS_MISMATCH_COST` |
| 0x18 | float | 0.0 | `SYLL_IN_WORD_MISMATCH_COST` |
| 0x1C | float | 0.0 | `WORD_IN_PHRASE_MISMATCH_COST` |
| 0x20 | float | 0.0 | `PHONE_IN_SYL_MISMATCH_COST` |
| 0x24 | float | 0.01 | `F0_EDGE_CHANGE_WEIGHT` |
| 0x28 | float | 0.1 | `ABS_F0_WEIGHT` |
| 0x2C | float | 0.25 | `JOIN_COST_WEIGHT` |
| 0x30 | float | 0.2 | `JOIN_COST_OFFSET` |
| 0x34 | float | 0.01 | `DUR_WEIGHT` |
| 0x38 | float | 1.0 | `UNIT_BIAS_WEIGHT` |
| 0x3C | float | -1.0 | `CHUNK_BIAS_WEIGHT` |
| 0x40 | str | NULL | `UNIT_SCORE_FILE` / `DUMP_NETWORK_FILE` |
| 0x44 | float | 0.6 | `CONTEXT_COST_WEIGHT` |
| 0x48 | int | 50 | `HALFPHONE_CAND_MAX_UNITS` |
| 0x4C | float | 3.0 | `HALFPHONE_CAND_PRUNE_THRESH` |
| 0x50 | float | 0.005 | `HALFPHONE_CAND_PRUNE_SLOPE` |
| 0x54 | float | 3.0 | `SYL_CAND_PRUNE_THRESH` |
| 0x58 | float | 0.005 | `SYL_CAND_PRUNE_SLOPE` |
| 0x5C | float | 3.0 | `WORD_CAND_PRUNE_THRESH` |
| 0x60 | float | 0.005 | `WORD_CAND_PRUNE_SLOPE` |
| 0x64 | str | NULL | `ACTIVE_UNIT_FILE` |
| 0x68 | float | 0.0 | `V0_JCW` (voiced-0 join cost weight) |
| 0x6C | float | 0.0 | `V0_JCO` (voiced-0 join cost offset) |
| 0x70 | float | 0.0 | `V1_JCW` |
| 0x74 | float | 0.0 | `V1_JCO` |
| 0x78 | float | 0.0 | `V2_JCW` |
| 0x7C | float | 0.0 | `V2_JCO` |
| 0x80 | float | 5.0 | `MISSING_JOIN_COST` |
| 0x84 | float | 1000.0 | `MISSING_F0_COST` |
| 0x88 | int | 0 | `APPLY_ALL_F0` |
| 0x8C | int | 0 | `APPLY_ALL_F0_EDGE` |
| 0x90 | int | 0 | `GET_RID_OF_PATH_F0` |
| 0x94 | int | 0 | `ACCENT_PHRASE_SINGLE` |
| 0x98 | byte | 0 | **`EMPH_ENABLED`** (undocumented) |
| 0x9C | float | 0.0 | **`EMPH1_F0_OFFSET`** (undocumented) |
| 0xA0 | float | 0.0 | **`EMPH2_F0_OFFSET`** (undocumented) |
| 0xA4 | float | 0.0 | **`EMPH3_F0_OFFSET`** (undocumented) |
| 0xA8 | float | 0.0 | **`EMPH1_DUR_OFFSET`** (undocumented) |
| 0xAC | float | 0.0 | **`EMPH2_DUR_OFFSET`** (undocumented) |
| 0xB0 | float | 0.0 | **`EMPH3_DUR_OFFSET`** (undocumented) |
| 0xB4 | byte | 0 | `RELOAD_ACTIVE_UNITS` |
| 0xB5 | byte | 0 | `RELOAD_USelExperimentConfig` |
| 0xB8 | str | NULL | `DUMP_RANK_STATS_FILE` |
| 0xBC | str | NULL | `DUMP_SCORE_SCATTER_FILE` |
| 0xC0 | str | NULL | `DUMP_PRESELECT_INFO_FILE` |
| 0xC4 | byte | 0 | `USE_DIPHONES` |

The `speedVsQuality` parameter (float 0-1) at the end scales `HALFPHONE_CAND_MAX_UNITS`:
`new_max = round(speedVsQuality * max_units)`, clamped to [1, current_max].

### Emphasis system (new discovery 2026-04-03)

The emphasis system is activated by `EMPH_ENABLED=1` in VCF (absent from all existing voices).
When active, the `word_prominence` ESPR feature triggers per-word F0/duration offsets:
- Emphasis level 1/2/3 adds `(1/stddev) * EMPH_F0_OFFSET` to predicted F0
- Same formula for duration offsets
- Only applies to words with `word_prominence` set (from SSML `<emphasis>` or ESPR)

### CART tree features used by durt/f0tr (from strings in USel.dll)

```
aspiration, fw_ident, accpos, wordpos, syllinword, syllpos,
lisp_mod_tobi_endtone, lisp_mod_tobi_accent,
lisp_stress_and_2accents, lisp_stress_and_accent,
lisp_final_boundary_strength, lisp_initial_boundary_strength,
wordprom, syllfoot, syl_break, onsetcoda,
syl_final, syl_initial, word_final, word_initial,
power_z, dur_z, power, mpitch, contentp, closure
```

---

## WSOLA Prosody Modes (confirmed 2026-04-03, Ghidra decompile)

### Two concatenation modes (FUN_08ee1160)

Set by flag at WSOLA_state+0x3614:
- **Mode 0 = "Selective F0 smoothing"**: Pitch-mark-based overlap-add at voiced joins.
  Used when voice has pitch mark data (f0tr tree loaded in VIN).
- **Mode 1 = "Plain WSOLA"**: Simple overlap-add without pitch-aware alignment.
  Used when no pitch mark data available.

### WSOLA VCF parameters (SWIttsWsolaCreateResource at 0x08EE6410)

| Parameter | Default | Description |
|-----------|---------|-------------|
| `apply_target_prosody` | 0 | Master prosody modification switch |
| `use_prosody` | 0 | Fallback for above |
| `dur_mods` | 1 | Duration modification enabled |
| `amp_mods` | 1 | Amplitude modification enabled |
| `genf0dur` | 0 | Generate F0/duration (Craig VCF: 0) |
| `pmindex` | (none) | Optional pitchmark index file path |
| `pmdata` | (none) | Optional pitchmark data file path |

Logic: `amp_enabled = (apply_target_prosody != 0) && (amp_mods != 0)`

### Duration modification in WSOLA (FUN_08ee2960)

When `apply_target_prosody` is enabled, per-subunit duration is modified:
```
rate = CONSTANT / target_rate_value
if (phone == "pau"):
    new_dl = dl * rate          // pauses: scale dl directly
else:
    new_dl = duration * rate    // speech: scale by prediction
```

The target rate comes from ESPR "Control" and "Target" relations (speech rate control),
NOT from the durt CART trees. **durt trees only influence unit SELECTION in USel, not
audio modification in WSOLA.**

### Voiced join types (wsola_join.cpp)

At voiced boundaries, the engine detects and handles:
- vowel-vowel, vowel-nasal, vowel-approximant
- nasal-nasal, approximant-approximant
- Pitch glitch detection at join points
- Minimum overlap requirements

### Key insight: prosody flow

```
durt trees -> influence which units Viterbi SELECTS (duration scoring in USel)
f0tr tree  -> influence which units Viterbi SELECTS (F0 scoring in USel)
             + pitch-mark smoothing at voiced joins in WSOLA (mode 0)

The actual output timing = next_unit.lp - this_unit.lp (from VIN unit table)
The actual output pitch = original recording pitch, smoothed at boundaries
```

**durt trees do NOT rewrite output duration. They bias unit selection toward
units with durations matching the predicted target.**

---

## Open Questions

1. **SOLVED (Frida 2026-03-13)**: Crash = cursor overflow (hypothesis A). cursor=-824 at call #140; n_bytes was normal (1704). Fix needed: prevent negative-dur units in Mara VIN feature_table.
   - **Fix plan**: Determine which VIN chunk populates feature_table (0x8EE6410 or 0x8EE53A0); confirm it uses unit.dur_like; add clamp in build_mara_voice.py so all units have dur_like >= 1.
2. Once crash mechanism confirmed: what negative-duration units are selected on long Mara texts, and why? Is it a Mara VIN data issue (bad dur value in feature_table) or an Engine.dll selection state issue?
3. What value does vtable[4] (0x8EE51C0) return? Is it per-unit VDB capacity or global VDB size?
4. Full vtable at 0x8EE9F14 (entries 0,1,3,5 not yet disassembled)
5. VDB segment structure: how many segments, what is `[audio_obj+0x34]` (segment index or byte offset?)
6. Feature table population: how does 0x8EE6410 (SWIttsWsolaCreateResource) load feature data from VIN?
7. **NEW**: SWIttsFe-en-US.dll -- frontend prosody assignment (stress, prominence, phrase type). How are these features generated from text? Can they be modified to change Craig's prosody?
8. **NEW**: How exactly does f0tr data flow into WSOLA pitch mark smoothing? The mode 0 path loads pitch marks per-unit from VDB and uses `FUN_08ee23d0` to process them -- needs further decompilation.
9. **SOLVED (2026-04-07)**: Emphasis system fully mapped. `FUN_08e8a250` in SWIttsUSel.dll reads `word_prominence` from frontend, maps to 3 emphasis levels, applies F0/DUR offsets to scoring targets. Triggered via SSML `<emphasis>` tags. Never enabled in shipped VCFs but fully functional. See README_TECHNICAL.md for full details.
10. **SOLVED (2026-04-07)**: SWIttsLex.dll fully mapped. XML dictionary parser using Xerces DOM. DTD embedded in DLL. Entries scored by language match (100=exact, 50=prefix). Registered via `tts.engine.dictionaries` in SWIttsConfig.xml with priority > 10000. See README_TECHNICAL.md.
