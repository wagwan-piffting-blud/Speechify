# Tom Voice Binary Format Notes

Status: concluded. This document only includes structures that have been validated against the current files.

## Scope

Files analyzed in this document:

- `tom.vin` (custom RIFF form `svin`)
- `tom8.vdb` (RIFF/WAVE with custom `indx`)

All bytes in target binaries are XOR-obfuscated with `0xCE`.

- Decode: `plain_byte = enc_byte ^ 0xCE`
- Encode: same operation (symmetric)

XOR key and mechanism are **confirmed by DLL disassembly** — see `SWIttsEngineUtil.dll` section below.

---

## Quick Container Map

After XOR decode:

- `tom.vin`: `RIFF` form `svin`
  - chunks: `LIST`, `vers`, `cnts`, `feat`, `mean`, `hash`, `ckls`, `cklx`, `unit`, `f0tr`, `durt`, `ccos`, `prsl`, `hist`
- `tom8.vdb`: `RIFF` form `WAVE`
  - chunks: `LIST`, `fmt `, `indx`, `data`

---

## `cklx` (confirmed)

`cklx` is an inverted index from token text -> list of token occurrence IDs.

### Layout

`cklx.data`:

1. `u32 group_count` (always `2`)
2. Repeated groups:
   - `u16 name_len`
   - `char[name_len] group_name` (`_WORD_` or `_SYL_`)
   - `u32 entry_count`
   - Repeated `entry_count` entries:
     - `u16 key_len`
     - `char[key_len] key`
     - `u32 posting_count`
     - `u32 posting_ids[posting_count]`

### Observed counts

- `_WORD_`: `entry_count=1075`, total postings `5108`, posting IDs in `[0..5107]`
- `_SYL_`: `entry_count=1350`, total postings `7918`, posting IDs in `[0..7917]`

### Invariants validated

- Posting lists are sorted ascending.
- Posting IDs are valid indices into `ckls` token streams.
- Cross-check: `ckls_token[posting_id] == key` for every posting (0 mismatches).

---

## `ckls` (confirmed structure + likely semantics)

`ckls` stores token occurrences with per-occurrence span values, plus aligned filename markers.

### Top-level `ckls` layout

`ckls.data`:

1. `u32 group_count` (always `2`)
2. Group 0 header:
   - `u16 name_len=6`
   - `"_WORD_"`
   - `u32 token_count=5108`
   - `u32 unk0=0`
3. Group 0 record region
4. Group 1 header:
   - `u16 name_len=5`
   - `"_SYL_"`
   - `u32 token_count=7918`
   - `u32 unk0=0`
5. Group 1 record region

### Record grammar inside each group region

Records are mixed and can be distinguished by record text:

- Token record (`text` is not a filename):
  - `u16 text_len`
  - `char[text_len] token_text`
  - `u32 span_start`
  - `u32 span_end`
- Filename record (`text` matches indexed utterance name like `date_005`, `news15_040`, ...):
  - `u16 text_len`
  - `char[text_len] filename`
  - `u32 file_id` (optional on final record only)

### Observed record properties

- Both groups are strictly alternating: token record, filename record, token record, filename record, ...
- `_WORD_`:
  - token records: `5108`
  - filename records: `5108`
  - file IDs: `1..5107`, final filename has no trailing `u32` (terminator style)
- `_SYL_`:
  - token records: `7918`
  - filename records: `7918`
  - file IDs: `1..7917`, final filename has no trailing `u32`

### Span fields (`span_start`, `span_end`) — confirmed

Both fields are **global unit indices** into the `unit` table (0..169578). Confirmed by cross-validating
against `unit.file_idx` for all 5108 word and 7918 syllable records (100% match).

- `span_start` = unit index of the **first-half unit of the first phoneme** of this token.
  - `unit[span_start].is_first_half` is always `1` (verified exhaustively).
- `span_end`   = unit index of the **second-half unit of the last phoneme** of this token.
  - `unit[span_end].is_first_half` is always `0` (verified exhaustively).
- The inclusive range `[span_start, span_end]` contains exactly `2 × n_phones` unit records,
  all from the same utterance (same `file_idx`).
- Delta formula: `span_end − span_start = 2 × n_phones − 1` (always odd).
- Non-decreasing across each stream; `span_end >= span_start` always.

| Group | span_start range | span_end range | mean delta | most common delta |
|-------|-----------------|----------------|------------|-------------------|
| `_WORD_` | `9..169,498` | `18..169,501` | 5.02 | 3 (2-phone words) |
| `_SYL_`  | `15..169,466` | `18..169,469` | 3.64 | 3 (2-phone syllables) |

**`feat.filename` lookup note:** `unit.file_idx` references the **stored index field** in each
`feat.filename` entry, not its position in the chunk. The stored indices are a permutation of 0..8117
(all present, no gaps), but entries are not stored in index order. Use a stored-index→name dictionary
for correct lookup, not a positional array.

---

## Relationship Between `ckls` and `cklx`

- `ckls` provides ordered token occurrence streams (`_WORD_` and `_SYL_`).
- `cklx` provides reverse lookup from text key -> occurrence IDs in those streams.
- Totals line up exactly:
  - `cklx._WORD_` total postings `5108` == `ckls._WORD_` token_count
  - `cklx._SYL_` total postings `7918` == `ckls._SYL_` token_count

---

## `unit` (confirmed table shape, partial field semantics)

`unit` is a nested chunk container:

- `unit/vers` (u32): `100006`
- `unit/data` (bytes): `4917791`

`unit/data` is a fixed-record table:

- Record count: `169579`
- Record size: `29` bytes
- Check: `169579 * 29 == 4917791` (exact)

### Record layout (offsets within each 29-byte row)

1. `+0x00 u32 unit_id`
   - Strictly sequential `0..169578`.
2. `+0x04 u16 file_idx`
   - Range `0..8117`.
   - Maps to `feat.filename` indices (names like `date_005`, `news15_040`, ...).
   - Only `6849` distinct values appear (matches number of unique audio boundaries in `tom8.vdb/indx`).
3. `+0x06 u16 local_pos`
   - Range `0..6038`.
   - Monotonic non-decreasing within each contiguous `file_idx` run.
   - **Unit: 8 μ-law bytes at 8 kHz (= 1 ms per unit).** Confirmed by engine error analysis.
   - Engine byte offset formula: `byte_offset = local_pos * 8` (= `local_pos * 4 * (BitsPerSample/8)` where `BitsPerSample=16` is read from the `fmt` chunk, and actual audio is 1 byte/sample μ-law — the factor of 2 comes from the fmt header, not the actual sample width).
   - Verified: for all 169,570 normal units, `(local_pos + dur_like) * 8 ≤ recording_bytes` (only 9 anomalous sentinel entries with `local_pos ≈ 65535` violate this bound, and these are never selected during synthesis).
4. `+0x08 u16` always `0`.
5. `+0x0A u16 dur_like`
   - Mostly `0..255`, with a few anomalous entries (`65532..65535` in sentinel units).
   - **Unit: 8 μ-law bytes at 8 kHz (= 1 ms per unit)** — same unit as `local_pos`.
   - `segment_bytes = dur_like * 8` — number of audio bytes (= samples, since μ-law is 1 byte/sample) in this unit's segment.
   - Strong relation to within-file position step:
     - for ~92.7% of within-file pairs: `next.local_pos - local_pos == dur_like`
     - common alternate: `next.local_pos - local_pos == dur_like + 40` (40-frame gap between consecutive units, see +40 gap analysis below)
6. `+0x0C u8 syl_type` — Syllable prosodic type (7 values: {1,3,4,5,6,7,8}).
   - Matches `sylTypeCosts` dimension in VCF (1-based): UNDEF=1, Stressed=3, PA=4, FirstPA=5, FirstPAInPhrase=6, LastPAInPhrase=7, LastPAInSent=8. Unstressed (2) absent from this corpus.
7. `+0x0D u8 syl_in_phrase` — Syllable-in-phrase position (7 values: {1..7}).
   - Matches first 7 of `sylInPhraseCosts` entries in VCF (1-based): UNDEF=1, PhrInitial=2, PhrMedial=3, PhrFinal=4, PhrSingle=5, SentFinal=6, WordInit=7.
8. `+0x0E u8 word_in_phrase` — Word/phone positional category (5 values: {1..5}).
   - Likely `wordInPhrase` or a combined phone-in-syllable + syllable-in-word encoding.
9. `+0x0F u8 phone_position` — Phone positional category (5 values: {1,2,3,5,6}). Never 0.
   - Used by unit selector to gate F0 processing: loaded into synthesis state; non-zero = use F0 for this unit.
10. `+0x10 u8 f0_start` — F0 at unit **start** boundary in raw Hz. `0` = unvoiced/silence.
    - **Encoding: direct Hz value (integer, u8).** Nonzero range: 99-150 Hz for Tom. Median nonzero = 118 Hz.
    - Voiced phones: p5=106, p25=114, p50=118, p75=121, p95=127, max=150. Zero rate: ~2% (even voiced).
    - Unvoiced phones (s, sh, f, t, etc.) also carry nonzero values in most cases (similar distribution).
    - 44,204 of 169,579 total units (26%) have f0_start=0 (pause/boundary/certain unvoiced).
    - Loaded into synthesis state field `+0x6c` (F0 edge tracking for join cost).
    - **Mara note**: `build_mara_voice.py` stores `round(harvest_hz * 0.641)` here. This maps
      Mara's avg 184 Hz to 118 Hz (matching Tom median). However this produces values 51-255 Hz
      whereas Tom's range is 99-150. Values below 99 or above 150 cause higher F0 cost vs Tom's
      tight cluster. Clamp to [99, 150] or narrow Mara's pitch scale to improve match quality.
11. `+0x11 u8 f0_end` — F0 at unit **end** boundary in raw Hz. Same encoding as `f0_start`.
    - Nonzero range: 99-156 Hz for Tom (slightly wider than f0_start). Median nonzero = 118 Hz.
    - Loaded into synthesis state field `+0x68`.
12. `+0x12 u8 f0_mid` — F0 at a midpoint in raw Hz. Same encoding as `f0_start`.
    - Nonzero values are **strictly >= 99** (Tom range: 99-155). Values 1-98 do not appear.
    - Distribution nearly identical to `f0_end` (median 118 Hz). Used in F0 cost computation.
13. `+0x13 u8 f0_context` — Per-phoneme-instance target/interpolated F0 (0–226, **never 0**).
    - Same value for both halves of a phone pair (first and second unit of same phoneme instance).
    - Represents predicted or interpolated pitch even for unvoiced phones (context F0 from surrounding voiced frames).
    - **Overwritten at runtime** by `SWIttsUSel.dll` loader (function at file 0x3160) with the ccos boundary label index — the stored VIN value is only used during file loading before this overwrite.
14. `+0x14 u8 phone_center`
    - Values `0..45`.
15. `+0x15 u8 is_first_half` — **1** = first half (left boundary) of the phone pair; **0** = second half (right boundary).
    - Each phoneme instance is split into exactly 2 consecutive unit records (same `file_idx`, adjacent `local_pos`). `is_first_half` distinguishes them.
    - Controls which ccos boundary entry is used (even loop iteration vs. odd).
16. `+0x16 u8` constant `3` — Always `3`. Purpose unknown (version tag or alignment byte).
17. `+0x17..+0x1A u8 phone_ctx[4]`
    - Values in `{0..45, 255}` (`255` = sentinel/none).
18. `+0x1B u8 flag_b` — Values `{0,1}`. Default 1 (89% of units).
    - Likely marks valid utterance-internal context (1) vs. utterance-boundary / context-unavailable unit (0).
    - Approx. 10.7% of units (18,186) have `flag_b=0`; these include recording boundaries and some initial/final pauses.
19. `+0x1C u8 context_cost` — Values `{0, 100}`. 36% of units have value `100`.
    - Used directly as an index into a prosody cost lookup table: `cost = table[context_cost * 4]`.
    - Value `100` maps to the "forbidden/unknown" cost tier (matching `UNDEF=100` / `ContextUnknown=100` in VCF `proscost` matrices).
    - Units with `context_cost=100` are effectively prohibited in contexts where prosodic position is meaningful — likely marks units at recording boundaries or with unknown prosodic context.

### In-memory unit record layout (24 bytes, stride 0x18)

The DLL loader at 0x8E86160-0x8E86395 reads 29-byte on-disk records and repacks them into 24-byte in-memory records. The mapping depends on the unit version (from `unit/vers` sub-chunk).

**Version dispatch** (confirmed at 0x8E85FF0-0x8E860B1):

| Version  | `[esp+0x24]` | `[esi+0xC0]` | `ebx`/`[esp+0x2C]` | Variable read size |
|----------|:---:|:---:|:---:|:---:|
| 100004   | 0 | 0 (NULL) | 0 | 0xB = 11 |
| 100005   | 0 | 0 (NULL) | 1 | 0xB+1 = 12 |
| 100006   | 0 | allocated | 1 | 0xB+5+1 = 17 |
| 100007   | 1 | 0 (NULL) | 1 | 0xC+1 = 13 |
| 100008   | 1 | allocated | 1 | 0xC+5+1 = 18 |

Tom uses version **100006**: `[esp+0x24]=0`, `[esi+0xC0]!=0`, `ebx=1` -> 17-byte variable read.

**Mapping for version 100006** (Tom/Mara, [esp+0x24]=0):

```
In-mem  On-disk  Type  Field
+0x00   +0x04    u16   file_idx
+0x02   (pad)    u16   (zero/padding)
+0x04   +0x06    u32   local_pos (u16) + zero_field (u16) packed as dword
+0x08   +0x0A    u16   dur_like
+0x0A   +0x0C    u8    syl_type
+0x0B   +0x0D    u8    syl_in_phrase
+0x0C   +0x0E    u8    word_in_phrase
+0x0D   +0x0F    u8    phone_position
+0x0E   ---      u8    constant 6 (version < 100007: [esp+0x24]=0)
+0x0F   +0x10    u8    f0_start         ** gates MISSING_F0_COST in scorer **
+0x10   +0x11    u8    f0_end
+0x11   +0x12    u8    f0_mid
+0x12   +0x13    u8    f0_context       ** used as UNIT BIAS input in scorer **
+0x13   ---      u8    ccos_label_index (COMPUTED at load time, not from file)
+0x14   +0x14    u8    phone_center
+0x15   +0x15    u8    is_first_half
         +0x16         constant_3 (byte 22, skipped by 'add eax,2')
         +0x17..+0x1A  phone_ctx[4] -> stored in separate array at voice_obj+0xC0
+0x16   +0x1B    u8    flag_b (voiced indicator)
+0x17   +0x1C    u8    context_cost (0 or 100; stress/prosody tier)
```

**Mapping for version 100007+** ([esp+0x24]=1, extra byte at +0x0E):

```
In-mem  On-disk  Type  Field
+0x0E   +0x10    u8    f0_start (from stream, not constant)
+0x0F   +0x11    u8    f0_end           ** gates MISSING_F0_COST **
+0x10   +0x12    u8    f0_mid
+0x11   +0x13    u8    f0_context
+0x12   +0x14    u8    phone_center     ** used as UNIT BIAS input **
(rest same as 100006 but shifted)
```

**IMPORTANT**: The version determines which on-disk byte maps to in-memory +0x0F (the field used by the F0 cost gate and chunk bias). For version 100006 (Tom/Mara), +0x0F = on-disk +0x10 = **f0_start**. For version 100007+, +0x0F = on-disk +0x11 = **f0_end**.

Dropped/relocated fields:
- `unit_id` (+0x00, 4 bytes): implicit (array index)
- `constant_3` (+0x16, 1 byte): skipped by `add eax, 2` in loader (always 3, never stored)
- `phone_ctx[0..3]` (+0x17..+0x1A): stored in separate array at `voice_obj+0xC0` (4 bytes per unit, indexed by unit_id)

The `+0x13` slot is initialized to 0 by the zeroing loop at 0x8E86160, then overwritten by the ccos label index loader at 0x8E831D0 (which reads `phone_center` from +0x12 = on-disk +0x14, maps it through a phone-to-ccos-index table, and writes the result to +0x13).

### Phoneme code linkage

- `phone_center` and `phone_ctx[*]` value domain aligns with the 46 non-empty labels from `f0tr/durt/ccos` `labl` table:
  - `['aa', 'ae', ..., 'zh']` (indexes `0..45`)
  - plus sentinel `255` in context fields.

### `file_idx` run behavior

- Records form contiguous runs by `file_idx` (run count `6849`).
- First/last examples:
  - `idx=0 -> date_001` run length `6`
  - `idx=8117 -> weather7_082` run length `50`
