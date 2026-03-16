# SpeechWorks Speechify Voice Format: Reverse Engineering Summary

> **Status:** Actively reversed. Most structures are confirmed; a handful of fields remain open questions.
> This document is kept in sync with `README.md` (the technical living document). `README.md` has the raw
> binary-level detail; this file has the big-picture explanation in plain English.

---

## Background

**SpeechWorks International** (acquired by Nuance in 2003, later folded into Microsoft) made a unit-selection
text-to-speech engine called Speechify, used widely in IVR phone systems, GPS navigation units, and — most
notably for our purposes — NOAA Weather Radio during the CRS era. The voice we're working with is "Tom",
version 3.0.0.0alpha, dated May 2003.

Unit-selection TTS works by recording a human speaker reading hundreds of carefully designed prompts, then
slicing those recordings into phoneme-sized segments ("units") and storing them in a database. At synthesis
time, the engine picks the best-matching sequence of units from the database and concatenates them, smoothing
over the joins as best it can. The result sounds like the original speaker, because it *is* the original
speaker — just cut up and reassembled.

**No public documentation of any SpeechWorks file format has ever existed.** This project is the first
reverse engineering effort to fully document them.

---

## The Three Files

Every SpeechWorks voice ships as a triplet:

| File | Full Name | Role |
|------|-----------|------|
| `tom.vin` | Voice INdex | The synthesis brain — unit catalog, decision trees, cost tables |
| `tom8.vdb` | Voice DataBase | The raw recorded audio |
| `tom.vcf` | Voice ConFiguration | Runtime tuning parameters and cost weights |

The `8` in `tom8.vdb` means 8 kHz. A 16 kHz version would be `tom16.vdb`. The engine figures out which
file to load from a template in the VCF.

---

## Encryption

All three files are encrypted, but each uses a *different* scheme.

### VIN and VDB: XOR with 0xCE

Simple byte-for-byte XOR. Every byte in the file is XOR'd with the constant `0xCE`. Since XOR is symmetric,
the same operation decrypts. This was confirmed by disassembling `SWIttsEngineUtil.dll`, which contains an
explicit loop: `xor byte ptr [eax + ebx], 0xce`.

After decryption, both files are standard **RIFF containers** (the same container format used by WAV files).

### VCF: Nibble-Expansion Cipher

More elaborate. Each plaintext byte is split into its upper 4 bits and lower 4 bits (two nibbles), and each
nibble is encoded as a separate byte using a substitution table. This doubles the file size: `tom.vcf` is
46,650 bytes encrypted, 23,325 bytes decrypted.

The substitution table:

| Encrypted | Nibble | Encrypted | Nibble |
|:---------:|:------:|:---------:|:------:|
| `0xDD` | 0 | `0xDA` | 7 |
| `0xDC` | 1 | `0xD5` | 8 |
| `0xDF` | 2 | `0xD4` | 9 |
| `0xDE` | 3 | `0xAC` | A |
| `0xD9` | 4 | `0xAF` | B |
| `0xD8` | 5 | `0xAE` | C |
| `0xDB` | 6 | `0xA9` | D |
|         |   | `0xA8` | E |
|         |   | `0xAB` | F |

To decrypt: read pairs of bytes, look each up in the table to get one nibble, combine as
`(high_nibble << 4) | low_nibble`.

---

## VDB — Voice DataBase (`tom8.vdb`)

The VDB is the simplest of the three files. After decryption it's a standard RIFF/WAVE file containing
the raw audio for every recording session.

### Chunks

| Chunk | Size | Purpose |
|-------|------|---------|
| `LIST INFO` | 100 B | Copyright notice and creation date |
| `fmt ` | 16 B | Audio format descriptor |
| `indx` | ~133 KB | Recording index — maps names to byte offsets in `data` |
| `data` | ~59.4 MB | Concatenated audio for all recording sessions |

### Audio Format

The audio data is **G.711 μ-law, 8 kHz, mono — 1 byte per sample**.

The `fmt ` chunk header is deliberately misleading: it declares `AudioFormat=7` (mu-law), `BitsPerSample=16`,
`BlockAlign=2`, `ByteRate=16000`. The `BitsPerSample=16` field is wrong for the actual data (which is 8-bit
μ-law), but it's what the SpeechWorks engine reads. The engine uses `BitsPerSample` from `fmt` when computing
audio byte offsets:

```
byte_offset = local_pos × 4 × (BitsPerSample / 8)
            = local_pos × 4 × 2
            = local_pos × 8
```

This means **1 `local_pos` unit = 8 bytes of audio = 8 μ-law samples = 1 ms**. Building a VDB with raw
16-bit PCM data instead of μ-law-encoded data produces audible corruption because the engine always decodes
the data as μ-law regardless of the fmt header.

The engine exports `SWIttsAudioCvtUlawToL16` to convert μ-law to 16-bit PCM at playback time.

### Recording Index (`indx`)

The index maps human-readable recording names (like `news3_047` or `weather7_082`) to byte offsets in the
`data` chunk. Its structure is a simple variable-length array:

```
u32             count                 (8139 total, including sentinel)
repeated count times:
    u32         byte_offset           Offset into data chunk
    u16         name_length
    char[]      name                  Recording name (empty string for sentinel)
```

The last entry is a sentinel with an empty name; its offset equals the total size of the `data` chunk.
The duration of recording `i` is `offset[i+1] - offset[i]`.

**Tom's audio inventory:**

| Metric | Value |
|--------|-------|
| Total recordings | 8,138 (+ 1 sentinel) |
| Recordings with actual audio | 6,849 |
| Zero-length entries | 1,289 (silence boundaries/padding) |
| Total audio | ~61.8 minutes |
| Duration range | 24.5 ms – 3,031.5 ms |
| Mean duration | ~542 ms |
| Recording categories | `number`, `letter`, `date`, `news`, `weather`, `driving`, `dip`, email, addresses, city names, and more |

---

## VIN — Voice INdex (`tom.vin`)

The VIN is where all the intelligence lives. After decryption it's a RIFF file with form type `svin`
containing 14 chunks. These fall into a few functional groups:

### Chunk Summary

| Chunk | Size | File(s) | Category | Status |
|-------|------|---------|----------|--------|
| `LIST` INFO | 100 B | VIN | Metadata | Confirmed |
| `vers` | 14 B | VIN | Metadata | Confirmed |
| `cnts` | 12 B | VIN | Metadata | Confirmed |
| `feat` | ~131 KB | VIN | Schema | Confirmed |
| `mean` | ~2.9 KB | VIN | Acoustic model | Confirmed |
| `hist` | 424 B | VIN | Acoustic model | Confirmed |
| `unit` | ~4.7 MB | VIN + VDB | Core unit catalog | Confirmed |
| `ckls` | ~385 KB | VIN | Prompt annotations | Confirmed (partial semantics) |
| `cklx` | ~79 KB | VIN | Checklist index | Confirmed |
| `f0tr` | ~2.4 KB | VIN | Prosody model | **CONFIRMED** -- 1 CART tree (109 nodes), variable-size nodes |
| `durt` | ~31 KB | VIN | Prosody model | **CONFIRMED** -- 47 CART trees (1 per phone), variable-size nodes |
| `ccos` | ~1.6 MB | VIN | Join cost | Confirmed |
| `prsl` | ~4.7 MB | VIN | Candidate selection | Confirmed |
| `hash` | ~21.1 MB | VIN | Join cost cache | Confirmed |

---

### Metadata Chunks

#### `LIST` / INFO

Standard RIFF INFO block, identical in structure to WAV file metadata:
- `ICOP`: `"Copyright 2003 SpeechWorks International, Inc. All Rights Reserved."`
- `ICRD`: `"2003-05-12"`

#### `vers`

A simple length-prefixed version string: `u16 length` + `char[]`. Tom's value: `"3.0.0.0alpha"`.

#### `cnts`

Three 32-bit integers summarizing the database:

| Value | Meaning |
|-------|---------|
| 92 | Number of phone variants (46 phones × 2 positions) |
| 16 | Number of feature keys in `feat` |
| 169,579 | Total unit records in `unit` chunk |

---

### Schema Chunks

#### `feat` — Feature Registry

A sequence of 16 named feature definitions. Each feature has a name and either a list of enumerated values
(categorical) or zero values (continuous — actual values are computed at synthesis time).

