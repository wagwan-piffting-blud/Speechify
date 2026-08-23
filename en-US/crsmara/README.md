# "CRS Mara" - the follow-up to "AI Mara", a (non-original) Speechify Voice

**True Mara is lost.** This is a _reconstruction_, not a _recovery_: a Speechify 3.0.5 unit-selection voice built from the only Mara audio that survived - NOAA Weather Radio CRS broadcasts - plus a neural model trained on them to cover everything those broadcasts never said. She is not a weather-radio voice by design. Mara began on the Microsoft Speech Server 2004 Beta alongside Tom and was replaced by Jill, so SpeechWorks built her as Jill's class: general purpose. The weather _skew_ is what survived, not the _intent_.

Aside: if you need the SPR for the word "cloudy", in the case CRS Mara does not emphasize it correctly, here is the best version I found: `\![1klWDi]`

---

## Files Shipped

| file | bytes | md5 |
|---|---|---|
| `crsmara.vin` | 39,425,094 | `de43022a498f28bb61afc14bd5ac98df` |
| `crsmara8.vdb` | 94,751,658 | `6af979cb698fe1c9d829ef1ae1815d47` |
| `crsmara.vcf` | 53,152 | `106effa23e12901c361a72a7db0007da` |
| `crsmara8.xml` | 1,924 | `f7053e074ae447929b82a24ca66adb8a` |

Every file is under **100 MB**, and for good reason. The repo attempts to keep all voices under 100MB because the WASM build (hosted on GH Pages) fetches the voice directly. That constraint is a correctness property here, not housekeeping, and it is the reason for the compaction step in the recipe below.

Self-contained: `MISSING_JOIN_COST = 1.0` is written into the `.vcf`, so the voice renders correctly with no environment variables set. Verified - the
shipped triple renders `nws_warning.txt` byte-identically to the approved `SPFY_MISSING_JOIN=1` build arm.

`spfy_vb_verify.exe --expect-clean`: **46 passed, 0 failed.**