- Most runs appear in filename-index order, with a few expected lexical-order drops (e.g. `news19_* -> news1_*`, `news29_* -> news2_*`).

### +40 gap between consecutive units

**CONFIRMED** (2026-03-12): Within a recording, 7.25% of consecutive unit pairs have `next.local_pos - local_pos == dur_like + 40`.

Exact statistics from full unit scan:
- `extra=0`:   150,918 pairs (92.74%)
- `extra=40`:   11,803 pairs  (7.25%)
- `extra=1..39`:       9 pairs  (0.01%, edge cases — likely alignment rounding)
- `extra > 40`:        0 pairs

Key confirmed facts:
- The gap is **always exactly 40** — no 80/20/variable amounts
- All +40 pairs are **within the same recording** (`is_last_in_fidx=False` for every sampled pair); cross-recording `local_pos` comparisons are meaningless since `local_pos` resets per recording
- `+40` = 40 ms = 320 μ-law bytes at 8 kHz
- The 40 ms of audio is present in the VDB but not assigned to any unit — it is skipped during synthesis playback

**Interpretation**: these 40 ms gaps are short silence/transition windows at phoneme or word boundaries within a recording session. The original corpus recording workflow inserted a fixed silence pad of 40 ms between certain adjacent utterances. The engine concatenates the two flanking units without playing the gap.

---

## `f0tr` / `durt` / `ccos` (CONFIRMED -- CART decision trees + spectral tables)

These three chunks share the same 47-label phone inventory via `labl`.

Phone labels (`47` entries; last is empty):
`aa, ae, ah, ao, aw, ax, ay, b, ch, dx, d, dh, eh, el, er, en, ey, f, g, hh, ih, ix, iy, jh, k, l, m, n, ng, ow, oy, p, pau, r, s, sh, t, th, uh, uw, v, w, xx, y, z, zh, ""`

### `f0tr` (CONFIRMED)

F0 (pitch) prediction tree. Single global CART tree for all phones.

- Subchunks:
  - `trhd` (446 bytes)
    - `labl` (47 phone labels)
    - `ques` (22 questions, types {1,2,8})
  - `tree` (1968 bytes) -- 1 tree, 109 nodes (54 branch + 55 leaf)

Leaf predictions: F0 mean in Hz (range 106.75..126.62 for Tom, centered ~117 Hz) with variance (0.12..0.48).

### `durt` (CONFIRMED)

Duration prediction trees. One CART tree per phone label (47 total).

- Subchunks:
  - `trhd` (2386 bytes)
    - `labl` (47 phone labels)
    - `ques` (154 questions, types {1,2,3,4,5,8})
  - `tree` x `47` (one per phone label; varied sizes, 24..2328 bytes)

Total across all 47 trees: 778 branch nodes + 825 leaf nodes.
Leaf predictions: duration mean (range ~57..193 in local_pos units) with variance (0.017..0.31).

### `ccos` (CONFIRMED -- detailed disassembly 2026-03-17)

Boundary spectral feature table AND duration-continuity cost table used for runtime join
cost computation on hash misses.

Sub-chunks:

| Sub-chunk | Size (bytes) | Content |
|-----------|-------------|---------|
| `labl` | 175 | `u32 n_labels=47`, then 47 x `{ u16 name_len; char[name_len] name }` |
| `data` | 1,628,832 | `47 x 722 x 48` bytes = `47 x 722 x 12` f32 boundary vectors |

**`ccos/labl`** -- 47 phone label strings (last is empty):
`aa, ae, ah, ao, aw, ax, ay, b, ch, dx, d, dh, eh, el, er, en, ey, f, g, hh, ih, ix, iy, jh, k, l, m, n, ng, ow, oy, p, pau, r, s, sh, t, th, uh, uw, v, w, xx, y, z, zh, ""`

**`ccos/data`** -- flat array, organized as:

```
47 phones x 722 entries x 12 f32s = 407,208 f32 values = 1,628,832 bytes (exact)
```

- Each **entry** is 48 bytes = 12 f32 spectral feature values (4 sub-entries of 3 floats).
- Each **phone block** contains 722 entries (361 left-boundary + 361 right-boundary, per loader).
- **Value range**: 0.0 .. 83.308 (all in MFCC-compatible range, 99.9% in [-20, 50]).

**Loader (0x8E86830-0x8E869F4):**
- Opens "ccos" chunk, then "labl" sub-chunk, then "data" sub-chunk
- Allocates `722 * 48 + 4 = 34,660` bytes for raw data buffer per phone
- Loop at 0x8E86920: iterates 722 times (2 x 361), calls 0x8E84130 for each entry
- is_first_half flag: entries 0-360 are first-half, 361-721 are second-half
- Per-entry reader at 0x8E84130: reads 4 sub-entries of 12 bytes each (total 48 bytes)
- Raw data stored at voice[0x610]
- Log strings: `"Loaded %d context tables"`, `"creating mapping num_phones %d num_labels %d"`

**Post-processor (0x8E83160):**
- Builds phone-to-label reverse mapping
- Writes ccos label index into each unit's byte at offset +0x13 (runtime-only field)
- This field is what the engine uses to look up the correct ccos boundary vectors

**Feature semantics**: 12-dimensional spectral feature vector per phone boundary frame.
Values are LPC or MFCC-derived boundary frame coefficients (confirmed by MFCC-range
values and the LPC-autocorrelation style normalization in the distance computation).

#### Runtime behavior: hash miss fallback and gate (CONFIRMED 2026-03-17)

When a hash lookup misses at 0x8E8B7E9, the fallback at 0x8E8B7F5 uses a **gated
duration-continuity cost** (NOT direct spectral distance from the ccos chunk data):

**Gate conditions** (ALL three must pass for cost computation):

```asm
0x8e8b7f5: mov eax, [ecx + 0x6c]   ; Gate1: candidate dl-like value
0x8e8b7f8: fld [0x8e9852c]          ; push 0.0 onto FPU (default cost)
0x8e8b7fe: cmp eax, 0x14            ; compare with 20
0x8e8b801: jle 0x8e8b83d            ; if <= 20, skip -> cost=0.0

0x8e8b803: cmp [edx+0x80], 0xf      ; Gate2: same-rec run counter
0x8e8b80a: jge 0x8e8b83d            ; if >= 15, skip -> cost=0.0

0x8e8b80c: mov esi, [edx+0x7c]      ; Gate3: predecessor dl-like value
0x8e8b80f: cmp esi, 0x14            ; compare with 20
0x8e8b812: jle 0x8e8b83d            ; if <= 20, skip -> cost=0.0
; ... gate pass -> compute cost from duration table
```

**Gate pass rate: 66%** (33/50 samples). CORRECTION: earlier analysis incorrectly
stated the gate "almost never" passes. For Mara, [ecx+0x6c] values are 105-150 (dl-like)
which always exceeds 20. Gate3 fails for ~34% of transitions (first candidate or zeroed
predecessor).

**Gate2 controlled by voice[0x94] = ckls entry count (NOT by use_edgeframes config):**

At 0x8E8B8D0-0x8E8B8F5 (after candidate accepted in Viterbi):
- `voice[0x94] != 0` (ckls exists): cross-rec -> candidate[0x80] = 0 (passes),
  same-rec -> candidate[0x80] = 100 (fails)
- `voice[0x94] == 0` (no ckls): candidate[0x80] increments per same-rec run (fails at 15+)

**Effect**: WITH ckls, ccos cost computed for ALL cross-rec transitions, NEVER for same-rec.
This is intentional: same-rec transitions are free (natural continuation), cross-rec
transitions get a duration-continuity penalty.

#### Duration-continuity cost table

The actual cost is a V-shaped table (NOT spectral distance from the boundary vectors).

**Table structure** at ptr0[0x80]:
```
u32 count = 100
i32 offset = -50
f32[100] values
```

**Scale factor** at ptr0[0xC8] = 0.6

**Index formula**: `index = [ecx+0x6c] + 50 - [edx+0x7c]` (dl_curr - dl_prev + 50)

**Full 100-entry table** (V-shaped, minimum at index 49):
```
[  0] 10.963 10.963 10.963 10.963 10.963 10.963 10.963 10.270 10.963 10.963
[ 10] 10.963 10.963 10.963 10.963 10.963 10.963 10.963 10.963 10.963 10.963
[ 20] 10.963 10.270 10.963  9.864 10.963 10.270 10.963  8.884  9.354  9.354
[ 30]  8.478  8.478  8.324  7.919  7.872  7.631  7.134  7.380  6.920  6.715
[ 40]  6.789  6.043  5.418  4.895  4.109  3.348  2.570  1.748  0.824  0.000
[ 50]  0.218  1.302  2.154  2.879  3.491  3.942  4.237  4.517  4.723  4.989
[ 60]  5.391  5.765  6.021  6.399  6.836  7.012  7.113  7.437  7.744  8.130
[ 70]  8.660  9.171 10.270  9.171 10.963 10.270  9.864 10.963 10.270 10.963
[ 80] 10.963 10.963 10.963 10.963 10.963 10.963 10.963 10.963 10.963 10.963
[ 90] 10.963 10.963 10.963 10.963 10.963 10.963 10.963 10.963 10.963 10.963
```

- Index 49 = 0.000 (perfect duration match between consecutive units)
- Index 0 and 99 = 10.963 (maximum penalty for large duration differences)
- Final cost = 0.6 * table[index], range 0.0 to 6.578
- Asymmetric: left side (shorter -> longer) has noisy plateau, right side (longer -> shorter)
  rises more smoothly

#### FPU flow through join cost path (0x8E8B7F8-0x8E8B854)

1. 0x8E8B7F8: `fld [0x8E9852C]` pushes 0.0. FPU: [0.0, accumulated, ...]
2. Gate fail -> jump to 0x8E8B83D with st(0)=0.0
3. Gate pass -> 0x8E8B818: `fstp st(0)` pops 0.0. FPU: [accumulated, ...]
4. 0x8E8B832: `fld [esp+0x48]` pushes scale(0.6), `fmul table[idx]`. FPU: [cost, accumulated]
5. 0x8E8B83D (common): `fst [esp+0x1c]` stores join cost component,
   `fadd [esp+0x10]` adds target cost component, `faddp st(1)` adds to accumulated total
6. 0x8E8B854: `fcom [esp+0x18]` compares with best score so far

#### Practical impact (Exp 65, 2026-03-17)

**Join cost is NOT the quality bottleneck.** A Frida code cave replacing the duration
table with real 12-dim MFCC spectral boundary distance produced byte-for-byte identical
output at multiple scale factors. The penalty hook (50) dominates all cross-rec decisions,
and the PRSL pool is small enough that the choice between cross-rec candidates rarely
changes regardless of cost function.

---

### `ques` format (CONFIRMED)

In `trhd/ques` (both `f0tr` and `durt`):

1. `u32 question_count`
2. Repeated `question_count` records:
   - `u8 key` (question type -- see mapping below)
   - `u32 value_count`
   - `u32[value_count] values` (set of values that answer YES)

In-memory question struct (12 bytes, loaded by question resolver):
```
+0x00: u32  type (= key from file)
+0x04: u32* values (pointer to value array)
+0x08: u32  count (= value_count)
```

Question counts:
- `f0tr`: 22 questions, types {1,2,8}
- `durt`: 154 questions, types {1,2,3,4,5,8}

**`ques` type-to-feature mapping (CONFIRMED by disassembly of 0x8E87C90)**:

The question evaluation function (`0x8E87C90`) dispatches on the type field and checks
whether the corresponding feature value appears in the question's value set:

| Type | Feature | Description | Value range |
|------|---------|-------------|-------------|
| 1 | `syl_type` | Syllable stress/type | {1..7} |
| 2 | `syl_in_phrase` | Position of syllable in phrase | {1..8} |
| 3 | `phone_left` | Left phone context (phone label index) | {0..45} |
| 4 | `phone_right` | Right phone context (phone label index) | {0..45} |
| 5 | `word_in_phrase` | Position of word in phrase | {1..9} |
| 7 | *(unused in f0tr/durt)* | *(passed as 0/ebx)* | -- |
| 8 | `phone_in_syl` | Position of phone within syllable | {1..6} |
| 9 | *(unused in f0tr/durt)* | *(phone_count in other trees)* | -- |

Question evaluation returns 1 (YES) if the feature value is found in the question's value
set (linear scan), 0 (NO) otherwise. For types 3, 4, 5, 8, 9 the scan is delegated to a
shared helper at `0x8E87C70`.

---

### `tree` wire layout (CONFIRMED by disassembly of loader at 0x8E83780)

**IMPORTANT**: Previous analysis assumed fixed 18-byte nodes. This was WRONG. Nodes are
**variable-size**: branches = 16 bytes, leaves = 20 bytes. There is no u16 root field.

For every `tree` chunk in `f0tr/durt`:

```
u32  n                      // node count (4 bytes)
node[0] .. node[n-1]        // variable-size nodes (see below)
```

Traversal always starts from node 0 (the root).

#### Branch node (on-disk: 16 bytes)
```
u32  node_index      // sequential index (dead field, never read during traversal)
s32  yes_child       // node index for YES path (>= 0 means branch)
u32  no_child        // node index for NO path
u32  question_index  // index into the ques array
```

#### Leaf node (on-disk: 20 bytes)
```
u32  node_index      // sequential index (dead field)
s32  -1              // 0xFFFFFFFF = leaf marker (always negative)
u32  0xFFFFFFFF      // sentinel (unused)
f32  mean            // predicted value (F0 in Hz, or duration in local_pos units)
f32  variance        // prediction variance (used as scale factor in cost formula)
```

The branch/leaf distinction is determined by the sign of the second u32:
`yes_child >= 0` means branch, `yes_child < 0` (i.e., 0xFFFFFFFF) means leaf.

#### In-memory node layout (24 bytes, 0x18)

The loader at `0x8E83780` allocates `n * 24` bytes and populates:
```
+0x00: ptr   question_ptr     // branch: resolved pointer to ques entry; leaf: 0
+0x04: u32   node_index       // dead field (= sequential index from file)
+0x08: s32   yes_child_index  // branch: child index; leaf: -1
+0x0C: u32   no_child_index   // branch: child index; leaf: 0xFFFFFFFF
+0x10: f32   mean             // leaf only: predicted value
+0x14: f32   variance         // leaf only: prediction variance
```

For branches, the question pointer (`+0x00`) is resolved at load time from the question
index: `node[0] = &ques_array[q_index * 12]` (each ques entry is 12 bytes in memory).

#### Tree traversal (CONFIRMED from disassembly of 0x8E87D90 and 0x8E87E10)

```
node = &tree_nodes[0]  // root
while node.yes_child >= 0:  // branch
    answer = question_eval(node.question_ptr, features...)
    if answer == YES:
        next_idx = node.yes_child
    else:
        next_idx = node.no_child
    node = &tree_nodes[next_idx]  // = base + next_idx * 24
return node  // leaf: result at +0x10 (mean) and +0x14 (variance)
```

Two traversal variants exist:
- `0x8E87D90`: Full 7-feature version (used for duration/durcomp scoring). Takes phone index
  to select which of the 47 per-phone trees to use.
- `0x8E87E10`: Simpler version (used for F0 target cost in unit selection loop). Uses the
  single global f0tr tree (tree index 0). Passes only 2 features + zeros for the rest.

### Node count examples

- `f0tr/tree`: 109 nodes (54 branch + 55 leaf), 1968 bytes
- `durt/tree[0]` (aa): 37 nodes (18 branch + 19 leaf), 672 bytes
- `durt/tree[36]` (t): 129 nodes (64 branch + 65 leaf), 2328 bytes (largest)
- `durt/tree[45]` (zh): 1 node (0 branch + 1 leaf), 24 bytes (smallest non-empty)

### Leaf value ranges