**Key features:**

| Feature | Type | Values / Notes |
|---------|------|----------------|
| `name` | categorical | 92 phone variants: `aa1, aa2, ae1, ae2, ... zh1, zh2` |
| `duration` | continuous | Segment duration in frames |
| `pitch` | continuous | F0 in Hz |
| `voice` | continuous | Voicing probability |
| `power` | continuous | Signal energy |
| `dur_z`, `pitch_z`, `voice_z`, `power_z` | continuous | Z-score normalized versions of the above |
| `Syllable.stress` | categorical | `0, 1, 2` (unstressed, stressed, primary) |
| `lisp_mod_tobi_accent` | categorical | `!H*, H*, L*, L*+H, L+!H*, L+H*, NONE, OTHER, 0` |
| `lisp_mod_tobi_endtone` | categorical | `H-, H-H%, L-, L-H%, L-L%, NONE, OTHER, 0` |
| `filename` | categorical | 8,118 utterance names (`date_001` through `weather7_082`) |

The phone variants `aa1`/`aa2` (and so on) represent the two halves of each diphone: the first half
(`1`) covers the left boundary (onset) of the phone, and the second half (`2`) covers the right boundary
(coda). Every phoneme instance in Tom's recordings is split into exactly two unit records.

---

### Acoustic Model Chunks

#### `mean` — Per-Phone Feature Means

A 92 × 8 float32 matrix: one row per phone variant, eight acoustic feature columns.

```
u32  n_phones    // 92
u32  n_features  // 8
f32[92][8]       // row-major matrix
```

Column order: `dur_mean, dur_std, pitch_mean, pitch_std, voice_mean, voice_std, power_mean, power_std`.

The odd columns are **per-halfphone standard deviations**, not global z-scores. Each (mean, std) pair
gives the engine the distribution of that feature across all units of that halfphone, used to compute
how "unusual" a candidate unit's feature value is compared to others of the same phone.

Sample values for Tom (male speaker, ~120 Hz fundamental):

| Phone | dur_mean (ms) | dur_std | pitch_mean (Hz) | pitch_std | voice_mean | voice_std | power_mean | power_std |
|-------|--------------|---------|-----------------|-----------|------------|-----------|------------|-----------|
| aa1 | 53.1 | 17.9 | 123.8 | 22.1 | 0.989 | 0.048 | 6.23 | 0.17 |
| ae1 | 58.2 | 18.4 | 123.0 | 21.8 | 0.989 | 0.050 | 6.18 | 0.18 |
| b1  | 36.8 | 12.1 | 83.3  | 18.4 | 0.749 | 0.210 | 4.64 | 0.31 |

Note: `power_mean` is in the scale of `ln(mean_abs_PCM16)`, typically 4–6 for Tom's recordings.
A new voice with different recording levels will have a different absolute power scale, which is
fine as long as it is internally consistent within the voice.

#### `hist` — Z-Score Histogram

100 float32 bins representing the negative log-probability of each Z-score bucket, used to compute how
"unusual" an acoustic feature value is for a given phone. The histogram covers Z-scores from -50 to +50
in steps of 1.0. Values near the distribution center are near 0.0 (common = low target cost); values at the
extremes are ~10.96 (rare = high target cost). Shape is an inverted bell curve, though empirically noisy
at the tails. Minimum is at bin 49 (Z = -1 by the bin formula), not bin 50 (Z=0), reflecting Tom's corpus
having a slight leftward skew in Z-scores.

**Head field layout (8 bytes):** `u32 n_bins=100` + `i32 range_start=-50`.
Raw bytes: `64 00 00 00 CE FF FF FF` (100 LE, -50 as signed i32 LE).
Bin index: `bin = clamp(floor(z_score + 50), 0, 99)`.
Single shared histogram used for all 8 continuous features (duration, pitch, voicing, power Z-scores).

For new voices: keep Tom's histogram unchanged. The `mean` chunk handles per-phone per-halfphone variation;
the histogram is just a global Z-score prior.

---

### Core Unit Catalog

#### `unit` — The Unit Database

This is the heart of the VIN: 169,579 records at 29 bytes each, one for every half-phoneme unit in the
voice database. A "unit" is one half of one phoneme instance in one specific recording.

**Record layout (29 bytes per unit):**

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| +0x00 | u32 | `unit_id` | Sequential 0..169,578 |
| +0x04 | u16 | `file_idx` | Index into `indx`/`feat[filename]` — which recording this unit is from |
| +0x06 | u16 | `local_pos` | Position within that recording; **unit = 8 μ-law bytes at 8 kHz (= 1 ms)**. `byte_offset = local_pos × 8`. |
| +0x08 | u16 | *(zero)* | Always 0 |
| +0x0A | u16 | `dur_like` | Duration in the same unit as `local_pos` (8 bytes each). `segment_bytes = dur_like × 8`. `next.local_pos − local_pos ≈ dur_like` (92.7%) or `dur_like + 40` (gap region). |
| +0x0C | u8 | `syl_type` | Syllable prosodic type: 1=UNDEF, 3=Stressed, 4=PA, 5=FirstPA, 6=FirstPAInPhrase, 7=LastPAInPhrase, 8=LastPAInSent |
| +0x0D | u8 | `syl_in_phrase` | Syllable position in phrase: 1=UNDEF, 2=PhrInitial, 3=PhrMedial, 4=PhrFinal, 5=PhrSingle, 6=SentFinal, 7=WordInit |
| +0x0E | u8 | `word_in_phrase` | Word/phone positional category (5 values: 1–5) |
| +0x0F | u8 | `phone_position` | Phone positional category (5 values); gates F0 processing in engine |
| +0x10 | u8 | `f0_start` | F0 at start boundary in **raw Hz** (integer). 0 = unvoiced/silence. Nonzero range 99-150 for Tom; median 118 Hz. |
| +0x11 | u8 | `f0_end` | F0 at end boundary in raw Hz. Same encoding; nonzero range 99-156 for Tom. |
| +0x12 | u8 | `f0_mid` | F0 at midpoint in raw Hz. Same encoding; nonzero values strictly >= 99 (1-98 do not appear in Tom). |
| +0x13 | u8 | `f0_context` | Per-instance target F0 context (never 0; overwritten at load time with ccos label index) |
| +0x14 | u8 | `phone_center` | Phone identity: 0..45 (indexes into 46-label ARPABET inventory) |
| +0x15 | u8 | `is_first_half` | 1 = left boundary half; 0 = right boundary half |
| +0x16 | u8 | *(constant 3)* | Always 3; purpose unknown (version tag or padding) |
| +0x17..+0x1A | u8[4] | `phone_ctx[4]` | Neighboring phone context: values 0..45 + 255 (sentinel = none) |
| +0x1B | u8 | `flag_b` | 1 = valid utterance-internal unit (89%); 0 = recording boundary or context unavailable |
| +0x1C | u8 | `context_cost` | {0, 100}; 100 = "forbidden/unknown" prosody tier (36% of units) |

**How units relate to recordings:** `file_idx` indexes into the `feat.filename` list inside the VIN, giving the recording name. The engine then does a **name-based lookup** into the `indx` table in the VDB — it does NOT use `file_idx` as a direct positional index into `indx`.

> **CRITICAL (confirmed 2026-03-11):** The `indx` ordering in `tom8.vdb` does NOT match the `feat.filename` ordering in `tom.vin`. Of 8,118 entries, **7,444 are in different positions**. Anyone building a custom voice must look up recording sizes by **name** (via `filenames[file_idx]`), not by VDB position. Using positional indexing produces silently wrong cap values and causes the engine to crash with "File end is beyond the speech DB end" for most units.

The 6,849 non-zero-size recordings in the VDB match the 6,849 distinct `file_idx` values in **count** only.

**The half-phoneme split:** Every phoneme instance becomes exactly two units with the same `file_idx`
and adjacent `local_pos`. The `is_first_half` flag distinguishes which is which. This is a standard
technique in unit-selection TTS: joining always happens at phoneme midpoints, never at boundaries,
which acoustically is a much smoother concatenation point.

**F0 encoding:** F0 (pitch) values at +0x10–0x12 are stored as quantized uint8 values roughly
proportional to Hz (male voice mean ~118 Hz). Zero means the unit is unvoiced at that boundary.
The `f0_context` field at +0x13 is written back at load time by the unit-selector engine with the
ccos label index, so the value stored in the VIN file is just a loading artifact.