⛔ **The `.vin` was repadded on 2026-08-22** - the hash chunk grew 52,048 bytes and the md5 above changed with it. Nothing else moved: the padding is empty cells, and 44 probe renders came back byte-identical to the pre-pad build through `spfy_synth`. See [The hash tail](#the-hash-tail-a-shipping-defect-that-only-bare-speechify-can-see) below; if you are holding an older copy of this voice, it can access-violate inside `SWIttsUSel.dll` and you want this one.

`build_tools/` holds every script and word list the recipe below names, plus its own README. The recipe references them by filename because that is where they are - nothing in it points at a path on the machine she was built on.

---

## What she is made of

| source | recordings | what it is |
|---|---|---|
| CRS broadcast | 304 | real Mara, MP3 off NWR archives, 16 kHz |
| StyleTTS2 | 9,382 | fine-tuned on those 304, general + weather text |
| **used after `--drop`** | **7,719** | 1,293,926 half-phone units |

The 304 are the whole ground truth. Everything else exists because 304 recordings of mostly marine forecasts cannot say "the members of the panel had never seen the footage before".

---

## THE RECIPE

Five steps. Two builds, because the cut has to be measured on the uncut voice. Full build ~4 min, compacted build ~1 min, on an AMD Ryzen 9 7900X with 20 OpenMP threads allocated of 24 total threads.

**Every tool and list the recipe names ships in `build_tools/` beside this file.** Nothing below points outside the repo except `$Work` - one directory of your choosing for the build arms, and the only line you have to edit.

Run the following from PowerShell, with `en-US/crsmara/` as the current working directory:

```powershell
$Work  = if ($env:SPFY4_SCRATCH) { $env:SPFY4_SCRATCH } else { "%TEMP%" }
$Bld   = "$Work\spfy_build32\src\cli"     # wherever you built spfy
$B     = "$Bld\spfy_vb_build.exe"
$V     = "$Bld\spfy_vb_verify.exe"
$PY    = "python"                         # 3.12 with numpy
$T     = "build_tools"
$D     = "build_tools\data"
$env:OMP_NUM_THREADS = "20"
```

`build_tools\paths.py` prints which of those resolve on your machine - run it **first** if something cannot be found.

### 0. Inputs

Shipped in `build_tools/data/`:

    keep_words_wx.txt        words the cut must keep WHOLE
    gate_rare_both.txt       words withheld from preselection
    drop_it2.txt             recordings withheld outright
    texts_brown.txt          2,588 general-prose lines; 1,588 held out
    arpa_fekey_px2.dict      9,511 headwords, 0 OOV

NOT shipped on GitHub, but available directly from [Wags' Puzzle Space](https://wagspuzzle.space/mara/crsmara/audio.7z):

    px2_audio                     9,686 .wav + .txt .lab .fe .sp .f0
    donnast2_tg8_px2_flat_ob      TextGrids, MFA 3.3.9 english_us_arpa

WARNING: The dictionary is keyed on the **FE spelling**, not the .txt word. Key it on the .txt and every word the front end respells loses its headword and aligns to `spn` (1.21% token OOV against 0.00%).

### 1. Full build

```powershell
& $B --voice crsmara `
    --wav-dir "$Work\px2_audio" `
    --tg-dir "$Work\donnast2_tg8_px2_flat_ob" `
    --out-dir "$Work\crsmara_px2O" `
    --template-vin "..\jill\jill.vin" `
    --template-vdb "..\jill\jill8.vdb" `
    --rvc-policy prefer-real --syn-anchors word `
    --dur-floor-pct 10.0 --dur-ceil-pct 99.0 `
    --f0 joinonly `
    --k-best 16 --prsl-backoff 12 --prsl-gate bigram `
    --gate-words "$D\gate_rare_both.txt" --gate-sole keep `
    --drop "$D\drop_it2.txt"
```

Result: 165.9 MB VIN, 494.1 MB VDB. Unshippable as-is, but also the best-sounding thing the entire project produced. The cut exists to keep that sound at a tenth the size.

WARNING: `--template-vin jill` supplies exactly one chunk that is still hers: `ccos`. See *Ownership* below. `--no-template` builds with no donor file at all and ships a zero `ccos`, which is audibly worse.

### 2. Sample demand in the regime the voice ships in

`MISSING_JOIN_COST=1` changes **which** units the selector picks, so demand measured at the default 1000 protects the wrong ones. Make a VCF-only copy:

```powershell
& $PY "$T\vb_vcfarm.py" `
    --voice "$Work\crsmara_px2O" --set MISSING_JOIN_COST=1 `
    --name px2O_m1 --out "$Work\miss_ab" --add
```

### 3. Measure what real speech actually asks for

```powershell
& $PY "$T\vb_pickmap.py"   --voice "$Work\miss_ab\px2O_m1" `
    --texts "$D\texts_brown.txt" --skip 1000 `
    --workers 24 --out "$Work\picks_px2O_m1.json"

& $PY "$T\vb_demandmap.py" --voice "$Work\miss_ab\px2O_m1" `
    --texts "$D\texts_brown.txt" --skip 1000 `
    --workers 24 --out "$Work\demand_px2O_m1.json"
```

1,588 held-out [Brown lines](https://en.wikipedia.org/wiki/Brown_Corpus) -> 58,310 units ever picked (4.5%), 26,298 of 95,868 contexts ever visited (27.4%).

WARNING: **Held out means held out.** Text folded into the corpus asks for exactly the contexts that text created, and calling that a measurement is circular.

### 4. Build the keep-span list

```powershell
& $PY "$T\vb_keepspans.py" --voice "$Work\crsmara_px2O" `
    --picks  "$Work\picks_px2O_m1.json" `
    --demand "$Work\demand_px2O_m1.json" `
    --floor 5 --cold-floor 2 `
    --keep-words "$D\keep_words_wx.txt" `
    --out "$Work\keep_px2O_f5c2w.tsv"
```

Four things decide what survives, in order:

| | units | why |
|---|---|---|
| picks | 58,310 | the selector demonstrably chose it |
| `--keep-words` | +214 | **named words survive whole, picked or not** |
| whole-word rounding | +118,919 | a half-kept `_WORD_` anchor is a dead anchor |
| demand-weighted floor | +52,468 | 5 candidates if visited, 2 if not |
| | **229,911** | 17.8%, 94.0 MB of audio |

Floor 5 / cold 2 is the highest setting that leaves margin under 100 MB. Floor 6 / cold 2 projects to 100.0 MB - the wrong side of the line to trust.

IMPORTANT NOTE: `--keep-words` is not optional and it is not cosmetic. The demand sample is general prose; `tornado` has 13 word anchors in this corpus and Brown touches **none** of them, so without the list, the builder drops all 13 and the selector splices the word out of half-phones. It is VERY audible. 214 units, 0.09 MB.

### 5. Rebuild with the cut, then ship

Step 1 again, plus the keep list, into a new directory:

```powershell
& $B --voice crsmara `
    --wav-dir "$Work\px2_audio" `
    --tg-dir "$Work\donnast2_tg8_px2_flat_ob" `
    --out-dir "$Work\crsmara_px2Ow" `
    --template-vin "..\jill\jill.vin" `
    --template-vdb "..\jill\jill8.vdb" `
    --rvc-policy prefer-real --syn-anchors word `
    --dur-floor-pct 10.0 --dur-ceil-pct 99.0 `
    --f0 joinonly `
    --k-best 16 --prsl-backoff 12 --prsl-gate bigram `
    --gate-words "$D\gate_rare_both.txt" --gate-sole keep `
    --drop "$D\drop_it2.txt" `
    --compress "$Work\keep_px2O_f5c2w.tsv"

& $V --vin "$Work\crsmara_px2Ow\crsmara.vin" `
     --vdb "$Work\crsmara_px2Ow\crsmara8.vdb" --expect-clean
```

The build writes a `.vcf` named **Jill** with no `MISSING_JOIN_COST` - the builder's `--vcf-set` refuses any key the embedded en-US payload does not already carry (`vb_vcf.c:99`, deliberately: a typo that changed nothing would look exactly like a weight that is inert). One pass fixes both:

```powershell
& $PY "$T\vb_shipvcf.py" `
    --in  "$Work\crsmara_px2Ow\crsmara.vcf" `
    --out "crsmara.vcf" `
    --set name=CRS_Mara --set MISSING_JOIN_COST=1.0

Copy-Item "$Work\crsmara_px2Ow\crsmara.vin",
          "$Work\crsmara_px2Ow\crsmara8.vdb",
          "$Work\crsmara_px2Ow\crsmara8.xml" .
```

That reproduces the shipped `.vcf` byte for byte - md5 `106effa23e12901c361a72a7db0007da`.

WARNING: No shipped SpeechWorks voice sets `MISSING_JOIN_COST`, so the name is attested from the engine (`SWIttsUSel.dll FUN_08e90dc0` stores 1000.0 at
`cfg+0x84`; the config read at `08e9122b` looks it up by name) and not from another voice file. `spfy_synth` reads it. A real Speechify **server** may still reject the VCF against its DTD - if it does, drop the param and set `SPFY_MISSING_JOIN=1` in the environment instead.

---

## What it took

Condensed. Each line is a thing that was tried, measured, and settled across a weeklong period.

- **The corpus was the hard part, twice.**
  - Zero-shot cloning was the wrong tool - Vevo and F5 both imported a foreign accent (one was heard as *Australian*) because a 5-second prompt cannot outvote a 101k-hour multilingual prior. Fine-tuning fixed that class of failure outright. Then the fine-tuned corpus was pitch-flat: 1.40 st of register separation per line against her real 6.22. Shifting the reference bank ±4 st through WORLD - F0 scaled, spectral envelope untouched - transferred at +0.772 st/st and took that to **6.50 st**, nothing collapsed.

- **Two corpora were built and one was dropped.**
  - AT&T Natural Voices "Crystal" brought 5.6 real hours with 7.68 st of natural spread. It lost anyway: brighter bank, 2.5x the 3-4 kHz energy, and *"she only hurts things."* The flat StyleTTS2 corpus won because grown duration and F0 trees have the most to give where the inventory is uniform - and because it carries 72,996 word anchors to Crystal's 49,514.

- **The selector, not the inventory, spends the variety.**
  - Three arms off one corpus, one unit table, one set of TextGrids, only the F0 flag differing: `--f0 joinonly` **0.551** `>2 st/s`, `--f0 absent` 1.168, F0 in the join cost 0.742. Same inventory in all three, so the selector is what spends the variety - the widest arm with the flag off got none of the benefit.

- **The containers stopped being borrowed.**
  - `durt` and `f0tr` are grown from this corpus over our own question inventory and beat each vendor's own tree on its own voice, 4 of 4. `feat` is 1,281 bytes of en-US language table proven byte-identical between two vendor voices and embedded in the builder. The `.vcf` and the `8.xml` sidecar are written, not copied. The join-table packer was rewritten around an occupancy bitmap - fill 31.0% -> 84.4% here, and on jill's own pairs 84.1% against the 71.8% she ships, so it is denser than the vendor's.

- **Two things nearly shipped wrong, and one metric lied twice.**
  - A CART leaf's second float is `1/sd`, not variance - writing the variance made the duration cost ~10^6x too large while 42 checks passed and the trees round-tripped clean. And `>2 st/s` twice ranked the worse arm first: once crowning a zero-`ccos` build that was dropping the half-phones carrying the words, once placing a rejected arm above the one called the best of the project. **The gates catch gross regressions. The ear decides.**

- **Then it got small.**
  - 1,293,926 units -> 229,911. VIN 165.9 -> 39.4 MB, VDB 494.1 -> 94.8 MB. The cut costs 18% more joins and 0.04 more `>2 st/s`. It also raised the accent on "NAtional" from +3.97 to +7.80 semitones, which is the direction the voice is supposed to go - the flat reading was the defect, not the rise.

---

## Ownership

Everything in these containers is generated by `spfy/src/vb` from this corpus, with one exception: **`ccos` is jill's, byte-identical, on purpose.** Three arms off one voice differing only in that table were judged by ear and hers won - ours regressed, a zeroed one regressed badly. `ccos` prices phone substitution, which is a property of the language rather than of the speaker, so it is a chunk to reproduce, not to replace. Our own estimator reaches r=0.146 against a split-half ceiling of 0.82; until that closes, hers stays.

`unit.context_cost` is 0 everywhere. It is phone-level and runs in ~5-phone blocks, and nothing available predicts it (best predictor 67.3% against a 44% base rate). Inventing a rule would be worse than leaving it.

---

## The hash tail, a shipping defect that only bare Speechify can see

Both CRS voices shipped with a `hash` chunk a few thousand cells too short, and it took an access violation inside `SWIttsUSel.dll` to find it. Worth reading before you build another voice, because **no test in this repo could have caught it and our own engine renders the broken file perfectly.**

The join-cost table is a row-displacement structure: `idx = rows[uid_right] + uid_left`, and a cell belongs to the row its stored validator names. The vendor's lookup, `SWIttsUSel.dll+0xb7e6`, is

```
cmp [cells + (rows[uid_right] + uid_left)*8], uid_right
```

with **no comparison against `n_cells` in front of it**. The key check *is* the miss test, and it happens after the read. Our `spfy_hash_lookup` guards the index and returns a miss; theirs reads the memory. So a table sized to its last *populated* cell is correct for every hit, correct for our engine on every miss, and a wild read for their engine on a miss whose index runs past the allocation.

crstom died on `attention signal.` - `rows[222144]` is 4,449,427, `uid_left` 278,391, sum 4,727,818 against 4,724,617 cells, reading 25,616 bytes past a 37,797,888-byte block. crsmara was 6,506 cells short of the same bound and had simply not been asked yet.

The correct width is not a margin to pick. **Every vendor voice satisfies `n_cells == max(rows[]) + n_rows` exactly:**

| voice | max(rows) | n_rows | n_cells | delta |
|---|---|---|---|---|
| tom | 1,724,291 | 692,190 | 2,416,481 | +0 |
| jill | 2,059,585 | 560,534 | 2,620,119 | +0 |
| javier | 1,638,488 | 668,348 | 2,306,836 | +0 |
| paulina | 1,367,589 | 663,410 | 2,030,999 | +0 |
| felix | 2,906,700 | 737,394 | 3,644,094 | +0 |

`spfy_hash_build` now pads to that at build time, `spfy_vb_verify` gates it (check 46, `hash tail absorbs the widest probe`), and `vb_hashpad.py --write` repairs a VIN already built. The padding is inert by construction - every added cell carries the empty key - and 88 probe renders across both voices came back byte-identical to the pre-pad builds.

The lesson generalizes past this chunk: **the vendor engine trusts the container in places our engine defends itself**, so "spfy_synth renders it" is not evidence that Speechify will. A container change wants a bare-Speechify soak, not just a verify pass.

---

## Known limits

- The corpus is weather-skewed. General vocabulary is covered by the neural renders, not by her.
- 82% of the audio is gone. Words far outside both the corpus and the Brown demand sample get spliced from half-phones. If one of them matters, add it to `keep_words_wx.txt` and rebuild - it costs kilobytes.
- 8 kHz u-law. The source MP3s carry real energy to 6,285 Hz that an 8 kHz container cannot hold.