For `f0tr` (Tom's pitch model):
- Mean: 106.75..126.62 Hz (overall mean ~117 Hz, matching Tom's male speaking pitch)
- Variance: 0.121..0.481
- All 55 leaves clustered within 3 semitones -- a "prosodic straitjacket" that limits
  intonation range regardless of sentence content

### f0tr predictions and WSOLA pitch modification (CONFIRMED 2026-03-20)

**f0tr leaf predictions feed DIRECTLY into the engine's WSOLA pitch modification
parameters.** This means modifying f0tr leaf values changes the actual pitch contour
of synthesized speech -- the only modification (out of 71+ experiments) that affects
voice PERFORMANCE (prosody, cadence) rather than just timbre (voice identity).

#### Double-correction issue with RVC voices

When using RVC for voice conversion (e.g., Tom -> Mara), there are TWO independent
pitch-shifting mechanisms:
1. **RVC f0up_key**: shifts the fundamental frequency during voice conversion
2. **WSOLA f0tr-based modification**: the engine applies pitch modification based on
   f0tr tree predictions at synthesis time

If f0tr leaf means are scaled (e.g., scale=1.58 to shift from Tom's 118 Hz toward
Mara's 176 Hz), BOTH mechanisms apply their pitch shift independently, resulting in
"double correction" -- the voice sounds "drunk" or over-modulated.

#### Correct patching approach (patch_f0tr.py)

The correct approach for RVC-converted voices:
- **scale=1.0** -- keep leaf medians at Tom's original pitch (RVC handles the shift)
- **expand=N** -- spread leaf values around the median to increase prosodic variation

Formula in `patch_f0tr.py`:
```
median_hz = median of all leaf means (Tom: ~118 Hz)
for each leaf:
    semitones_from_median = 12 * log2(leaf.mean / median_hz)
    expanded_semitones = semitones_from_median * expand
    new_mean = median_hz * 2^(expanded_semitones / 12)
    leaf.mean = scale * new_mean
```

With scale=1.0 and expand=6.0: the median stays at 118 Hz (Tom's pitch, which RVC
will shift to Mara's range), but the variation around that median increases 6x,
restoring natural prosodic contours (questions rise, statements fall, emphasis peaks).

For `durt` (Tom's duration model, per-phone):
- Mean: 57..193 (in local_pos units = 4 samples at 8 kHz = 0.5 ms)
- Variance: 0.017..0.311
- Largest trees: `t` (129 nodes), `n` (113 nodes), `r` (101 nodes)
- Smallest trees: `zh` (1 node), `en` (5 nodes), `oy` (5 nodes)

### How tree predictions are used in scoring

At the duration/unit bias computation (`0x8E8925F`):
```c
// durt tree traversal (per-phone tree selected by phone_center)
leaf = tree_traverse(durt_trees[phone_idx], features);
prediction = leaf->mean;      // +0x10 -- in f0_context domain, NOT raw dl
scale      = leaf->variance;  // +0x14

// Unit bias cost (candidate+0x08):
diff = (float)unit.f0_context - prediction;
scaled = diff * scale;
cost = DUR_WEIGHT * |scaled|^2;

// CRITICAL: leaf.mean must be in f0_context domain (byte 19, ~60-200).
// f0_context correlates with log(dur_like) at r=0.97 but is a different
// numeric range. Using raw dl values as leaf.mean causes cost explosion.

// The durcomp debug string confirms feature order:
// "durcomp target_index %d syl_type %d syl_context %d
//  phones (%d %d %d) phone_count %d phone_in_syl %d node_index %d"
```

---

---

## DLL Analysis: `SWIttsEngineUtil.dll`

**Source**: SpeechWorks International, Speechify 3.0.5 (build 5046), compiled 2003.
**Architecture**: x86-32, MSVC. PDB path confirms: `Speechify30\ttsEngine\src\util`.

### `SWIttsRiffEncryption` enum (confirmed by disassembly)

```
NONE   = 0   // plain binary, no transform
XOR_CE = 1   // every byte XOR'd with 0xCE on read/write
```

Evidence:
- `readBytes` at `0x06b41c36–41`: `mov eax, [esi+8]` / `cmp eax, 1` / `jne skip_xor`
- `writeBytes` at `0x06b42165`: `cmp dword ptr [edi+8], 1` / `jne skip_xor`
- `create` at `0x06b41ece`: `mov dword ptr [esi+8], ecx` — stores encryption param into object field `+0x08`

Both `tom.vin` and `tom8.vdb` use `XOR_CE = 1`.

### XOR loops (disassembled)

**Read path** (`readBytes`, in-place after `fread`):
```asm
; 0x06b41c50
loop: xor byte ptr [eax + ebx], 0xce
      inc eax
      cmp eax, edi         ; edi = byte count
      jb  loop
```

**Write path** (`writeBytes`, 4 KB-chunked into temp buffer before `fwrite`):
```asm
; 0x06b42194
loop: mov dl, byte ptr [ecx + eax]
      xor dl, 0xce
      mov byte ptr [esp + eax + 0x14], dl
      inc eax
      cmp eax, esi         ; esi = min(remaining, 0x1000)
      jb  loop
; then fwrite(temp_buf, esi, 1, file)
```

### `SWIttsRiffResult` error codes

Confirmed from `mov eax, N` / `ret` patterns:

| Value | Meaning |
|-------|---------|
| `0` | OK / success |
| `1` | Invalid arg (null/empty filename) |
| `3` | Format error (bad FOURCC, chunk overrun, wrong magic) |
| `4` | I/O error (`fread`/`fwrite` returned wrong count) |
| `6` | File not open / already open |

### RIFF chunk wire format (confirmed)

`openChunk` always reads exactly:
1. `4 bytes FOURCC` → via `readBytes(buf, 4)` then `buf[4] = '\0'`
2. `4 bytes u32 size` (little-endian) → via `readBytes(&size, 4)`

Then validates that every FOURCC byte is printable (`isprint`) or space (`0x20`), and that `size <= remaining_bytes_in_parent_chunk`.

`create` (reader) additionally:
- Calls `openChunk` (required=true) expecting FOURCC `"RIFF"` (magic compared at `0x06b41f44`)
- Immediately calls `readFOURCC` to read the 4-byte **form type** that follows the RIFF chunk header

This matches the standard RIFF layout exactly:
```
RIFF <u32 file_size-8> <4-char form_type> [chunks...]
```

### `SWIttsRiffReader` object layout

Reconstructed from ctor (`0x06b41980`) and method field accesses:

| Offset | Type | Name | Notes |
|--------|------|------|-------|
| `+0x00` | `VXIlogInterface*` | logger | first ctor arg |
| `+0x04` | `FILE*` | file_handle | `fopen` result; `0` when closed |
| `+0x08` | `u32` | encryption | `SWIttsRiffEncryption` value |
| `+0x0C` | `u32` | cur_pos | running byte offset into file |
| `+0x10` | `u32` | chunk_end | absolute file offset of current chunk's end |
| `+0x14` | `std::string` | filename | SSO string (inline buf at `+0x18`, length at `+0x2C`, SSO threshold `0x10`) |
| `+0x30` | deque/stack | chunk_stack | FOURCC strings for open chunks |
| `+0x3C` | `u32` | stack_size | chunk stack element count |
| `+0x40` | `u32` | chunk_depth | nesting level; 0 = no chunk open |

### MD5 exports → `hash` chunk

The DLL exports `MD5Init`, `MD5Update`, `MD5Final` directly. The `hash` chunk in `tom.vin` is an MD5 digest computed over file content (exact input not yet determined — likely over the decoded payload of one or more chunks).

### Audio codec exports → `tom8.vdb`

The DLL exports:
- `SWIttsAudioCvtUlawToL16`, `SWIttsAudioCvtAlawToL16`, etc.
- `SWIttsAudioCvtInit` / `SWIttsAudioCvtShutDown`

This confirms `tom8.vdb` audio data is **G.711 mu-law (ulaw) at 8000 Hz mono** — consistent with `file(1)` output on the decoded file. The `fmt ` chunk in `tom8.vdb/WAVE` should be a standard WAVE `fmt ` block encoding these parameters.

### `writeInfoChunk` → `LIST/INFO` block

The writer method `writeInfoChunk` writes a `LIST INFO` sub-chunk containing:
- `ICRD` — creation date string (`%d-%02d-%02d` format)
- `ICOP` — copyright string (`"Copyright %d SpeechWorks International, Inc. All Rights Reserved."`)

The `LIST` chunk in `tom.vin` likely corresponds to this block.

---

---

## `tom8.vdb` chunk layout (confirmed)

### `fmt ` (16 bytes, standard WAVE)

| Field | Value | Notes |
|-------|-------|-------|
| `wFormatTag` | `0x0007` | WAVE_FORMAT_MULAW (tag only; actual samples are 16-bit) |
| `nChannels` | `1` | mono |
| `nSamplesPerSec` | `8000` | 8 kHz |
| `nAvgBytesPerSec` | `16000` | = 8000 * 2 |
| `nBlockAlign` | `2` | 2 bytes per sample frame |
| `wBitsPerSample` | `16` | 16-bit samples (signed linear PCM despite MULAW tag) |

Effective rate: **16000 bytes/sec**. The MULAW tag is nominal; `nBlockAlign=2` and `wBitsPerSample=16` confirm 16-bit storage. The DLL's `SWIttsAudioCvtUlawToL16` / `AlawToL16` routines convert to/from this format at runtime.

### `indx` (confirmed)

Format: variable-length entry array.

```
u32 count                  // number of named segment entries (= 8138 real + counts sentinel)

// count entries follow, the LAST is a sentinel:
repeated:
  u32 data_byte_offset     // absolute byte offset into `data` chunk
  u16 name_len
  char[name_len] name      // utterance name e.g. "date_001"; "" for sentinel

// sentinel entry: data_byte_offset = data_chunk_total_size, name = ""
```

Audio for entry `i`:
- Start: `entries[i].data_byte_offset`
- End (exclusive): `entries[i+1].data_byte_offset`
- Size: `entries[i+1].data_byte_offset - entries[i].data_byte_offset`

Entries where `size == 0` are zero-length (no audio data); they share a position boundary with an adjacent entry.

#### Observed counts

| Property | Value |
|----------|-------|
| `count` header value | `8139` |
| Named entries (non-sentinel) | `8138` |
| Non-zero-size entries | `6849` |
| Zero-size entries | `1289` |
| Entries referenced by `unit.file_idx` | `6849 distinct` |
| Max `unit.file_idx` | `8117` |

The `6849` non-zero-size entries match exactly the `6849` distinct `unit.file_idx` values in terms of **count**, but `unit.file_idx` is **NOT** a direct positional index into the `indx` entry array.

**CONFIRMED (2026-03-11):** The `indx` entry ordering in `tom8.vdb` does NOT match the `filename` ordering in `tom.vin/feat`. Of 8118 entries compared, **7444 have different positions**. Example: `news09_032` is at VIN feat position 2103 but VDB indx position 2110.

The engine resolves `file_idx` to audio via: `filenames[file_idx]` (from VIN `feat` chunk) → name-based lookup into `indx` (NOT positional). Any voice-building tool that assumes positional alignment between VDB indx and VIN feat filenames will compute wrong sample counts and produce out-of-bounds unit positions.

**Correct approach for custom voice building:** index recording sample counts by **name**, not by VDB position. Look up via `filenames[unit.file_idx]` → `name_n_samples[name]`.

#### Audio segment stats

- Duration range: **24.5 ms .. 3031.5 ms**
- Mean duration: **541.8 ms**
- Total audio: `59374776 bytes / 16000 bytes·s⁻¹ ≈ 3711 s ≈ 61.8 min`

#### Entry name corpus (prefix distribution, top categories)

`number`, `letter`, `slowLetter`, `dip1`..`dip4`, `driving1`/`driving2`, `email2`..`email4`, `date`, `news`, `weather7`, `usEmAdd1`, `intEmAdd1`, `shortAdd1`, ...

Zero-size entries are found within normal series (e.g. `driving2_061`, `driving2_063` ...) and represent utterance positions without recorded audio (silence boundaries or padding frames).

### `data`

Raw PCM audio, `59374776` bytes. Accessed via `indx` offsets. No internal structure — contiguous concatenation of all utterance segments.

---

## Join Cost Computation (confirmed from `SWIttsUSel.dll`)

The engine supports two join cost modes, selected by `tts.voiceCfg`:

| Mode | Config flag | Behavior |
|------|-------------|----------|
| 1 | `use_joincache` | Look up precomputed cost from `hash` chunk |
| 2 | `use_edgeframes` | Compute cost at runtime from `ccos` boundary vectors |

### Precomputed mode (hash lookup)

See `hash` chunk above. Lookup: `cell[rows[uid_right] + uid_left]` -- single indexed access (compressed perfect hash); return `cells_B[index]` if `cells_A[index] == uid_left`, else miss.

### Edge-frames mode (runtime computation)

Distance is computed between two 12-dim boundary feature vectors:
- `ccos[phone_label_right]` entry for the **left boundary** of `uid_right`
- `ccos[phone_label_left]` entry for the **right boundary** of `uid_left`

Computation involves:
- **LPC/autocorrelation-style normalization**: `x²/(2n), fsqrt, ×weight`
- **Two weighted components**: `joinweights[0]` and `joinweights[1]`
- Result capped at **10,000** (large-penalty sentinel for incompatible pairs)

### Float constants (from `.rdata` at file offsets)

| VA | f32 value | Role |
|----|-----------|------|
| `0x8e98520` | 0.025 | Histogram bin step (1/40) |
| `0x8e98524` | 40.0 | Histogram bin count |
| `0x8e98528` | 10000.0 | Max/penalty cost cap |
| `0x8e971d8` | 0.1 | Epsilon added before weighting |
| `0x8e96bc8` | 0.5 | Component 0 normalization multiplier (after fsqrt) |
| `0x8e96b9c` | 0.333... | Component 1 normalization multiplier (after fsqrt) |

The format string `"joinweights[%d] = %f"` is at wide-string VA `0x8e96b10`.

### Histogram normalization

Join costs are binned into **40 bins of width 0.025** over `[0.0, 1.0]` for the target-cost histogram (`hist` chunk uses 100 bins of width 1.0 over `[-50, 50]` for Z-score features; join cost uses a separate 40-bin [0, 1] histogram).

---

## `SWIttsUSel.dll` field name → `unit` record mapping

The unit-selector DLL references the following field names that correspond to `unit` record fields:

| USel field | `unit` offset | Notes |
|-----------|--------------|-------|
| `filename` | `+0x04 u16 file_idx` | index into `indx` entry array |
| `start` | via `ckls span_start` | global timeline position |
| `duration` | `+0x0A u16 dur_like` | local frame duration |
| `phoneset` | `+0x14 u8 phone_center` | phone label index (0..45) |

Categorical feature labels observed in USel (matching `unit +0x0C..+0x0F` low-cardinality bytes):

- Phone-in-syllable: `PhnSInit` / `PhnSMed` / `PhnSFin` (3 values)
- Phone-in-word: `PhnWInit` / `PhnWFin` (2 values)
- Syllable-in-word: `SylWrdInit` / `SylWrdMed` / `SylWrdFin` (3 values)
- Syllable-in-phrase: `SylPhrInit` / `SylPhrMed` / `SylPhrFin` / `SylPhrSing` (4 values)
- Word-in-phrase: `WrdPhrInit` / `WrdPhrMed` / `WrdPhrFin` (+ sing) (4 values)
- Word-in-sentence: `WrdSentInit` / `WrdSentFin` (2 values)
- Accent: `noAcc` / `onAcc` / `ppAcc` (3 values)
- Syllable type: `FinPAIS` / `FinPAIPH` / `1PAIPH` / `1PAIS` (4 values)

These categorical labels directly explain `unit +0x0C..+0x0F` (4 bytes, each a small enum) and the binary flags at `+0x15` / `+0x1B`.

`pitch_z` and `pitch` (F0) fields correspond to `f0tr` tree outputs. `duration` corresponds to `durt` tree outputs.

---

## `LIST` / `INFO` (confirmed)

Standard RIFF `LIST INFO` block written by `writeInfoChunk`:

| Sub-chunk | Content |
|-----------|---------|
| `ICOP` | `"Copyright 2003 SpeechWorks International, Inc. All Rights Reserved."` |
| `ICRD` | `"2003-05-12"` (ISO date string) |

---

## `vers` (confirmed)

Format: `u16 len` + `char[len] version_string`

Observed value: `len=12`, string `"3.0.0.0alpha"`.

---

## `cnts` (confirmed)

Three `u32le` values:

| Index | Value | Meaning |
|-------|-------|---------|
| `[0]` | `92` | Number of phone-variant entries in `feat["name"]` |
| `[1]` | `16` | Number of feature keys in `feat` |
| `[2]` | `169579` | Total unit count (= `unit/data` record count) |

---

## `feat` (confirmed)

Feature registry: a sequence of named feature definitions.

### Format

```
repeated until EOF:
  u16 key_len
  char[key_len] key_name
  u32 entry_count
  repeated entry_count times:
    u16 name_len
    char[name_len] name
    u32 index         // sequential 0..entry_count-1 for most keys
```

### Observed keys (16 total)

| Key | `entry_count` | Type | Notes |
|-----|--------------|------|-------|
| `name` | `92` | categorical | Phone+position variants: `aa1,aa2,ae1,ae2,...,zh1,zh2` |
| `start` | `0` | continuous | Utterance start time (no enumerated values) |
| `duration` | `0` | continuous | Segment duration (no enumerated values) |
| `dur_z` | `0` | continuous | Duration Z-score |
| `pitch` | `0` | continuous | F0 pitch value |
| `pitch_z` | `0` | continuous | Pitch Z-score |
| `voice` | `0` | continuous | Voicing probability |
| `voice_z` | `0` | continuous | Voicing Z-score |
| `power` | `0` | continuous | Power/energy |
| `power_z` | `0` | continuous | Power Z-score |
| `lisp_initial_boundary_strength` | `3` | categorical | Values `0,1,2` |
| `lisp_final_boundary_strength` | `3` | categorical | Values `0,1,2` |
| `Syllable.stress` | `3` | categorical | Values `0,1,2` |
| `lisp_mod_tobi_accent` | `9` | categorical | Values `!H*,0,H*,L*,L*+H,L+!H*,L+H*,NONE,OTHER` |
| `lisp_mod_tobi_endtone` | `8` | categorical | Values `0,H-,H-H%,L-,L-H%,L-L%,NONE,OTHER` |
| `filename` | `8118` | categorical | Utterance file names `date_001..weather7_082` |

#### `name` entries (phone variant list)

92 entries = 46 phones × 2 positions (1=initial/stressed, 2=final/unstressed):
`aa1,aa2, ae1,ae2, ah1,ah2, ..., zh1,zh2`

These index into the `mean` table rows (0..91).

---

## `mean` (confirmed)

Per-phone-variant feature mean table, used for Z-score normalization.

### Format

```
u32 n_phones      // = 92
u32 n_features    // = 8
f32[n_phones][n_features]   // row-major, n_phones × n_features float32 matrix
```

Total size: `8 + 92 × 8 × 4 = 2952` bytes (confirmed exact).

### Feature column order

| Col | Feature | Description |
|-----|---------|-------------|
| 0 | `duration` | Mean segment duration (ms) |
| 1 | `dur_z` | Mean duration Z-score normalizer |
| 2 | `pitch` | Mean F0 (Hz) |
| 3 | `pitch_z` | Mean pitch Z-score normalizer |
| 4 | `voice` | Mean voicing probability |
| 5 | `voice_z` | Mean voicing Z-score normalizer |
| 6 | `power` | Mean power (dB-like) |
| 7 | `power_z` | Mean power Z-score normalizer |

### Sample rows

| Phone | duration | dur_z | pitch | pitch_z | voice | voice_z | power | power_z |
|-------|----------|-------|-------|---------|-------|---------|-------|---------|
| aa1 | 53.1 | 18.0 | 123.8 | 22.1 | 0.989 | 0.054 | 6.23 | 0.17 |
| ae1 | 58.2 | 17.1 | 123.0 | 22.9 | 0.989 | 0.048 | 6.11 | 0.20 |
| b1  | 36.8 | 14.1 | 83.3 | 48.6 | 0.749 | 0.411 | 4.77 | 1.38 |
| pau1 | – | – | – | – | – | – | – | – |

Voiced phones (aa,ae,…) show high `voice` (~0.99) and low `dur_z`; obstruents (b,d,…) show lower `voice` and higher `dur_z`/`pitch_z` (more variable).

---

## `hist` (confirmed)

Per-feature Z-score histogram used for target-cost lookup.

### Format

Two nested sub-chunks (RIFF-style `tag + u32_size`):

**`head`** (8 bytes):

| Field | Type | Value | Notes |
|-------|------|-------|-------|
| `n_bins` | `u32` | `100` | Number of histogram bins |
| `range_start` | `i32` | `-50` | Bin 0 lower bound |

Bin width = 1.0; range covers Z-scores in `[-50, 50]`.

**`data`** (400 bytes = 100 × f32):

One `f32` per bin: the **negative log-probability** `−log P(Z ∈ bin)`.

- Values at extremes (Z < -40 or Z > 40): `~10.963` (clipped "infinity" — rare Z-scores treated as maximum cost)
- Values near Z=0: near `0.0` (peak of natural distribution = lowest target cost)
- Shape: inverted bell curve (−log Gaussian): cost minimum at bin 50 (Z=0), rising toward extremes
  - Bin index formula: `bin = clamp(int(z_score - range_start), 0, 99)` = `clamp(int(z + 50), 0, 99)`
  - Bin 50 = Z-score 0.0 (mean), bin 0 = Z-score -50, bin 99 = Z-score +49
- All values are non-negative (−log P ≥ 0 for P ≤ 1)
- Single shared histogram for all 8 continuous features (duration, pitch, voicing, power Z-scores use the same table — it encodes the empirical prior distribution of Z-score magnitudes across the corpus)

The histogram is used to compute target cost for continuous features: given a Z-score, look up the corresponding bin's `−log P` value as the cost contribution.

**Confirmed shape** (2026-03-12 — full 100-value array dumped):

- All values non-negative (`-log P >= 0`)
- Minimum = **0.0 at bin 49** (Z = -1 by the `floor(z+50)` formula — empirically most common Z-score in Tom's corpus)
- Maximum = **10.963** at extremes (clip value for zero-count bins)
- Shape is NOT a smooth bell curve — the empirical data is sparse and noisy at the tails (many bins at the 10.963 clip value)
- Asymmetric left/right decay: the distribution is slightly left-skewed for Tom's recordings

Selected bin values (confirmed):
```
bin  49:  0.000  (min; Z = -1)
bin  50:  0.218
bin  48:  0.824
bin  47:  1.748
bin  46:  2.570
bin  45:  3.348
bin  44:  4.109
bin  43:  4.895
bin  42:  5.418
bin  41:  6.043
bin  55:  3.942
bin  60:  5.391
bin  70:  8.660
bin   0:  10.963  (extremes clipped at max)
bin  99:  10.963
```

For a new voice, keep Tom's histogram unchanged unless the new speaker has a very different acoustic distribution. The histogram is a global prior — per-phone means/stds in `mean` already handle per-phone variation.

---

## `hash` (confirmed)

Precomputed spectral join-cost table. Loaded by `load_join_cost_hash()` in `SWIttsUSel.dll`.
Maps any `(uid_left, uid_right)` unit pair to a precomputed f32 join cost (0..~12).
Not an MD5 digest — the name refers to the hash-table organization of the data.

### Container layout

Three nested sub-chunks (RIFF-style `tag + u32_size`):

| Sub-chunk | Size (bytes) | Content |
|-----------|-------------|---------|
| `head` | 8 | `u32 n_rows=692190`, `u32 n_cells=2416481` |
| `rows` | `n_rows × 4` = 2,768,760 | `u32[n_rows]` — chain start indices |
| `cell` | `n_cells × 8` = 19,331,848 | Two flat arrays (SoA): `u32[n_cells]` then `f32[n_cells]` |

### `cell` sub-chunk — Structure of Arrays (file format)

The `cell` sub-chunk stores two flat arrays back-to-back (Structure of Arrays):

```
u32[n_cells]   cells_A   // uid_left values; 0xFFFFFFFF = sentinel
f32[n_cells]   cells_B   // join_cost values; -1.0f at sentinel positions
```

The DLL loader (`load_join_cost_hash`) converts this to Array of Structures in memory:

```
struct Cell { u32 uid_left; f32 join_cost; };   // 8 bytes each
Cell cells[n_cells];
// cells[i].uid_left  = cells_A[i]
// cells[i].join_cost = cells_B[i]
```

### `rows` sub-chunk

`rows[uid_right]` = start index of the join-cost chain for that `uid_right`.

- Value `0` means no entry (empty bucket).
- Only `rows[0..169578]` can be non-zero; `rows[169579..692189]` are always `0`.
  - `n_rows=692190` is the hash table capacity; only the unit-ID range is populated.
- `159,982` non-empty entries (range `1..169577`).
- `134,277` distinct start values (multiple `uid_rights` can share one start via suffix sharing, see below).

### Hash organization -- compressed perfect hash (CORRECTED 2026-03-16)

**Previous understanding (WRONG):** Sequential chain scan with suffix sharing. This was
inferred from statistical analysis but contradicted by disassembly.

**Actual structure (confirmed by disassembly at 0x8E8B7E6):** The hash is a **compressed
perfect hash table** with direct indexed access -- NO chain walking or sequential scan.

Lookup formula: `cell[rows[uid_right] + uid_left]`

This is a SINGLE indexed access:
- `rows[uid_right]` gives the base offset for that uid_right
- Add `uid_left` to get the cell index
- ONE comparison: if `cells_A[index] == uid_left`, it's a hit; otherwise miss
- SENTINEL (0xFFFFFFFF) marks empty slots, NOT chain terminators

The "suffix sharing" observed statistically is actually a consequence of the compressed
layout: multiple uid_rights can share the same base offset when their populated uid_left
positions happen to align (same region of the cell array serves multiple uid_rights).

**n_rows = 692,190** is the hash table bucket count (NOT the number of units). Only
`rows[0..169578]` can be non-zero; `rows[169579..692189]` are capacity padding.

**Occupancy:** 1,621,241 data entries in 2,416,481 cells = 67% occupancy. The remaining
~795,240 cells are sentinels (empty slots).

### Cell array statistics

| Metric | Value |
|--------|-------|
| Total data entries (`uid_left` <= 169578) | 1,621,241 |
| Total sentinel entries (empty slots) | 795,240 |
| Occupancy | 67% (1,621,241 / 2,416,481) |
| Non-empty rows (uid_rights with entries) | 159,982 |
| n_rows (hash capacity) | 692,190 |
| Populated row range | rows[0..169578]; rows[169579..692189] always 0 |

### Join-cost lookup for `(uid_left, uid_right)` (CORRECTED 2026-03-16)

```
index = rows[uid_right] + uid_left
if cells_A[index] == uid_left:
    return cells_B[index]           // HIT: precomputed cost
else:
    return MISS                     // fallback (0.0 or ccos distance)
```

**Assembly (0x8E8B7BC-0x8E8B7EB):**
```asm
0x8e8b7bc:  mov eax, [edx + 0x10]       ; eax = uid_left (from candidate struct)
0x8e8b7e2:  mov esi, [esp + 0x40]        ; esi = hashBase + rows[uid_right] * 8
0x8e8b7e6:  cmp [esi + eax*8], ebx       ; ONE comparison (ebx = uid_left)
0x8e8b7e9:  jne 0x8e8b7f5               ; miss -> fallback (NO loop back)
0x8e8b7eb:  fld [esi + eax*8 + 4]       ; HIT -> load f32 cost
```

Note: the old chain-scan pseudocode was WRONG. There is no loop.

### Confirmed field ranges

| Array | Non-sentinel values | Sentinel | Range |
|-------|---------------------|----------|-------|
| `cells_A` | uid_left, min=1, max=169578 | `0xFFFFFFFF` | all ≤ UNIT_MAX=169578 |
| `cells_B` | join cost f32, min=0.0, max≈11.72 | `-1.0f` | ≥0, typical 0..3 |

### Within-recording vs. cross-recording pairs (open question)

The 1,621,241 uid_left entries span all valid unit IDs (1..169578). Key question: do chains contain mostly within-recording pairs (same `file_idx`) or cross-recording pairs?

**Analytical estimate:**
- 169,579 units across 6,849 recordings = average ~24.8 units per recording
- Within-recording adjacent pairs: ~169,579 - 6,849 = ~162,730 total possible
- Total hash entries: 1,621,241 = ~10× the within-recording pair count

This strongly implies **cross-recording pairs dominate** (at least ~90% of entries). The hash table's purpose is precisely to cache the expensive spectral distance computation for cross-recording pairs that would be concatenated at synthesis time. Within-recording pairs always have cost = 0.0 (natural cut point) and would appear at the start of chains if included.

The suffix-sharing compression (119,911 uid_rights sharing one sentinel) also implies cross-recording pairs dominate: groups of phonetically similar units across recordings all share the same set of compatible left-neighbors.

**Analytical conclusion** (confirmed 2026-03-12): cross-recording pairs dominate by ~10x. Running a full scan of 20 chains would give exact percentages but would not change the conclusion. Within-recording pairs (same `file_idx`, natural concatenation boundary) do not need precomputed join costs because the engine uses the existing audio cut directly; the hash table exists precisely to store the expensive cross-recording spectral distances.

### Hash structure immutability and crash mechanism (CRITICAL -- confirmed 2026-03-16)

The hash chunk's `n_rows` and `n_cells` values are **immutable**. The engine allocates a
fixed-size buffer (`n_cells * 8` bytes) during `load_join_cost_hash()` and reads exactly
`n_cells` entries from the cell sub-chunk.

**Why appending cells crashes (understood 2026-03-16):** The engine uses uid_left as a
DIRECT INDEX into the cell array: `cell[rows[uid_right] + uid_left]`. When extra uid_rights
were given rows[] offsets pointing into a small appended region, any uid_left value larger
than the appended region caused `rows[extra_uid] + uid_left` to exceed the buffer size.
The access violation at `0x8E8B7E6` is an out-of-bounds read on `[esi + eax*8]` where
`eax` (uid_left) indexes past the end of the allocated buffer.

This means:
- Tom's `n_cells=2,416,481` is a hard ceiling for any voice built on Tom's hash.
- `n_rows=692,190` is similarly fixed (the rows array size).
- The `cells_B` (f32 join cost values) CAN be freely modified without crashing, as long
  as the structural layout (n_rows, n_cells, cells_A ordering, sentinel positions) is
  preserved identical to Tom's original.

**Shared-offset extension technique (discovered 2026-03-16):** Extra uid_rights can share
ONE offset = n_cells_original. An extension region is appended with `{uid_left, 0.0}` for
same-recording neighbors. Different recordings can share cells since the stored uid_left is
the same value. Only ~176K new cells needed (trivial overhead). This also provides OOB
safety padding for Tom uid_rights accessing extra uid_lefts. Confirmed working: extra unit
UID 176310 was selected by the engine using this technique.

**Practical implication for new voices:** Extra recordings added beyond Tom's original
8,118 recording slots cannot have their join costs included in the hash without the
shared-offset extension. Without it, they receive `MISSING_JOIN_COST=10,000` during
Viterbi evaluation, making them extremely unlikely to be selected.

### use_edgeframes mode (NO-OP -- confirmed 2026-03-17)

The `use_edgeframes` config flag is essentially a **no-op**. Detailed disassembly of the
config dispatch at 0x8E86E67 reveals:

1. `tts.voiceCfg.use_edgeframes` -> sets voice[0x78] = 2
2. `tts.voiceCfg.use_joincache` -> sets voice[0x78] = 1 (**OVERRIDES** edgeframes)
3. Switch at 0x8E86EC1: mode 1 vs mode 2 only changes which **log message** is printed
4. Both modes then execute the **same initialization code**

The actual runtime behavior (whether ccos cost is computed on hash misses) is controlled
by `voice[0x94]` (the ckls entry count), NOT by `voice[0x78]`. See the ccos gate conditions
above. With ckls present, ccos cost is computed for all cross-rec transitions regardless
of the use_edgeframes config setting.

The earlier belief that "use_edgeframes requires an unknown chunk" was incorrect. The mode
simply doesn't do anything different from use_joincache because the config is overridden
and the switch only affects logging.

### Hash loader internals (confirmed 2026-03-16, updated with Frida Stalker trace)

The hash is loaded by `load_join_cost_hash()` at `0x8E854A8` in `SWIttsUSel.dll`. Key
code flow:

1. **readBytes** at `0x8E87930` reads the raw sub-chunk data from the VIN RIFF.
2. **Buffer allocation** at `0x8E855F3`: `lea edx,[ebx*8]` computes `n_cells * 8` bytes,
   then `call 0x8E94E73` allocates the combined AoS buffer. The resulting pointer is
   stored at `[esi+0x80]` (the interleaved `Cell[n_cells]` array).
3. **Rows pointer** stored at `[esi+0x84]` (the `u32[n_rows]` chain-start array).

**Allocation trace (Frida Stalker, 2026-03-16):**
- `readBytes(692,190)` -> rows (malloc 2,768,760 bytes at 0x8E87954)
- `readBytes(2,416,481)` -> cells_A (malloc 9,665,924 bytes at 0x8E87954)
- `readBytes(2,416,481)` -> cells_B (malloc 9,665,924 bytes at 0x8E87954)
- `malloc(n_cells * 8)` at 0x8E85606 -> interleaved runtime buffer
- All mallocs go through 0x8E94E73

**Key insight:** Allocations scale dynamically from the head's `n_cells` value. The earlier
"allocation mystery" (no calls to 0x8E94E73 detected) was resolved by Frida Stalker tracing
which showed all three readBytes calls plus the interleave malloc do go through 0x8E94E73.

### Viterbi hash lookup -- compressed perfect hash (CORRECTED 2026-03-16)

During the Viterbi forward pass, join cost lookup happens at `0x8E8B7E6`. The hash is
a **compressed perfect hash** with direct indexed access, NOT a chain walk.

**Full disassembly of lookup (0x8E8B7BC-0x8E8B7EB):**
```asm
0x8e8b7bc:  mov eax, [edx + 0x10]       ; eax = uid_left (from candidate struct)
0x8e8b7e2:  mov esi, [esp + 0x40]        ; esi = hashBase + rows[uid_right] * 8
0x8e8b7e6:  cmp [esi + eax*8], ebx       ; ONE comparison: cell[offset + uid_left].key vs uid_left
0x8e8b7e9:  jne 0x8e8b7f5               ; miss -> fallback (NO loop back to retry)
0x8e8b7eb:  fld [esi + eax*8 + 4]       ; HIT -> load f32 join cost onto FPU stack
```

**Key points:**
- `esi` = `hashBase + rows[uid_right] * 8` (pre-computed base for this uid_right)
- `eax` = uid_left used as DIRECT INDEX (not a scan variable)
- ONE comparison at `[esi + eax*8]`, then either hit or miss -- no loop
- SENTINEL (0xFFFFFFFF) at empty slots causes automatic miss (never equals any valid uid)
- **NO bounds check** on `eax` -- if `rows[uid_right] + uid_left >= n_cells`, out-of-bounds

**Hash miss fallback** at `0x8E8B7F5` (CORRECTED 2026-03-17):
```
0x8e8b7f5: mov eax, [ecx + 0x6c]     ; Gate1: candidate dl-like value
0x8e8b7f8: fld [0x8e9852c]            ; push 0.0 onto FPU (default cost)
0x8e8b7fe: cmp eax, 0x14              ; compare with 20
0x8e8b801: jle 0x8e8b83d              ; if <= 20, skip -> cost=0.0

0x8e8b803: cmp [edx+0x80], 0xf        ; Gate2: same-rec run counter
0x8e8b80a: jge 0x8e8b83d              ; if >= 15, skip -> cost=0.0

0x8e8b80c: mov esi, [edx+0x7c]        ; Gate3: predecessor dl-like value
0x8e8b80f: cmp esi, 0x14              ; compare with 20
0x8e8b812: jle 0x8e8b83d              ; if <= 20, skip -> cost=0.0
; gate pass -> compute cost from V-shaped duration table (see ccos section)
```

Three gate conditions (ALL must pass for cost computation):
1. `[ecx + 0x6c] > 20` -- candidate dl-like value (Mara values 105-150: ALWAYS passes)
2. `[edx + 0x80] < 15` -- same-rec run counter (see gate2 behavior below)
3. `[edx + 0x7c] > 20` -- predecessor dl-like value (fails ~34% for first/zeroed pred)

**Gate pass rate: 66%** (33/50 samples). CORRECTION: The 2026-03-16 analysis stating
the gate "almost never" passes was WRONG. Gate1 always passes for Mara (dl values
105-150 >> 20). The gate primarily fails on Gate3 (predecessor has no dl value yet).

**Gate2 behavior** (controlled by `voice[0x94]` = ckls entry count):
At 0x8E8B8D0-0x8E8B8F5, after a candidate is accepted in the Viterbi loop:
- `voice[0x94] != 0` (ckls exists): cross-rec -> `candidate[0x80] = 0` (passes),
  same-rec -> `candidate[0x80] = 100` (fails: 100 >= 15)
- `voice[0x94] == 0` (no ckls): `candidate[0x80] = candidate[0x78]` (increments
  per same-rec continuation; fails after 15+ consecutive same-rec units)

This means WITH ckls, the ccos duration cost is computed for ALL cross-rec transitions
and NEVER for same-rec transitions. This is correct behavior by design.

**Cost computation** (when gate passes): uses V-shaped duration-continuity table.
See the `ccos` chunk section above for the full 100-entry table and FPU flow.
Final cost = 0.6 * table[dl_curr + 50 - dl_prev], range 0.0 to 6.578.

**Practical impact (UPDATED 2026-03-17):** The ccos gate DOES pass for Mara, but the
join cost is NOT the quality bottleneck. Experiment 65 (spectral join cost cave) proved
that even replacing the duration table with real MFCC spectral distances produces
identical output. The penalty hook (50) dominates all cross-rec decisions, and the PRSL
pool is small enough that the choice between cross-rec candidates rarely changes.

**Indexing direction CONFIRMED (2026-03-16):** `rows[uid_right]` is the base offset.
`uid_left` is the direct index added to that base. This was confirmed by:
- Exception handler trace (Exp 48): ESI != hashBase; ESI = hashBase + rows[uid_right]*8
- In-memory buffer verification (Exp 49): sentinels present at expected positions
- Disassembly (Exp 50): single `cmp` + `jne`, no loop instruction

### use_edgeframes config logic (CORRECTED 2026-03-17 -- NO-OP)

At `0x8E86E67`, the config dispatch:
- `tts.voiceCfg.use_edgeframes` -> sets voice[0x78] = 2
- `tts.voiceCfg.use_joincache` -> sets voice[0x78] = 1 (**OVERRIDES** edgeframes)
- Switch at 0x8E86EC1: mode 1 vs mode 2 only changes which log message is printed
- Both modes execute the same initialization code
- `"ccos"` chunk opened at `0x8E86831` (for both modes -- ccos is always loaded)
- `ccos` is indexed by phone (47 phones x 722 entries x 12 f32), NOT by unit ID
- **The actual gate behavior is controlled by voice[0x94] (ckls count), NOT voice[0x78]**

### Extra recordings and hash limitations (confirmed + resolved 2026-03-16)

Four strategies were attempted to add extra recordings beyond Tom's 8,118:

1. **APPEND (naive)**: Add new cells beyond `n_cells=2,416,481` -- crashes (AV at
   `0x8E8B7E6`). The engine uses `uid_left` as a DIRECT INDEX: `cell[rows[uid_right] +
   uid_left]`. Setting `rows[extra_uid]` to point into a small appended region meant
   `cell[rows[extra] + uid_left]` went out of bounds for any `uid_left` larger than the
   appended region size.
2. **REPLACE**: Replace existing Tom cells with new-recording pairs -- scattered single-unit
   runs make this impractical (chain structure must be preserved).
3. **DLL patch**: Attempted to find and enlarge the allocation -- initially appeared to fail,
   but Frida Stalker later revealed the allocation path (see hash loader internals).
4. **Shared-offset extension (WORKING)**: All extra uid_rights share ONE offset =
   n_cells_original. Extension region appended with `{uid_left, 0.0}` for same-rec
   neighbors. Different recordings share cells since stored uid_left is the same. Only
   ~176K new cells needed. Provides OOB safety padding for Tom uid_rights accessing extra
   uid_lefts. **Confirmed working: extra unit UID 176310 selected by engine.**

**Result**: Shared-offset extension solves the hash immutability problem. Extra recordings
can now participate in Viterbi search with valid join costs.

### Prosodic field requirements for extra recordings (discovered 2026-03-16)

Extra units added to the corpus require Tom's per-phone MODE values for prosodic fields:
- `syl_type` (byte 8): must match the most common value for that phone
- `syl_in_phrase`: syllable position in phrase
- `word_in_phrase`: word position in phrase
- `phone_pos` (byte 2): phone position in syllable
- `pctx3` (byte 3): phone context field

Without these values matching Tom's distribution, the CONTEXT_COST_WEIGHT=1.0 penalty
makes extra units uncompetitive even if they were in the hash.

---

## `prsl` (confirmed)

Preselection cache. Maps synthesis context keys to lists of candidate unit IDs.

Confirmed from `SWIttsUSel.dll` strings: `"load_preselection_cache_data(%d items)"`, `"Preselection cache not loaded"` and `"joinweights[%d] = %f"` all reference this chunk's loading code.

### Format

```
u32 count                    // number of preselection groups (= 76676)

// count variable-length groups, each:
  u32 n                      // total entries in this group (key + candidates)
  u32 context_key            // synthesis context identifier (always position 0)
  u32[n-1] candidate_ids     // candidate unit IDs from unit table (0..169578)
  // 0xFFFFFFFF sentinel appended IN MEMORY only; not stored in file
```

Total size: `4 + Σ(4 + n×4) over all groups = 4,969,856` (confirmed exact).

### Observed properties

| Property | Value |
|----------|-------|
| Group count | `76,676` |
| Total candidate unit IDs | `1,089,111` |
| Average candidates per group | `~14.2` |
| Most common candidate count (`n-1`) | `1` (12,409 groups) |
| Max candidates in one group | `4831` |

### `context_key` field — CONFIRMED

- Always position 0 in each group's entry list.
- Strictly monotonically increasing across all 76,676 groups.
- Range: `54..929,192`.
- `14,089` keys fall in the valid unit ID range (≤ 169,578); `62,587` exceed it.

**Formula (confirmed from `SWIttsUSel.dll` assembly at `0x8e917f0`):**

```
context_key = left_hp * 10000 + center_hp * 100 + right_hp
```

This is a TRIGRAM on halfphone indices. Assembly uses two `imul eax, 0x64` (multiply by 100) instructions.

**Halfphone index encoding:**

```
halfphone_idx = hp_base[unit.phone_center] + (1 - unit.is_first_half)
```

- `hp_base[pc]` is always even (= `2 * sorted_phone_position`)
- Even halfphone index (hp_base) = left-boundary half (is_first_half=1)
- Odd halfphone index (hp_base+1) = right-boundary half (is_first_half=0)
- Silence/boundary marker = `92` (= 2*46, beyond the 46 phone variants 0..45)

**`hp_base` table** (empirically confirmed; mostly `2*pc`, anomalies shown):

```
pc  0..8   (aa1..aw1): hp_base = 2*pc     (normal)
pc  9      (aw2):      hp_base = 22        (anomaly; 2*9=18 expected)
pc 10      (ax1):      hp_base = 18        (anomaly; 2*10=20 expected)
pc 11      (ax2):      hp_base = 20        (anomaly; 2*11=22 expected)
pc 12..13  (ay1..ay2): hp_base = 2*pc     (normal)
pc 14      (b1):       hp_base = 30        (anomaly; 2*14=28 expected)
pc 15      (b2):       hp_base = 28        (anomaly; 2*15=30 expected)
pc 16..45  (ch1..iy2): hp_base = 2*pc     (normal)
pc 42      (ix1):      hp_base = 84        (2*pc, no prsl groups observed)
```

**Context encoding rules** (99.94% accuracy on all 76,676 groups):

```
hp_base_for_center = HP_BASE[unit.phone_center]  # always even

if A == 0 or (A == 92 and C == 92):
    center_hp = hp_base_for_center          # even; aggregate (both halves)
elif A == 92:
    center_hp = hp_base_for_center + 1      # odd; coda (before utterance-end silence)
else:
    center_hp = hp_base_for_center + (A % 2)  # A_even -> onset (even), A_odd -> coda (odd)
```

Breakdown:
- `A=0, C=*`: utterance start or no left context; B=even (base); candidates may be either half
- `A=92, C=92`: isolated unit (silence both sides); B=even (base); candidates are mixed halves
- `A=92, C!=92`: unit just before speech-end boundary; B=odd (coda); candidates are is_first_half=0
- `A=even, 0<A<92`: left context is onset half; B=even (onset); candidates mostly is_first_half=1
- `A=odd`: left context is coda half; B=odd (coda); all candidates are is_first_half=0

The C (right context) follows the same hp_base encoding. C=0 and C=92 are both silence markers.

All candidates in a group share the same `unit.phone_center` value (100% confirmed).

### Candidate IDs

- All entries at positions `1..n-1` are confirmed valid unit IDs (`0..169578`).
- Zero out-of-range values across all 1,089,111 candidates (verified exhaustively).
- These are the pre-selected database units the Viterbi search will evaluate for a given synthesis context.

---

## `.vcf` — Voice Configuration File (confirmed)

The `.vcf` file is the runtime configuration for the voice. It contains all unit-selection cost weights, prosody cost matrices, and file path templates loaded by the engine at startup.

### Encryption — nibble-expansion cipher

**Distinct from VIN/VDB** (which use XOR 0xCE). Each plaintext byte is split into two nibbles; each nibble is encoded as one byte, doubling the file size.

- Even-position encrypted bytes encode the **high nibble** (always `0xD_` high nibble).
- Odd-position bytes encode the **low nibble** (`0xD_` or `0xA_` high nibble).

Substitution table:

| Enc byte | Nibble | Enc byte | Nibble |
|----------|--------|----------|--------|
| `0xDD` | 0 | `0xDA` | 7 |
| `0xDC` | 1 | `0xD5` | 8 |
| `0xDF` | 2 | `0xD4` | 9 |
| `0xDE` | 3 | `0xAC` | A |
| `0xD9` | 4 | `0xAF` | B |
| `0xD8` | 5 | `0xAE` | C |
| `0xDB` | 6 | `0xA9` | D |
|         |   | `0xA8` | E |
|         |   | `0xAB` | F |

Decryption: read byte pairs → look up each byte → `(hi_nibble << 4) | lo_nibble`.

`tom.vcf`: 46,650 encrypted bytes → 23,325 plaintext bytes.

### Plaintext format

Standard XML (ISO-8859-1) with associated DTD (`SWIttsConfig.dtd`) and XSL stylesheet (`SWIttsConfig.xsl`):

```xml
<?xml version="1.0" encoding="ISO-8859-1"?>
<!DOCTYPE SWIttsConfig PUBLIC "-//SpeechWorks//DTD SPEECHIFY CONFIG 1.0//EN" "SWIttsConfig.dtd">
<SWIttsConfig version="1.0.0">
  <lang name="Default">
    <param name="tts.voiceCfg.PARAM_NAME">
      <value> value </value>
    </param>
    ...
  </lang>
</SWIttsConfig>
```

### Tom voice parameters (`tom.vcf`)

#### File paths (template variables `${xml:base}` and `${tts.voice.name}`)

| Parameter | Value | Notes |
|-----------|-------|-------|
| `tts.voiceCfg.name` | `Tom` | Voice name |
| `tts.voiceCfg.language` | `en-US` | Language |
| `tts.voiceCfg.gender` | `male` | Gender |
| `tts.voiceCfg.phoneset` | `swi_plus_ix` | Phone set identifier |
| `tts.voiceCfg.version` | `3.0.0.0` | Voice version |
| `tts.voiceCfg.index` | `${xml:base}/${tts.voice.name}.vin` | VIN file path template |
| `tts.voiceCfg.speechdb` | `${xml:base}/${tts.voice.name}${tts.voice.format}.vdb` | VDB file path template |

#### Cost function weights

| Parameter | Value | Role |
|-----------|-------|------|
| `JOIN_COST_WEIGHT` | 0.7 | Global join cost scale |
| `JOIN_COST_OFFSET` | 0.2 | Offset added to every join cost |
| `CONTEXT_COST_WEIGHT` | 1.0 | Target cost scale |
| `DUR_WEIGHT` | 0.3 | Duration cost weight |
| `ABS_F0_WEIGHT` | 0.2 | Absolute F0 cost weight |
| `F0_EDGE_CHANGE_WEIGHT` | 0.6 | F0 edge change cost weight |
| `CHUNK_BIAS_WEIGHT` | 0.25 | Chunk reuse bias |
| `UNIT_BIAS_WEIGHT` | 0.25 | Unit reuse bias |
| `PHONE_IN_SYL_MISMATCH_COST` | 0 | Phone-in-syllable mismatch cost |
| `PHRASE_POS_MISMATCH_COST` | 0.05 | Phrase position mismatch |
| `STRESS_MISMATCH_COST` | 0.05 | Stress mismatch cost |
| `SYLL_IN_WORD_MISMATCH_COST` | 0.05 | Syllable-in-word mismatch |
| `WORD_IN_PHRASE_MISMATCH_COST` | 0.05 | Word-in-phrase mismatch |

#### Join cost mode (for Tom)

| Parameter | Value | Meaning |
|-----------|-------|---------|
| `use_joincache` | 1 | **Enabled** — use precomputed `hash` chunk |
| `use_edgeframes` | 0 | **Disabled** — do not compute at runtime |
| `use_dynamic_cost` | 1 | Dynamic cost enabled |

#### Candidate pruning thresholds

| Parameter | Value |
|-----------|-------|
| `HALFPHONE_CAND_PRUNE_THRESH` | 0.8 |
| `HALFPHONE_CAND_PRUNE_SLOPE` | 0.005 |
| `SYL_CAND_PRUNE_THRESH` | 0.7 |
| `SYL_CAND_PRUNE_SLOPE` | 0.005 |
| `WORD_CAND_PRUNE_THRESH` | 0.7 |
| `WORD_CAND_PRUNE_SLOPE` | 0.005 |

#### Phone inventory with voicing annotations

The VCF defines voicing classes for each phone (used in prosody cost decisions):

- **voiced vowel**: aa, ae, ah, ao, aw, ax, ay, eh, el, er, en, ey, ih, ix, iy, ow, oy, uh, uw, xx
- **voiced** (sonorant/liquid): jh, l, m, n, ng, r, w, y
- **unvoiced** (empty annotation): b, ch, dx, d, dh, f, g, hh, k, p, pau, s, sh, t, th, v, z, zh

#### Prosody cost matrices (`proscost`)

Four cost matrices are defined as `target_context → candidate_context → cost` lookup tables:

**`sylInPhraseCosts`** (syllable-in-phrase position mismatch) — 10×10 matrix.
Rows/cols: `UNDEF, PhrInitial, PhrMedial, PhrFinal, PhrSingle, SentFinal, WordInit, WordMedial, WordFinal, ContextUnknown`.
- Diagonal (same position) = 0. Mismatches up to 10 (SentFinal↔PhrInitial).
- `PhrSingle` and `ContextUnknown` = 100 (forbidden).

**`sylInWordCosts`** (syllable accent position) — 7×7 matrix.
Cols: `UNDEF, NoAccent, BeforeAccent, OnAccent, PostAccent, PostPostAccent, AccUnknown`.
- All non-UNDEF mismatches = 0 (accent position is not penalized for Tom).

**`sylTypeCosts`** (syllable prosodic type) — 9×9 matrix.
Cols: `UNDEF, Unstressed, Stressed, PA, FirstPA, FirstPAInPhrase, LastPAInPhrase, LastPAInSent, SylUnknown`.
- Diagonal = 0. Cross-type penalties reflect prosodic hierarchy (sentence-final vs. medial heavily penalized).

**`wordInPhraseCosts`** (word-in-phrase position) — 7×7 matrix.
Cols: `UNDEF, WordPhrInitial, WordPhrMedial, WordPhrFinal, WordPhrUnknown, WordSentInitial, WordSentFinal`.
- Phrase-final ↔ phrase-medial asymmetries up to 5.

### Creating a new voice VCF

To create a new voice `NewVoice`:
1. Set `tts.voiceCfg.name` to `NewVoice`
2. Update `tts.voiceCfg.gender` as appropriate
3. Keep all cost weights and matrices (or tune them to the new corpus)
4. The engine will load `NewVoice.vin` and `NewVoice8.vdb` (or `NewVoice16.vdb` depending on `${tts.voice.format}`) from the same directory
5. Re-encrypt with the nibble-expansion cipher

---

## Building a New Voice

### `feat.filename` stored-id vs positional index

Each `feat.filename` entry ends with a `u32 stored_id` field. The entries in the chunk are **not** stored in stored_id order — 2,345 of 8,118 entries have `stored_id ≠ positional_index` in `tom.vin`. `unit.file_idx` is the stored_id, not the positional index. Any tool that builds a positional array (`filenames.append(name)`) and then looks up `filenames[file_idx]` will silently retrieve the **wrong name** for the ~29% of units whose file_idx falls in the mismatched range.

**Correct pattern:**
```python
filenames = {}  # stored_id → name
for _ in range(fn_count):
    nlen = struct.unpack_from('<H', feat, p)[0]
    name = feat[p+2:p+2+nlen].decode('latin-1')
    stored_id = struct.unpack_from('<I', feat, p+2+nlen)[0]
    filenames[stored_id] = name
    p += 2 + nlen + 4
```

### Bounds check for VDB audio

Engine check (WSOLA module): `(local_pos + dur_like) * 4 > recording_samples` → "File end is beyond the speech DB end". Equivalently: `(local_pos + dur_like) * 8 > byte_size`. Safe cap formula for custom voices:
```
cap = (recording_byte_size // 2) // 4 - 1
# guarantees: (new_lp + new_dl) * 8 < byte_size
```

Zero-size VDB recordings (silence boundaries) require `lp = dl = 0`; any non-zero values will always trigger the bounds error.

### Mara voice build notes

- Source WAVs: `en-US/mara/output/mara_voice_wavs/` (6,730 WAVs including 270 re-recorded)
- Downsampled to 8 kHz mu-law for `mara8.vdb`
- Build pipeline (run in order):
  1. `build_mara_voice.py` -- builds mara8.vdb + mara.vin; lp/dl from MFA phone boundaries
  2. `build_mara_rest.py` -- patches hash/prsl/mean chunks; preserves ALL Tom prsl keys
  3. `build_mara_hash.py` -- patches hash with same-recording bias (HIGH_CROSS_COST=3.0)
  4. `build_mara_trees.py` -- patches f0tr/durt leaves (SKIP_DURT_RECOMPUTE=True)
- `mara.vin` unit `local_pos`/`dur_like`: MFA phone boundaries mapped directly for recordings
  with TextGrids; proportional fallback for remaining recordings.
- MFA coverage: 6,730 TextGrids (99% of recordings).

#### Mara lp/dl invariants (critical -- confirmed 2026-03-16)

- **Never set dl=0 for any unit.** The same-recording continuation bypass means dl=0 units
  can still be selected and produce silence. Always use `dl = max(1, ...)`.
- **Never create same-lp clusters.** Consecutive units in the same recording must have
  strictly increasing lp. Enforcement: `if _lp <= _prev: _lp = _prev + 1`.
- **Minimum unit spacing = 15ms** (global), **25ms** for MFA-derived units.
  Below 15ms, WSOLA output duration becomes perceptually inaudible (near-silence).
- **dl inflation**: All units (MFA and non-MFA) must have dl inflated to at least Tom's
  minimum dl for that phone, so WSOLA has sufficient source material to work with.
  Do NOT skip dl inflation for MFA-aligned units.

---

## WSOLA Engine -- local_pos Monotonicity Invariant

**This is the hardest-to-diagnose crash in the entire build pipeline.** It produces an
`EXCEPTION_ACCESS_VIOLATION` inside `SWIttsWsola.dll` during long-text synthesis, with no error
message from the engine. Short texts work fine.

### The constraint

Within each recording (i.e., all units sharing the same `file_idx`), the `local_pos` values **must
be monotonically non-decreasing** when the units are sorted by their original Tom lp order. Formally:

```
for each consecutive pair (unit_i, unit_j) in the same recording, sorted by Tom lp:
    unit_j.local_pos >= unit_i.local_pos
```

If any unit has a `local_pos` that is less than the `local_pos` of the preceding unit in the same
recording, the WSOLA engine will eventually crash with an access violation.

### Why -- the WSOLA configure accumulator

`SWIttsWsola.dll:configure` (0x8EE6010) builds a `WsolaUnit` array from the Engine's input unit
list. For each unit, it accumulates sub-unit contributions into a running sum stored at `[esp+0x1c]`.
The final sum is written to `WsolaUnit[+0x10]` as `start_pos`:

```
WsolaUnit.start_pos = sum of sub-unit[+0x0c] values across all sub-units
```

The sub-unit `[+0x0c]` values are derived from consecutive `local_pos` differences. If any
`local_pos` value is smaller than the previous one (a backwards jump), the corresponding difference
is negative, the running sum goes below zero, and `start_pos` ends up negative.

At synthesis time, `process_unit` (0x8EE2960) uses `start_pos` to set the VDB read cursor:

```
cursor = start_pos * 8 + 80 + 160
```

With `start_pos = -133`, for example: `cursor = -1064 + 240 = -824`. The cursor is then passed
as a `u32` to the page-copy function at 0x8EE4130, where it becomes the enormous value
`0xFFFFFCCC`. The loop-exit test is an unsigned comparison, so it does NOT exit early; instead the
engine computes a wildly wrong page pointer and executes `rep movsd` into garbage memory.

### Crash signature

```
EXCEPTION_ACCESS_VIOLATION at 0x8EE41C7 (rep movsd in SWIttsWsola.dll:0x8EE4130)
Only on long texts (typically >100 synthesis units); short texts pass through by luck
No error message -- the crash happens in native code with no logging
```

To locate the offending unit with Frida, hook `process_unit` (0x8EE2960) and watch for
`start < 0` or `delta = start[N] - start[N-1] < 0`. The unit where `delta` first goes negative
is the one whose recording has a `local_pos` reversal.

### Root cause in build_mara_voice.py (confirmed 2026-03-13)

When MFA alignment succeeds, `build_mara_voice.py` uses **two different scaling methods** for units
in the same recording:

- **Speech units** (non-silence `phone_center`): per-phoneme MFA scaling -- each Tom phoneme group
  is mapped to its matched MFA time interval; `new_lp` is interpolated within that interval.
- **Silence units** (`phone_center` in `{32, 255}`): whole-recording proportional scaling --
  `new_lp = round(tom_lp * mara_n / (tom_max_end * 8))`.

These two formulas use different scale factors. If a silence unit whose Tom lp is near the
end of the recording gets a proportionally-scaled `new_lp` that is less than a preceding speech
unit's MFA-scaled `new_lp`, the monotonicity invariant is violated.

**Concrete example (recording `weather2_018`, fidx=7557, confirmed in production crash):**

| uid | Tom lp | Tom dl | Mara lp (buggy) | Mara lp (fixed) |
|-----|--------|--------|-----------------|-----------------|
| 157473 | 0 | 167 | 0 | 0 |
| 157474 | 167 | 167 | 223 | 223 |
| 157475 | 334 | 16 | **40** (wrong) | 446 (correct) |

Unit 157475 is a silence/boundary unit at Tom lp=334 (near end of recording). The MFA-scaled
speech units pushed `new_lp` to 223 for unit 157474, but the proportional scaling for unit
157475 produced `new_lp=40` -- a backwards jump of 183 units. This caused
`start_pos=-133 -> cursor=-824` in WSOLA.

### The fix

`build_mara_voice.py` now enforces monotonicity as a post-processing step on the MFA path,
after all units (speech and silence) have been computed. Units are sorted into their original
Tom lp order, then any `new_lp` that is less than the previous `new_lp` is clamped upward:

```python
_uid_tom_lp = {u[0]: u[1] for u in units}
rec_units_out.sort(key=lambda e: _uid_tom_lp.get(e[0], 0))
_prev_lp = 0
for _uid, _lp, _dl, _pc, _f0s, _f0e, _f0m in rec_units_out:
    if _lp < _prev_lp:
        _lp = _prev_lp
        _dl = max(0, min(cap - _lp, _dl))
    _prev_lp = max(_prev_lp, _lp)
    ...
```

This is STATE_VERSION 14+ in `build_mara_voice.py`.

### Additional impact: output duration (confirmed 2026-03-15)

The `local_pos` spacing determines not only crash safety but also the **audible output
duration** of each unit. WSOLA computes per-unit output duration as:

```
WsolaUnit[+0x0C] = next_unit.local_pos - this_unit.local_pos
```

This means units with the SAME `local_pos` produce 0ms output (silence). Units where
`local_pos` goes backwards produce NEGATIVE output (the unit is skipped entirely). This was
the root cause of missing phonemes in the Mara voice: the direct phone-boundary mapping
placed multiple halfphones at the same lp within narrow MFA phone intervals.

**Key insight**: DUR_WEIGHT and durt trees control unit SELECTION cost only. They do NOT
affect output duration. Output duration is determined entirely by lp spacing in the VIN.

The fix evolved through multiple iterations (STATE_VERSION 42-64):
- v42: Index-based even spacing within MFA phone intervals
- v47: Discovered STATE_VERSION cache bug (v42-46 changes were never applied)
- v48: Identified silence relocation as the real source of lp overlap
- v50-51: Clean rewrite of `process_recording()` with `_refine_mfa_interval()`
  that searches entire recordings for speech at MFA boundaries
- v54: Final RMS audit disables units still pointing to silence (LATER REMOVED -- see below)
- v55-57: Same-lp cluster fix (monotonicity `<=` not `<`), dl=0 prohibition
- v58-62: MIN_UNIT_DUR = 25ms, Tom-floor spacing, 15ms minimum in all monotonicity passes,
  dl inflation enabled for MFA units

### Same-lp cluster constraint (confirmed 2026-03-16)

Tom's unit table has **zero** same-lp clusters (consecutive units in the same recording
sharing the same `local_pos`). Mara's build produced 21,593 such clusters before the fix.

When the engine selects two consecutive units from the same recording with identical lp values:
```
WSOLA output_dur = next.lp - this.lp = 0 -> silence or crash
```

**Root cause of cluster generation**: Monotonicity enforcement used `if _lp < _prev` (strict),
allowing equal values through. Fix: change to `if _lp <= _prev` (non-strict), bumping
collisions to `_prev + 1`. Same-lp clusters drop to 0 (confirmed by scan).

### Minimum unit spacing (confirmed 2026-03-16)

MFA produces tight phone intervals (10-25ms for some short phones). Without a minimum spacing
floor, the even-spacing formula `max(1, span // n)` creates 1-5ms lp gaps between halfphones
in those intervals. These tiny gaps produce near-silence output from WSOLA.

**Minimum spacing requirements (confirmed empirically)**:

| Enforcement point | Minimum | Rationale |
|-------------------|---------|-----------|
| MFA even-spacing formula | 25 ms | Smallest perceptible output unit duration |
| Tom-floor comparison | Tom's original spacing | Prevents shrinking below what Tom used |
| All monotonicity passes | 15 ms | Global minimum throughout all passes |

The 25ms floor applies to the initial lp spacing calculation within each MFA phone interval.
The 15ms floor applies to the final global monotonicity enforcement pass (prevents cumulative
compression from multiple passes reducing spacing below perceptible duration).

### Same-recording continuation bypass (CRITICAL -- confirmed 2026-03-16)

The engine has a **same-recording continuation optimization** in the preselection step that
bypasses prsl entirely. When the Viterbi selects consecutive units from the same recording
(e.g., UIDs N and N+1), the engine automatically includes the **next sequential units in that
recording** (N+2, N+3, ...) as candidates for subsequent halfphone positions, regardless of
what prsl contains for those positions.

**Consequence**: Setting `dl=0` does NOT safely disable a unit. Any unit adjacent to a
selected unit in the same recording can be selected by the engine via the continuation path,
even if that unit was never added to prsl. A selected dl=0 unit produces zero-duration output
(silence) -- exactly the symptom that led to compressed 2.5s output instead of expected 5s.

**Confirmed by Frida** (2026-03-16): UIDs 45998/45999 (both dl=0, both absent from prsl) were
being selected because UIDs 45996/45997 were chosen first and the continuation mechanism
automatically included 45998/45999 as next-candidates.

**Design rule for custom voices**: Never set `dl=0` for any unit. Alternatives:
- For unmatched phone groups: use proportional fallback lp/dl from Tom's proportional mapping.
- For silence/boundary units: use `dl = max(1, ...)`.
- For zero-audio recordings (no VDB data): keep Tom's original lp with `dl = max(1, scaled_dl)`.
- For genuinely problematic units: set lp/dl to a valid region of nearby audio (the unit will
  produce some audio, but a suboptimal one is better than silence or a crash).

**RMS audit removed** (2026-03-16): The post-build RMS audit that set `dl=0` for quiet units
(RMS < 500) was disabling 13,154 units (8.5% of pool). This gutted prsl candidate coverage,
caused massive back-fill with phonetically poor Tom substitutes (prsl bloat from ~5MB to
~40MB), and did not prevent those units from being selected via continuation anyway. The audit
was removed entirely.

### Validation

To verify no monotonicity violations exist in a built VIN, read all unit records sorted by
`(file_idx, local_pos)` and check `local_pos[i] <= local_pos[i+1]` for all consecutive records
sharing the same `file_idx`.

---

## Unit Selection / Viterbi Search (SWIttsUSel.dll)

### DLL Overview

`SWIttsUSel.dll` (image base `0x08E80000`, .text at `0x08E81000`) implements the unit selection
algorithm. Key export: `SWIttsUSelUnitSelection` at `0x08E819E0`.

### High-level call flow from SWIttsUSelUnitSelection

(Updated 2026-03-15 with confirmed Viterbi forward-pass and backtrack analysis)

```
0x8e819e0  SWIttsUSelUnitSelection(resource, utterance, ?, out_result)
  |-- 0x8e8f280  Initialize prosody/duration target computation
  |-- 0x8e90dc0  Load VCF config params into config struct (if reload flag set)
  |-- 0x8e90da0  Prepare scoring context from config + utterance
  |-- 0x8e89a20  Initialize per-utterance scoring state (zero out struct)
  |-- 0x8e8d4a0  Candidate setup (preselection + list building)
  |   |-- 0x8e89a70  Preselect candidates (prsl lookup + context key)
  |   |-- 0x8e8a130  Build candidate list from preselection results
  |   |-- 0x8e8c700  Post-preselection processing
  |-- 0x8e8cbb0  Build halfphone candidate list with pruning thresholds
  |-- 0x8e920f0  ** PER-HALFPHONE SCORING LOOP ** (iterates all halfphone positions)
  |   |-- 0x8e91dc0  Preselection/candidate setup per halfphone
  |   |-- 0x8e88de0  Inner scorer (scores all candidates for one halfphone)
  |   |   |-- 0x8e87d90  Join cost lookup (hash or edge-frame)
  |   |   |-- 0x8e87e10  Join cost lookup (alternate path)
  |   |   |-- 0x8e81fe0  Unit data lookup by candidate ID
  |   |   |-- 0x8e88830  Histogram-based candidate pruning (beam control)
  |   |-- 0x8e91fd0  Copy candidate results to output array
  |-- 0x8e8d210  Post-scoring processing (ckls/context adjustments)
  |   |-- 0x8e8aae0  (sub-function)
  |   |-- 0x8e8ce60  (sub-function)
  |   |-- 0x8e8abe0  (sub-function)
  |-- [CONDITIONAL] ** VITERBI FORWARD PASS ** (path selection):
  |   |-- 0x8e8edd0  Mode A: WITH join cost (normal path)
  |   |   |-- For each halfphone 1..N-1:
  |   |   |     For each candidate, find best predecessor (min cum_score)
  |   |   |     Store predecessor pointer at candidate+0x24
  |   |   |-- 0x8e8ed20  Join cost distance computation (per candidate pair)
  |   |   |-- 0x8e8b580  Heapsort candidates by cum_score (at +0x20)
  |   |-- 0x8e8b620  Mode B: WITHOUT join cost (fallback/fast path)
  |   |   |-- Same structure but skips join cost computation
  |-- 0x8e8de20  ** BACKTRACK + OUTPUT ** (0xEFB bytes, the largest function)
  |   |-- Traces backward through predecessor pointers (candidate+0x24)
  |   |-- Allocates 6 output arrays (selected uid, candidate ptr per halfphone)
  |   |-- Populates output arrays at [obj+0x20] and [obj+0x28]
  |   |-- Contains "TOTAL PATH" and "score_stats" log format strings
  |   |-- 0x8e8ba10  Final output formatting (concatenation plan)
  |-- 0x8e92360  (minor processing)
  |-- 0x8e92510  Output results / score statistics assembly
  |-- 0x8e8c620  Cleanup scoring state (frees candidate arrays)
  |-- 0x8e8f790  Final cleanup (frees remaining buffers)
```

### Viterbi Forward Pass -- Detailed (confirmed 2026-03-15)

The forward pass implements a standard Viterbi dynamic programming search over the candidate
lattice. Two modes exist (selected by a condition at `0x8E81D20`):

**Mode A (0x8E8EDD0, "with join cost")** -- the normal path for Tom:

1. **Initialization** (halfphone 0): For each candidate `cand` in the first halfphone:
   - `cand+0x20 = cand+0x2C` (cumulative score = target cost alone)
   - `cand+0x24 = 0` (no predecessor)
   - `cand+0x1C = 1` (path length = 1)
   - Score components at +0x50..+0x64 zeroed (or copied from +0x3C..+0x4C)

2. **Heapsort** candidates by cum_score at +0x20 using `0x8E8B580`.
   The sort operates on an array of pointers (in `ebx[]`), comparing
   `[ptr+0x20]` values using `fcomp`. This is a standard in-place heapsort
   (halve gap, compare-and-swap, repeat).

3. **Main loop** (halfphones 1..N-1, backward jump at `0x8E8F1B9 -> 0x8E8EE93`):
   For each candidate `esi` at the current halfphone:
   - Inner loop over predecessor candidates `edi` from previous halfphone:
     - Compute join cost via `0x8E8ED20(esi, edi)` (spectral distance)
     - `total = predecessor.cum_score + join_cost + modifier`
     - If `total < best_so_far`: update best:
       - `esi+0x20 = total` (new cumulative score)
       - **`esi+0x24 = edi`** (pointer to best predecessor candidate)
       - Accumulate 6 score components (S, D, DU, SP, J, F0) at +0x50..+0x64
   - After all predecessors checked: update chain metrics at +0x7C, +0x80
   - Heapsort candidates for this halfphone

4. **Find best final candidate** (`0x8E8F1C8-0x8E8F1F5`):
   - Get last halfphone's candidate list via `[ebp+0x18][last_hp_idx]`
   - Linear scan: find candidate with minimum `+0x20` (cumulative score)
   - Store winner index in `[ebp+0x2C]`

**Mode B (0x8E8B620, "without join cost")** -- same structure but skips
join cost computation. Used when `use_joincache=0` and `use_edgeframes=0`.

### Extended Candidate Struct (Viterbi phase)

During the forward pass, the candidate struct is larger than the inner scorer's
0x18-byte stride. The inner scorer populates the first 0x18 bytes; the Viterbi
pass uses an extended struct with these additional fields:

```
Offset  Type     Name                 Notes
------  ----     ----                 -----
+0x00   u32      uid                  Unit ID (from inner scorer)
+0x04   f32      inner_score          Total score from inner scorer
+0x08   f32      dur_cost             Duration/unit bias component
+0x0C   u32      unit_id              Unit database ID (for join cost lookup)
+0x10   u32      prev_unit_id         Previous unit ID (join context)
+0x14   f32      base_cost            Base/initial cost
+0x18   u32      chain_len            Accumulated path length
+0x1C   u32      pred_count           Predecessor count (init=1)
+0x20   f32      cum_score            ** CUMULATIVE Viterbi path score **
+0x24   ptr      predecessor          ** Pointer to best prev candidate **
+0x28   ptr      name_or_context      Context/name pointer
+0x2C   f32      target_cost          Individual target cost
+0x30   f32      join_cost_component  Join cost detail
+0x34   f32      concat_cost_detail   Concatenation cost detail
+0x38   f32      join_cost_stored     Join cost (from 0x8E8ED20 result)
+0x3C   f32      comp_spectral        Spectral component
+0x40   f32      comp_dur_unit        Duration-unit component
+0x44   f32      comp_spectral_pred   Spectral-predicted component
+0x4C   f32      comp_dur             Duration component
+0x50   f32      acc_spectral         Accumulated spectral score
+0x54   f32      acc_dur_unit         Accumulated duration-unit score
+0x58   f32      acc_spectral_pred    Accumulated spectral-predicted score
+0x5C   f32      acc_dur              Accumulated duration score
+0x60   f32      acc_f0               Accumulated F0 score
+0x64   f32      acc_join             Accumulated join score
+0x68   u32      counter_a            Counter (selection frequency?)
+0x6C   u32      counter_b            Counter
+0x78   u32      chain_metric         Chain metric
+0x7C   u32      repetition_count     Repetition count (anti-repetition)
+0x80   u32      repetition_limit     Repetition limit (compared with 0x64=100, 0x0F=15)
```

### Backtrack (0x8E8DE20)

The backtrack function traces the optimal path backward through predecessor
pointers. Starting from the best final candidate (stored by the forward pass
in `[ebp+0x2C]`), it follows `candidate+0x24` backward through each halfphone:

1. Allocate output arrays for N_halfphones
2. Start at last halfphone, best candidate
3. For each halfphone (backward): store selected unit info, follow `+0x24` to predecessor
4. Populate `[obj+0x20]` (selected candidate index per halfphone) and
   `[obj+0x28]` (selected candidate pointer per halfphone)
5. Print "TOTAL PATH %d units scores (S %f D %f DU %f SP %f J %f F0 %f)"
   using accumulated components at +0x50..+0x64 divided by config weights

### Beam / Pruning Control

The ONLY beam control is the histogram-based PRUNE function (`0x8E88830`)
called at the end of each inner scoring iteration. There is NO additional
beam pruning in the Viterbi forward pass -- it exhaustively evaluates all
surviving candidates against all surviving predecessors.

The prune function:
- Builds a 40-bin histogram of candidate scores (relative to best score)
- Score bins: `bin = floor((score - best) * multiplier)`, capped at 39 (0x27)
- Computes cumulative count from bin 0 upward
- Prune threshold: when `cumulative_count > max_candidates` (from config+0x48)
- Also uses slope-based threshold: `threshold = prune_thresh - slope * cumulative_count`
- Candidates beyond threshold are discarded
- After pruning, heapsort remaining candidates
- Key config params: `HALFPHONE_CAND_MAX_UNITS` (50), `HALFPHONE_CAND_PRUNE_THRESH` (0.95 for Tom),
  `HALFPHONE_CAND_PRUNE_SLOPE` (0.005)

### Config struct layout (initialized at 0x08E90DC0)

The function at `0x08E90DC0` allocates a ~200-byte config struct (zeroed with `rep stosd ecx=50`)
and populates it from VCF parameters. The struct pointer is held in `esi` throughout.

```
Offset  Default              VCF Parameter
------  -------              -------------
+0x00   0                    SKIP_WORDS (int)
+0x04   0                    SKIP_SYLS (int)
+0x08   NULL                 STATS_LOG_FILE (string ptr)
+0x0C   0/1                  LOG_COMPONENT_SCORES (bool)
+0x10   0.1                  PHRASE_POS_MISMATCH_COST (f32)
+0x14   0.1                  STRESS_MISMATCH_COST (f32)
+0x18   (from VCF)           SYL_IN_WORD_MISMATCH_COST (f32)
+0x1C   (from VCF)           WORD_IN_PHRASE_MISMATCH_COST (f32)
+0x20   (from VCF)           PHONE_IN_SYL_MISMATCH_COST (f32)
+0x24   0.04                 ABS_F0_WEIGHT (f32)
+0x28   (from VCF)           F0_EDGE_CHANGE_WEIGHT (f32)
+0x2C   0.25                 JOIN_COST_WEIGHT (f32)
+0x30   0.20                 JOIN_COST_OFFSET (f32)
+0x34   0.04                 DUR_WEIGHT (f32)
+0x38   1.0                  UNIT_BIAS_WEIGHT (f32)
+0x3C   -1.0                 CHUNK_BIAS_WEIGHT (f32)
+0x40   NULL                 DUMP_NETWORK_FILE (string ptr)
+0x44   0.6                  CONTEXT_COST_WEIGHT (f32)
+0x48   50                   HALFPHONE_CAND_MAX_UNITS (int)
+0x4C   3.0                  HALFPHONE_CAND_PRUNE_THRESH (f32)
+0x50   0.005                HALFPHONE_CAND_PRUNE_SLOPE (f32)
+0x54   3.0                  SYL_CAND_PRUNE_THRESH (f32)
+0x58   0.005                SYL_CAND_PRUNE_SLOPE (f32)
+0x5C   3.0                  WORD_CAND_PRUNE_THRESH (f32)
+0x60   0.005                WORD_CAND_PRUNE_SLOPE (f32)
+0x64   NULL                 ACTIVE_UNIT_FILE (string ptr)
+0x68   (from VCF)           V0_JCW (f32) -- per-voicing-class join cost weight
+0x6C   (from VCF)           V0_JCO (f32) -- per-voicing-class join cost offset
+0x70   (from VCF)           V1_JCW (f32)
+0x74   (from VCF)           V1_JCO (f32)
+0x78   (from VCF)           V2_JCW (f32)
+0x7C   (from VCF)           V2_JCO (f32)
+0x80   5.0                  MISSING_F0_COST (f32)
+0x84   1000.0               MISSING_JOIN_COST (f32)
+0x88   (from VCF)           ACCENT_PHRASE_SINGLE (int)
+0x8C   (from VCF)           APPLY_ALL_F0 (int)
+0x90   (from VCF)           APPLY_ALL_F0_EDGE (int)
+0x94   (from VCF)           GET_RID_OF_PATH_F0 (int)
+0x98   0/1                  EMPH_ENABLED (bool)
+0x9C   (from VCF)           EMPH1_F0_OFFSET (f32)
+0xA0   (from VCF)           EMPH2_F0_OFFSET (f32)
+0xA4   (from VCF)           EMPH3_F0_OFFSET (f32)
+0xA8   (from VCF)           EMPH1_DUR_OFFSET (f32)
+0xAC   (from VCF)           EMPH2_DUR_OFFSET (f32)
+0xB0   (from VCF)           EMPH3_DUR_OFFSET (f32)
+0xB4   0/1                  RELOAD_USelExperimentConfig (bool)
+0xB5   0/1                  RELOAD_ACTIVE_UNITS (bool)
+0xB8   NULL                 DUMP_RANK_STATS_FILE (string ptr)
+0xBC   NULL                 DUMP_SCORE_SCATTER_FILE (string ptr)
```

Note: The VCF config for Tom overrides several defaults. For example, `JOIN_COST_WEIGHT=0.7`
(overriding default 0.25), `DUR_WEIGHT=0.3` (overriding 0.04), `ABS_F0_WEIGHT=0.2` (overriding 0.04),
`CONTEXT_COST_WEIGHT=1.0` (overriding 0.6), `HALFPHONE_CAND_PRUNE_THRESH=0.95` (overriding 3.0).

### Inner scoring loop (0x08E890A3 - 0x08E8937F)

The Viterbi inner loop iterates over all candidates for one halfphone position. For each
candidate `ebx` (index), the candidate struct is at `[esi+0x18] + ebx*0x18` (stride = 24 bytes).

Per-candidate cost computation (from the inner loop disassembly):

1. **Stress additive** (0x8E89110-0x8E8911F): Before the main cost loop, a stress bonus is added:
   `total += (float)context_cost * UNIT_BIAS_WEIGHT * 0.01`
   where `context_cost` = in-memory +0x17 = on-disk byte 28 (values 0 or 100).
   For stressed units (100): adds `100 * 0.25 * 0.01 = 0.25` to the running score.
   For unstressed (0): adds nothing.

2. **Target cost** (0x8E89121-0x8E89173): Weighted sum of 5 CART-tree-predicted costs using
   prosodic category fields at in-memory +0x0A..+0x0E (syl_type, syl_in_phrase, word_in_phrase,
   phone_position, and +0x0E). Each tree leaf yields a float cost, multiplied by the corresponding
   config weight (config+0x10..+0x20). Sum stored at candidate+0x10 when logging.

3. **Context cost** (0x8E891A8-0x8E89232, prosody mismatch): 4-component context cost from prosody
   cost tables (loaded from `ckls` data). Uses signed byte indices from the context array
   (voice_obj+0xC0, = on-disk bytes 23-26). Result weighted by `CONTEXT_COST_WEIGHT` (config+0x44).
   Stored at candidate+0x14.

4. **Duration / Unit Bias cost** (0x8E8925F-0x8E892AF): Quadratic penalty on f0_context deviation.
   The input field is **in-memory +0x12** = on-disk +0x13 = `f0_context` (for version 100006)
   or `phone_center` (for version 100007+). This value correlates strongly with log(dur_like)
   (r=0.97 for Tom). Formula:
   ```
   diff = (float)unit[+0x12] - tree_prediction
   scaled = diff * tree_variance_param
   cost = DUR_WEIGHT * |scaled|^2
   ```
   where `tree_prediction` comes from the f0tr/durt tree traversal result+0x10, and
   `tree_variance_param` from result+0x14. Stored at candidate+0x08 when logging.

5. **F0 / Chunk Bias cost** (0x8E892B5-0x8E89361): Pitch boundary matching penalty.
   The gate field is **in-memory +0x0F** = on-disk +0x10 = `f0_start` (for version 100006)
   or `f0_end` (for version 100007+).
   - If `unit[+0x0F] == 0` (unvoiced) AND a valid f0 target exists (`[esp+0x3c] != 0`):
     `cost += MISSING_F0_COST` (config+0x80 = 5.0)
   - If `unit[+0x0F] > 0`:
     ```
     diff = (float)unit[+0x0F] - f0_reference
     scaled = diff * f0_scale_param
     cost = ABS_F0_WEIGHT * |scaled|^2
     ```
     where `f0_reference` is from the target F0 model. Stored at candidate+0x0C when logging.
   - If no f0 target exists (`[esp+0x3c] == 0`): f0_cost = 0 (no penalty).

6. **Join cost** (from 0x8e87d90/0x8e87e10): Hash table lookup or edge-frame spectral distance.
   Result capped at 10000.0 (`fld [0x8e98528]`). If join cost unavailable (no hash entry),
   MISSING_JOIN_COST (1000.0) is used.

7. **Accumulation**: `total = stress_add + target + context + dur_cost + f0_cost + join_cost`
   The running best is tracked per halfphone position. If `total < current_best`, update
   best cost and best prev pointer.

**Mara vs Tom concat cost analysis**: For Tom/Mara (version 100006), the "concat(bias)" cost
reported by Frida component logging is the sum of items 4 (unit bias, at +0x08) and 5 (chunk bias,
at +0x0C). The unit bias field (on-disk byte 19 = f0_context) is **identical** between Tom and Mara
(byte 19 is never modified by build_mara_voice.py). The chunk bias field (on-disk byte 16 = f0_start)
**differs 93%** between Tom and Mara. Key differences:
- Tom f0_start: range 0-150 Hz (direct Hz), nonzero range 99-150, p25=114, p50=118, p75=121 (tight cluster); 44,204 zeros (26%)
- Mara f0_start: range 0-255, p25=104, p50=118, p75=132 (wider spread); 26,223 zeros (15%)
- 7.4% of Mara's non-zero f0_start values exceed Tom's max (150 Hz)
- The quadratic cost function amplifies outliers: values far from the tree-predicted reference
  incur disproportionately high costs, explaining Mara's higher concat mean (~0.33 vs Tom's ~0.18)
- Improvement opportunity: clamp Mara f0 values to [99, 150] after scaling to match Tom's range,
  or use a formula that maps Mara's pitch distribution to exactly [99, 150] (e.g. linear map
  from Mara's [80, 350] Hz to Tom's [99, 150] Hz).

6. **Pruning**: After all candidates scored, the pruning function at `0x8e88830` is called with:
   - `HALFPHONE_CAND_MAX_UNITS` (config+0x48, default 50)
   - `HALFPHONE_CAND_PRUNE_THRESH` (config+0x4C)
   - `HALFPHONE_CAND_PRUNE_SLOPE` (config+0x50)
   - Best score from the current halfphone (tracked at `[esp+0x30]` in the caller)

### Pruning algorithm (0x08E88830, confirmed by disassembly)

The prune function is `__thiscall` with signature:
```
void CandList::prune(int max_units, float prune_thresh, float prune_slope, int max_candidates)
```
(`ret 0x10` = 4 stack args, `this` in ecx)

**Phase 1 -- Histogram build** (0x88830-0x8889B):
For each of the N candidates, read score from `[this+0x18][i*0x18 + 4]` (the +4 field in
each 24-byte candidate entry). Compute `bin = ftoi((score - best_score) * 40.0)`, clamped
to max 39 (0x27). Increment a 40-bin histogram on the stack. Sub-function `0x8E9504C` is
the float-to-int helper.

**Phase 2 -- Find cutoff bin** (0x8889B-0x88B9C, unrolled 10x):
Walk histogram bins from low to high, accumulating candidate count `cum`. At each bin `k`:
- `bin_value = (k - 1) * 0.025`
- `cutoff = prune_thresh - cum * prune_slope`
- If `bin_value >= cutoff`, stop (candidates past this bin exceed the adaptive threshold)

The loop body is unrolled 10 bins per iteration (esi increments by 10 each pass, compared
against 0x2A = 42), with early-exit jumps for each bin that update `edx` (the stopped-bin
index) and break out.

**Phase 3 -- Compute score threshold** (0x88B9C-0x88BAA):
`threshold = best_score + stopped_bin * 0.025`
(the `fadd [esp+0xF0]` adds back the original best_score; the bin index was already scaled
by 0.025 during accumulation)

**Phase 4 -- Count survivors** (0x88BB0-0x88BD7):
Scan candidates linearly; count those with `score <= threshold` (using `fcomp` + `fnstsw`).

**Phase 5 -- Compact** (0x88BD7-0x88C58):
Copy surviving candidates (score <= threshold) into the front of the array, compacting gaps.
Each entry is 0x18 bytes; full struct is copied (unit_id + score + 3 optional debug floats).
A global debug flag at `0x8E9D564` controls whether the 3 extra floats (+0x08, +0x0C, +0x10)
are copied; they are only populated when `LOG_COMPONENT_SCORES` is enabled.

**Phase 6 -- Shell sort** (0x88C58-0x88DB5):
Shell sort with gap = count/2. Compares by score (+4 field); tiebreaks by unit_id (+0 field,
lower ID wins). Swaps full 0x18-byte entries.

**Phase 7 -- Cap** (0x88DB5-0x88DC5):
If surviving count > `max_candidates` (arg4, = `HALFPHONE_CAND_MAX_UNITS`), truncate to
that limit: `[this+0] = max_candidates`.

**Summary formula**: the effective prune threshold is NOT simply
`best_score + THRESH + SLOPE * N`. Instead it is histogram-based:
```
for k = 0..39:
    cum += histogram[k]
    bin_value = (k-1) * 0.025
    if bin_value >= PRUNE_THRESH - cum * PRUNE_SLOPE:
        break
threshold = best_score + k * 0.025
```
This is an adaptive cutoff that accounts for the distribution of candidate scores, not
just the raw count.

### Candidate struct layout (stride = 0x18 = 24 bytes)

```
+0x00  u32  unit_id (candidate unit index into unit table)
+0x04  f32  total_score (the weighted sum of all cost components below)
+0x08  f32  unit_bias / dur_cost (DUR_WEIGHT * |scaled_diff|^2; only if LOG_COMPONENT_SCORES)
+0x0C  f32  chunk_bias / f0_cost (ABS_F0_WEIGHT * |scaled_diff|^2 or MISSING_F0_COST; only if LOG)
+0x10  f32  target_cost (sum of 5 tree-predicted prosodic costs; only if LOG_COMPONENT_SCORES)
+0x14  f32  weighted_context (context_cost * CONTEXT_COST_WEIGHT; only if LOG_COMPONENT_SCORES)
```

Note: The +0x08/+0x0C/+0x10/+0x14 fields are only populated when the debug flag at
`0x8E9D564` (controlled by `LOG_COMPONENT_SCORES` in VCF) is set. The prune function
copies them during compaction only when this flag is set. The +0x04 `total_score` is
always populated and is the sole field used for pruning decisions.

### Path evaluation struct (stride ~0x50, offsets observed in best-path comparison)

The larger scoring/path structs use these offsets for total cost accumulation:

```
+0x3C  f32  base_cost (context or join related)
+0x40  f32  component_1 (duration?)
+0x44  f32  component_2 (F0/pitch?)
+0x48  f32  component_3 (spectral/join?)
+0x4C  f32  component_4 (other)
total_path_cost = [+0x3C] + [+0x40] + [+0x44] + [+0x48] + [+0x4C]
```

This matches the `score_stats` debug format string:
`"score_stats %d %d %s (S %5.3f D %5.3f DU %5.3f SP %5.3f J %5.3f F0 %5.3f)"`
Where S=total, D=dur, DU=dur_unit?, SP=spectral?, J=join, F0=pitch.

And the total path format:
`"TOTAL PATH %d units scores (S %f D %f DU %f SP %f J %f F0 %f)"`

### Histogram binning (used in pruning)

The pruning function builds a 40-bin histogram of candidate score deltas:
- `bin = ftoi((score - best_score) * 40.0)`, clamped to max 39 (0x27)
- Bin width: 0.025 (constant at `0x8e98520`)
- Scale factor: 40.0 (constant at `0x8e98524`)
- The histogram is walked low-to-high with an adaptive cutoff (see Pruning algorithm above)

Note: separate from join cost normalization, which also uses histogram binning
with 50 bins (`cmp eax, 0x32`) and a bin multiplier of 10.0 (`0x8e984e4`).

### Float constants referenced in .text

```
Address     Value      Usage
0x8e96b9c   0.333      1/3 multiplier (join cost normalization)
0x8e96bc8   0.500      1/2 multiplier (join cost normalization / F0 averaging)
0x8e971d8   0.100      Default mismatch cost
0x8e984e0   0.008      Small threshold
0x8e984e4   10.0       Histogram bin multiplier
0x8e98520   0.025      Histogram bin width
0x8e98524   40.0       Histogram range
0x8e98528   10000.0    Join cost cap
0x8e9852c   0.0        Zero (float compare reference)
0x8e9857c   1.0        Unity (float normalization)
0x8e98580   0.01       Small threshold
0x8e98a24   50.0       Max candidate float
0x8e98ba0   1.0 (f64)  Double-precision 1.0
0x8e99218   50.0 (f64) Double-precision 50.0
0x8e99220   1000.0     Missing join cost (f32)
0x8e99228   5.0 (f64)  Missing F0 cost
0x8e99230   0.02 (f64) Small threshold
0x8e99788   0.00001    Epsilon
0x8e99d08   0.5 (f64)  Double-precision 0.5
0x8e99d10   1000.0(f64) Double-precision 1000
```

### Debug strings in SWIttsUSel.dll

Key diagnostic format strings (useful for understanding scoring components):

```
"durcomp target_index %d syl_type %d syl_context %d phones (%d %d %d) phone_count %d phone_in_syl %d node_index %d"
"ABS_F0 %d %f -> %f diff %f scaled %f tot %f"
"ABS_F0 %d %f -> 0 MISSING"
"CONT %d of %d = %f WORD %d of %d = %f syl %d of %d = %f joins %d %d %d %d"
"score_stats %d %d %s (S %5.3f D %5.3f DU %5.3f SP %5.3f J %5.3f F0 %5.3f)"
"RAW TOTAL PATH %d units scores (S %f D %f DU %f SP %f J %f F0 %f)"
"TOTAL PATH %d units scores (S %f D %f DU %f SP %f J %f F0 %f)"
"Preselection cache not loaded...."
```

Score component abbreviations: S=total_Score, D=Duration, DU=Duration_Unit(?),
SP=SPectral(join), J=Join_cost, F0=pitch.

---

## WSOLA Concat Unit List Format (confirmed 2026-03-15)

The `SWIttsWsolaConcat` function at `0x8EE65E0` (cdecl) receives the final Viterbi-selected
unit path as input. The unit list is at arg4 (esp+16):

- `[arg4+0x04]` = `u32 unit_count` (matches halfphone count)
- `[arg4+0x08]` = pointer to unit array, stride `0x18` (24 bytes)

Entry format within the unit array:

| Offset | Type | Field | Notes |
|--------|------|-------|-------|
| `+0x00` | `u32` | `uid` | Unit ID (index into unit table) |
| `+0x04` | `u32` | `group_count` | 0 = continuation of prev group; N>0 = start of N-unit same-recording run |
| `+0x08` | `i32` | `wsola_param_1` | Appears to be pitch shift or time offset (signed) |
| `+0x0C` | `i32` | `wsola_param_2` | Second WSOLA parameter |

The `group_count` field reveals that the engine passes units to WSOLA in same-recording groups.
Within a group, WSOLA processes units as contiguous source audio from one recording. Cross-group
boundaries are where concatenation artifacts occur.

### Diagnostic note: diag_stutter.py captures WRONG units

`diag_stutter.py` hooks the prune function (`0x8E88830`) and reports the pre-prune best candidate
(lowest `total_score` at `candidate+0x04`). This is NOT the Viterbi-selected unit. The Viterbi
forward pass (`0x8E8EDD0`) recomputes costs from component fields + join cost hash lookups and
does NOT read `total_score`. Only 31-37% of pre-prune best UIDs match the actual Viterbi path.

To capture the true path, hook `SWIttsWsolaConcat` and read the unit list from arg4. See
`c:/tmp/diag_ground_truth.py` for implementation.

### Viterbi NoJoin inner loop (disassembled 2026-03-17)

The active Viterbi for Mara is at `0x8E8B620` (NoJoin variant, since hash misses dominate).

**Candidate struct fields used by Viterbi:**
| Offset | Type | Field | Description |
|--------|------|-------|-------------|
| `+0x0C` | `u32` | `uid` | Used as hash row index (`rows[uid]`) and adjacency check |
| `+0x10` | `u32` | `uid_alt` | Used as hash cell index (predecessor lookup) |
| `+0x20` | `f32` | `cum_score` | Accumulated path cost (Viterbi state) |
| `+0x24` | `ptr` | `predecessor` | Pointer to best predecessor candidate |
| `+0x2C` | `f32` | `initial_cost` | Target cost from scorer (copied to +0x20 in init loop) |

**Adjacency check** at `0x8E8B854`:
```asm
cmp ebx, eax       ; ebx = candidate.uid, eax = predecessor.uid_alt + 1
jne 0x8E8B862      ; if not adjacent -> normal path (apply join cost)
; Adjacent -> FREE transition: join=0, context=0
```
This is the ONLY mechanism for same-recording preference in the Viterbi. It checks
`candidate.uid == predecessor.uid + 1`, which is true for consecutive halfphones within
the same recording (UIDs are sequential per recording in the unit table).

**Hookable for recording-switch penalty:** The 7-byte `cmp/jne` at 0x8E8B854 can be
replaced with `jmp cave` (5 bytes + 2 nops). The cave adds a penalty to the FPU stack
when file_idx differs between candidate and predecessor. See Exp 56.

### Candidate pruning pipeline (confirmed 2026-03-17)

The scoring-to-Viterbi pipeline has a critical pruning step:

1. PRSL returns ~50-200 raw candidates per halfphone position
2. InnerScorer (`0x8E88DE0`) computes target cost for each candidate
3. Prune (`0x8E88830`) removes candidates with total_score >= `HALFPHONE_CAND_PRUNE_THRESH`
   (VCF parameter, Tom default = 0.95, effective with Mara at 0.8)
4. Post-prune survivors (~5-14 per position) are copied to HP candidate objects
5. Viterbi reads only from HP candidate objects (`[hp+0x34]` pointer array)

**Prune threshold is the recording-switch bottleneck:** With prune=0.8, only ~14 candidates
survive per position. Among these, very few share recordings at adjacent positions (3.3%
of Viterbi transitions are same-rec). Raising prune to 3.0 lets more candidates through,
increasing same-rec transitions to 0.7% of a much larger pool (320K transitions) and
reducing switches from 32 to 29. But prune=10.0 (effectively disabled) degrades audio
quality by letting phonetically wrong candidates dominate.

---

## VDB Recording Structure (confirmed 2026-03-15)

Tom's VDB recordings are **pre-cut fragments**, not complete utterances. SpeechWorks cut at the
source before storing in the VDB. Example: recording `dip5_009` contains audio for phones
`pau pau pau p l iy z er r ay t` (19 units, UIDs 6877-6895), but the `ckls` WORD record only
labels `"right"` (UIDs 6890-6895, phones r ay t). The `"please"` portion (p l iy z, UIDs
6880-6887) and `"er"` (UIDs 6888-6889) are unlabeled leftover content from the original
recording session.

This has implications for building new voices: Mara's Qwen re-synths were based on transcribing
what was heard from the fragments, matching the LABELED content (not the full fragment). This
creates content mismatches for unlabeled phone groups -- those units have no corresponding audio
in Mara's recordings and should be disabled (`dl=0`).

---

## Open Questions

- Why final filename record in each ckls group omits `file_id` (sentinel vs. parser convenience).
- How `ckls.file_id` maps to other chunks (`unit`, `prsl`, `hash`) beyond simple sequence.
- The `+40` gap between some consecutive local_pos values (representing silence/padding regions between recording utterances) -- exact semantics unconfirmed.
- Full semantic decode of `f0tr/durt tree` node fields -- CONFIRMED (2026-03-13). Variable-size nodes (branch=16 bytes, leaf=20 bytes), not fixed 18. See tree section above for complete format.
- `prsl.context_key` exact semantics: CONFIRMED -- trigram formula `left_hp*10000 + center_hp*100 + right_hp`. See prsl section for full encoding details. Remaining sub-question: what exact sort order the engine uses when building its internal phone-info table (explains the 5 hp_base anomalies). For prsl rebuild, use the empirical hp_base table directly.
- Exact join cost distance formula (two weighted LPC/autocorrelation-style components; weights 0.5 and 0.333 are normalization multipliers, not the `joinweights[0/1]` values loaded at runtime from config).
- Which `(uid_left, uid_right)` pairs were included in the hash and why (threshold, K-NN, or exhaustive).
- WSOLA unit list `+0x08` and `+0x0C` fields: signed integers, likely pitch shift and time stretch parameters. Exact semantics TBD.
- Exact mechanism of the same-recording continuation bypass in the preselection step: which
  function implements it, which data structure stores the "previous winner's recording", and
  how many forward steps the engine looks ahead. Knowing this would allow us to explicitly
  disable problematic units at the preselection level rather than relying on lp/dl conventions.
- Whether the engine also has a "same-recording penalty" that discourages switching recordings
  mid-word (the complement of the continuation reward). If so, tuning this via DLL or VCF
  would be an alternative to hash cost manipulation.
- **ANSWERED (2026-03-17):** The engine has NO built-in recording-switch penalty. The ONLY
  same-recording preference is the uid adjacency check at `0x8E8B854` (free transition for
  uid == prev_uid + 1). A Frida code cave at this address can add a penalty (Exp 56), but
  the prune threshold (Exp 59) has more impact than penalty magnitude.