---

### Prompt Annotation Chunks

These two chunks encode what *words* and *syllables* appear in each recording, so the engine can look
up which recordings contain the phonetic content it needs.

#### `ckls` — Checklist

Stores token occurrence streams for two linguistic levels: words (`_WORD_`) and syllables (`_SYL_`).
Each stream is an alternating sequence of token records and filename records:

```
u32 group_count = 2

Group "_WORD_":  5,108 token records, 5,108 filename records
Group "_SYL_":   7,918 token records, 7,918 filename records

Token record:     u16 text_len + char[] text + u32 span_start + u32 span_end
Filename record:  u16 text_len + char[] filename + u32 file_id (omitted on last)
```

`span_start` and `span_end` are **global unit indices** into the `unit` table (0..169,578).
Confirmed exhaustively (100% of word and syllable records cross-validate):

- `span_start` always points to the **first-half unit of the first phoneme** of the token (`is_first_half=1`)
- `span_end` always points to the **second-half unit of the last phoneme** of the token (`is_first_half=0`)
- The inclusive range `[span_start, span_end]` covers exactly `2 × n_phones` unit records, all in the same utterance
- Delta formula: `span_end − span_start = 2 × n_phones − 1` (always odd)

For example, the word "thursday" (5 phones: th-er-z-d-ey) has span [9, 18], delta=9=2×5−1, covering 10 units.

#### `cklx` — Checklist Index

The reverse lookup table: given a word or syllable string, find all its occurrence IDs in `ckls`.

```
u32 group_count = 2

Group "_WORD_":  1,075 unique words   → 5,108 total postings
Group "_SYL_":   1,350 unique syllables → 7,918 total postings
```

Every posting ID in `cklx` is a valid index into the corresponding `ckls` token stream. The two
chunks are perfectly cross-consistent.

---

### Prosody Model Chunks (CONFIRMED 2026-03-13)

These chunks contain CART (Classification and Regression Tree) models for predicting F0 (pitch) and
duration for any phone in any context. The engine uses them to set synthesis targets, and candidate
units are scored by how well they match these predictions.

Both `f0tr` and `durt` share a **47-label phone inventory** (the 46 ARPABET phones + empty sentinel):
`aa, ae, ah, ao, aw, ax, ay, b, ch, dx, d, dh, eh, el, er, en, ey, f, g, hh, ih, ix, iy, jh, k, l, m, n, ng, ow, oy, p, pau, r, s, sh, t, th, uh, uw, v, w, xx, y, z, zh`

#### `f0tr` -- F0 (Pitch) Tree

One tree covering all phones. Contains:
- `trhd` sub-chunk: phone labels (`labl`) + 22 decision questions (`ques`, types {1,2,8})
- `tree` sub-chunk: 109 nodes (54 branch + 55 leaf), 1968 bytes

The tree predicts F0 in Hz for any prosodic context. All 55 leaf nodes predict values in
106.75-126.62 Hz (mean ~117 Hz) -- Tom's male pitch range. The tree only uses syl_type,
syl_in_phrase, and phone_in_syl features (no phone identity questions -- those are handled
by using a separate durt tree per phone).

#### `durt` -- Duration Tree

47 individual trees (one per phone label, indexed by phone label index 0..46). Contains:
- `trhd` sub-chunk: phone labels + 154 decision questions (types {1,2,3,4,5,8})
- 47 x `tree` sub-chunks (from 24 bytes / 1 node for `zh` up to 2328 bytes / 129 nodes for `t`)

Total: 778 branch nodes + 825 leaf nodes across all 47 trees.
Duration predictions range from ~57 to ~193 in local_pos units (1 unit = 0.5 ms at 8 kHz).

#### Question Format (`ques`)

Each question is a feature type + a set of values that answer YES:
```
u32 question_count
repeated:
    u8  type         // feature being tested (see table below)
    u32 value_count
    u32[value_count] values  // if feature value is in this set -> YES
```

Feature type mapping (confirmed from disassembly of question evaluator at `0x8E87C90`):

| Type | Feature | Description | Value range |
|------|---------|-------------|-------------|
| 1 | syl_type | Syllable stress/type category | 1..7 |
| 2 | syl_in_phrase | Position of syllable within phrase | 1..8 |
| 3 | phone_left | Left phone context (phone label index) | 0..45 |
| 4 | phone_right | Right phone context (phone label index) | 0..45 |
| 5 | word_in_phrase | Position of word within phrase | 1..9 |
| 8 | phone_in_syl | Position of phone within syllable | 1..6 |

#### Tree Node Format (`tree`) -- CONFIRMED

**IMPORTANT**: Nodes are **variable-size** on disk (not fixed-size as initially hypothesized).

```
u32 n              // node count (NO root field -- traversal always starts at node 0)
node[0]..node[n-1] // variable-size: branch = 16 bytes, leaf = 20 bytes
```

**Branch node** (16 bytes on disk, `yes_child >= 0`):
```
u32 node_index       // sequential index (dead field, not read during traversal)
s32 yes_child        // node index for YES path (>= 0 signals this is a branch)
u32 no_child         // node index for NO path
u32 question_index   // index into the ques array (resolved to a pointer at load time)
```

**Leaf node** (20 bytes on disk, `yes_child < 0`):
```
u32 node_index       // sequential index (dead field)
s32 -1               // 0xFFFFFFFF = leaf sentinel
u32 0xFFFFFFFF       // sentinel (unused)
f32 mean             // predicted value (Hz for f0tr, f0_context domain for durt)
f32 variance         // used as scale factor in the quadratic cost formula
```

The in-memory layout is 24 bytes per node (padded from 16/20):
```
+0x00: ptr  question_ptr  // branch: pointer to in-memory ques entry; leaf: 0
+0x04: u32  node_index    // dead (never read by traversal code)
+0x08: s32  yes_child     // >= 0 = branch, < 0 = leaf
+0x0C: u32  no_child      // branch: child index; leaf: 0xFFFFFFFF
+0x10: f32  mean          // leaf: predicted target value
+0x14: f32  variance      // leaf: cost scaling parameter
```

Tree traversal (confirmed from `0x8E87D90` and `0x8E87E10`):
- Start at node 0 (root).
- While `node[+0x08] >= 0` (branch): evaluate question, go to `yes_child` or `no_child`.
- Return leaf pointer. Caller reads `[leaf+0x10]` (mean) and `[leaf+0x14]` (variance).

The f0tr tree has 109 nodes (54 branch + 55 leaf). All 55 leaf nodes predict F0 in 106.75-126.62 Hz
(mean ~117 Hz) -- squarely in Tom's male voice pitch range. This means the tree was trained on Tom's
speech data and will penalize any voice with substantially different F0 characteristics.

**Critical: durt leaf mean domain.** The durt leaf `mean` values are in the **f0_context domain**
(on-disk byte 19, range ~60-200), NOT in raw `dur_like` units. The engine's unit bias formula
computes `DUR_WEIGHT * |variance * (unit.f0_context - leaf.mean)|^2`, so leaf means must match
the numeric range of f0_context values. f0_context correlates with log(dur_like) at r=0.97 but
is in a different numeric range (~100-170 for Tom vs dl values of ~10-200).

---

### Join Cost Chunks

These chunks support the engine's most expensive task: deciding which unit to pick next by evaluating
how well the proposed concatenation point will sound.

#### How Join Cost Works

The engine has two modes, configured in the VCF:

| Mode | VCF Flag | Status for Tom |
|------|----------|---------------|
| Hash lookup | `use_joincache = 1` | **Active** |
| Runtime computation | `use_edgeframes = 0` | **Disabled** |

Tom uses precomputed costs. The hash table was built offline during voice construction and covers the
most likely unit pairs the engine will encounter.

The overall join cost applied to a candidate pair is:
```
final_cost = JOIN_COST_WEIGHT × raw_cost + JOIN_COST_OFFSET
           = 0.7 × raw_cost + 0.2
```

#### `hash` — Precomputed Join Cost Table

This is the largest chunk at ~21 MB. It maps `(uid_left, uid_right)` unit pairs to precomputed
spectral distance costs (float32, range 0..~12, typical 0..3).

The internal structure is a hash table organized as three sub-chunks:

| Sub-chunk | Size | Contents |
|-----------|------|---------|
| `head` | 8 B | `u32 n_rows = 692,190`, `u32 n_cells = 2,416,481` |
| `rows` | 2.77 MB | `u32[692,190]` — chain start index for each `uid_right` |
| `cell` | 19.3 MB | Two flat arrays: `u32[n_cells]` uid_left values + `f32[n_cells]` costs |

**How to look up `(uid_left, uid_right)` (CORRECTED 2026-03-16):**
```
index = rows[uid_right] + uid_left
if cells_A[index] == uid_left: return cells_B[index]   // HIT
else: return MISS                                       // empty slot (sentinel)
```
This is a **compressed perfect hash** -- single indexed access, NO chain walking or sequential scan.
`rows[uid_right]` gives the base offset; `uid_left` is added directly as an index. Empty slots
contain SENTINEL (0xFFFFFFFF) which never matches any valid uid. Confirmed by disassembly at
`0x8E8B7E6`: one `cmp` instruction, one `jne` to miss fallback, no loop.

**Space layout:** `n_rows = 692,190` is the bucket count (hash capacity); only `rows[0..169578]`
are populated. Multiple uid_rights can share the same base offset when their populated uid_left
positions align in the cell array. Occupancy: 1,621,241 data entries / 2,416,481 total cells = 67%.

**Statistics:** 1,621,241 actual `(uid_left, uid_right)` pairs are precomputed, covering ~159,982
distinct `uid_right` values.

#### `ccos` — Concatenation Cost Spectral Vectors

Used in the alternative edge-frames mode (disabled for Tom, but still present). Contains the boundary
spectral feature vectors used to compute join cost at runtime:

| Sub-chunk | Size | Contents |
|-----------|------|---------|
| `labl` | 175 B | 47 phone labels (same inventory as f0tr/durt) |
| `data` | 1,628,832 B | `47 × 722 × 12` float32 values |

Each phone has 722 boundary entries: 361 left-boundary frames + 361 right-boundary frames. Each entry
is a 12-dimensional spectral feature vector (LPC or MFCC coefficients). Values range 0..83.3,
consistent with MFCC scale.

At load time, the unit selector overwrites `unit[i].f0_context` with the phone's ccos label index
(from `labl`), so the engine can quickly look up the right spectral vector for any unit's boundary.

#### `prsl` — Preselection Cache

Before running the Viterbi search, the engine narrows down candidates using this chunk. For each
synthesis context, it provides a pre-filtered list of suitable units:

```
u32 count = 76,676    // number of preselection groups

repeated count times:
    u32 n             // entries in this group
    u32 context_key   // synthesis context identifier (sorted 54..929,192)
    u32[n-1]          // candidate unit IDs (all confirmed valid 0..169,578)
```

**Statistics:**
- 76,676 synthesis contexts preselected
- 1,089,111 total candidate unit IDs
- ~14.2 candidates per context on average
- Most common group size: 1 candidate

**`context_key` formula (CONFIRMED from assembly at 0x8e917f0 in SWIttsUSel.dll):**

```
context_key = left_hp * 10000 + center_hp * 100 + right_hp
```

Where `left_hp`, `center_hp`, and `right_hp` are **halfphone indices** into a sorted table of all
phone variants. The index formula is:

```
halfphone_idx = hp_base[unit.phone_center] + (1 - unit.is_first_half)
```

That is: even halfphone index = left boundary half (is_first_half=1); odd = right boundary half
(is_first_half=0).

**`hp_base` table** (derived empirically from prsl data; mostly `2 * pc` with 5 anomalies):

| `unit.pc` | Phone | `hp_base` | Note |
|-----------|-------|-----------|------|
| 0–8 | aa1..aw1 | `2*pc` | Normal |
| 9 | aw2 | **22** | Anomaly (expected 18) |
| 10 | ax1 | **18** | Anomaly (expected 20) |
| 11 | ax2 | **20** | Anomaly (expected 22) |
| 12–13 | ay1..ay2 | `2*pc` | Normal |
| 14 | b1 | **30** | Anomaly (expected 28) |
| 15 | b2 | **28** | Anomaly (expected 30) |
| 16–45 | ch1..iy2 | `2*pc` | Normal |

The anomalies for aw/ax (pc=9,10,11) and b (pc=14,15) are due to a different sort order used by the
engine when building its internal phone-info table at load time. Pc=42 (ix1) has no prsl groups;
its `hp_base` = 84 by the `2*pc` formula.

**Silence/boundary marker:** `hp_base = 92` is used for left or right context when there is no
adjacent unit (i.e., word boundary or utterance start/end). This corresponds to the 47th entry in
the sorted phone table (`jh1` base at sort_pos=46 → 2*46=92). The value 92 appears as A=0 is NOT
the silence marker — A=0 means "silence as left context, ignoring halfphone".

**Context encoding rules** (99.94% accuracy on all 76,676 groups):

- **A=0** (utterance start): center_hp = hp_base[pc] (even); both halves aggregated.
- **A=92, C=92** (isolated, silence both sides): center_hp = hp_base[pc] (even); both halves.
- **A=92, C≠92** (utterance end before speech continues): center_hp = hp_base[pc]+1 (odd, coda).
- **A=even, 0<A<92** (onset of left phone): center_hp = hp_base[pc] (even, onset).
- **A=odd** (coda of left phone): center_hp = hp_base[pc]+1 (odd, coda).

Compact form: `center_hp = hp_base[pc] + (1 if A==92 and C!=92 else A%2)` for A!=0.

The C (right context) follows the same hp_base encoding. C=92 = utterance-end silence.

**Assembly confirmation:** The function at `0x8e917f0` in `SWIttsUSel.dll` computes the key as:
`(A*100 + B)*100 + C` using two `imul eax, 0x64` instructions. B (center) is read from the
phone-info table: `phone_info_table[phone_center * 16 + 4]`, where each entry is 16 bytes and
field +4 holds the halfphone base. A and C come from a 3-element context array passed by the caller.

---

## VCF — Voice Configuration (`tom.vcf`)

After decryption, the VCF is an **ISO-8859-1 XML file** following the `SWIttsConfig` DTD. It contains
all the tuning parameters and cost weights the engine uses at synthesis time.

### Voice Identity

| Parameter | Value |
|-----------|-------|
| `tts.voiceCfg.name` | `Tom` |
| `tts.voiceCfg.language` | `en-US` |
| `tts.voiceCfg.gender` | `male` |
| `tts.voiceCfg.phoneset` | `swi_plus_ix` |
| `tts.voiceCfg.version` | `3.0.0.0` |

### File Path Templates

| Parameter | Template | Result |
|-----------|----------|--------|
| `tts.voiceCfg.index` | `${xml:base}/${tts.voice.name}.vin` | `tom.vin` |
| `tts.voiceCfg.speechdb` | `${xml:base}/${tts.voice.name}${tts.voice.format}.vdb` | `tom8.vdb` |

The `${tts.voice.format}` variable is why the VDB is called `tom8.vdb` — the engine substitutes the
format tag (8 for 8 kHz) at runtime. A 16 kHz voice would load `tom16.vdb`.

### Cost Function Weights

| Parameter | Value | What It Controls |
|-----------|-------|-----------------|
| `JOIN_COST_WEIGHT` | 0.7 | How much spectral smoothness matters |
| `JOIN_COST_OFFSET` | 0.2 | Baseline join cost (prevents all zeros) |
| `CONTEXT_COST_WEIGHT` | 1.0 | How much phonetic context match matters |
| `DUR_WEIGHT` | 0.3 | How much duration accuracy matters |
| `ABS_F0_WEIGHT` | 0.2 | How much absolute pitch match matters |
| `F0_EDGE_CHANGE_WEIGHT` | 0.6 | How much pitch continuity at joins matters |
| `CHUNK_BIAS_WEIGHT` | 0.25 | Bonus for reusing units from the same recording |
| `UNIT_BIAS_WEIGHT` | 0.25 | Bonus for reusing recently used units |

### Engine Mode Flags

| Flag | Value (Tom) | Effect |
|------|-------------|--------|
| `use_joincache` | 1 | Use precomputed `hash` chunk for join costs; set to 0 to disable |
| `use_edgeframes` | 0 | Compute join costs at runtime from `ccos` boundary vectors; if both flags are 0, no join cost is applied |
| `use_dynamic_cost` | 1 | Enable dynamic cost computation |
| `USE_F0_PROBABILITIES` | 1 | Use probabilistic F0 prediction |
| `USE_STRESS_AND_PA` | 1 | Include stress and pitch accent in selection |

### Prosody Mismatch Costs

Small penalties for using a unit recorded in the wrong prosodic context:

| Mismatch Type | Penalty |
|---------------|---------|
| Syllable stress | 0.05 |
| Phrase position | 0.05 |
| Syllable in word | 0.05 |
| Word in phrase | 0.05 |
| Phone in syllable | 0 (not penalized for Tom) |

### Candidate Pruning

| Level | Threshold | Slope |
|-------|-----------|-------|
| Half-phoneme | 0.8 | 0.005 |
| Syllable | 0.7 | 0.005 |
| Word | 0.7 | 0.005 |

### Prosody Cost Matrices

Four detailed cost tables define how badly each prosodic mismatch is penalized. These are square
matrices where rows are the target context and columns are the candidate context; zero = perfect match.

- **`sylInPhraseCosts`** (10×10): Syllable position in phrase. PhrSingle/ContextUnknown = 100 (forbidden).
- **`sylTypeCosts`** (9×9): Syllable prosodic type (stressed/unstressed/pitch-accent/etc.)
- **`sylInWordCosts`** (7×7): Syllable accent position. All mismatches = 0 for Tom (not penalized).
- **`wordInPhraseCosts`** (7×7): Word position in phrase.

The `context_cost=100` value in `unit +0x1C` maps directly to the "ContextUnknown=100" column/row
in these matrices — marking units where prosodic context is unknown or forbidden.

---

## How the Three Files Work Together

```
VCF: "Load Tom.vin and tom8.vdb; use these cost weights"
    ↓
VIN feat/cnts: "There are 169,579 units across 8,118 utterances with these 16 feature types"
    ↓
VIN unit: "Unit #1234 is in recording date_005 at position 42, phone /aa/, stressed, first half,
           F0 start=112, F0 end=118, phonetic context [b, aa, k, n]..."
    ↓
VDB indx: "Recording date_005 starts at byte offset 1,234,567 in the data chunk"
    ↓
VDB data: [raw mu-law audio bytes for date_005]
```

At synthesis time:
1. The engine converts the input text to a phoneme sequence.
2. `prsl` gives it a short list of candidate units for each phoneme context.
3. `f0tr`/`durt` predict target F0 and duration for each position.
4. The Viterbi algorithm finds the best path through candidates, penalizing:
   - Poor join quality (`hash` lookup, weighted by `JOIN_COST_WEIGHT`)
   - Wrong phonetic context (`unit +0x0C..0x0F` vs. target, weighted by `CONTEXT_COST_WEIGHT`)
   - Wrong duration/F0 (`mean`/`hist` Z-score lookup, weighted by `DUR_WEIGHT`/`ABS_F0_WEIGHT`)
5. Selected units are retrieved from the VDB and concatenated.

---

## Making a New Voice

This section documents the **proven workflow** used to create Mara (female speaker) as a drop-in
replacement for Tom. Mara uses Tom's phone inventory, trees, and corpus structure, substituting
only the audio recordings and derived acoustic statistics.

### Prerequisites

- Python 3.12 with: `numpy`, `scipy`, `librosa`, `wave`, `audioop`, `struct`
- Montreal Forced Aligner (MFA) for phoneme-level audio alignment
- A TTS synthesizer (e.g. Qwen3-TTS) to generate audio from transcripts, or your own recordings
- Engine binary: `bin\spfy_dumpwav32_8khz.exe` (use the 8 kHz variant, NOT the 16 kHz one)

### Step 1 -- Prepare Audio Recordings

The engine expects one WAV per utterance, matching Tom's utterance names
(`feat.filename` list in `tom.vin`). All utterances that Mara does not already have
must be synthesized or recorded and placed in `en-US/mara/output/mara_voice_wavs/<name>.wav`.

1. Run `gen_mara_gap_transcripts.py` to find which Tom utterances Mara does not yet have, and
   generate a synthesis CSV with transcripts recovered from the `ckls` stream and any manual
   transcription CSVs.
2. Synthesize missing utterances with `08a_batch_synthesize_transcripts.py` (Qwen3-TTS), producing
   24 kHz WAVs stored flat in `en-US/mara/output/mara_voice_wavs/`.
3. For utterances whose transcripts are not in `ckls` (common for phonetically controlled TTS training
   material like numbers, letters, addresses), use `extract_tom_audio_for_transcription.py` to pull
   Tom's own audio clips, then transcribe them manually with
   `reveng/~__WORKDIR__.~/transcribe_session.py`.

### Step 2 -- Run MFA (Forced Alignment)

Forced alignment maps each recording to phone-level time boundaries, which `build_mara_voice.py`
uses to compute `unit.local_pos` and `unit.dur_like`.

```
# Prepare: one WAV + one .lab (transcript text) per file in mfa_guided_input/
# Output: TextGrid files in mfa_guided_output/
mfa align mfa_guided_input/ english_us_arpa mfa_guided_output/ --clean
```

**MFA must run before the build pipeline.** The build script reads TextGrid files to compute
unit positions. Units outside an aligned region default to Tom's proportionally-scaled positions.

### Step 3 -- Build the Voice Files (4-Script Pipeline)

Run these four scripts in order. Each patches the VIN/VDB incrementally.

#### Script 1: `build_mara_voice.py`

Builds `mara8.vdb` and `mara.vin` from scratch.

- Reads all WAVs in `en-US/mara/output/mara_voice_wavs/`
- Downsamples from 24 kHz to 8 kHz (3:1), converts to u-law with `audioop.lin2ulaw`
- Writes `mara8.vdb`: RIFF/WAVE with `fmt ` (AudioFormat=7, BitsPerSample=16, BlockAlign=2),
  `indx` (name -> byte offset), and `data` (raw u-law bytes)
- Clones Tom's `unit`, `mean`, `hash`, `prsl`, `hist`, and tree chunks, scaling `local_pos` and
  `dur_like` from TextGrid times: `lp = round(start_sec * 1000)`, `dl = round(dur_sec * 1000)`
- Cap enforced: `lp + dl <= mara_n // 8 - 1` where `mara_n` = total u-law bytes in that recording

**Critical bug to avoid:** `feat.filename` entries each have a `u32 stored_id` field that is NOT
the positional index (29% of entries are out of order). Always use
`filenames = {stored_id: name}` (dict keyed by stored_id), never a positional list.

#### Script 2: `build_mara_hash.py`

Patches the `hash` chunk with Mara-specific join costs computed from Mara's MFCC boundary vectors.

- Computes MFCC boundary vectors per unit from the aligned WAVs
- Calculates spectral distances between candidate unit pairs
- Writes updated hash chunk in SoA format: `u32[n_cells] cells_A` then `f32[n_cells] cells_B`

#### Script 3: `build_mara_prsl.py`

Patches the `prsl` chunk so synthesis contexts prefer Mara's units.

- Iterates all (left, center, right) unit trigrams from Mara's aligned recordings
- Computes `context_key = left_hp * 10000 + center_hp * 100 + right_hp`
  (see prsl section for hp_base table and anomalies at aw/ax/b)
- Groups candidate unit IDs by context_key
- Falls back to Tom's prsl entries for any keys Mara does not cover

Mara result: 12,413 native keys + 73,657 Tom fallback = 86,070 total (Tom has 76,676).

#### Script 4: `build_mara_mean.py`

Patches the `mean` chunk with Mara-specific per-halfphone acoustic statistics.

- Computes per-halfphone `(mean, std)` for: duration, pitch (F0), voicing, power
- `power_mean` is in `ln(mean_abs_PCM16)` scale -- will differ from Tom's for different recording levels
- Writes 92x8 f32 row-major matrix
- Leaves `hist` unchanged (keep Tom's empirical histogram)

### Step 4 -- Test with the Engine

```
bin\spfy_dumpwav32_8khz.exe "Hello world." mara en-US bin\out.wav
```

Listen to `bin/out.wav`. If the engine prints "File end is beyond speech DB end":
- A `local_pos + dur_like` value exceeds the VDB byte limit for that recording
- Check the cap formula in `build_mara_voice.py`: `cap = mara_n // 8 - 1`
- Also verify the `feat.filename` stored_id dict (not a positional list)

### Step 5 -- Test with Long Text

**Short texts are not sufficient for validation.** The most dangerous crash only manifests after
roughly 100+ synthesis units (a few sentences). Always test with something like:

```
bin\spfy_dumpwav32_8khz.exe "The following message is transmitted at the request of the Wisconsin Emergency Management Agency Milwaukee Sullivan Wisconsin. Driving is extremely hazardous tonight on all roads in southern Wisconsin. If you become stranded, emergency vehicles may not be able to reach you. If you are stranded, do not leave your vehicle. Run your vehicle 10 minutes per hour, and crack a downwind window for ventilation." mara en-US bin\out.wav
```

If the engine crashes with `EXCEPTION_ACCESS_VIOLATION` (no error message, just a crash), see
"The WSOLA Crash" section below.

---

### Known Crash Causes and How to Diagnose Them

#### Crash 1: "File end is beyond speech DB end"

**Symptom:** Engine prints this message (or similar) and produces silence or truncated audio.

**Cause:** `(local_pos + dur_like) * 8 > recording_byte_size` for some unit. The engine tried to
read audio beyond the end of that recording's data in the VDB.

**Fix:** Ensure the cap formula is applied: `cap = mara_n // 8 - 1` where `mara_n` is the
u-law byte count for that recording. The build script applies this automatically.

**Frequent trap:** Using positional index into the indx table instead of name-based lookup.
The VDB indx ordering does NOT match the VIN `feat.filename` ordering (7,444/8,118 mismatch).
Always look up recording sizes by name, never by `file_idx` as a direct VDB position index.

---

#### Crash 2: WSOLA Access Violation on Long Text (the hard one)

**Symptom:** `EXCEPTION_ACCESS_VIOLATION` at `rep movsd` inside `SWIttsWsola.dll`. No error
message. Short texts work; long texts crash. The crash appears random but is deterministic.

**Root cause:** The `local_pos` values for units within the same recording must be
**monotonically non-decreasing** (in the order units appear in the recording). If any unit has a
`local_pos` smaller than the previous unit from the same recording, the WSOLA `configure`
function accumulates a negative running sum, which becomes a negative `start_pos`. A negative
`start_pos` causes the audio cursor to wrap to a huge unsigned value, sending `rep movsd` off
into unmapped memory.

**Why it only happens on long texts:** Each `configure` call handles one unit (and its silence
boundary neighbors). The cursor carries over between units. A single backwards lp jump will
cause the cursor to go deeply negative, but this only becomes fatal once that particular unit
is reached — which can take dozens of prior units to accumulate to call #140 or so.

**How to trigger this in build_mara_voice.py:** The MFA path scales speech units
(non-silence) with per-phoneme MFA time intervals, but silence units with whole-recording
proportional scaling. These two scale factors can diverge, placing a silence unit's
`new_lp` before a speech unit that precedes it in Tom order.

**The fix:** `build_mara_voice.py` (STATE_VERSION 14+) enforces monotonicity as a
post-processing step: after all units in a recording are computed, they are sorted back into
Tom lp order and any `new_lp` that decreased is clamped to the previous value.

**If you write your own build script**, you MUST enforce this invariant. After computing
all `local_pos` values for a recording:
1. Sort units by their original Tom `local_pos` order.
2. Walk in order: if `unit[i].local_pos < unit[i-1].local_pos`, set
   `unit[i].local_pos = unit[i-1].local_pos` (and clamp `dur_like` accordingly).

**Diagnosing with Frida:** If you suspect this crash on a different voice, attach Frida to
`Speechify.exe` and hook `process_unit` (0x8EE2960). Watch for `start < 0` or a negative
delta between consecutive `start` values. The first negative delta identifies the offending
unit; its `file_idx` reveals which recording has the monotonicity violation. See
`c:/tmp/frida_wsola_delta_start.py` for the hook script.

### Creating a VCF for the New Voice

Copy `en-US/tom/tom.vcf` to `en-US/mara/mara.vcf`. Decrypt (nibble expansion cipher -- see VCF
section), edit the XML, and re-encrypt.

Key XML attributes to change:
- Voice name path references: update `%{xml:base}` paths to point to `mara`
- Optionally tune cost weights for the new speaker:

| Weight | Default | Notes |
|--------|---------|-------|
| `JOIN_COST_WEIGHT` | 0.7 | Spectral smoothness penalty at concatenation points |
| `JOIN_COST_OFFSET` | 0.2 | Constant added to all join costs (prevents over-selecting near zero) |
| `DUR_WEIGHT` | 0.3 | Duration deviation penalty |
| `ABS_F0_WEIGHT` | 0.2 | Absolute F0 deviation penalty |
| `CONTEXT_COST_WEIGHT` | 1.0 | Prosodic context cost penalty |

For a female speaker with cleaner recordings, try lowering `JOIN_COST_WEIGHT` to 0.5-0.6 and
raising `ABS_F0_WEIGHT` to 0.3 for more natural prosody.

### What to Reuse vs. Rebuild

| Component | Reuse Tom's? | How to Build for New Voice |
|-----------|-------------|---------------------------|
| `unit` positions (lp, dl) | No | MFA alignment + `build_mara_voice.py` |
| `unit` prosodic fields | Yes (copy from Tom) | Optionally: prosodic tagger on transcripts |
| `hash` join costs | No | `build_mara_hash.py` |
| `prsl` preselection | Partial (Tom fallback) | `build_mara_prsl.py` |
| `mean` acoustic stats | No | `build_mara_mean.py` |
| `hist` histogram | Yes | Keep unchanged |
| `ccos` boundary vectors | No | Extract from aligned recordings |
| `f0tr` / `durt` trees | **No** (see F0 scale mismatch) | Scale f0 values or rebuild tree |
| `ckls` / `cklx` index | Yes | Keep unchanged |
| `feat` / `cnts` | Partially | Update filename list; keep phone inventory |
| VDB audio | No | `build_mara_voice.py` from synthesized/recorded WAVs |
| VCF config | Partial | Clone + rename + optionally tune weights |

---

## Open Questions

Most chunks are confirmed. The remaining open questions are:

- **`local_pos + 40` gap**: CONFIRMED (2026-03-12). `extra=40` for 11,803 of 162,730 within-recording
  consecutive pairs (7.25%). Always exactly 40 (no 80/20/variable). Always within the same recording
  (`local_pos` resets between recordings so cross-recording comparisons are undefined). The 40 ms of
  audio exists in VDB but is unassigned to any unit; the engine skips it during playback. Interpretation:
  fixed inter-phoneme silence pad inserted during the original recording session.

- **`f0tr`/`durt` tree node semantics**: FULLY CONFIRMED (2026-03-13). Previous hypothesis of fixed
  18-byte nodes with `u16 q_or_tag` prefix was WRONG. Actual format: **variable-size nodes** --
  branch = 16 bytes (`u32 node_idx, s32 yes_child, u32 no_child, u32 question_idx`),
  leaf = 20 bytes (`u32 node_idx, s32 -1, u32 0xFFFFFFFF, f32 mean, f32 variance`).
  Branch/leaf determined by sign of second field (>= 0 = branch, < 0 = leaf). No root pointer --
  traversal always starts at node 0. Confirmed by disassembly of loader at `0x8E83780` and
  traversal at `0x8E87D90`/`0x8E87E10`. All 48 trees (1 f0tr + 47 durt) parse perfectly with
  this format: consumed bytes match chunk sizes exactly, all nodes reachable from root, all
  question indices valid. f0tr: 109 nodes (54 branch + 55 leaf), predictions 106.75-126.62 Hz.
  durt: 778 branch + 825 leaf nodes total across 47 per-phone trees.
  Question type semantics also confirmed from evaluator at `0x8E87C90`:
  type 1 = syl_type, 2 = syl_in_phrase, 3 = phone_left, 4 = phone_right,
  5 = word_in_phrase, 8 = phone_in_syl. Questions check set membership (linear scan).
  **F0 ENCODING CONFIRMED (2026-03-13)**: f0_start/f0_end/f0_mid are stored as **direct Hz integers** (u8).
  Tom's nonzero range is 99-150 Hz; median 118 Hz (typical male pitch). Values 1-98 and 151-255 do NOT
  appear in Tom's f0_start/f0_mid (f0_end has a slightly wider range up to 156).
  `build_mara_voice.py` uses `F0_SCALE = 0.641` which maps Mara's avg 184 Hz -> 118 Hz (correct mean),
  but Mara values range 51-255 Hz (wider spread than Tom's 99-150). To reduce F0 cost penalty:
  option (a) clamp Mara f0 values to [99, 150] after scaling, option (b) rebuild f0tr tree for Mara's
  pitch range, or option (c) set ABS_F0_WEIGHT=0.0 in VCF.
  The f0tr tree predicts F0 in 106.8-126.6 Hz (55 leaves, mean ~117 Hz) -- Tom's range.

- **`prsl.context_key` derivation**: CONFIRMED. Formula: `left_hp * 10000 + center_hp * 100 + right_hp`.
  Remaining sub-question: the exact sorted order used by the engine's phone-info table (explains the
  aw/ax and b anomalies). Empirical hp_base table is known and sufficient for rebuilding prsl.

- **`prsl` rebuild for new voices**: The context_key formula is confirmed. To rebuild prsl, iterate
  over all (left_unit, center_unit, right_unit) trigrams in training utterances, compute the key,
  and group center_unit IDs by key. The hp_base anomalies (aw/ax, b) must be replicated exactly.

- **`hash` pair types (within vs. cross-recording)**: CONFIRMED analytically. 1,621,241 precomputed
  pairs vs. ~162,730 possible within-recording adjacent pairs. Cross-recording pairs dominate (~90%+).
  The hash exists to cache expensive spectral distance computations for cross-recording concatenations.

- **`hash` pair selection**: Which `(uid_left, uid_right)` pairs were included? K-nearest neighbors,
  threshold-based, or exhaustive within a phonetic window? Unknown. The shared-offset extension
  technique (2026-03-16) bypasses this question for extra recordings by using cost=0.0 for
  same-recording neighbors.

- **Exact join cost distance formula**: The edge-frames path uses LPC/autocorrelation-style computation
  with component weights 0.5 and 0.333 (internal normalization, separate from VCF weights). The exact
  formula combining the two components has not been fully traced. Does not affect voices using
  `use_joincache=1` (hash-based mode), which is the default for Tom and Mara.

---

## Unit Selection / Viterbi Search

The unit selection algorithm lives in `SWIttsUSel.dll`. The main entry point is the exported
function `SWIttsUSelUnitSelection`. Here is how it works at a high level:

### Pipeline

1. **Config loading**: Read ~50 VCF parameters (weights, thresholds, pruning settings) into an
   internal config struct. Important parameters include `JOIN_COST_WEIGHT`, `DUR_WEIGHT`,
   `ABS_F0_WEIGHT`, `CONTEXT_COST_WEIGHT`, `HALFPHONE_CAND_MAX_UNITS` (default 50), and
   `HALFPHONE_CAND_PRUNE_THRESH`.

2. **Target computation**: For each halfphone position in the utterance, compute the target
   duration and F0 (pitch) from the prosody model (using `durt`/`f0tr` decision trees + VCF
   prosody cost matrices).

3. **Candidate preselection**: Use the `prsl` cache to find candidate units for each halfphone
   context (left-center-right trigram). The preselection cache dramatically narrows the search
   space.

4. **Viterbi scoring**: For each halfphone position, iterate over all candidates and compute a
   multi-component cost:
   - **Stress additive**: `(float)context_cost_byte * UNIT_BIAS_WEIGHT * 0.01`. For stressed
     units (byte 28 = 100): adds 0.25 to the score. For unstressed (0): nothing.
   - **Target cost**: Sum of 5 CART-tree-predicted costs from prosodic category fields (syl_type,
     syl_in_phrase, word_in_phrase, phone_position), each weighted by its config parameter.
   - **Context cost**: Prosody mismatch from 4-component cost tables (syllable position, word
     position, stress, phrase position). Weighted by `CONTEXT_COST_WEIGHT` (Tom: 1.0).
   - **Unit bias / Duration cost** (stored at candidate+0x08): Quadratic penalty on the
     `f0_context` field (on-disk byte 19, in-memory +0x12 for version 100006). This field
     correlates very strongly with log(dur_like) (r=0.97). Formula:
     `DUR_WEIGHT * |tree_scale * (f0_context - tree_prediction)|^2`.
     Tom VCF sets `DUR_WEIGHT=0.3`.
   - **Chunk bias / F0 cost** (stored at candidate+0x0C): Pitch boundary matching.
     The gate field is **`f0_start`** (on-disk +0x10, in-memory +0x0F for version 100006):
     - If `f0_start == 0` (unvoiced) AND target has valid f0tr prediction:
       `MISSING_F0_COST` (5.0) is added.
     - If `f0_start > 0`: `ABS_F0_WEIGHT * |f0_scale * (f0_start - f0_reference)|^2`
     - If no f0 target prediction: f0_cost = 0 (no penalty).
     Tom VCF sets `ABS_F0_WEIGHT=0.2`.
   - **Join cost**: Spectral distance between the boundary of the previous unit and this candidate.
     Looked up from the `hash` table (precomputed) or computed from `ccos` edge frames at runtime.
     Weighted by `JOIN_COST_WEIGHT` (default 0.25, Tom VCF overrides to 0.7). Missing joins use
     `MISSING_JOIN_COST` (default 1000.0).

   **Version-dependent field mapping**: For Tom/Mara (unit version 100006), the field at in-memory
   +0x0F is on-disk byte 16 = `f0_start`. For version 100007+, it would be on-disk byte 17 =
   `f0_end`. Similarly, +0x12 maps to `f0_context` (100006) or `phone_center` (100007+). See the
   in-memory mapping table in README_TECHNICAL.md for full details.

   **Mara concat cost analysis**: The "concat(bias)" Frida component is the sum of unit_bias (+0x08)
   and chunk_bias (+0x0C). Unit bias uses byte 19 (f0_context), which is **identical** between Tom
   and Mara (never modified by build_mara_voice.py). Chunk bias uses byte 16 (f0_start), which
   differs 93% between Tom and Mara:
   - Tom f0_start: **raw Hz**, nonzero range 99-150 Hz, tight cluster around median 118; 26% zeros (44,204 units)
   - Mara f0_start: raw Hz scaled by 0.641, nonzero range ~51-255 Hz, wider spread around median 118; 15% zeros
   - 7.4% of Mara's non-zero values exceed Tom's max (150 Hz)
   - The quadratic cost amplifies outliers: Mara concat mean ~0.33 vs Tom ~0.18
   - Fix: clamp Mara f0 values to [99, 150] after `round(harvest_hz * 0.641)`

5. **Pruning**: After scoring each halfphone's candidates, the prune function at `0x8e88830`
   uses a histogram-based adaptive cutoff (NOT a simple threshold + slope * N formula):
   - Build a 40-bin histogram of `(score - best_score) * 40.0` (bin width = 0.025)
   - Walk bins low-to-high, accumulating candidate count `cum`
   - At each bin k: stop when `(k-1) * 0.025 >= PRUNE_THRESH - cum * PRUNE_SLOPE`
   - `threshold = best_score + stopped_bin * 0.025`
   - Discard candidates with score > threshold
   - Shell-sort survivors by score (tiebreak: lower unit_id wins)
   - Cap at `HALFPHONE_CAND_MAX_UNITS` (default 50)

   Tom's VCF sets PRUNE_THRESH to 0.95 (very aggressive relative to the default of 3.0).

6. **Viterbi forward pass** (confirmed 2026-03-15): After ALL halfphones have been scored
   and pruned, a separate forward-pass function runs the actual Viterbi dynamic programming.
   Two modes exist:

   - **Mode A** (`0x8E8EDD0`, with join cost): The normal path for Tom. For each halfphone
     from 1 to N-1, for each surviving candidate, the algorithm evaluates ALL surviving
     predecessors from the previous halfphone, computing `total = prev.cumulative_score +
     join_cost + modifiers`. The best predecessor is stored at **candidate+0x24** (a pointer
     to the best previous candidate struct). The cumulative score is stored at
     **candidate+0x20**. After processing each halfphone, candidates are heapsorted by
     cumulative score (using `0x8E8B580`, a standard in-place heapsort).

   - **Mode B** (`0x8E8B620`, without join cost): Same structure but skips the join cost
     computation. Used as a fallback when neither join hash nor edge frames are available.

   At the end, the algorithm scans the last halfphone's candidates to find the one with the
   minimum cumulative score, storing the winner index for the backtrack.

   **Key point**: The Viterbi forward pass has NO additional beam/pruning. It exhaustively
   evaluates all surviving candidate-predecessor pairs. The ONLY beam control is the
   histogram-based prune in step 5 above.

7. **Backtrack** (`0x8E8DE20`, 0xEFB bytes -- the largest function in the pipeline):
   Starting from the best final candidate found in step 6, traces backward through
   predecessor pointers (`candidate+0x24`) to reconstruct the optimal path. For each
   halfphone (from last to first), it records the selected unit and candidate struct into
   output arrays at `[obj+0x20]` (candidate index) and `[obj+0x28]` (candidate pointer).
   Also computes per-component score statistics and logs them:
   - `"TOTAL PATH %d units scores (S %f D %f DU %f SP %f J %f F0 %f)"`
   - `"RAW TOTAL PATH ..."` (unnormalized)
   - Per-unit `"score_stats %d %d %s (S ... D ... DU ... SP ... J ... F0 ...)"`.

8. **Output**: The selected unit sequence is passed to WSOLA for waveform concatenation.

### Score Components (from debug strings)

The engine logs scores using the format:
`"score_stats %d %d %s (S %5.3f D %5.3f DU %5.3f SP %5.3f J %5.3f F0 %5.3f)"`

Where: S = total Score, D = Duration cost, DU = Duration unit(?), SP = SPectral/prosody,
J = Join cost, F0 = pitch cost.

### Config defaults vs Tom VCF overrides

| Parameter | DLL Default | Tom VCF Override |
|-----------|------------|-----------------|
| JOIN_COST_WEIGHT | 0.25 | 0.7 |
| JOIN_COST_OFFSET | 0.20 | 0.2 |
| DUR_WEIGHT | 0.04 | 0.3 |
| ABS_F0_WEIGHT | 0.04 | 0.2 |
| CONTEXT_COST_WEIGHT | 0.60 | 1.0 |
| HALFPHONE_CAND_PRUNE_THRESH | 3.0 | 0.95 |
| HALFPHONE_CAND_PRUNE_SLOPE | 0.005 | (default) |
| HALFPHONE_CAND_MAX_UNITS | 50 | (default) |
| MISSING_JOIN_COST | 1000.0 | (default) |
| MISSING_F0_COST | 5.0 | (default) |
| UNIT_BIAS_WEIGHT | 1.0 | (from VCF) |
| CHUNK_BIAS_WEIGHT | -1.0 | (from VCF) |

Tom's VCF dramatically increases the weight of join cost (0.25 -> 0.7) and context cost
(0.6 -> 1.0), while also setting a much tighter pruning threshold (3.0 -> 0.95). This means
Tom's voice strongly prefers smooth joins and matching prosody, at the expense of broader
candidate search. The engine also supports per-voicing-class join cost weights (V0/V1/V2_JCW/JCO)
for finer control over voiced, unvoiced, and mixed boundary costs.

---

## Mara Voice: Current Status (2026-03-16)

Mara is the first custom voice built for the Speechify engine. She is a female speaker
synthesized with Qwen3-TTS and aligned with Montreal Forced Aligner.

### Output Quality

- **Comprehensible output** with mild residual stutter from recording fragmentation
- Weather test sentence: **33 recording switches** (down from 42 earlier, and 66.7% baseline)
- Phone number test: "Please call us at five ive ive 01...3" -- clearer than previous builds
- First half of weather sentence sounds good; "with a high" section still stuttery
- 95% MFA phone coverage after english_mfa dictionary fix and re-synthesis of 270 recordings

### Build Pipeline (STATE_VERSION 70)

Four scripts run in order:
1. `build_mara_voice.py` -- builds mara8.vdb + mara.vin from scratch
2. `build_mara_rest.py` -- patches hash/prsl/mean with Mara-specific data
3. `build_mara_hash.py` -- patches hash cells_B with spectral clustering + run-potential penalty
4. `build_mara_trees.py` -- patches f0tr/durt trees

### Hash Structure Breakthrough (2026-03-16)

The hash was previously thought to use chain-walking (sequential scan). Disassembly and
Frida Stalker tracing revealed it is actually a **compressed perfect hash** with direct
indexed access: `cell[rows[uid_right] + uid_left]`. This explains why naive appending
crashed -- uid_left is used as a direct index, so appended regions were accessed out of
bounds by large uid_left values.

**Shared-offset extension technique** resolves the immutability problem: all extra
uid_rights share one base offset at `n_cells_original`, with an appended extension region
containing `{uid_left, 0.0}` pairs for same-recording neighbors. Confirmed working: extra
unit UID 176310 was selected by the engine.

Remaining constraints:
- `use_edgeframes` (runtime join cost) requires an unknown chunk and is non-functional
- ~33 recording switches per 100 halfphones still the baseline to beat

### Join Cost Non-Functionality (2026-03-16)

VCF weight tuning experiments (Exp 52-54) revealed that **join cost is effectively
non-functional for Mara**. Tom's hash has only 1.6M entries out of ~29 billion possible
`(uid_left, uid_right)` pairs -- only 0.006% of transitions produce hash HITs. For the
vast majority of transitions:

1. Hash lookup returns MISS
2. Miss fallback at `0x8E8B7F5` loads constant 0.0 as raw cost (when `[ecx+0x6c] <= 20`)
3. Final join cost = `0.7 * 0.0 + 0.2 = 0.2` regardless of `JOIN_COST_WEIGHT` value

This means:
- Changing `JOIN_COST_WEIGHT` (0.7 -> 1.0): no effect (33 switches, identical UIDs)
- Reducing `CONTEXT_COST_WEIGHT` (1.0 -> 0.5): worse (36 switches -- less discrimination)
- Increasing run penalty parameters: no effect (remaining switches are between high-run units)

The only effective hash-based mechanism is the **run-potential penalty** in `build_mara_hash.py`,
which creates HIGH-cost hash HITs for penalized low-run units (reduced switches 42 -> 33).
All other join cost tuning is inert. The `[ecx+0x6c]` field that gates ccos distance
computation remains unidentified -- understanding it could unlock real join costs for misses.

### Next Steps

1. Investigate `[ecx+0x6c]` to understand when ccos distance is actually computed
2. Replace low-value main-pool recordings with targeted phrases for problematic sequences
3. Per-phone spectral clustering for finer timbral consistency
4. Profile which recordings the engine selects most to focus quality improvement
5. Test with additional sentences for generalization

---

## Key Numbers (Quick Reference)

| Parameter | Value |
|-----------|-------|
| XOR key (VIN/VDB) | `0xCE` |
| VIN RIFF form type | `svin` |
| VDB RIFF form type | `WAVE` |
| VCF cipher | Nibble expansion (2:1), custom substitution table |
| Unit record stride | 29 bytes |
| `local_pos`/`dur_like` unit | 8 μ-law bytes @ 8 kHz (= 1 ms per unit); `byte_offset = local_pos × 8` |
| Total units | 169,579 |
| Distinct source recordings | 6,849 |
| Total utterance names | 8,138 |
| Phone inventory | 46 ARPABET phones (92 diphone halves) |
| Acoustic feature dimensions | 8 (pitch, duration, voicing, power + z-scores) |
| Audio encoding | G.711 mu-law, 8 kHz, mono |
| Male F0 (Tom) | ~118 Hz mean nonzero |
| Hash table pairs | 1,621,241 precomputed join costs |
| Preselection contexts | 76,676 |
| ccos vectors | 47 phones × 722 entries × 12 f32 each |

---

*This document is generated from analysis of the Tom voice files (`tom.vin`, `tom8.vdb`, `tom.vcf`) and `dll/SWIttsEngineUtil.dll` / `dll/SWIttsUSel.dll`. All technical details have been verified against the original binary data. This file, and the README.md file, are both entirely generated with Claude Code and are updated periodically as new insights are discovered.*
