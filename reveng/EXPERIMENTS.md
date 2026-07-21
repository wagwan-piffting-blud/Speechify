# Mara Voice Quality Experiments Log

Tracks what has been tried, what worked, what didn't, and what remains untried.
Updated each time a new approach is tested.

---

## Goal
Reduce Mara's ~67% recording-switch rate during synthesis, improving timbral
continuity and reducing audible "stitching" artifacts.

## Baseline
- Recording-switch rate: **66.7%** (26/39 speech-to-speech joins)
- Mean run length: 1.5 units before switching recordings
- Test sentence: "Please call us at five five five, zero one two three."

---

## TRIED - FAILED

### 1. Rebuild hash with same-recording-only pairs (fresh chain builder)
- **Date**: 2026-03-14
- **What**: build_mara_hash.py generated same-recording (uid_left, uid_right) pairs,
  computed MFCC spectral distances, built fresh hash chains from scratch.
- **Variants tested**:
  - Windowed same-rec pairs (PAIR_WINDOW neighbors): 1,922,604 cells, 18.1 MB hash
  - Forward-lp-only pairs (j > i constraint): 1,039,283 cells, 11.1 MB hash
  - Tom's exact pairs rebuilt through fresh builder: same data, new chain structure
- **Result**: ALL crash the engine. Silent crash during SWIttsUSelUnitSelection.
- **Root cause**: Chain builder output is structurally incompatible with the engine.
  Even Tom's exact pairs crash when rebuilt through our builder. The binary layout
  differs from Tom's original in ways that the engine cannot handle.
- **Structural differences from Tom's hash**:
  - No suffix sharing (Tom uses extensive suffix sharing: 134,277 distinct starts for 159,982 non-empty rows)
  - Different n_cells count
  - No trailing padding sentinels (Tom has ~534K)
  - No empty chains (Tom has 4,339 rows pointing directly to sentinel)
  - Chain entries sorted by uid_left (Tom's ordering unknown)
- **Unknown**: Which specific difference causes the crash. Could be suffix sharing
  requirement, n_cells bounds, or something else entirely.

### 2. Copy Tom's hash structure, patch cells_B costs (same-rec bias)
- **Date**: 2026-03-14
- **What**: Copied Tom's exact binary hash (rows, cells_A, suffix sharing, everything).
  Only modified cells_B: same-rec pairs -> 0.0, cross-rec pairs -> 8.0.
- **Result**: Engine runs fine (no crash). But **zero effect on switch rate** (still 66.7%).
- **Root cause**: Tom's hash contains Tom's (uid_left, uid_right) pairs. These are ~90%
  cross-recording pairs (for Tom's recordings). Mara's same-recording pairs are simply
  NOT in Tom's hash. They still get MISSING_JOIN_COST = 1000.
- **Conclusion**: Modifying costs of existing pairs is useless when the needed pairs
  aren't in the hash.

### 3. use_edgeframes=1 (runtime join cost from ccos vectors)
- **Date**: 2026-03-14 (tried by user)
- **What**: Changed VCF/XML config to use_joincache=0, use_edgeframes=1 to compute
  join costs at runtime from ccos boundary vectors instead of hash lookup.
- **Result**: Did NOT work (per user report).
- **Details**: Unknown specifics -- may have crashed or produced bad output.

### 4. Set JOIN_COST_WEIGHT=0.0 in VCF
- **Date**: 2026-03-14
- **What**: Set JOIN_COST_WEIGHT from 0.7 to 0.0 in mara.vcf, eliminating ALL join
  cost influence (including MISSING_JOIN_COST=1000 penalty for non-hash pairs).
- **Result**: **Zero effect on switch rate** (still 66.7%, 26/39 switches).
  Nearly identical unit selections as baseline.
- **Conclusion**: **Join cost does NOT drive recording switches.** The switches are
  caused by target cost (duration + F0 prediction) and context cost, which select the
  best phonetic match per-halfphone regardless of recording continuity.
- **Key insight**: The problem is NOT in the cost function weighting. The engine picks
  the phonetically best unit for each position, and those units happen to be spread
  across many recordings. No amount of join cost tuning will change this because the
  per-unit target/context costs dominate selection.

### 5. Front-load same-recording candidates in prsl
- **Date**: 2026-03-14
- **What**: Modified prsl generation to put same-recording clusters first (up to 120
  slots), sorted by largest cluster. Tom's curated candidates fill remaining slots.
- **Result**: **Made things WORSE** -- 84.6% switches (up from 66.7% baseline).
- **Root cause**: The engine evaluates ALL candidates by cost regardless of prsl order.
  Front-loading same-recording units displaced Tom's better-quality curated candidates,
  and the Mara adjacency candidates often had worse target/context costs. Example:
  HP 24 picked uid 56289 (dl/t=5.0x, cost=6.743) vs baseline uid 24534 (dl/t=1.0,
  cost=0.131).
- **Key insight**: prsl candidate ORDER doesn't matter -- the engine scores all candidates
  and picks the lowest cost. Adding low-quality same-recording candidates just dilutes
  the pool. Tom's curated candidates were specifically selected for low concatenation
  cost by SpeechWorks' original tools.
- **REVERTED**: Restored original Tom-first prsl generation.

### 6. Frida runtime recording-switch penalty (pre-prune score injection)
- **Date**: 2026-03-14
- **What**: Frida hook on Viterbi prune function (0x8E88830). Before pruning, adds a
  flat penalty to total_score of candidates whose file_idx differs from the previous
  winner's recording. Tested penalties: 0 (baseline), 5.0, 50.0.
- **Results**:
  - penalty=0.0 (baseline): 78.6% switches (55/70 joins, 72 halfphones, 2 USEL calls)
  - penalty=5.0: **74.3% switches** (52/70) -- best result, 4.3pp improvement
  - penalty=50.0: **80.0% switches** (56/70) -- WORSE than baseline
- **Root cause of modest improvement**: Only 40-53% of positions have ANY same-recording
  candidate in the prsl pool. At positions without same-rec candidates, penalty applies
  uniformly to all candidates, changing nothing.
- **Root cause of penalty=50 regression**: Large penalties compress relative score
  differences (0.5 vs 50.5 instead of 0.5 vs 1.0), distorting the adaptive histogram
  pruning (HALFPHONE_CAND_PRUNE_THRESH=0.95). More aggressive pruning eliminates good
  candidates that would have survived at natural score scales.
- **Recording coverage analysis**: Best single recording covers only 34/46 phones and
  10% of prsl context groups. Median recording has 9 phones, 19 units. The data is
  inherently fragmented across 6849 short recordings.
- **Conclusion**: Penalty approach ceiling is ~4pp improvement. The recording-switch
  problem is fundamentally a data coverage issue, not a scoring issue.

### 9. DLL code cave: pre-prune penalty (SWIttsUSel.dll patching)
- **Date**: 2026-03-15
- **What**: Binary patch of SWIttsUSel.dll. 166-byte code cave in .text section gap
  (VA 0x8E95AC6, 1338 bytes available). Redirects prune call at 0x8E89397 to cave.
  Cave adds penalty to candidate total_score (+0x04) for non-matching recordings
  BEFORE calling prune, then captures winner's file_idx for next HP.
  BSS storage: prevBestFidx at 0x8E9D5F0, init_flag at 0x8E9D5F4.
  .text VSize expanded from 0x14AC6 to 0x15000 (safe: SizeOfCode already 0x15000).
- **Progressive testing** (tests A-G):
  - Test D (wrapper only, no penalty): 44,144 bytes -- PASS
  - Test E (save/restore + wrapper): 44,144 bytes -- PASS
  - Test F (full cave, penalty=0.0): 44,144 bytes -- PASS (proves cave mechanics correct)
  - Test G (full cave, penalty=5.0): 800 bytes -- FAIL (WSOLA truncation)
- **Root cause discovered via Frida WSOLA diagnostic**:
  - Prune threshold (HALFPHONE_CAND_PRUNE_THRESH=0.950) is **ABSOLUTE**, not relative.
  - Candidates with total_score >= 0.95 are eliminated by prune.
  - Adding penalty BEFORE prune pushes non-matching candidates above 0.95.
  - When no same-recording candidate exists for an HP, ALL candidates get penalized,
    ALL exceed 0.95 threshold, prune eliminates ALL -> 0 candidates survive.
  - Viterbi backtrack with 0-candidate HPs produces sentinel uid=169578 (N_UNITS-1), dl=0.
  - WSOLA receives 1 zero-duration sentinel unit instead of 20-32 real units.
  - Confirmed by Frida: original HP0 51->2 survivors; broken HP0 51->0 survivors.
- **Penalty sweep results** (pre-prune): 0.05=44K, 0.1=38K, 0.5=31K, 0.6=28K, 1.0=800.
  Degradation is gradual as more HPs lose all candidates above 0.95 threshold.
- **Scripts**: c:/tmp/patch_dll_sweep.py, c:/tmp/patch_dll_test.py, c:/tmp/patch_dll_test2.py

### 10. DLL code cave: post-prune penalty (v2)
- **Date**: 2026-03-15
- **What**: Restructured cave to call prune FIRST with original scores, THEN add
  penalty to post-prune survivors. Penalty loop uses [esi+0x00] (post-prune count)
  instead of [esi+0x14] (pre-prune count). 165-byte cave.
- **Result**: ALL penalty values (0.5 through 10.0) produce full-length audio (44,144
  bytes). No more WSOLA truncation. **BUT zero effect on unit selection.**
  Switch rate with penalty=10.0: 70.3% (45/64) -- identical to original DLL.
  Per-halfphone UIDs: identical to unpatched engine.
- **Root cause**: The Viterbi forward pass (0x8E8EDD0) does NOT read total_score
  from candidate+0x04. It recomputes path costs from component score fields and/or
  join cost hash lookups. Modifying +0x04 after prune is invisible to the Viterbi.
- **Conclusion**: To affect Viterbi path selection, the penalty must be injected into
  the Viterbi forward pass inner loop itself (where cumulative cost is computed from
  predecessor.cumulative + join_cost + target_cost). This requires disassembling the
  Viterbi inner loop at 0x8E8EDD0 to find the exact cost accumulation point.
- **Script**: c:/tmp/patch_dll_v2.py

### 7. Fix durt tree phone_right offset bug + CART leaf recomputation
- **Date**: 2026-03-14
- **What**: Fixed build_mara_trees.py reading phone_right from offset 0x19 (phone_ctx[2])
  instead of 0x18 (phone_ctx[1]). This caused all 169,579 units to be routed through durt
  trees with the wrong phone_right feature, landing in wrong leaves. Recomputed 543/825
  durt leaves from Mara's actual f0_context values.
- **Result**: Switch rate 69.2% (diag_stutter) -- modest improvement from ~78.6% pre-fix.
  Audio still largely garbled with frequent recording switches.

### 8. CART tree variance calibration (mean-only patching)
- **Date**: 2026-03-14
- **What**: Calibrated tree variance convention by routing Tom's 169,579 units through
  Tom's own durt trees and comparing stored leaf variance to actual stddev of f0_context
  values reaching each leaf. Script: `c:/tmp/calibrate_tree_variance.py`.
- **Key finding**: Tom's durt tree variance is **~constant (~0.052-0.057)** across all
  543 leaves regardless of actual stddev (R^2=0.03, log-log exponent=-0.16). The variance
  field is a tuned cost sensitivity parameter, NOT a statistical property (stddev or 1/stddev).
- **Fix**: Removed variance patching from build_mara_trees.py. Now only leaf means are
  recomputed from Mara's data; Tom's variance values are preserved unchanged.
  Previously: `new_var = 0.005 * actual_stddev` (wrong -- imposed a proportional
  relationship that doesn't exist in Tom's trees).
- **Result**: **70.3% switch rate** (45/64 speech joins, 72 HPs across 2 USEL calls).
  No meaningful change from experiment 7. The old DURT_VAR_K=0.005 happened to produce
  values in a similar range by accident (0.005*17=0.085 vs Tom's mean 0.057).
- **Note**: Earlier measurement of 44 HPs was due to diag_stutter.py bug (only reading
  USEL call 1). Fixed: now collects all USEL calls. Full sentence = 72 HPs / 2 calls.
- **WSOLA stretch correction**: dl/t ratios > 2x are NOT a quality problem. dl is source
  material extent (how much audio WSOLA reads), NOT output duration. Larger dl = more
  source material = better WSOLA quality. The prosody model (durt trees) determines
  output duration separately. The inflation step in build_mara_voice.py correctly
  ensures dl >= tom_dl for adequate source material.
- **Conclusion**: Tree patching (means + variance) is now correct but does not
  materially improve quality. The dominant issue is fundamental recording
  fragmentation from data coverage limitations (70% of runs are single units,
  max run 5 HPs).

---

## TRIED - WORKED

### A. Copy Tom's hash verbatim (build_mara_rest.py)
- **Date**: 2026-03-13
- **What**: build_mara_rest.py copies Tom's entire hash chunk byte-for-byte into mara.vin.
- **Result**: Engine runs fine. Baseline quality. 66.7% switch rate.
- **Conclusion**: Tom's hash structure is valid but doesn't help Mara since the pairs
  are for Tom's recordings.

### B. Copy Tom's hash structure, modify only cells_B (no crash)
- **Date**: 2026-03-14
- **What**: Same as experiment 2 above. Proves we CAN modify f32 cost values without
  crashing, as long as the structural bytes (rows, cells_A) are identical to Tom's.

---

## NOT YET TRIED

### i. Modify prsl to heavily favor same-recording candidates
- **Status**: DEPRIORITIZED -- experiment 6 proved that even with 40-53% of positions
  having same-rec candidates, the coverage ceiling limits improvement to ~4pp. The best
  single recording covers only 10% of prsl groups. Flooding prsl with same-rec candidates
  (experiment 5) made things worse by displacing Tom's curated ones.

### ii. Debug chain builder binary diff
- **Rationale**: Write a byte-level comparison of Tom's hash vs our rebuilt version to
  find the EXACT structural difference that crashes the engine. If we can match Tom's
  binary format, we can build custom hash chains with Mara's same-recording pairs.
- **Priority**: LOW -- even if we fix the builder, experiment 4 proved join cost doesn't
  drive switches. Hash changes alone won't help.
- **Effort**: High (reverse engineering).

### iii. Build Mara-specific ccos boundary vectors
- **Rationale**: If use_edgeframes mode can be made to work, Mara-specific ccos vectors
  would give the engine accurate spectral distance computation at runtime, naturally
  favoring same-recording joins.
- **Priority**: LOW -- join cost doesn't drive switches (experiment 4).
- **Prerequisite**: Understand why use_edgeframes=1 failed (experiment 3).

### iv. Reduce MISSING_JOIN_COST via DLL patching
- **Status**: MOOT -- the 1000.0f at 0x8E99220 is actually a hash-loading SCALE FACTOR
  (multiplies [0,1] stored costs to [0,1000] range), NOT the missing cost sentinel.
  The actual missing cost is 10000.0f, hardcoded as inline mov instructions.
  And experiment 4 proved join cost weight=0 has no effect regardless.

### v. Hybrid: Tom's hash + inject same-rec pairs into unused sentinel slots
- **Priority**: MOOT -- join cost doesn't drive switches.

### vi. Increase CONTEXT_COST_WEIGHT or add same-recording bonus to context cost
- **Rationale**: Context cost is one of the dominant cost components. If we can make
  it recording-aware (penalize recording switches), it would directly influence selection.
- **Risk**: Context cost may be computed from phone context features only, with no
  recording-awareness hook available.
- **Effort**: Requires understanding context cost computation in detail.

### vii. Modify unit metadata to make same-recording units score better on target cost
- **Status**: DONE (experiments 7+8) -- tree leaf means recomputed from Mara data,
  variance kept from Tom. No material quality improvement.

### viii. ~~Investigate and reduce extreme WSOLA stretching~~
- **Status**: INVESTIGATED, NOT AN ISSUE. dl is source material extent (how much audio
  WSOLA reads from VDB), NOT output duration. Large dl/t ratios mean MORE source material
  = better WSOLA quality. The prosody model determines output duration separately.
  The inflation step in build_mara_voice.py already ensures dl >= tom_dl.
  8.3% of units have dl > 2x Tom's -- these are cases where Mara segments are genuinely
  longer, giving WSOLA more material to work with (good, not bad).

---

## Key Technical Constraints
- Hash chain builder (fresh chains from scratch) crashes the engine -- root cause unknown
- Copying Tom's binary hash structure and modifying only costs is safe (no crash)
- **Join cost does NOT drive recording switches** (proven by experiment 4)
- Target cost + context cost are the dominant factors in unit selection
- prsl preselection cache controls candidate set -- most direct lever for recording continuity
- use_joincache=1 is the current mode; use_edgeframes=1 already tried and failed
- **Recording-switch problem is fundamentally a data coverage issue**: 6849 recordings,
  median 9 phones and 19 units each. Best recording covers 34/46 phones, 10% of prsl
  groups. No scoring trick can fix insufficient phonetic coverage per recording.
- **Optimal Frida penalty ~5.0** (pre-prune injection); larger penalties distort pruning
- **MISSING_JOIN_COST is 10000.0f** (inline), not the 1000.0f at 0x8E99220 (scale factor)
- **1338 bytes free** at end of .text section (VA 0x8E95AC6) for code cave if needed
- **CART tree variance is constant** (~0.052-0.057): tuned cost sensitivity, not statistical.
  Proven by calibrating Tom's units through Tom's trees (R^2=0.03 vs stddev). Mean-only
  patching is the correct approach for new voices.
- **dl is source material extent, NOT output duration**: Large dl/t ratios are benign (more
  source material for WSOLA). Output duration is determined by prosody model (durt trees).
- **diag_stutter.py bug fixed**: Was only reading USEL call 1; now collects all calls.
  Full test sentence = 72 HPs across 2 USEL calls.

### DLL patching findings (2026-03-15)
- **Prune threshold is ABSOLUTE**: HALFPHONE_CAND_PRUNE_THRESH=0.950 eliminates candidates
  with total_score >= 0.95. This is NOT relative to the best candidate. Injecting additive
  penalties before prune causes catastrophic candidate elimination.
- **Viterbi ignores total_score (+0x04)**: The Viterbi forward pass (0x8E8EDD0) does NOT
  read the total_score field at candidate+0x04. Modifying it post-prune has zero effect.
  The Viterbi recomputes costs from component fields and/or join cost lookups.
- **Code cave infrastructure is proven**: .text VSize expansion, call redirection, BSS
  storage, prune delegation all work correctly. The cave mechanics are sound; only the
  injection point (pre-prune vs Viterbi) matters.

### Unit selection pipeline (confirmed 2026-03-15)
```
0x8E920F0  Scoring loop (per HP: inner scorer + prune)
0x8E88DE0  Inner scorer (scores all candidates, stride 0x18)
0x8E88830  Prune (__thiscall, ret 0x10; histogram beam, thresh=0.95 ABSOLUTE)
0x8E8D210  Post-scoring adjustments (may recompute total_score)
0x8E8EDD0  Viterbi forward pass Mode A (exhaustive, NO beam)
0x8E8B620  Viterbi forward pass Mode B (without join cost)
0x8E8ED20  Join cost calculator (spectral distance)
0x8E8B580  Heapsort (sorts candidates by cumulative +0x20)
0x8E8DE20  Backtrack (follows predecessor +0x24 pointers, builds output)
```
- Candidate struct: +0x00=uid, +0x04=total_score, +0x08..+0x1C=components,
  +0x20=cumulative_score (Viterbi), +0x24=predecessor_ptr (Viterbi)
- To affect path selection, penalty must be injected into the Viterbi forward pass
  inner loop where cumulative cost is computed (join_cost + target recomputation).

---

## BREAKTHROUGH: Diagnostic Methodology Error (2026-03-15)

### 11. diag_stutter.py captures pre-prune best, NOT Viterbi-selected path
- **Date**: 2026-03-15
- **What**: Built `diag_ground_truth.py` that hooks BOTH the prune function (pre-prune best)
  AND the WSOLA concat input (actual Viterbi path, at arg4+0x08 array, arg4+0x04 count,
  stride 0x18).
- **Result**: **Only 31-37% of UIDs match** between pre-prune best and Viterbi-selected path.
  All prior analysis (experiments 1-10) was analyzing the wrong units.
- **True recording switch rate**: ~42% (27-30 switches), not the reported 70%
- **True phone match rate**: 93-100% (Viterbi almost always picks correct phone)
- **WSOLA unit list +0x04 field**: group count (non-zero = start of same-recording run)
- **Key insight**: The Viterbi forward pass recomputes costs from component fields + join cost
  hash lookups. It does NOT read total_score at candidate+0x04. The pre-prune best (lowest
  total_score) is often NOT on the optimal Viterbi path.

### 12. Content mismatch audit: 52.5% of recordings have wrong content
- **Date**: 2026-03-15
- **What**: Per-unit RMS correlation between Tom and Mara VDBs across all 6,744 recordings.
- **Result**: Bad (r < 0.3): 3,540 recordings (52.5%), 106,024 units
  Suspect (0.3-0.7): 2,448 recordings (36.3%), 55,228 units
  Good (r >= 0.7): 756 recordings (11.2%), 8,196 units
- **Root cause discovered**: Tom's VDB recordings are pre-cut FRAGMENTS (cut at source by
  SpeechWorks). Example: Tom's fragment contains "please...right" but only "right" is labeled
  in the ckls word chunk. Mara's Qwen re-synth was based on transcribing what was heard, so
  Mara says just "right". The piecewise warp mapped Tom's "p l iy z" (please) units into
  Mara's "r ay t" (right) audio -- completely wrong phonetic content.
- **Note**: RMS correlation is a crude metric. Low correlation can also mean different vocal
  characteristics (female vs male-derived) rather than wrong content. But confirmed cases
  like dip5_009 show genuine content mismatch.

### 13. Direct phone-boundary mapping (STATE_VERSION 36-38)
- **Date**: 2026-03-15
- **What**: Replaced the piecewise-linear warp in build_mara_voice.py with direct MFA phone
  boundary placement. For each matched phone group, units are placed proportionally within the
  MFA-detected phone interval. Unmatched phone groups (phone not in Mara's MFA) get dl=0.
  Also added exact-phone filter (reject seq_align matches where phones differ after normalization).
- **Critical fix**: Post-processing (dl inflation, LP gap closure, silence relocation) was
  overriding the MFA-based positions. These steps now skip MFA-mapped units entirely.
- **MFA coverage**: Obtained TextGrids for 1,189 additional recordings (MFA retry with
  beam=400, retry_beam=1000). Final coverage: 99% MFA seq exact, 0% energy-region fallback.
- **Current status (STATE_VERSION 38)**: dl extends to END of MFA phone interval (overlapping
  source regions, matching the MFA stitch approach). Smoother output, no harsh cuts, but still
  missing some phonemes.

### 14. MFA stitch proof-of-concept
- **Date**: 2026-03-15
- **What**: `diag_mfa_stitch.py` bypasses the unit table entirely. Takes MFA phone boundaries
  directly from TextGrids, extracts audio from VDB, concatenates.
- **Result**: "Way better" -- clearly intelligible words, slightly choppy at phone boundaries.
- **Key insight**: Mara's audio IS good enough for concatenative synthesis. The problem is in
  how the unit table maps to the audio, not in the audio quality itself.

---

## Key Technical Discoveries (2026-03-15)

### WSOLA concat unit list format
- Hook: `SWIttsWsolaConcat` at 0x8EE65E0 (cdecl)
- arg4 (esp+16) = output struct
- `[arg4+0x04]` = unit count (matches HP count)
- `[arg4+0x08]` = pointer to unit array, stride 0x18
- Entry format: `+0x00=uid`, `+0x04=group_count` (0=continuation, N>0=start of N-unit run)
- `+0x08` and `+0x0C` appear to be WSOLA parameters (pitch shift, time offset?)

### Tom VDB fragment structure
- Tom's VDB recordings are pre-cut fragments, NOT complete utterances
- SpeechWorks cut at the source; the VDB stores only the fragments
- ckls word records only cover the LABELED words, not the full fragment content
- Example: dip5_009 has 19 units (pau+p+l+iy+z+er+r+ay+t) but ckls only labels
  "right" (r+ay+t, UIDs 6890-6895). The "please" portion (p+l+iy+z) is unlabeled.
- Mara's Qwen re-synths were based on transcribing heard audio, so they match the
  LABELED content (not the full fragment), creating systematic content mismatches.

### Post-processing interference with MFA mapping
- dl inflation (MIN_DL_FLOOR=10): extends units past phone boundaries
- LP gap closure (MAX_LP_GAP=5): shifts units away from MFA positions
- Silence relocation (SILENCE_RMS_THRESH=500): moves 30K+ units to wrong positions
- These steps were designed for the old proportional mapping and corrupt MFA-based positions
- Fix: skip all three for MFA-mapped units (tracked via mfa_aligned_uids set)

---

### 15. Re-recording 270 mismatched recordings
- **Date**: 2026-03-15
- **What**: Identified 270 recordings with real content mismatches (excluding xx-only).
  Generated corrected .lab files from Tom's unit table phone sequences. Used original
  English transcripts from `tom_transcript.csv` to create CSV for Qwen batch synthesis.
  Re-synthesized with Qwen, re-ran MFA (beam=400, retry_beam=1000), rebuilt full pipeline.
- **Result**: MFA coverage now 6,730 TextGrids (85% exact + 14% aligned = 99%).
  Voice substantially improved: "-call us at 5 0 1 3" (from garbled mess).
  Still missing "please", "five five", "two".

### 16. Duration compression: initial hypothesis (durt trees) -- WRONG
- **Date**: 2026-03-15
- **What**: Compared WSOLA entry +0x08/+0x0C fields between Tom and Mara for same sentence.
- **Initial hypothesis**: Output durations come from durt tree predictions. Tested by
  reverting durt leaf means to Tom's originals (SKIP_DURT_RECOMPUTE=True).
- **Result**: ZERO change in +0x0C values. Durt trees do NOT control output duration.

### 17. Duration compression: actual root cause -- lp overlap (CONFIRMED)
- **Date**: 2026-03-15
- **What**: Discovered that WSOLA +0x08 values exactly match the unit's `local_pos` (lp) from
  the VIN. +0x0C = next_unit.lp - this_unit.lp = output duration. Confirmed by matching all
  5 units in dip5_009 group: Mara lp values [0, 47, 47, 40, 54] = WSOLA +0x08 [0, 47, 47, 40, 54].
- **Root cause**: The proportional distribution in the direct phone-boundary mapping placed
  multiple halfphones at the SAME lp position (e.g., two "p" halfphones both at lp=47) because
  MFA phone intervals are narrow. When the engine computes next_lp - this_lp, overlapping lps
  give 0ms; backwards lps give negative durations.
- **Fix (STATE_VERSION 42)**: Changed from proportional distribution
  (`rel = (lp - lp_min) / tom_span`) to index-based even spacing
  (`new_lp = mfa_start + idx * spacing` where `spacing = max(1, mfa_span // n_units)`).
  Guarantees strictly increasing lp values with minimum separation.
- **Key insight**: DUR_WEIGHT and durt trees affect unit SELECTION cost only, NOT output
  duration. Output duration is determined entirely by lp spacing in the VIN unit table.
  DUR_WEIGHT can be safely restored to 0.2.

### 18. Cache bug discovered -- STATE_VERSION not bumped (v42-v46 wasted)
- **Date**: 2026-03-15 (late session)
- **What**: Even-spacing code was added at v42 but STATE_VERSION was not bumped from
  the previous build. All builds from v42-v46 served CACHED results from v41.
  None of our lp spacing changes had any effect.
- **Discovery**: Added debug prints to process_recording() for dip5_009 UIDs.
  Prints never appeared. Realized: `cache: 6849 hits, 0 misses` = everything cached.
- **Fix**: Bumped STATE_VERSION to 47, confirming even-spacing code works correctly.
  Debug prints showed lp=0,20,40,55,70,85 (strictly increasing). Cached values
  had been lp=47,47,40,55,70,85 (overlapping, from v41 era).

### 19. Silence relocation identified as the real lp corruption source
- **Date**: 2026-03-15 (late session)
- **What**: After v47 cache fix, VIN check STILL showed lp=47 for both p-halfphones.
  Ran check_lp_values.py between build_mara_voice.py and rest scripts -- STILL wrong.
  process_recording() output was correct but silence relocation moved lp=0 -> lp=47
  (forward to first speech) and lp=20 -> lp=47 (same position), recreating overlap.
- **Fix (STATE_VERSION 48)**: Skip silence relocation for MFA-aligned units.
  Relocation count dropped from 27,120 to 4.

### 20. Clean process_recording() rewrite (STATE_VERSION 50-51)
- **Date**: 2026-03-15 (late session)
- **What**: Rewrote process_recording() from scratch with clean MFA-based mapping:
  - `_refine_mfa_interval()`: per-phone audio energy correction at MFA boundaries
  - Halfphone split: each phone interval divided evenly among units
  - Minimal monotonicity enforcement (sort by Tom lp, clamp backwards jumps)
  - No inflation, no gap closure, no relocation for MFA units
- **Result**: "Please" restored! Output: "Please call at 5 0 oh 1 3"
  Way less choppy. First time "please" has been audible.
- **v51**: Added back monotonicity enforcement to prevent WSOLA crash.

### 21. _refine_mfa_interval() widened to full-recording search (v52)
- **Date**: 2026-03-15 (late session)
- **What**: Refinement was only searching 50ms from MFA position. Widened to search
  entire recording (interleaving forward/backward). Also returns (-1,-1) if no
  speech found anywhere, causing the phone group to be disabled (dl=0).
- **Result**: Silent units reduced from 11 to 8. news6_100 "l" went from 190->615 RMS.
  weather4_084 "f" went from dl=6 to dl=30 with RMS=11778.

### 22. Final RMS audit (v54, testing)
- **Date**: 2026-03-15
- **What**: Post-processing step that checks every speech unit's actual audio RMS
  after all mapping and refinement. Units with RMS < 500 get dl=0 (disabled).
  This removes hopeless recordings from the candidate pool so the engine picks
  alternatives with audible audio.
- **Status**: Building, awaiting test.

---

## Session 2026-03-16: Stability, coverage, and duration fixes

### 23. Zero-gap crash from same-lp clusters (STATE_VERSION 55-57)
- **Date**: 2026-03-16
- **What**: Audit of Mara unit table revealed 21,593 units sharing the same lp as the previous
  unit in the same recording (same-lp clusters). Tom has 0 such clusters. When the engine picks
  consecutive units from the same recording with identical lp, WSOLA computes
  `output_dur = next.lp - this.lp = 0` and crashes or produces silence.
- **Root cause**: Monotonicity enforcement used `if _lp < _prev` (strict less-than), allowing
  equal lp values to pass through unchanged.
- **Fix**: Changed to `if _lp <= _prev` (less-than-or-equal), bumping collisions to `_prev + 1`.
  Same-lp cluster count dropped to 0.
- **Affected script**: `build_mara_voice.py`

### 24. "No valid units" crash for phone `k` (build_mara_rest.py)
- **Date**: 2026-03-16
- **What**: Engine produced "no valid units found at index=24" error during synthesis of the
  test sentence. Phone `k` had zero candidates in prsl.
- **Root cause**: `build_mara_rest.py` loaded Tom's prsl keys but dropped any key where ALL
  Mara candidates had `dl=0` after the build pipeline. Those keys were excluded from
  `all_target_keys`, so they were never given back-fill candidates. 2,967 Tom keys had lost
  all valid Mara candidates (dl>0); 65 of those were for phone `k` contexts.
- **Fix**: Added `tom_all_keys = set(all Tom prsl keys)` that preserves ALL Tom keys regardless
  of candidate validity. `all_target_keys = tom_all_keys | set(mara_ck.keys())`. Now every key
  in Tom's original prsl is guaranteed a back-fill entry, even if Mara has no coverage there.
- **Affected script**: `build_mara_rest.py`

### 25. Same-recording continuation bypass (CRITICAL ENGINE DISCOVERY)
- **Date**: 2026-03-16
- **What**: Frida diagnostic on the dip5_009 group revealed that UIDs 45998/45999 (el/2nd half,
  both with lp=0 dl=0 = disabled) were being selected despite being filtered from prsl.
- **Root cause (confirmed via Frida)**: The engine has a **same-recording continuation
  optimization** that bypasses prsl entirely. When the Viterbi selects two consecutive units
  from the same recording (e.g., UIDs 45996 and 45997), the engine automatically includes the
  **next sequential units** (45998, 45999) as candidates for the subsequent halfphone position,
  regardless of prsl coverage. This happens inside the preselection step before scoring.
- **Implication**: Setting `dl=0` is NOT a safe way to disable units. Any unit adjacent to
  an active unit in the same recording can be selected by the engine regardless of prsl.
  Units with dl=0 produce zero-duration output (silence) or crash.
- **Fix**: NEVER set `dl=0` for any unit. Three specific cases changed:
  1. Unmatched phone groups now use proportional fallback lp/dl (Tom's proportional mapping)
     instead of `(0, 0)`.
  2. Silence units use `max(1, ...)` for dl.
  3. No-audio recordings (no VDB data) now keep Tom's original lp with `dl = max(1, scaled_dl)`.
- **Affected script**: `build_mara_voice.py`

### 26. Compressed output duration from tight lp spacing
- **Date**: 2026-03-16
- **What**: Frida WSOLA diagnostic showed many `f0c` (output duration) values of 1-5ms.
  Total output was ~2.5s instead of expected ~5s for the test sentence.
- **Root cause**: MFA gives tight phone intervals (10-25ms for some phones). The even-spacing
  formula `max(1, span // n)` created 1-5ms lp gaps between halfphones. For same-recording
  consecutive units, WSOLA output_dur = next.lp - this.lp, so tiny gaps = tiny output.
  Also: last unit in each same-rec group gets output_dur = dl. Tiny dl = tiny output.
- **Fixes applied**:
  - `MIN_UNIT_DUR = 25` (ms minimum lp spacing for MFA-based units)
  - When Tom's original spacing is larger than the computed MFA spacing, use Tom's instead
  - Minimum spacing of 15ms enforced in ALL monotonicity passes: per-recording, fallback,
    and final global pass (was only enforcing `>= prev`, not `>= prev + 15`)
  - dl inflation now applies to MFA units too (previously skipped for MFA-aligned units,
    leaving many dl values below the Tom minimum)
- **Result (STATE_VERSION ~58-62)**: Output noticeably longer and more complete.
- **Affected script**: `build_mara_voice.py`

### 27. RMS audit removal
- **Date**: 2026-03-16
- **What**: The final RMS audit (post-build pass that checks every speech unit's actual audio
  RMS and sets dl=0 for units with RMS < 500) was disabling 13,154 units (8.5% of pool).
- **Impact**: 13,154 disabled units meant 13,154 prsl keys now had no coverage in Mara,
  forcing build_mara_rest.py to back-fill with phonetically poor Tom substitutes. The prsl
  file bloated from ~5MB to ~40MB. And experiment 25 showed that dl=0 units can still be
  selected by the engine anyway via same-recording continuation.
- **Fix**: Removed the RMS audit entirely. MFA boundaries are the ground truth for unit
  positions; a quiet unit is better than a missing one.
- **Affected script**: `build_mara_voice.py`

### 28. Silence relocation removal
- **Date**: 2026-03-16
- **What**: A post-processing step in `process_recording()` relocated non-MFA units forward
  to the next speech region (using energy detection). This step was pushing units past other
  units without checking monotonicity, breaking the invariant.
- **Fix**: Removed entirely. MFA boundaries and the fallback proportional mapping already
  handle unit placement correctly. Silence relocation caused more problems than it solved.
- **Affected script**: `build_mara_voice.py`

### 29. Hash cross-recording cost reduction (build_mara_hash.py)
- **Date**: 2026-03-16
- **What**: Cross-recording join cost in Tom's hash was being overwritten with 8.0 (very high)
  for all non-same-recording pairs in Mara's build. This was intended to lock the engine into
  same-recording runs, but it was too punitive.
- **Analysis**: With 8.0, the engine was so strongly discouraged from cross-recording joins that
  it would use a phonetically poor same-recording unit rather than a phonetically excellent
  cross-recording one. This hurt intelligibility.
- **Fix**: Changed from `HIGH_CROSS_COST = 8.0` to `HIGH_CROSS_COST = 3.0`. Still above Tom's
  average join cost (~1-2) but allows the engine more freedom to find phonetically correct units.
- **Result**: Improved intelligibility at the cost of slightly more recording switches.
- **Affected script**: `build_mara_hash.py`

### 30. Re-recording target generation
- **Date**: 2026-03-16
- **What**: Wrote `gen_rerecord_targets.py` that compares Tom's phone sequences (from unit table,
  deduplicated to halfphone phonemes per recording) against MFA phone sequences from Mara's
  TextGrids. Computed a per-recording match rate.
- **Results**: 1,941 recordings (28.8%) have poor phone match rates (< 80% of phones aligned).
  Generated CSV at `c:/tmp/rerecord_targets/rerecord.csv` with 1,920 recordings for Qwen
  re-synthesis.
- **Script**: `c:/tmp/gen_rerecord_targets.py`

---

## Session 2026-03-16b: Synthesis coverage, hash improvements, immutability discovery

### 31. Overnight Qwen re-synthesis (5,917 recordings)
- **Date**: 2026-03-16
- **What**: Re-synthesized 5,917 recordings overnight using Qwen batch from
  `c:\tmp\rerecord_targets\rerecord.csv`. Re-ran MFA alignment with english_mfa dictionary.
- **Result**: MFA coverage jumped from 9% to 95% (10% seq exact + 85% aligned). Only 3%
  proportional fallback remaining.
- **MFA dictionary fix**: Was using mfa_forced_dict.txt (a 45-line phone mapping, NOT a word
  dictionary -- caused all <unk>). Switched to english_mfa bundled dictionary. Required expanding
  IPA-to-ARPAbet mapping with palatalized consonants, dental variants, retroflex, palatal phones.

### 32. Proportional fallback for unmatched phone groups
- **Date**: 2026-03-16
- **What**: Previously, unmatched phone groups (phone not in Mara's MFA) were set to (lp=0, dl=0).
  The engine's same-recording continuation bypass selected these disabled units, causing crashes
  and silence. Changed to proportional fallback: unmatched groups get Tom's proportional lp/dl
  mapping instead of (0,0).
- **Result**: Weather sentence crash eliminated. Engine no longer selects zero-duration units.
- **Affected script**: `build_mara_voice.py` (STATE_VERSION 56->68)

### 33. MIN_UNIT_DUR and MIN_LP_SPACING enforcement
- **Date**: 2026-03-16
- **What**: MFA gives tight phone intervals (10-25ms for some phones). The even-spacing formula
  created 1-5ms lp gaps between halfphones, producing compressed audio (~2.5s instead of ~5s).
  Added MIN_UNIT_DUR=25ms minimum spacing for MFA-based units, and MIN_LP_SPACING=15ms enforced
  in ALL monotonicity passes (per-recording, fallback, and final global).
- **Result**: Output duration went from ~2.5s to ~5s for weather sentence. Units have adequate
  spacing for WSOLA to produce audible output.
- **Affected script**: `build_mara_voice.py`

### 34. Spectral clustering in hash (build_mara_hash.py)
- **Date**: 2026-03-16
- **What**: Computed per-recording spectral fingerprints (mel-spectrogram statistics). For each
  cross-recording pair in Tom's hash, computed spectral distance between their recordings.
  Pairs between spectrally similar recordings (within p25 distance threshold) get their join cost
  multiplied by CLUSTER_DISCOUNT (0.3), making them cheaper for the engine to choose.
- **Rationale**: Reduces audible timbral discontinuity at recording boundaries by biasing the
  engine toward transitions between spectrally similar recordings.
- **Result**: Modest improvement in timbral continuity. Works in combination with run-potential
  penalty (experiment 35).

### 35. Run-potential penalty in hash (build_mara_hash.py)
- **Date**: 2026-03-16
- **What**: Computed per-unit "run potential" = how many active same-recording neighbors each unit
  has. For each pair in Tom's hash where the right unit (destination) has run potential < 4,
  multiplied the join cost by up to RUN_PENALTY_MULT (5.0). This penalizes transitions TO units
  that cannot sustain a long same-recording run.
- **Result**: Recording switches reduced from 42 to 33 (21% reduction) on weather sentence.
  Combined with spectral clustering, this is the most effective hash-based improvement found.
- **Settings**: RUN_PENALTY_MIN=4, RUN_PENALTY_MULT=5.0

### 36. More aggressive spectral EQ (build_mara_voice.py)
- **Date**: 2026-03-16
- **What**: Increased spectral equalization parameters: SPEC_N_MELS from 20 to 40 (finer frequency
  resolution), SPEC_MAX_DB from 10 to 20 dB (allows larger corrections). Forces all recordings
  toward a more similar spectral envelope.
- **Result**: Slight improvement in cross-recording timbral consistency.

### 37. VCF reverted to Tom's original weights
- **Date**: 2026-03-16
- **What**: Restored all VCF cost weights to Tom's original values: CHUNK_BIAS_WEIGHT=0.25,
  UNIT_BIAS_WEIGHT=0.25, HALFPHONE_CAND_PRUNE_THRESH=0.8, DUR_WEIGHT=0.3, JOIN_COST_OFFSET=0.2.
  These were SpeechWorks' tuned values for this engine architecture.
- **Rationale**: Previous experiments with modified weights (DUR_WEIGHT=0.2, etc.) showed no
  benefit. Tom's values were empirically optimized by the original developers.
- **Result**: Stable baseline. No regressions.

### 38. build_mara_extra.py -- extra recordings added
- **Date**: 2026-03-16
- **What**: Restored and updated build_mara_extra.py. Adds 70 entirely new recordings to VIN/VDB
  (50 general + 20 targeted "with a high" phrases). Updated with expanded IPA mapping,
  MIN_UNIT_DUR, TARGET_RMS=6500.
- **Result**: Recordings added successfully to VIN/VDB. However, these extra recordings CANNOT
  participate in Viterbi path selection because the hash cell array size is immutable. The engine
  allocates a fixed buffer from Tom's n_cells (2,416,481). Extra recordings always get
  MISSING_JOIN_COST=10000 in join cost evaluation.
- **Conclusion**: Adding recordings beyond Tom's original 8,118 is effectively useless for
  Viterbi quality. They can only be selected if they happen to be in a same-recording continuation
  chain, which they never are (they are new recordings with no neighbors).

### 39. Hash immutability confirmed (CRITICAL ENGINE CONSTRAINT)
- **Date**: 2026-03-16
- **What**: Frida crash diagnostic confirmed that the hash structure (n_rows, n_cells) cannot be
  modified in size. The engine's hash loader allocates a fixed buffer based on n_cells from the
  head sub-chunk. Appending additional cells to the cell sub-chunk causes access violation at
  EIP=0x8e8b7e6 during USel (unit selection).
- **Root cause**: The engine reads n_cells from head, allocates `n_cells * 8` bytes for the
  AoS cell array, then reads exactly n_cells entries. Any cells beyond n_cells are never loaded
  but the file size mismatch may cause downstream RIFF parsing errors.
- **Implication**: The hash table is effectively a fixed-size resource. New (uid_left, uid_right)
  pairs can only be added by REPLACING existing pairs, not by appending. The total number of
  cells (2,416,481) is the hard ceiling.

### 40. use_edgeframes confirmed non-functional
- **Date**: 2026-03-16
- **What**: use_edgeframes=1 (runtime join cost from ccos vectors) was tried again. Engine fails
  because it requires an unknown chunk that is not present in the VIN file.
- **Conclusion**: Not viable as an alternative to hash-based join cost. The engine requires some
  additional data structure for edge-frame mode that we have not identified.

### 41. diag_ground_truth.py visualization
- **Date**: 2026-03-16
- **What**: Added timeline PNG generation to diag_ground_truth.py. Shows recording runs, switch
  points, and output durations per halfphone. Color-coded by recording ID.
- **Result**: Very useful for identifying stutter patterns. Clearly shows that "with a high"
  section has 4 single-unit recording runs (hh/ay phones have no long-run candidates in pool).

### 42. DLL analysis of hash loader (address-level code flow)
- **Date**: 2026-03-16
- **What**: Disassembled the hash loader function at 0x8E854A8 in SWIttsUSel.dll. Traced
  readBytes at 0x8E87930, combined buffer allocation at 0x8E855F3 (lea edx,[ebx*8] then
  call 0x8E94E73). Buffer stored at [esi+0x80], rows at [esi+0x84].
- **Key finding**: The Viterbi hash lookup at 0x8E8B7E6 does `cmp [esi+eax*8],ebx` with
  NO bounds check -- relies on SENTINEL to terminate. Code at 0x8E8B720-0x8E8B723 uses
  the PREVIOUS candidate's field for rows[] lookup, suggesting hash may be indexed by
  uid_LEFT (not uid_right as previously assumed).
- **Hash miss fallback**: At 0x8E8B7F5, loads 0.0 default, checks [ecx+0x6C] vs 20,
  optionally computes ccos distance.
- **Result**: Deepened understanding of hash internals. The uid_left vs uid_right indexing
  question needs further validation but is consistent with the disassembly.

### 43. Frida spawn hook to intercept hash allocation
- **Date**: 2026-03-16
- **What**: Used Frida to spawn+hook the allocation function at 0x8E94E73 during voice
  loading, attempting to observe the n_cells*8 allocation for the hash buffer.
- **Result**: FAILED -- no large allocations detected at that address. The actual heap
  allocation likely happens inside readBytes (0x8E87930), not at the explicit call site.
  The buffer still ends up at [esi+0x80] regardless.
- **Conclusion**: Cannot easily patch the allocation size to enlarge the hash buffer. The
  allocation path is more indirect than the disassembly suggested.

### 44. Extra recording REPLACE strategy (prosodic fields fixed)
- **Date**: 2026-03-16
- **What**: Attempted to add extra recordings by replacing existing Tom hash entries with
  new-recording pairs. Also fixed prosodic fields (syl_type, syl_in_phrase, word_in_phrase,
  phone_pos, pctx3) on extra units to match Tom's per-phone mode values.
- **Result**: Structural mismatch -- even with valid cell values, the chain organization
  (suffix sharing, sentinel positions) must remain identical to Tom's original. The engine
  still does not select extra units.
- **Conclusion**: REPLACE strategy is not viable without fully replicating Tom's chain
  structure, which we cannot reconstruct (experiment #1 proved this).

### 45. Frida diag_extra_selection: extra unit evaluation confirmed zero
- **Date**: 2026-03-16
- **What**: Frida diagnostic script hooked the candidate cost function to track how many
  extra-range units (uid > Tom's original count) are evaluated during synthesis.
- **Result**: 0 extra units evaluated by the candidate cost function. 1 extra unit appeared
  in WSOLA output (final pau/silence only, not speech). Confirms that extra recordings
  cannot participate in Viterbi due to hash miss penalty (MISSING_JOIN_COST=10000).
- **Conclusion**: Extra recordings are dead weight in the current architecture. The only
  path to using them is to evict Tom pairs from the hash (which breaks chain structure).

### 46. Run-potential penalty tuning (3x vs 5x)
- **Date**: 2026-03-16
- **What**: Tested RUN_PENALTY_MULT=3.0 (lighter penalty for recording switches) vs 5.0
  (heavier penalty). Both with COST_SCALE=0.0, CLUSTER_DISCOUNT=0.3, RUN_PENALTY_MIN=4.
- **Result**: Same output at 5x as at 3x -- 33 recording switches for the weather test
  sentence. The penalty is already saturated; further increases do not help because the
  engine's candidate pool for problematic phone sequences (ih->th->ax->hh->ay) simply
  lacks long-run alternatives.
- **Conclusion**: Run-potential penalty has reached diminishing returns. The 33-switch
  floor is set by the available recording pool, not by cost tuning.

### 47. Frida Stalker trace of hash loading (allocation mechanism)
- **Date**: 2026-03-16
- **What**: Used Frida Stalker (instruction-level tracing) on the hash loader function at
  0x8E854A8 to trace every malloc call during voice initialization.
- **Result**: SUCCESS. Revealed the complete allocation sequence:
  - readBytes(692,190) -> rows (malloc 2,768,760 at 0x8E87954)
  - readBytes(2,416,481) -> cells_A (malloc 9,665,924 at 0x8E87954)
  - readBytes(2,416,481) -> cells_B (malloc 9,665,924 at 0x8E87954)
  - malloc(n_cells * 8) at 0x8E85606 -> interleaved runtime AoS buffer
  - All mallocs go through 0x8E94E73
- **Conclusion**: Allocations scale dynamically from head's n_cells value. The earlier
  "allocation mystery" (Exp 43) was a Frida hooking issue, not a real code path difference.

### 48. Frida exception handler on hash crash (ESI analysis)
- **Date**: 2026-03-16
- **What**: Installed a Frida exception handler to capture register state when the AV at
  0x8E8B7E6 fires during unit selection with extra recordings.
- **Result**: ESI != hashBase. ESI = hashBase + rows[uid_right] * 8. This proved that
  the engine pre-computes the base pointer for each uid_right BEFORE the cell comparison.
  The crash occurs because rows[extra_uid_right] + uid_left exceeds n_cells.
- **Conclusion**: The hash is NOT a chain table. ESI points to a specific offset within
  the cell buffer, and eax (uid_left) is used as a direct index from that offset.

### 49. In-memory buffer verification (sentinels + interleaving)
- **Date**: 2026-03-16
- **What**: Used Frida to dump sections of the runtime interleaved hash buffer and verify
  sentinel placement and SoA-to-AoS interleaving correctness.
- **Result**: Sentinels (0xFFFFFFFF) present at expected empty-slot positions. Interleaving
  matches: runtime cell[i] = {cells_A[i], cells_B[i]} for all sampled positions.
- **Conclusion**: The loader correctly interleaves SoA (file) -> AoS (memory). Empty slots
  contain sentinel values that cause automatic miss on comparison.

### 50. Disassembly of 0x8E8B7E6 -- compressed perfect hash confirmed
- **Date**: 2026-03-16
- **What**: Full disassembly of the Viterbi hash lookup path (0x8E8B7BC through 0x8E8B7F5)
  to determine whether the access is a loop (chain walk) or single indexed access.
- **Key instructions:**
  - 0x8E8B7BC: `mov eax, [edx + 0x10]` -- loads uid_left from candidate struct
  - 0x8E8B7E2: `mov esi, [esp + 0x40]` -- loads hashBase + rows[uid_right]*8
  - 0x8E8B7E6: `cmp [esi + eax*8], ebx` -- ONE comparison
  - 0x8E8B7E9: `jne 0x8E8B7F5` -- miss fallback, NO loop back
  - 0x8E8B7EB: `fld [esi + eax*8 + 4]` -- load f32 cost on hit
- **Result**: CONFIRMED single-access compressed perfect hash. No loop instruction. The
  lookup formula is `cell[rows[uid_right] + uid_left]` with one comparison.
- **Conclusion**: Previous chain-walk model was WRONG. Suffix sharing is actually shared
  base offsets in a compressed perfect hash. This fully explains why appending crashed
  (uid_left as direct index goes OOB) and why the structure seemed immutable.

### 51. Shared-offset extension for extra recordings
- **Date**: 2026-03-16
- **What**: All extra uid_rights share ONE rows[] offset = n_cells_original. Extension
  region appended to cell array with {uid_left, 0.0} for same-recording neighbors.
  Different recordings share cells since the stored uid_left value is the same.
  Only ~176K new cells needed. Also provides OOB safety padding for Tom uid_rights
  accessing extra uid_lefts (reads sentinels instead of crashing).
- **Result**: SUCCESS. Extra unit UID 176310 was selected by the engine during synthesis.
  No crashes. The shared-offset technique resolves the hash immutability blocker.
- **Conclusion**: Extra recordings can now participate in Viterbi search. The practical
  path forward is to use shared-offset extension for all new recordings.

### 52. VCF JOIN_COST_WEIGHT 0.7 -> 1.0
- **Date**: 2026-03-16
- **What**: Increased JOIN_COST_WEIGHT from 0.7 to 1.0 in mara.vcf to test whether
  higher join cost weight reduces recording switches. Tested on weather sentence
  (100 halfphones).
- **Result**: NO EFFECT. 33 switches, identical UIDs selected.
- **Root cause**: Join cost formula is `weight * raw_cost + offset`. Almost all
  transitions are hash MISSES with raw_cost=0.0, so join cost = weight * 0 + 0.2
  = 0.2 regardless of weight value. Tom's hash has only 1.6M entries out of ~29B
  possible (uid_left, uid_right) pairs -- only 0.006% of transitions produce HITs.
- **Conclusion**: VCF weight changes that affect join cost have ZERO impact on Mara
  because the hash miss fallback returns a constant 0.0 raw cost.

### 53. VCF CONTEXT_COST_WEIGHT 1.0 -> 0.5
- **Date**: 2026-03-16
- **What**: Halved CONTEXT_COST_WEIGHT from 1.0 to 0.5 to test whether reduced
  context discrimination allows more same-recording selections.
- **Result**: WORSE. 36 switches (up from 33).
- **Root cause**: Less context discrimination means the engine picks more random
  units from different recordings. Context cost is one of the few functional cost
  components, so reducing it removes useful signal.
- **Conclusion**: Context cost weight should stay at 1.0 or above. It is one of the
  few levers that actually influences unit selection for Mara.

### 54. VCF RUN_PENALTY tuning (MIN=6, MULT=8.0)
- **Date**: 2026-03-16
- **What**: Increased RUN_PENALTY_MIN from 4 to 6 and RUN_PENALTY_MULT from 5.0
  to 8.0 in build_mara_hash.py to more aggressively penalize low-run units.
- **Result**: NO EFFECT. 33 switches, identical UIDs selected.
- **Root cause**: Remaining switches are between HIGH-run units where the penalty
  does not apply. The penalty only affects units with run < RUN_PENALTY_MIN, but
  the problematic transitions are already between units that pass the threshold.
- **Conclusion**: Run-potential penalty has hit diminishing returns. The 33-switch
  floor is set by the available recording pool, not by cost tuning.

---

## Key insight: Join cost is non-functional for Mara (2026-03-16)

The hash miss fallback at 0x8E8B7F5 returns raw_cost=0.0 for virtually all Mara
transitions. The miss fallback disassembly:
```
0x8e8b7f5: mov eax, [ecx + 0x6c]     ; load threshold/counter
0x8e8b7f8: fld [0x8e9852c]            ; load constant (0.0)
0x8e8b7fe: cmp eax, 0x14              ; compare with 20
0x8e8b801: jle 0x8e8b83d              ; if <= 20, use 0.0 (skip ccos)
0x8e8b803: cmp [edx+0x80], 0xf        ; additional check
0x8e8b80a: jge 0x8e8b83d              ; if >= 15, also skip
0x8e8b80c: mov esi, [edx+0x7c]        ; load another value
0x8e8b80f: cmp esi, 0x14              ; compare with 20
0x8e8b812: jle 0x8e8b83d              ; if <= 20, skip
; ... else compute ccos distance
```
Since [ecx+0x6c] <= 20 is almost always true, the ccos distance is never computed,
and raw_cost=0.0 always. Final join cost = 0.7 * 0.0 + 0.2 = 0.2 for ALL transitions.

Understanding [ecx+0x6c] (possibly halfphone position counter, candidate count, or
voice parameter) could reveal when ccos distance IS computed. If the condition can be
made to fail (> 20), more transitions would get real spectral costs instead of 0.0.

---

## Experiment 55: Viterbi Forward Pass Disassembly (2026-03-17)

**Goal:** Fully understand the Viterbi inner loop to find hookable points for
recording-switch reduction.

**Method:** Capstone disassembly of three key functions in SWIttsUSel.dll:
- `0x8E8EDD0` -- Viterbi forward pass (with join cost)
- `0x8E8ED20` -- Join cost calculator
- `0x8E8B620` -- "NoJoin" Viterbi (the one actually used by Mara, since hash misses)

**Findings:**
The NoJoin Viterbi at `0x8E8B620` is the active code path for Mara. Key structure:
```
Outer loop: for each HP position i = 1..N-1:
  esi = HP[i] candidate list
  [esi+0x2c] = candidate count
  [esi+0x34] = pointer array to candidate objects

  Inner loop: for each candidate c at position i:
    ecx = candidate_ptr (from [esi+0x34][j*4])
    ebx = candidate uid (from [ecx+0x0c])

    Predecessor loop: for each candidate p at position i-1:
      edx = predecessor_ptr
      [edx+0x10] = predecessor uid_alt
      [edx+0x20] = predecessor cum_score

      Hash lookup: cell[rows[ebx] + [edx+0x10]]
        HIT -> join_cost = cell.cost_f32
        MISS -> join_cost = 0.0 (via fallback at 0x8E8B7F5)

      Adjacency check at 0x8E8B854:
        if candidate.uid == predecessor.uid + 1:
          join_cost = 0, context_cost = 0 (FREE transition)

      new_cum = predecessor.cum_score + join_cost + context_cost
      if new_cum < candidate.best_cum:
        candidate.cum_score = new_cum
        candidate.predecessor = p
```

**Critical discovery:** The adjacency check at `0x8E8B854` (`cmp ebx, eax; jne`)
is the ONLY point where same-recording transitions get preferential treatment.
This is a pure UID adjacency check (uid == prev_uid + 1), not a recording check.

---

## Experiment 56: Viterbi Penalty Hook (2026-03-17)

**Goal:** Add a recording-switch penalty to the Viterbi inner loop via Frida code cave.

**Method:** Patch the adjacency check at `0x8E8B854` to jump to a code cave that:
1. Loads file_idx for both candidate and predecessor from a lookup table
2. If file_idx differs (recording switch), adds penalty to join cost via `fadd`
3. Falls through to the original adjacency check

**Hook location:** `0x8E8B854` (7-byte `cmp ebx,eax; jne` -> `jmp cave; nop nop`)

**Results:**

| Penalty | Switches | Run mean | Run max | Notes |
|---------|----------|----------|---------|-------|
| 0 (baseline) | 40 | 2.3 | 10 | No hook |
| 50 | 32 | 2.9 | 10 | First improvement |
| 100 | 32 | 2.9 | 10 | Saturated |
| 200 | 32 | 2.9 | 10 | Saturated |
| 500 | 32 | 2.9 | 10 | Saturated |

**Conclusion:** Penalty saturates at p=50 (32 switches). The penalty can only choose
among candidates that survive pruning. With ~14 post-prune candidates per position,
too few share recordings at adjacent positions for the penalty to help further.

---

## Experiment 57: Theoretical Minimum Recording Switches (2026-03-17)

**Goal:** Compute the absolute minimum number of recording switches for the test
sentence, ignoring all costs except recording identity.

**Method:** Dijkstra on a recording-level graph:
- Nodes: (hp_position, file_idx)
- Edges: cost 0 if same file_idx, cost 1 if different
- Candidate pool: ALL prsl candidates with matching phone_center (not context-filtered)

**Results:**
- **Theoretical minimum: 2 switches** (for 100 halfphones)
- 3 recordings cover the entire sentence:
  - `news09_035` (fidx 2106): HPs 1-28
  - `news32_047` (fidx 4520): HPs 29-64
  - `news7_032` (fidx 4905): HPs 65-100
- **Every boundary (99/99) has same-recording transitions available**
- Gap from theoretical (2) to actual (32) = the candidate pool bottleneck

---

## Experiment 58: Runtime Candidate Injection via Frida (2026-03-17)

**Goal:** Inject candidates from high-coverage recordings into the Viterbi candidate
lists at runtime, combined with the penalty hook.

**Method:** Two approaches tried:
1. **Pre-scorer injection** (hook inner scorer onEnter, replace last N candidates):
   Injected candidates scored by engine but then PRUNED (total_score > 0.95 threshold).
   Result: 32 switches (unchanged).
2. **Post-prune injection** (hook prune onLeave, append candidates after pruning):
   Appended candidates with copied template scores. Same-rec transitions 4x'd (577->2268)
   but Viterbi still selected 32 switches -- injected candidates had wrong component scores.

**Conclusion:** Runtime injection doesn't work because:
- Pre-scorer: candidates get pruned (wrong triphone context = high target cost)
- Post-prune: candidates have wrong component scores (copied from template unit)
The engine's scoring pipeline must evaluate candidates naturally.

---

## Experiment 59: PRSL + Prune Threshold Tuning (2026-03-17)

**Goal:** Add target-recording UIDs to PRSL at build time so they go through the
full scoring pipeline, combined with relaxed prune threshold.

**Method:**
1. Modified `build_mara_rest.py` to inject 301 UIDs from 3 target recordings
   (news09_035, news32_047, news7_032) into every relevant PRSL context group.
   669,207 target_rec candidates added across 86,731 groups.
2. Raised `HALFPHONE_CAND_PRUNE_THRESH` in VCF from 0.8 to various values.
3. Raised `HALFPHONE_CAND_MAX_UNITS` from 50 to 200.

**Results:**

| Prune | Penalty | Switches | Transitions | Audio quality |
|-------|---------|----------|-------------|---------------|
| 0.8 | 50 | 32 | 17K | Stuttery but improved |
| 3.0 | 50 | **29** | 320K | Better, stutter reduced |
| 3.0 | 10 | 30 | 318K | Nearly identical to p=50 |
| 10.0 | 50 | **24** | 448K | Bad -- "Waysether today..." |

**Key findings:**
- **Prune threshold is the bottleneck**, not penalty magnitude (p=50 vs p=10 = 1 switch)
- prune=3.0 is the sweet spot: 29 switches with acceptable audio quality
- prune=10.0 reaches 24 but lets garbage candidates dominate (wrong phones selected)
- Same-rec % actually DROPS with more candidates (0.7% at 320K vs 3.3% at 17K) --
  more candidates = more total transitions but same-rec count doesn't scale proportionally

**Best config found:** prune=3.0, MAX_UNITS=200, penalty=50 = **29 switches** (was 40)

---

## Experiment 60: Transcript Reconstruction from ckls (2026-03-17)

**Goal:** Programmatically reconstruct full transcripts for all Tom recordings using
the ckls word records as ground truth, to improve Mara re-synthesis quality.

**Background:** Tom's VDB recordings are pre-cut fragments with unlabeled portions.
Manual transcription (basis for current Mara audio) misidentified many words because
the source audio has mid-word cuts. ASR (Whisper, faster-whisper, Meta model) all
failed on the 8kHz u-law fragments.

**Method:**
1. Parse Tom's ckls chunk for _WORD_ records (word -> span_start/span_end -> file_name)
2. Cross-reference with unit table phone sequences per recording
3. Use ckls words as anchors to correct old transcripts via SequenceMatcher alignment

**Results:**
- 2,816 recordings have ckls word labels (out of 8,118 total filenames)
- 2,111 confirmed correct (ckls validates old transcript)
- 671 corrected (ckls fixed specific misheard words)
- 3,472 kept as-is (no ckls coverage)
- 1,830 empty (no transcript)

**Example corrections:**
| Recording | Old (misheard) | Corrected (ckls truth) |
|-----------|----------------|----------------------|
| date_063 | "nine" | "ninth" |
| dip1_033 | "twin" | "when" |
| dip2_069 | "ian been after that" | "and after that" |
| dip2_090 | "we watched the" | "we watched out" |
| dip3_020 | "enjoy it sure" | "enjoy shore" |

**Output:** `c:\tmp\resynth_final.csv` (6,288 recordings ready for Qwen re-synthesis)

---

## Experiment 61: Audio Trim v1 -- First-Half Only (2026-03-17)

**Goal:** Trim Mara recordings to match Tom's VDB fragment structure, removing
audio before/after the phone regions actually used by VIN units.

**Method:** Computed trim boundaries from first-half phone alignment only.
Trimmed WAVs, rebuilt VDB + VIN with trimmed audio.

**Results:**
- 5,474 recordings trimmed to avg 51% of original (3,675 sec saved)
- FAILED: caused mass dl=1 artifacts because trim only used first-half phones
- Cutting removed audio needed by second-half units
- 42 switches (worse than 40 baseline)

**Root cause:** Trim boundaries must account for ALL halfphone units (both halves),
not just the first-half phone sequence.

---

## Experiment 62: Audio Trim v2 -- All Halves (2026-03-17)

**Goal:** Fix trim to use ALL unit positions (both halves) for trim boundaries.

**Method:** Computed trim boundaries from ALL units in each recording (both first-half
and second-half). More conservative trim. Rebuilt with fresh MFA alignment on trimmed WAVs.

**Results:**
- 3,876 recordings trimmed to avg 68% of original (1,729 sec saved)
- No dl=1 artifacts
- 37 switches baseline (40 -> 37, 3 fewer from trim alone)
- Required fresh MFA alignment on trimmed WAVs

**Key insight:** Trim ensures VDB audio content matches what the engine's lp/dl
offsets expect. Even without penalty hooks, quality improves because unit boundaries
point to correct audio regions.

---

## Experiment 63: VDB Bounds Safety Clamp (2026-03-17)

**Goal:** Fix VDB corruption crash caused by post-processing pushing units past
recording bounds.

**Problem:** Post-processing steps (dl inflation, gap-closing, monotonicity enforcement)
pushed 12,580 units past recording bounds, causing "File end is beyond speech DB end"
crash at engine load time.

**Fix:** Added final safety clamp after ALL post-processing steps, ensuring
lp+dl <= cap for every unit. STATE_VERSION bumped to 76.

**Key insight:** Post-processing invariants must be enforced AFTER all transformations,
not just after the initial mapping pass.

---

## Experiment 64: Full Stack -- Trim v2 + Penalty + PRSL + Prune (2026-03-17)

**Goal:** Combine ALL improvements into a single configuration.

**Method:** Combined:
- Audio trim v2 (all-halves boundaries)
- Fresh MFA alignment on trimmed WAVs
- VDB bounds safety clamp (STATE_VERSION 76)
- PRSL target recording injection (669K candidates)
- VCF: HALFPHONE_CAND_PRUNE_THRESH=3.0, HALFPHONE_CAND_MAX_UNITS=200
- Frida penalty hook at 0x8E8B854 with p=50

**Results:**
- **29 switches** with dramatically improved audio quality
- User assessment: "This one sounds damn good. Like, REALLY damn good. It's working."
- Trim ensures audio content matches what engine expects (content quality)
- Penalty/prsl/prune reduces recording switches (selection quality)
- Together they solve both the content mismatch AND the switching problems
- Neither improvement alone is sufficient -- both are required

---

## Experiment 65: Spectral Join Cost Cave (2026-03-17)

**Goal:** Replace the duration-table join cost with real 12-dim MFCC spectral boundary
distance via Frida code cave, to improve cross-recording transition quality.

**Method:**
1. Built `mara_spectral.bin` sidecar file: 16.3 MB, 169,579 units x 12 floats x 2
   (left boundary + right boundary MFCCs)
2. Generated by `build_mara_spectral.py` in ~3 seconds (reuses build_mara_hash.py spectral code)
3. Frida code cave at 0x8E8B814: 251 bytes of unrolled x86 FPU code computing
   `sqrt(sum((right_boundary[i] - left_boundary[i])^2))` across 12 MFCC dimensions
4. Cave replaces the gate-pass path (after all 3 gate conditions pass)

**Results:**
- scale=0.089: No quality change. **Byte-for-byte identical output** to penalty-only.
- scale=1.0: Same output hash.
- scale=10.0: Same output hash (12,990 spectral hits confirmed).
- Verified cave IS executing: 2,155 spectral hits at default scale.
- Verified cave DOES affect FPU: constant 999.0 produces different output hash.

**Analysis:**
The penalty hook (50) dominates all cross-rec decisions. The join cost only matters when
choosing BETWEEN cross-rec candidates at the same HP position. But the PRSL pool is small
enough that the choice between cross-rec candidates rarely changes regardless of cost
function. The penalty pushes strongly toward same-rec, and among the few cross-rec options
available at each position, the spectral distance doesn't differentiate enough to change
the Viterbi path.

**CONCLUSION: Join cost is NOT the quality bottleneck.** Further investment in join cost
refinement (spectral, duration-based, or otherwise) will not improve output quality.
The bottleneck is elsewhere -- likely source audio consistency across recordings.

---

## Experiment 66: CCOS Gate Investigation (2026-03-17)

**Goal:** Fully understand the hash miss fallback gate at 0x8E8B7F5 to determine whether
ccos join cost is actually computed for Mara.

**Method:** Detailed disassembly of the gate conditions, runtime sampling with Frida,
and tracing the gate2 update logic after candidate acceptance.

**Findings -- CRITICAL CORRECTIONS to prior understanding:**

1. **Gate passes 66% of hash misses** (33/50 samples), NOT "almost never" as assumed.
   The earlier analysis that condition 1 ([ecx+0x6c]>20) "almost always fails" was WRONG.
   For Mara's units, [ecx+0x6c] values are 105-150 (dl-like), which always exceeds 20.

2. **Gate conditions:**
   - Gate1: `[ecx+0x6c] > 20` -- candidate dl-like (ALWAYS passes for Mara)
   - Gate2: `[edx+0x80] < 15` -- same-rec run counter
   - Gate3: `[edx+0x7c] > 20` -- predecessor dl-like

3. **Gate2 is controlled by voice[0x94] = ckls entry count, NOT by use_edgeframes config:**
   At 0x8E8B8D0-0x8E8B8F5 (after join cost computed, candidate accepted):
   - WITH ckls (Mara has it): cross-rec -> candidate[0x80]=0 (passes), same-rec -> 100 (fails)
   - WITHOUT ckls: candidate[0x80] increments per same-rec continuation (fails after 15+)
   This is PERFECT behavior: ccos cost computed for ALL cross-rec, NEVER for same-rec.

4. **Cost table is V-shaped duration-continuity curve, NOT flat:**
   - 100 entries, offset=-50, index = dl_curr + 50 - dl_prev
   - Index 49 = 0.000 (perfect match), edges = 10.963 (maximum)
   - Scale factor = 0.6 (from ptr0[0xC8])
   - Final cost range: 0.0 to 6.578

5. **use_edgeframes is a NO-OP:**
   - Config dispatch at 0x8E86E67 sets voice[0x78]=2 for edgeframes, then use_joincache
     overwrites to voice[0x78]=1
   - Switch at 0x8E86EC1: mode 1 vs 2 only changes log message text
   - Both modes execute identical initialization code
   - Actual gate behavior controlled by voice[0x94], not voice[0x78]

**CONCLUSION:** The ccos gate works correctly for Mara. It provides a V-shaped duration
cost for cross-rec transitions. However, per Exp 65, this cost doesn't improve quality
because the penalty hook dominates all cross-rec decisions.

See `discovery_ccos_gate.md` for full disassembly, FPU flow, and cost table values.

---

## Experiment 67: RVC Voice Normalization (2026-03-17) -- SUCCESS (partial)

**Goal:** Use RVC (Retrieval-based Voice Conversion) as a post-processing normalizer
to reduce voice inconsistency across Qwen-synthesized recordings.

**Problem:** Qwen3-TTS synthesizes each recording independently, producing inconsistent
prosody, pitch, and speaking rate across recordings. This causes audible "stutter" at
concatenation points even with perfect join costs, because the underlying voice character
changes at every recording boundary.

**Method:**
1. `find_best_mara_wavs.py` (c:/tmp/) selects top 106 WAVs (~8 min) by quality metrics:
   duration > 2.5s, speech ratio > 50%, no clipping, stable RMS
2. Train RVC model on curated WAVs using `rvc-no-gui` CLI tool:
   - model_name=mara, 500 epochs, RTX 4070 Ti Super
   - Training on QWEN OUTPUTS (not original NWR audio) teaches RVC "what consistent
     Mara should sound like"
3. Batch-convert all 5,745 synth WAVs through trained RVC model (0 failures, ~2 hours)
4. Then: trim -> MFA -> build pipeline as usual

**Results:**
- **Cross-recording stutter is SOLVED.** Transitions between recordings are seamless.
- RVC normalizes timbre/pitch/rate across all recordings
- Mean spectral correction dropped from 4.5 dB to 3.3 dB (more consistent source audio)
- Phantom phonemes remain (content quality issue from Qwen transcripts, not transition quality)

**Key insight:** The quality bottleneck is not join cost (Exp 65 proved this) but
source audio consistency. RVC normalizes voice characteristics (timbre, pitch range,
speaking rate) across all recordings, addressing the ROOT CAUSE of concatenation artifacts.
Training on Qwen outputs (not clean reference) is key -- it learns Mara's actual voice.

**IMPORTANT LIMITATION (discovered Exp 69):** RVC trained on Qwen outputs works well for
Qwen->Mara conversion (similar input domain), but raw 8kHz Tom->Mara conversion produces
robotic output ("Terminator chainsmoker"). The 8kHz u-law source quality is too low for
convincing voice conversion. FlashSR upscaling (Exp 69) fixes this.

---

## Experiment 68: RVC + Trim v2 + Penalty Full Stack (2026-03-18)

**Goal:** Combine RVC-normalized audio with the full build pipeline (trim v2, MFA,
penalty hook, PRSL injection, prune) to get the best possible output quality.

**Method:**
1. RVC-normalized WAVs from Exp 67
2. Trim v2 on RVC output (3,082 recordings trimmed, 1,390 sec saved)
3. Fresh MFA alignment on trimmed RVC WAVs
4. Full build: `build_voice_pipeline.py mara --wav-dir resynth_rvc_trimmed --tg-dir resynth_rvc_trimmed`
5. Penalty hook p=50, PRSL injection, prune=3.0

**Results:**
- **31 recording switches** (vs 33 without trim, 29 best ever with non-RVC audio)
- Transitions are seamless (RVC solved cross-rec stutter)
- Mean spectral correction: 3.7 dB (down from 4.5 pre-RVC)
- Similar-recording pairs: 691,987 (up from 632K pre-RVC)
- User assessment: "comprehensible, an improvement" but phantom phonemes remain

**Remaining artifacts heard by user:**
- "weatherthem" -- excess content in recording (transcript issue)
- "cloudy-e" -- trailing phoneme at recording boundary (trim not aggressive enough)
- "(SE)venty" with Tom's voice leaking through (coverage gap, no Mara recording)
- "degrees?" with wrong intonation (Qwen prosody mismatch)

**Analysis:** RVC solves the transition quality problem completely, but content quality
issues remain: phantom phonemes from over-long recordings, coverage gaps (~1,830
recordings with no Mara audio), and Qwen prosody mismatches. These are source content
problems, not pipeline problems.

---

## Experiment 69: AudioSR Upscaling + RVC "Voice Skin" Pipeline (2026-03-18)

**Goal:** Instead of re-recording Mara audio with Qwen (which has transcript/prosody
issues), use Tom's original 8kHz u-law audio as the source, upscale it to high quality
with AudioSR, then convert to Mara's voice with RVC. This is the "voice skin" approach:
keep Tom's entire phonetic structure (lp, dl, unit positions), swap just the voice timbre.

**Problem:** Direct Tom 8kHz -> RVC produces robotic output (Exp 67 limitation). The
8kHz u-law source quality is too low for convincing male->female voice conversion.

**Method:**
1. Decode Tom's 8kHz u-law VDB audio to PCM WAV (one WAV per recording)
2. AudioSR upscale: speech model, ddim_steps=20, guidance_scale=3.5 -> 48kHz WAV
   - Batch script: `c:/tmp/audiosr_batch.py` (processes all 6,849 recordings)
   - Speed: ~2-3 seconds/file on RTX 4070 Ti Super (~4-6 hours total)
3. RVC convert upscaled audio: Mara model, 8 threaded workers (~0.8 files/s/worker)
   - Batch script: `c:/tmp/rvc_batch.py`
4. Build with `build_voice_pipeline.py` using RVC output as WAV source
   - No MFA needed (Tom's lp/dl values are already correct)
   - No trimming needed (Tom's recordings are already proper length)
   - No transcript corrections needed (Tom's audio has perfect phone content)

**Key findings:**
- AudioSR upscaled audio sounds "extremely crisp" per user evaluation
- This eliminates ALL content quality problems from the Qwen approach:
  - No phantom phonemes (Tom's audio matches Tom's transcripts perfectly)
  - No coverage gaps (all 6,849 recordings have audio)
  - No prosody mismatches (Tom's prosody IS the engine's expected prosody)
- The only variable is RVC conversion quality on upscaled male->female

**Results:**
- 6,606 recordings processed via AudioSR + RVC pipeline
- 243 short recordings failed AudioSR (too short for diffusion model block size)
  - These were handled via direct RVC on raw 8kHz Tom audio (Exp 71)
- All 6,849 Tom recordings now have Mara audio -- zero Tom leaks
- **28 recording switches** without penalty hook
- **24 recording switches** with penalty hook (p=50)
- Pitch: 176.1 Hz output (target 175.7 Hz, 0.0 semitone error)
- User assessment: **"CLEAN CLEAN CLEAN"**
- build_voice_skin.py is the primary build tool for this approach

**STATUS:** SUCCESS -- Mara voice COMPLETE

---

## Experiment 70: True Mara RVC Model (mara_v2) (2026-03-18)

**Goal:** Train RVC on real Mara recordings (NWR corpus, 2003-2016) instead of
Qwen-synthesized outputs, to get a more authentic Mara voice character.

**Method:**
1. Collected 5 min 18 sec of real NWR Mara recordings as training data
2. Trained RVC model (mara_v2.pth): 500 epochs
3. f0up_key=8 (verified: 176.1 Hz output vs 175.7 Hz reference)

**Results:**
- loss_mel settled at 18-19 (higher than Qwen model's 10.7 due to bandwidth-limited source)
- Produces authentic "radio-like" Mara voice (bandwidth-limited, as expected from
  2003-2016 recordings -- source audio was never wideband)
- Model: mara_v2.pth

**STATUS:** SUCCESS -- alternative model available for authentic Mara character

---

## Experiment 71: Tom Leak Fix (Short Recording Fallback) (2026-03-18)

**Goal:** Eliminate Tom's male voice leaking through on short recordings (letters
like "A", "P", "I") that failed AudioSR upscaling.

**Problem:** 243 short recordings were too short for AudioSR's diffusion model block
size, so they had no Mara audio. When the engine selected units from these recordings,
Tom's male voice leaked through.

**Method:**
1. Extract short recordings from Tom VDB
2. Run RVC directly on raw 8kHz audio (skip AudioSR step)
3. Slightly lower quality than AudioSR+RVC but eliminates male voice leakage

**Results:**
- All 243 short recordings now have Mara audio
- Total coverage: 6,849/6,849 recordings (100%)
- Quality is acceptable -- these are short phonemes where AudioSR quality
  matters less than voice identity

**STATUS:** SUCCESS -- zero Tom leaks remaining

---

## Experiment 72: WSOLA Boundary Smoothing (2026-03-20)

**Goal:** Reduce spectral discontinuities at unit boundaries in the WSOLA output
by applying Gaussian smoothing at lp boundary points.

**Method:**
1. Created `wsola_boundary_smooth.py` -- applies Gaussian kernel crossfade at lp boundaries
2. Tested strength values: 0.3, 0.5, 0.7, 1.0
3. Compared 13 audio quality metrics (spectral flux, pitch stability, HNR, etc.)

**Results:**
- strength=0.5 won 10/13 metrics vs unsmoothed baseline
- Spectral flux improvement: +21.3%
- Pitch stability improvement: +8.5%
- HNR improvement: +3.9%
- strength=0.5 and 0.7 produce identical results through engine (8kHz u-law bottleneck
  quantizes away the difference)

**STATUS:** CONFIRMED IMPROVEMENT -- strength=0.5 adopted as default

---

## Experiment 73: f0tr CART Tree Patching (2026-03-20)

**Goal:** Modify f0tr tree leaf predictions to improve Mara's prosodic range.

**Background:** f0tr leaf predictions feed DIRECTLY into WSOLA pitch modification
parameters. This is the first modification in 71+ experiments that changes voice
PERFORMANCE (prosody, cadence) rather than just timbre (voice identity).

**Method:**
1. Created `patch_f0tr.py` -- modifies f0tr leaf mean values
2. Tom's 55 leaves clustered tightly: 106.75-126.62 Hz (only 3 semitones of range)
   This creates a "prosodic straitjacket" -- monotone delivery regardless of content
3. Tested scale factor (multiply all leaf means) and expand factor (spread from median)
4. Formula: convert mean to semitones from median, multiply by expand, convert back

**Key discovery -- double correction bug:**
- scale=1.58 caused "drunk" sounding voice
- Root cause: BOTH RVC and WSOLA were shifting pitch independently
  - RVC already shifts Tom->Mara pitch via f0up_key
  - WSOLA then applies f0tr-based pitch modification ON TOP of RVC output
  - Result: double correction (pitch shifted twice)

**Correct approach:**
- scale=1.0 (keep median at Tom's 118 Hz -- RVC handles the pitch shift)
- expand=6.0 (spread leaf values around median to increase prosodic variation range)
- This gives WSOLA more dynamic range for intonation without fighting RVC

**Results:**
- User assessment: "sounds like Mara's broadcast cadence"
- Natural rising/falling intonation patterns restored
- No more monotone delivery on questions, lists, emphasis

**STATUS:** MAJOR BREAKTHROUGH -- first prosody-affecting modification

---

## Experiment 74: Spin-v2 Embedder for RVC (2026-03-20)

**Goal:** Test Spin-v2 as alternative to ContentVec embedder in RVC pipeline.

**Method:**
1. Trained mara_spinv2 model: 500 epochs on 30-minute dataset
2. Compared output quality metrics against ContentVec-based model

**Results:**
- e500: F0 doubled to 399 Hz (catastrophic pitch error)
- HNR degraded by -19.2 dB
- MFCC distance: ContentVec=48 vs Spin-v2=311 (6.5x worse)
- Spin-v2 is categorically worse than ContentVec for this voice conversion task

**STATUS:** FAILED -- ContentVec remains the correct embedder choice

---

## Experiment 75: Engine-Trained RVC Model (2026-03-20)

**Goal:** Train RVC on engine-synthesized output (100 sentences through Speechify)
to create a model that naturally handles engine audio characteristics.

**Method:**
1. Synthesized 100 sentences through the Speechify engine (6.2 min corpus)
2. Trained RVC model on this corpus: 500 epochs

**Results:**
- Loss bottomed at epoch 181 (27.74), then overtrained to 36.7 at e500
- Model learned "noisy low-bandwidth speech" characteristics, not Mara's voice
- 6 minutes of 8kHz engine output is insufficient training data
- Quality fundamentally limited by source audio bandwidth

**STATUS:** FAILED -- engine output too degraded for RVC training source

---

## Experiment 76: Pure Source RVC Model (2026-03-20)

**Goal:** Train RVC on ONLY confirmed direct Mara recordings (no weather-radio-
through-speaker recordings) to test whether source purity matters more than quantity.

**Method:**
1. Curated ~4 minutes of confirmed direct Mara recordings (clean, no rebroadcast)
2. Trained RVC model: 150 epochs
3. Compared against 30-minute mixed-quality model

**Results:**
- Converged extremely fast: loss ~19 at epoch 50 (vs much slower with mixed data)
- e100 won 6/6 articulation metrics
- e150 won 6/6 tonal quality metrics
- MFCC distance 129 (worse on paper than ContentVec's 48) but perceptually closer to real Mara
- Confirms key insight: **Source recording purity matters more than quantity** for RVC training. 4 minutes of clean direct recordings beats 30 minutes of mixed quality.
- User perceptual evaluation beats aggregate metrics for voice identity assessment

**STATUS:** CONFIRMED -- pure-source model is best for voice character, even if metrics are worse

---

## Experiment 77: FlashSR Evaluation (2026-03-20)

**Goal:** Evaluate FlashSR as replacement for AudioSR in the upscaling pipeline.
FlashSR uses one-step diffusion distillation, claimed 22x faster than AudioSR.

**Method:**
1. Tested ONNX version (too compressed, poor quality)
2. Tested GPU version (better quality but "carbonated" artifacts)
3. Tested lowpass_input parameter: False is better for 8kHz source material
4. Input requirement: 16kHz minimum, so pipeline is 8kHz -> librosa 16kHz -> FlashSR -> 48kHz

**Results:**
- Speed: confirmed ~22x faster than AudioSR
- Quality: promising but "carbonated" (fizzy) artifacts in some outputs
- ONNX version too aggressively quantized for this use case
- GPU version usable but needs further evaluation against AudioSR baseline

**STATUS:** CONFIRMED -- FlashSR is much faster and sounds slightly better than AudioSR. Artifacting is minimal (if any). FlashSR is a viable alternative for the upscaling step in the voice skin pipeline.

---

## Current status (2026-03-20)

### Recording switches progress
| Configuration | Switches | Method |
|--------------|----------|--------|
| Baseline (no hooks) | 40 | -- |
| Trim v1 (first-half only) | 42 | FAILED -- dl=1 artifacts |
| Trim v2 (all halves) | 37 | Trim alone, no hooks |
| Penalty hook (p=50) | 32 | Frida code cave at 0x8E8B854 |
| + PRSL injection + prune=3.0 | 29 | build_mara_rest.py + VCF |
| Full stack (trim v2 + all) | **29** | Best quality, user-approved |
| Spectral join cost cave | **29** | No change (Exp 65) |
| RVC + penalty (no trim) | 33 | Seamless transitions |
| RVC + trim v2 + full stack | **31** | Seamless, some phantoms |
| Voice skin (True Mara v2) | **28 / 24** | 28 raw, 24 with hook (Exp 69) |
| + f0tr patch + WSOLA smooth | **28 / 24** | Enhanced prosody (Exp 72-73) |
| Theoretical minimum | 2 | Dijkstra on recording graph |

### Current best config (2026-03-20)
- **Voice skin pipeline:** Tom 8kHz u-law -> FlashSR 48kHz -> RVC -> build_voice_skin.py
- **Post-processing:** patch_f0tr.py (scale=1.0, expand=6.0) + wsola_boundary_smooth.py (strength=0.5)
- **RVC settings:** pitch=8, protect=0.1, ContentVec embedder, Applio framework
- **RVC model:** Pure-source (~4 min clean direct recordings, Exp 76) or aimara.pth
- 6,606 recordings via FlashSR+RVC, 243 short via direct RVC fallback
- VCF: HALFPHONE_CAND_PRUNE_THRESH=3.0, HALFPHONE_CAND_MAX_UNITS=200
- Frida penalty hook: p=50 at 0x8E8B854

### Key discoveries (2026-03-20)
1. **f0tr predictions feed DIRECTLY into WSOLA pitch modification** -- first mod that changes
   voice PERFORMANCE (prosody/cadence) not just timbre (Exp 73)
2. **Double correction bug**: RVC shifts pitch via f0up_key, WSOLA shifts again via f0tr --
   scale must be 1.0 to avoid compounding. expand=6.0 for prosodic range.
3. **Source purity > quantity** for RVC training: 4 min clean beats 30 min mixed (Exp 76)
4. **Spin-v2 categorically worse than ContentVec** for this task (Exp 74)
5. **WSOLA boundary smoothing** improves spectral flux +21.3%, pitch stability +8.5% (Exp 72)
6. **User perceptual evaluation beats aggregate metrics** for voice identity assessment
7. **FlashSR** is 22x faster than AudioSR but has "carbonated" artifacts (Exp 77, in progress)

### Build pipeline (2026-03-20)
- `build_voice_skin.py` -- primary voice skin builder
- `patch_f0tr.py` -- f0tr CART tree leaf patching (scale=1.0, expand=6.0)
- `wsola_boundary_smooth.py` -- Gaussian smoothing at unit boundaries (strength=0.5)
- `build_voice_pipeline.py` -- consolidated MFA pipeline (Qwen approach, superseded)
- `batch_synth.py` -- batch synthesis for testing (superseded by build_voice_skin.py)

### Project status
- Mara voice: COMPLETE + ENHANCED (prosody via f0tr, smoothing via WSOLA)
- Craig voice: IN PROGRESS (DLL deep dive complete, patch_durt.py next)
- All reverse engineering goals achieved: VIN/VDB/VCF formats fully understood

---

## Experiment 78: DLL Deep Dive via Ghidra MCP (2026-04-03)

**Goal:** Understand the full synthesis pipeline across all three DLLs to identify levers
for improving Craig's prosody (currently sounds like "Tom's rhythm with Craig's voice").

**Method:** Connected Ghidra MCP server and decompiled key functions across:
1. SWIttsEngine.dll (orchestrator)
2. SWIttsUSel.dll (unit selection)
3. SWIttsWsola.dll (WSOLA synthesis)

**Key findings:**

### 1. Complete synthesis pipeline mapped
```
Text -> ESPR (Frontend) -> USel (unit selection + Viterbi) -> WSOLA (audio concat) -> Output
```
- ConcatTTSEngine::enhancedSPRCallback at 0x06B18F70 orchestrates the entire flow
- USel and WSOLA voices are initialized together; WSOLA receives USel voice handle
- Festival/Flite heritage confirmed (utterance relations, value types, CART features)

### 2. USel scoring has 6 components: S, D, DU, SP, J, F0
- Duration scoring: `|scale * (actual_dur - durt_prediction)|^2` weighted by DUR_WEIGHT
- F0 scoring: `|scale * (actual_f0 - f0tr_prediction)|^2` weighted by ABS_F0_WEIGHT
- Full config struct mapped (0xC8 bytes, 40+ parameters) from disassembly of FUN_08e90dc0

### 3. Undocumented emphasis system discovered
- `EMPH_ENABLED` (byte, default 0) -- absent from all existing VCF files
- `EMPH1/2/3_F0_OFFSET` (float) -- shifts f0tr predictions for emphasized words
- `EMPH1/2/3_DUR_OFFSET` (float) -- shifts durt predictions for emphasized words
- Triggered by `word_prominence` ESPR feature (SSML `<emphasis>` tags)

### 4. Craig vs Tom VCF comparison
- Only difference: `ABS_F0_WEIGHT = 0.05` (Craig) vs `0.2` (Tom)
- All other weights identical (DUR_WEIGHT=0.3, JOIN_COST_WEIGHT=0.7, etc.)
- HALFPHONE_CAND_MAX_UNITS missing from Craig VCF (defaults to 50)
- No emphasis parameters in either voice

### 5. WSOLA has two concat modes
- Mode 0 "Selective F0 smoothing": pitch-mark overlap-add at voiced joins (when f0tr loaded)
- Mode 1 "Plain WSOLA": simple overlap-add (no pitch data)
- Additional VCF params: `apply_target_prosody`, `dur_mods`, `amp_mods`
- **Critical**: durt trees do NOT modify output duration in WSOLA -- they only bias unit SELECTION

### 6. Root cause of Craig's "Tom prosody" problem
The Viterbi search optimizes: "find Craig units that best match TOM's predicted rhythm/pitch"
- Tom's durt trees -> Tom's target durations -> units selected to match Tom's timing
- Tom's f0tr tree -> Tom's target pitch -> units selected to match Tom's F0
- Per-unit dur_z and pitch_z in VIN still reflect Tom's original recordings
- Output timing = lp differences from Tom's VIN, not from durt predictions

### Proposed fixes (priority order)
1. **patch_durt.py**: Modify durt tree leaf values to match Craig's natural rhythm
2. **Update per-unit metadata**: Rewrite dur_z/pitch_z bytes to match Craig's actual audio
3. **VCF tuning**: Reduce DUR_WEIGHT, increase STRESS_MISMATCH_COST, add HALFPHONE_CAND_MAX_UNITS
4. **patch_f0tr.py**: Already proven for Mara; apply with Craig-appropriate parameters

**STATUS:** COMPLETE -- full DLL analysis done. Proceeding to patch_durt.py implementation.

See `reveng/DLL_ANALYSIS.md` for full decompilation details, struct layouts, and function maps.
- Build pipeline proven and repeatable for new voices

## Experiment 79: Per-word ToBI marks — live vs no-op A/B (2026-07-01)

**Question:** are the ToBI intonation marks the FE emits per word (`,H*` pitch
accents, `;L-L%`/`;H-H%` boundary tones) actually consumed by the back end, or
dead annotation?

**Method:** added an env-gated `SPFY_TAGGED_FILE` hook to `spfy_synth.c` that
feeds a tagged-output file verbatim into `spfy_fe_synth_tagged`, bypassing the
FE text pass. Captured the DLL FE's canonical tagged output for
"The men ran to the store." (`men` = `.1,H*`, `ran` = `.1` unaccented,
`store` = `.1,H*;L-L%`), then synthesized hand-edited variants and compared
WAV MD5 + `SPFY_DUMP_PATH` UID paths + `SPFY_SP_TARGET_DUMP` sp vectors.

| Variant | Edit | Result vs baseline |
| --- | --- | --- |
| v0 | none (verbatim tagged) | **byte-identical to the normal text path** (hook sanity) |
| v1 | remove `,H*` from "men" | **DIFFERENT audio** — units for "the men" fully re-selected (50151…52451,138479,21307 → 7338…7342,100672,12254,43219,43220), path re-converges at "ran" |
| v2 | add `,H*` to "ran" | sp targets DID change (sp[1] 2→3, sp[2] 1→3 on all 6 HP slots) but **byte-identical audio** — Viterbi optimum absorbed the bias |
| v3 | `H*` → `L*` on "men" | **byte-identical** — accent TYPE is discarded (only `*` presence survives into `syl_accent`) |
| v4 | `;L-L%` → `;H-H%` on "store" | **byte-identical** — boundary tones are inert in the stock 3.0.5 back end |
| v4 + `SPFY_PROSODY_REALIZE=1 BT_GAIN=2` | same | **DIFFERENT audio** (and v0-realize ≠ v4-realize) — the gated Option A layer makes tones live |

**Conclusions:**

1. Pitch-accent **presence** is live: `syl_accent` → sp[1] sylType / sp[2]
   sylInWord → durt/f0tr CART targets + SP cost matrices → target cost →
   selection. It **biases** selection; it does not command it (v2 shows the
   optimum can absorb a target change when the candidate pool doesn't offer a
   cheaper contoured alternative — consistent with Tom's degenerate all-zero
   `sylInWordCosts` matrix neutralizing sp[2]).
2. Pitch-accent **type** (`H*` vs `L*` vs `L+H*`…) is an explicit no-op in the
   stock path: the accent-code array built by `FUN_08e8a250` (`H*`=1…`L*+H`=6) is only
   consumed by the EMPH system, which is `EMPH_ENABLED=0` in every shipped
   VCF. (The FE only ever emits `H*` for plain text anyway.)
3. Boundary tones are a no-op in the stock back end — matching the fe-decomp
   finding that their consumers were never located in tier3. Sentence-final vs
   continuation prosody actually comes from the phrase terminator punctuation
   (`local_10` in the sp populator), which merely correlates with the tone mark.
4. Expressiveness levers that DO work: `EMPH_ENABLED=1` + SSML `<emphasis>`
   (engine-native, shifts f0tr/durt targets; audible on Tom), the
   `SPFY_PROSODY_REALIZE`/`SPFY_PROSODY_BT_GAIN` boundary-tone bias, per-word
   `pitch_st`/`rate_pct` markup, and now arbitrary hand-authored tagged text
   via `SPFY_TAGGED_FILE`. All selection-driven; corpus coverage caps the
   achievable contour depth.

## Experiment 80: Non-Tom voice formats — Jill support (2026-07-20)

**Goal**: work out how Jill / Felix / Javier / Paulina differ from Tom at
the voice-data level, and add support, starting with Jill.

### 80a. Container survey — all five voices

Walked the RIFF chunk tree of every `.vin` / `.vdb`. Result: **the
container is identical across all five voices** — same 14 top-level
chunks in the same order (`LIST vers cnts feat mean hash ckls cklx unit
f0tr durt ccos prsl hist`), all XOR-0xCE, all VDBs `fmt = (7,1,8000,
16000,2,16)` i.e. µ-law 8 kHz. Nothing exotic; the deltas are all
*inside* chunks.

Sizes that actually differ and matter: `ccos` scales as
`2*N*4*(8 + N(N-1)/2*4)` with `N = n_labels` (Tom 47, Jill/Felix 46,
Javier/Paulina 31), `mean` as `8 + n_halfphones*8*4` (92 vs 62).

### 80b. Unit record versions

| voice | `unit/vers` | stride | n_units |
|-------|:-----------:|:------:|--------:|
| Tom, Felix, Javier | 100006 | 29 | 169579 / 259660 / 219501 |
| Jill | **100008** | **30** | 185475 |
| Paulina | **100005** | **24** | 663410 |

**Felix and Javier are the same record format as Tom** — a genuinely
useful surprise, since it means their remaining work is front-end /
phoneset, not voice data.

Method: per-byte-column distribution statistics over each unit table,
aligned against the known v100006 layout. Jill's alignment is
unambiguous — one byte inserted at `+0x10`, and every structural
signature after it (the f0 triple's ~20% zero mode, the 0/1
`is_first_half` split, the constant pad, the 4 context bytes with their
255 sentinel, the 0/100 `context_cost`) reappears shifted by exactly one.

The new byte has 6 distinct values, range 1..6. Jill's VCF is the only
one that ships a `proscost.phoneInSylCosts` matrix, whose non-`UNDEF`
columns number exactly six. That identifies it: it is the column source
for the 5th proscost matrix, which v100006 voices simply do not have
(the loader hardcodes 6 = `SyllUnknown`).

This also **falsified** the README's long-standing claim that v100007+
shifts field *meanings* so in-mem `+0x0F` becomes `f0_end`. The
decompiled `load_chunky_index` writes the new byte to in-mem `+0x0E`
then advances the read pointer; semantics are version-invariant. Jill's
disk `+0x11` carries f0_start's zero-rate signature, not f0_end's.

### 80c. hp_class derivation — replaces the Frida dump entirely

Chased `tom_hpclass.bin` (a 169579-byte Frida capture) to see what a
Jill equivalent would cost. It costs nothing: it is derivable.

Steps, each falsifiable:

1. `hp_class = phone_center*2 + (1 - is_first_half)` → 93330 mismatches.
   Rejected.
2. Correlated the hp_class low bit against **every bit of every byte** in
   the record. Exactly one hit at 1.0000: `unit_id` bit 0. So the half
   side is the parity of the unit id — units are stored as consecutive
   (left, right) pairs. `is_first_half` is something else.
3. `phone_center*2 + (uid & 1)` → still 17275 mismatches, all of the form
   "phone index permuted". Per-phone the map was a clean bijection onto
   `{2p, 2p+1}` pairs, so a phone permutation, not a formula error.
4. Parsed `ccos/labl` (Pascal strings, not NUL-separated — the first
   attempt mis-split them) and `feat["name"]`. Tom's labl is **not**
   alphabetical: `ch dx d dh` and `el er en`. His feat order is. Matching
   the two by name gives exactly the observed permutation.

Final: `hp_class(uid) = labl_to_feat[phone_center] * 2 + (uid & 1)`,
verified **169579/169579 exact** against the Frida dump, in Python and
then again in C via the new `spfy_dump_voice --hpclass`.

Measured permutations: Tom = the d/dh/dx 3-cycle + en/er swap;
Jill/Felix/Javier = identity; **Paulina = 28 of 31 labels permuted**
(so the previous identity assumption would have produced garbage
S-costs for her).

This closes the `README_TECHNICAL.md` open question about "the 5 hp_base
anomalies" (`hp_base[pc] = labl_to_feat[pc] * 2`) and the
`voice_runtime.c` comment that no half-phone-name source was known.

Also: the pre-existing `spfy/data/jill_hpclass.bin` was 191871 bytes —
`unit/data ÷ 29` on a 30-byte table. Deleted; it was never valid.

### 80d. phoneInSyl target rule — recovered from ckls, no RE needed

Our FE was writing `sp[4]` as onset/nucleus/coda `0/1/2`. The matrix rows
are `UNDEF, WordInitial, SyllInitial, SyllMedial, SyllFinal, WordFinal,
SyllUnknown` — so `0` meant `UNDEF` (cost 100). Inert on Tom (weight 0),
wrong on Jill (weight 0.3).

Rather than guess, cross-referenced two independent sources inside Jill's
own VIN: the per-unit phoneInSyl byte, and the `_WORD_` / `_SYL_` unit
spans in `ckls`. Hypothesis tested on the 3504 units covered by both:

- word edge > syllable edge > medial, `SyllInitial` before `SyllFinal`
  → 99.20%, with all 28 errors of one type (predicted SyllInitial, actual
  SyllFinal — i.e. one-phone syllables).
- same but `SyllFinal` before `SyllInitial` → **3504/3504 = 100.00%**,
  confusion matrix fully diagonal.

### 80e. Result

Jill synthesizes. Tom's audit is unchanged at **8532/8532 PATH UID,
8684/8684 slot fidelity**, including with the derived hp_class table
substituted for the Frida dump — so the derivation is not merely
equal-looking, it is drop-in exact.

Per-voice cost constants are now read from each voice's VCF instead of
being hardcoded to Tom's. Tom's audit staying at 100% is itself a
check on the key→field mapping, since his VCF values equal the constants
they replaced.

**Not done**: Felix / Javier / Paulina need an es-MX / fr-CA front end
(`SWIttsFe-es-MX.dll`, `SWIttsFe-fr-CA.dll` exist in `bin/` but are
unhosted, and the phoneme tables are en-US). Paulina additionally needs
the v100005 gaps handled — no on-disk `phone_ctx[4]`, `flag_b` forced to
1 — before her S-cost and join shortcut mean anything.

## Experiment 81: Jill UID fidelity vs live engine (2026-07-20)

Server switched to Jill; captured 77 corpus phrases with the master hook
into `spfy/test/oracle/traces_master_jill`. The master child hooks needed no changes
(the `for hc < 94` voicing read over-reads 2 entries on a 46-label voice,
but those indices are never queried).

**Headline: structure 76/76, slot fidelity 2114/2114 (100%), path UID
1983/2114 (93.8%).** Tom unchanged at 8532/8532 throughout.

### 81a. Two independent confirmations of the 2026-07-20 RE

The capture validated Experiment 80 against the live engine:

- `hp_class_remap` (the engine's own voice+0x608 table) is **identity** for
  Jill and carries the `d/dh/dx` 3-cycle + `en/er` swap for Tom — exactly
  the permutation derived offline from `labl` x `feat`. Compared entry by
  entry: **92/92 exact for both voices** (Tom's 2 trailing entries are the
  hook reading past a 46-phone table with `n_labels=47`).
- `unit_layout_probe` in-memory `+0x0E`: **Jill = 1, Tom = 6** — the
  read-from-disk vs hardcoded-6 `phone_in_syl` split, observed live.

### 81b. `tom_swap` was still hardcoded — biggest single fix

Experiment 80 generalised `voice_runtime.c` but left two hand-written Tom
permutations: `tom_swap_label` (anchor_score.c) and `tom_swap`
(spfy_synth.c), which pick the **durt tree index** and the ccos S-cost
label. Both applied Tom's swaps unconditionally.

First Jill run showed it plainly: every error categorised `durt_both_diff`.
Replacing both with the voice's own `feat_to_labl` table:

| | slot fidelity | path UID |
|---|---|---|
| before | 87.4% | 71.3% |
| after  | **100%** | 79.1% |

### 81c. The engine dumps its own weights — found a swapped mapping

`inner_scorer.weights` carries the engine's live per-voice cost weights:

```
Tom  sp [0.05, 0.05, 0.05, 0.05, 0  ]  f0 0.2  d 0.3  flag 0.25  ccos 1     w_4c 0.8
Jill sp [0.2,  0.5,  0,    0.2,  0.3]  f0 0.3  d 0.2  flag 0.1   ccos 1.75  w_4c 1
```

Jill's VCF has `SYL_IN_WORD=0` and `WORD_IN_PHRASE=0.2`, so **sp[2] is the
sylInWord weight and sp[3] the wordInPhrase weight** — the opposite of the
obvious key-name reading, and consistent with the matrix order already in
`vcf_matrix.c`. Tom's four identical 0.05s made this unobservable on the
Tom corpus; Jill disambiguates it. Fixing the swap: **79.1% -> 93.8%**.

Everything else in that dump now matches our loader exactly, including
`w_4c` = `HALFPHONE_CAND_PRUNE_THRESH` (Tom 0.8 / Jill 1.0) and `flag` =
`UNIT_BIAS_WEIGHT`.

Jill also spells `SYL_IN_WORD_MISMATCH_COST` where every other voice
spells `SYLL_IN_WORD_`; without the alias lookup that weight silently
keeps its default.

### 81d. Residual localisation (93.8% -> where the 6.2% lives)

Two things were ruled OUT by direct measurement, not argument:

- **Preselection is not the problem.** Candidate pool sizes are identical
  engine-vs-ours on every slot of every phrase checked (text_004: 1/16/2/2/9/1;
  text_022: all 16 slots).
- **Per-candidate target cost is not the problem.** Sampled candidates are
  bit-exact: slot 0 uid 0 engine 13.27714 / ours 13.277136; slot 1 uid
  122032 engine 3.27961 / ours 3.279607; uid 145842 engine 3.62985 / ours
  3.629850.

What remains is Viterbi path choice among correctly-scored candidates from
correct pools. E.g. text_004 ("I.") — the engine takes the contiguous
same-recording run `145842..145845`; we take the locally cheaper 122032 at
slot 1 and lose the run. 52 of 76 phrases are exactly right; the misses
cluster in short utterances and `phn_*` items.

### 81e. REFUTED: JOIN_COST_WEIGHT / JOIN_COST_OFFSET on hash hits

Natural hypothesis for 81d: Jill weights joins 1.75 vs Tom's 0.7 and we
apply neither, so we under-value run continuity. Implemented
`cost = JOIN_COST_WEIGHT * cell + JOIN_COST_OFFSET` on the hash-hit path
and measured with each voice's own VCF values:

| voice | before | after |
|-------|--------|-------|
| Tom   | 100%   | **93.2%** |
| Jill  | 93.8%  | **89.7%** |

Both get worse, so the hypothesis is wrong: the hash cells are already
baked with whatever weighting the voice build applied. Reverted, with the
negative result recorded in-code at the `join_ctx_t` definition so nobody
re-tries it. (This also corroborates the older note in
`spfy_viterbi_replay.c` that applying JOIN_COST_WEIGHT here was a bug.)

## Experiment 82: Jill's last 6.2% — the prune is load-bearing (2026-07-20)

Goal: close Jill's 93.8% -> 100% **without moving Tom off 8532/8532**.
Result: not achievable by tuning the half-phone prune, and the reason why
is more interesting than the gap.

### 82a. The mechanism is the HP candidate prune

Drilled `text_004` ("I.", 6 slots, 2-candidate pools) against the engine's
`viterbi_leave` per-candidate `pre_dp`:

- slot 0 uid 0     engine 13.277136  ours 13.277136
- slot 1 uid 122032 engine  3.279607  ours  3.279607
- slot 1 uid 145842 engine  3.629850  ours  3.629850
- 14 further slot-1 cands: engine 10000 (reject sentinel), we score them

So target costs are bit-exact and pools are identical; at slot 1 our prune
even keeps exactly the same 2 the engine keeps. But at the next HP our
prune keeps 1 of 2 (`thresh = best + 1.0`, the other at `best + 1.883`)
while the **engine keeps both** — and the one we drop, uid 145844, is on
the engine's chosen contiguous run `145842..145845`.

Confirmed by A/B on the four worst phrases (text_004/019/021/022):

| | path UID |
|---|---|
| prune ON  | 28/70 (40.0%) |
| prune OFF | 68/70 (97.1%) |

### 82b. The engine demonstrably does not prune small pools

Tabulated every DP slot in both trace sets by pool size, recording the
largest surviving `pre_dp - best` delta:

```
pool_n   Tom max kept-delta   Jill max kept-delta
2        7.8152               7.3398
3        8.9585               8.4990
6        3.6080               3.5879
9        0.7750               0.9716
17       0.7492               0.9242
25       ~0.70                0.8987
```

For pool_n >= ~9 the plateau is exactly `THRESH - cum*SLOPE`
(Tom 0.8-0.005c, Jill 1.0-0.005c) — our formula is right there. Below
that the engine keeps candidates 4-10x further out than any threshold we
compute. Note this is true of **Tom too**, yet Tom audits 100% with our
tighter prune.

Also worth recording: Tom's traces contain **no 10000 sentinels at all**
while Jill's do, so the two voices don't even surface pruning the same way
in the capture.

### 82c. Three fixes tried; all improve Jill and all break Tom

| variant | Jill | Tom |
|---------|------|-----|
| baseline | 93.8% | **100.0%** |
| skip prune when pool_n < 3 | 94.0% | 99.9% |
| skip prune when pool_n < 5 | 93.6% | 97.5% |
| skip prune when pool_n < 13 | 94.4% | 95.1% |
| keep at least 2 cands | **95.4%** | 97.0% |
| keep at least 3 cands | 94.3% | 95.7% |
| prune off entirely | 80.6% | 85.4% |

Every variant that moves our prune **toward** the engine's observed
behaviour (keep more at small pools) gains Jill and loses Tom. That is
the finding:

> **Our HP prune is compensating for a downstream DP/join error.** Tom's
> 100% is not purely engine-faithful end-to-end — an over-tight prune is
> removing candidates our DP would otherwise mis-rank, and that masking
> happens to be exactly right on the Tom corpus.

Consequently Jill's residual cannot be closed from the prune, and cannot
be diagnosed from Tom's corpus at all: Tom is blind to it by construction.
The next lever is the join/DP scoring on cross-recording transitions,
which Jill exercises far more than Tom (Jill's paths take noticeably more
cross-rec joins).

Reverted to baseline. `SPFY_HP_PRUNE_MIN_KEEP=N` is kept as a diagnostic
(default off) since it is what demonstrated the masking; the rejected
pool-size cutoff is recorded in-code so it is not re-tried.

### 82d. Also ruled out this session

- **V0_/V1_/V2_ voiced-join weights**: absent from all five shipped VCFs,
  so that parameter family is simply unused. No lever there.
- `USE_F0_PROBABILITIES`, `use_joincache`, `use_edgeframes` are identical
  across all voices.

## Experiment 83: Jill residual isolated to the sparse-pool prune (2026-07-20)

Constraint lifted (engine-faithfulness over the audit number), so this pass
went after the actual mechanism rather than protecting Tom's 100%.

### 83a. Join cost is NOT the bug — 100% exact, both voices

The DP trace lets the engine's real join cost be recovered per transition:

```
engine_join(pred -> c) = dp_20(c) - pre_dp(c) - dp_20(pred)
```

Compared against `SPFY_JOIN_DUMP` (keyed by uid pair, so no slot alignment
needed):

| voice | transitions | agree |
|-------|------------:|------:|
| Tom   | 9,080  | **9,080 (100.00%)** |
| Jill  | 14,599 | **14,599 (100.00%)** |

Exact on all four paths — `same_rec`, `hash_hit`, `miss_gate`,
`miss_no_gate`. Combined with per-candidate `pre_dp` being bit-exact and
preselection pools being identical, **every input to the DP is correct for
Jill**; the earlier "downstream DP/join error" framing was wrong.

*Harness caveat worth remembering*: the same `(prev_join_key, curr_uid)`
pair legitimately has DIFFERENT join costs depending on which SLOT the
predecessor sits in, because the F0-gate inputs `c7c`/`c80` are
slot-context, not uid-derived. A first pass that keyed a dict on the uid
pair reported a spurious Tom disagreement (edge_007, "lighthouse",
81458->123738); collecting all values per pair removes it. Not an engine
bug.

### 83b. It IS the prune, and only at sparse pools

Survivor-set size histograms, engine vs ours (30 phrases each):

| size | Tom eng | Tom ours | Jill eng | Jill ours |
|------|--------:|---------:|---------:|----------:|
| 1    | 90 | 88 | 67 | **77** |
| 2    | 35 | 33 |  9 | **21** |
| 3    | 25 | 21 | 30 | **46** |
| 4    | 30 | 32 | 17 | **35** |
| 5    | 31 | 32 | 41 | **28** |
| 10   | 13 | 12 | 36 | **22** |
| mean | 15.28 | 14.95 | 14.66 | **13.92** |

**Tom's distribution matches the engine; Jill's is systematically
left-shifted** — we produce far more 1-4 candidate slots and far fewer
mid-size ones. So the histogram-prune formula is right (it reproduces Tom,
and Jill's mid/large pools), and the divergence is confined to sparse
pools.

### 83c. Why our threshold cannot reach the engine's at sparse pools

Our threshold is `best + bin_dist` with `bin_dist = (k+1) * 0.025` at the
break, and the break fires when `THRESH - cum*SLOPE < bin_dist`. Since
`cum >= 1` always (the best candidate is in bin 0), for Jill's `THRESH=1.0`
we get `lhs <= 0.995`, so the break always lands by `k=39`, i.e.

    bin_dist <= 1.0  for Jill,  always.

But the engine demonstrably keeps candidates well beyond that at sparse
pools — text_004 keeps one at `best + 1.883`, and measured max kept-deltas
reach 7.3 (Jill) / 7.8 (Tom) at pool_n=2. Enlarging the 40-bin array does
not help: with `cum>=1` the break still occurs at `bd=1.0`. So the engine
is not merely running our formula with a longer loop — it has additional
sparse-pool behaviour we have not reversed.

### 83d. Hacks that approximate it are NOT engine-faithful

`SPFY_HP_PRUNE_MIN_KEEP=2` gets Jill to 95.4%, but drops Tom to 97.0% —
and Tom's histogram already matches the engine, so forcing a minimum there
moves Tom AWAY from engine behaviour. The engine really does keep 67
single-candidate slots on Jill. A blanket minimum is therefore the wrong
shape and was not landed. Same verdict for the pool-size cutoff (Exp 82).

### 83e. Status / blocker

Everything measurable is now verified exact for Jill: preselection pools,
per-candidate target costs, all four join-cost paths, sp targets, ctx,
durt/f0tr leaves, and hp_class. The entire 6.2% residual is the
half-phone prune's survivor set on sparse pools.

Closing it needs the decompile of **FUN_08e88830** (the histogram prune).
Ghidra MCP was unavailable this session. A Frida hook would not help:
entry-only gives inputs we already have, and the computed threshold lives
mid-function in exactly the hot-path class that has killed the server
before.

Kept as diagnostics: `SPFY_PRUNE_DEBUG` now also emits `kept_uids`;
`SPFY_HP_PRUNE_MIN_KEEP` (default off).

## Experiment 84: FUN_08e88830 decompiled — prune solved, but it has a partner (2026-07-20)

Ghidra came up. `FUN_08e88830` is in **SWIttsUSel.dll** (base 0x08E80000),
called from exactly one site, `FUN_08e88de0` (InnerScorer) at 0x08e89397.

### 84a. Signature and parameters (from the call site)

```asm
08e8937f: MOV EAX,[ESI+0x24]     ; weights/config struct
08e89384: MOV ECX,[ESP+0x30]     ; running min cost
08e89388: MOV EDX,[EAX+0x50]     ; -> param_3  SLOPE
08e8938c: MOV ECX,[EAX+0x4c]     ; -> param_2  THRESH
08e89390: MOV EDX,[EAX+0x48]     ; -> param_1  MAX
08e89397: CALL 0x08e88830
```

`FUN_08e88830(this, int MAX, float THRESH, float SLOPE, float best)`.
`this+0x00` = n_cands, `this+0x18` = cand array, stride 0x18, cost at +4.

This **confirms `cfg+0x4c` == THRESH == the `w_4c` field the inner_scorer
hook already dumps** (Tom 0.8 / Jill 1.0), so the
HALFPHONE_CAND_PRUNE_THRESH mapping is right.

### 84b. Our histogram/threshold/sort/cap logic is all correct

Verified line by line against the decompile:

- binning: `(cost - best) * 40 < 40` -> `trunc(delta*40)`, else bin 39. Ours matches.
- scan: unrolled x10, `bd = (k+1) * 0.025`, break on
  `MAX < cum || THRESH - cum*SLOPE < bd`. Ours matches, including the
  `local_c8` starting at 2 (hence `(k+1)`, not `k`).
- keep test: the odd `*p < t == (*p == t)` idiom is just "break at first
  cost > thresh", i.e. keep `cost <= thresh`. Ours matches.
- final `if (MAX < *this) *this = MAX`. Ours matches.
- 40 bins exactly (`local_c8 < 0x2a` over 4 blocks of 10).

### 84c. The one thing we were missing: the bin-39 guard

```c
} while (local_c8 < 0x2a);
fVar2 = fVar2 + param_4;
if (iVar7 < 0x27) {          /* 39 */
    ... compact survivors ...
    *(int *)this = iVar9;
}
/* shell sort + MAX cap run regardless */
```

`iVar7` is the bin index the scan broke at (40 if it never broke). **The
engine only applies the threshold filter when the break bin is < 39** --
bin 39 is the clamp/overflow bin, so a threshold that reaches it is
meaningless and the engine declines to cut. We had no such guard.

It predicts both voices exactly, since `cum >= 1` always (best is in bin 0):

- Tom `THRESH=0.8`: `lhs <= 0.795` -> breaks by k=31, always < 39 -> Tom
  ALWAYS prunes. The guard is provably a **no-op for Tom** (confirmed:
  8532/8532 with it enabled).
- Jill `THRESH=1.0`: `lhs <= 0.995` -> needs `(k+1)*0.025 > 0.995` -> k=39
  -> **no prune** on sparse pools. Exactly the observed behaviour
  (text_004 keeping uid 145844 at best+1.883).

### 84d. ...but it is only HALF the mechanism

Enabling the guard alone made Jill **worse**: 93.8% -> 92.6%, survivor
mean 13.92 -> **17.33** against an engine mean of 14.66. We overshoot.

The caller explains why. `FUN_08e88de0` pre-initialises every candidate's
cost to the sentinel and abandons scoring early:

```asm
08e890ca: MOV dword ptr [EDX+EBX*0x1+0x4],0x461c4000   ; cost = 10000.0f
...
08e8918c/08e89250/08e892a6/08e8931f:
          FCOMP [ESP+0x10] ; TEST AH,0x41 ; JZ <skip>  ; partial > bound -> bail
08e8932a: FCOM  [ESP+0x30]                              ; vs running min
08e89335: FST   [ESP+0x30]                              ; running_min = cost
08e89339: FLD   [ESP+0x5c] ; FADD ; FSTP [ESP+0x10]     ; bound = min + slack
```

`0x461c4000` is exactly `10000.0f` — **this is the origin of the 10000
sentinels seen in Jill's DP traces** (Exp 82/83), not a prune marker.

So the engine cuts candidates twice: a running-min early-exit during
scoring, then the histogram prune. We implement only the second, and our
lack of the guard has been silently standing in for the first. Removing
the compensation without adding the real mechanism is a net loss.

### 84e. Status

Guard implemented but **gated off** behind `SPFY_HP_PRUNE_BIN39_GUARD=1`,
with the reasoning recorded at the code site. Enable it together with the
scoring-time early-exit in FUN_08e88de0, not before.

Baseline unchanged: Tom 8532/8532, Jill 1983/2114 (93.8%).

**Next step is now concrete**: implement the running-min early-exit in our
per-HP scorer (init cost to 10000, track `running_min`, bail when a
partial sum exceeds `running_min + slack`), then flip the guard on. The
one unknown left is the source of `slack` at `[ESP+0x5c]`.

## Experiment 85: the other half — running-min early exit. Jill 93.8% -> 98.0% (2026-07-20)

Implemented the companion to Exp 84's bin-39 guard. Both landed together.

### 85a. What FUN_08e88de0 actually does

Decompiled (SWIttsUSel.dll, `USelNetworkSlice::all_half_phone_costs`):

```c
fVar4    = *(float *)(cfg + 0x4c);   /* slack */
local_30 = 10000.0;                  /* running_min */
local_50 = 10000.0;                  /* bound       */
for each cand:
    cand.cost = 0x461c4000;          /* 10000.0f sentinel, pre-set */
    fVar14 = SP_sum + flag*w + 0;
    if (fVar14 <= local_50) { fVar14 += ccos_4cell * cfg[0x44];
    if (fVar14 <= local_50) { fVar14 += D_cost;
    if (fVar14 <= local_50) { fVar14 += F0_cost;
    if (fVar14 <= local_50) {
        if (fVar14 < local_30) { local_30 = fVar14; local_50 = fVar4 + fVar14; }
        cand.cost = fVar14;
    }}}}
FUN_08e88830(this, cfg[0x48], cfg[0x4c], cfg[0x50], local_30);
```

**The slack is `cfg+0x4c` -- the SAME field the prune takes as THRESH,
i.e. `HALFPHONE_CAND_PRUNE_THRESH`** (Tom 0.8, Jill 1.0). No new constant
to recover; the `[ESP+0x5c]` unknown from Exp 84 is just that field spilled.

Also note `best` handed to the prune is `local_30`, the running minimum
over SURVIVORS -- not a post-hoc min over all costs.

### 85b. Staged bail-out == single post-hoc test

Every component is non-negative (proscost cells, flag term, scaled ccos,
squared D, squared/MISSING F0), so the partial sums are monotonic:
`partial > bound` implies `full > bound`, and `full <= bound` implies every
partial passed. So the four staged tests collapse to one test on the full
cost. What does NOT collapse is the ordering: the bound tightens **in pool
order**, so this must be a left-to-right sweep, never a min-then-filter.

Implemented in spfy_synth.c immediately after the inner-scorer loop. This
finally fills in the `SPFY_HP_EARLY_EXIT_VAL` stub that had carried a
"fVar4 value is TBD via decomp of voice config loader" note since
2026-05-14.

### 85c. Result — both halves together

| configuration | Tom | Jill |
|---|---|---|
| neither (previous baseline) | **8532/8532 (100%)** | 1983/2114 (93.8%) |
| bin-39 guard only (Exp 84) | 8532/8532 (100%) | 1957/2114 (92.6%) |
| **both** | **8532/8532 (100%)** | **2071/2114 (98.0%)** |

Jill **93.8% -> 98.0%**, phrases with any wrong UID 24 -> 11 (65 of 76 now
perfect), structure 76/76 and slot fidelity 2114/2114 unchanged. **Tom is
byte-for-byte unmoved at 8532/8532**, as predicted: his THRESH=0.8 means
the scan always breaks by k=31, so the guard is a no-op for him, and the
early exit reproduces the candidate reduction his tighter histogram cut was
already approximating.

The A/B is exact: setting `SPFY_NO_HP_EARLY_EXIT=1` +
`SPFY_NO_HP_PRUNE_BIN39_GUARD=1` restores 100% / 93.8% precisely.

Audio unaffected (Jill pangram: 2.88 s, 65 units, speech-shaped envelope).

### 85d. Remaining Jill residual (2.0%)

11 phrases, concentrated in `phn_002` / `phn_007` (18/28 each) and
`nat_002` (6/18). Everything measurable is still exact: pools, per-cand
target costs, all four join-cost paths, sp targets, ctx, durt/f0tr,
hp_class. Next candidate is the shell-sort tie-break -- the engine sorts
survivors by cost with a secondary ascending compare on the int at cand+0
(`*(int *)(iVar4 + iVar12) <= *(int *)(iVar4 + iVar9)`), whereas ours is a
selection sort on cost alone. That would only bite on exact cost ties, so
it is a plausible fit for a small, stubborn residual.

## Experiment 86: Jill 98.0% -> 100.0%. The prune compare is x87-extended (2026-07-20)

Chased the last 2%. It was a single floating-point boundary, and fixing it
also corrected a 2026-05-14 mis-diagnosis.

### 86a. Localisation

`phn_002` ("This is the ae sound.") hp=17: pool 15, engine keeps 5, we
kept all 15 (`break_k=39, filtered=0`). Verified first that nothing else
differed:

- **pool order is identical** engine-vs-ours on all 28 slots (the engine's
  `prsl_slot` uid list vs our pre-prune pool). This matters because the
  early-exit bound tightens in pool order.
- survivor sets are a strict SUPERSET of the engine's everywhere -- we
  never lose a candidate the engine kept, we only keep extras.
- our early exit leaves 7 survivors at that slot; histogram `cum` reaches
  5 by bin 26 and stays there.

At k=38 the break test is `1.0 - 5*0.005f  <  39*0.025f`:

```
extended : 0.9750000005588  <  0.9750000145286   -> TRUE,  break at k=38
float    : 0.97500002384    <  0.97500002384     -> FALSE, no break
```

Both sides round to the SAME float, so a 32-bit comparison sees equality
and misses the break. Breaking at k=38 keeps exactly the 5 the engine
keeps.

### 86b. The disassembly is unambiguous

`FUN_08e88830` @ 08e888c6..08e888ee:

```asm
FILD [ESP+0x14]        ; (k+1)
FMUL [0x08e98520]      ; bd  = (k+1)*BIN_WIDTH      <- stays in ST, extended
FILD [ESP+0x10]        ; cum
FMUL [ESP+0xec]        ; cum*SLOPE
FSUBR [ESP+0xe8]       ; lhs = THRESH - cum*SLOPE   <- stays in ST, extended
FLD ST1 ; FCOMPP       ; break when bd > lhs
...
FADD  [ESP+0xf0]       ; bd += best, STILL extended
FSTP  dword [ESP+0x18] ; single rounding to float, here and nowhere else
```

Neither operand is rounded to 32-bit before the compare, and `best` is
added at extended precision before the one and only store.

### 86c. Corrects a 2026-05-14 note

That note claimed "the LHS must be stored to a float local before the
comparison ... engine (SSE2 / 32-bit precision) produces equality". The
engine is x87, not SSE2, and rounds neither side. The old fix and its
`SPFY_PRUNE_X87` escape hatch are removed.

Note `SPFY_PRUNE_X87=1` alone was ALSO wrong -- it left `lhs` extended but
compared against an already-rounded `bd`. That mix scores Jill 99.9% but
costs Tom 23 slots (8509/8532). Only extending BOTH sides satisfies both
voices.

### 86d. Result

| step | Tom | Jill |
|---|---|---|
| Exp 85 (early exit + bin-39 guard) | 8532/8532 | 2071/2114 (98.0%) |
| + x87 lhs only (`SPFY_PRUNE_X87`)  | 8509/8532 (99.7%) | 2111/2114 (99.9%) |
| **+ both sides extended**          | **8532/8532 (100.0%)** | **2113/2114 (100.0%)** |

**Jill: structure 76/76, slot fidelity 2114/2114, path UID 2113/2114.**
Tom unmoved at 8532/8532 and 8684/8684. Audio unaffected.

A/B (`SPFY_NO_HP_EARLY_EXIT=1`) drops Jill to 2018/2114 (95.5%), so the
early exit remains load-bearing.

### 86e. Sort tie-break: implemented, currently inert

The engine's shell sort breaks equal-cost ties on the int at cand+0 (the
uid), descending -- decoded from
`cost_j >= cost_jg && (cost_j != cost_jg || uid_j <= uid_jg)`. Implemented
as a total comparator (cost asc, uid desc), which makes our selection sort
agree with a shell sort regardless of algorithm since uids are unique in a
pool. Measured effect on both voices: **zero** -- exact cost ties do not
occur in either corpus. Kept because it is what the disassembly says;
`SPFY_NO_HP_SORT_UID_TIE=1` reverts.

### 86f. Remaining

One slot: `nat_001` 17/18. Everything else on the captured Jill corpus is
exact.

## Experiment 87: Jill on the FULL 235-entry corpus — 99.5% (2026-07-20)

Captured the remaining 159 corpus entries (`SPFY_DUMPWAV_TIMEOUT=120` for
the long ones); `spfy/test/oracle/traces_master_jill` now holds all 235. Verified
mid-flight that the server was still on Jill (`hp_class_remap n_labels=46`).

### 87a. Headline

| | Tom | Jill |
|---|---|---|
| phrases clean | 226/226 | 226/226 |
| structure | 225/226 (99.6%) | 225/226 (99.6%) |
| slot fidelity | **8684/8684 (100.0%)** | **8664/8684 (99.8%)** |
| path UID | **8532/8532 (100.0%)** | **8548/8592 (99.5%)** |

(Jill's UID denominator is larger than Tom's -- 8592 vs 8532 -- because her
phrases expand to more half-phone slots.) The one structure miss is
`longtext_sioux_001`, identical on both voices.

Only **6 of 226 phrases** diverge at all: nat_001, nat_024, nat_033,
nat_035, nat_043, nat_049.

### 87b. The residual splits cleanly in two

**(i) FE phoneme choice — 24 UIDs, 20 slots, 2 phrases.**
`nat_024` and `nat_049` carry every slot-fidelity failure
(ctx=20, ctx_neighbor=16, ctx_center=4, pool_n=12, durt_both_diff=4). The
pool/durt categories are downstream consequences: a different phone gives
a different triphone key, hence a different PRSL pool.

Root cause is one phone. `nat_024` = "...closed due to ongoing...":

```
TOM  engine:  d uw  dx uw  aa ng ...      "to" -> dx + uw
JILL engine:  d uw  dx ix  aa ng ...      "to" -> dx + ix
```

Both differ ONLY at slots 40/41. The FE DLL's tagged output is byte
identical for both voices (`<to (29,2) prep,0 [.0 dx(p100) ih(p100) ]>`) --
the tagged text is lossy and collapses ix/ih, so the real choice happens in
the refinement we apply on top.

Corpus-wide scan of every "to"-type site (t|dx + reduced vowel), both
voices:

| tom | jill | next is vowel | count |
|-----|------|---------------|-------|
| ax | ax | no  | 19 |
| ix | ix | no  | 12 |
| uw | uw | **yes** | 3 |
| uw | ix | **yes** | 2  <-- the only disagreements |
| ih | ih | no  | 1 |
| uw | uw | no  | 1 |

34 of 36 sites agree. Both disagreements are "to" before a vowel -- but
**Jill herself picks `uw` before a vowel at 3 other sites**, so this is NOT
a per-voice rule flip and cannot be fixed with a voice flag. Our `to`
vowel rule (project_to_word_rule_2026_05_13) was fit to Tom's traces and
is under-specified; the real engine rule keys on context we have not
identified. Deliberately NOT patched -- any heuristic that flips these 2
sites risks the 3 agreeing before-vowel sites.

**(ii) Pure selection — 20 UIDs, 4 phrases.**
nat_001 (17/18), nat_033 (70/72), nat_035 (165/174), nat_043 (112/120) all
have **100% slot fidelity** and no categories: correct ctx, sp, durt, f0tr,
pools. On nat_001 the survivor SETS match on all 18 slots and only the
ORDER differs (15 slots).

Relevant observation: the engine's DP candidate array is **not** sorted by
the `pre_dp` values the trace reports -- e.g. nat_001 slot 4 reads
`0.500749, 0.000002, 0.610103, 0.698729, 0.794023`. That is consistent
with the known two-sweep scoring: the array is ordered by sweep-1 costs
while the trace shows sweep-2 values. So a post-prune order difference is
expected and usually harmless; it only bites where a DP tie is decided by
position. This is the likely mechanism for these 20 UIDs.

### 87c. State

Tom byte-for-byte unchanged at 8684/8684 and 8532/8532 throughout. Build
clean, zero warnings.

## Experiment 88: multi-language FE — all five voices synthesize (2026-07-20)

Bundled the fr-CA and es-MX front-end DLLs alongside en-US and route the
choice off the voice's own VCF, so selecting a voice selects its FE.

### 88a. Build-time registry instead of one hardcoded DLL

`fe_host/CMakeLists.txt` previously embedded exactly one image
(`SWIttsFe-en-US.dll` -> `swittsfe_dll_data[]`). It now loops over
`SPFY_FE_LANGS` (default `en-US;fr-CA;es-MX`), embedding one blob per
language and generating a registry table mapping VCF language tag -> blob.
`spfy_fe_dll_for_lang()` does a case-insensitive lookup treating `-` and
`_` as equivalent.

New API `spfy_fe_open_lang(lang, ...)`; the old `spfy_fe_open()` forwards
with `lang = NULL`, which falls back to the first embedded image. Both the
native PE backend (fe_host.c) and the emulator backend (fe_host_emu.c)
implement it -- the emulator maps one image per process, so the first voice
opened wins there.

`spfy_voice_load` reads `tts.voiceCfg.language` from the VCF (already
parsed at that point, which it has to be since the DLL is chosen at open
time) and passes it through.

Sizes: en-US 7.5 MB, fr-CA 1.5 MB, es-MX 0.9 MB of DLL; the generated C is
~6.3x that. Trim `SPFY_FE_LANGS` for a faster build.

CMake gotcha worth recording: a literal `;` cannot be written inline in
the generated-source string -- CMake treats it as a list separator and
`\;` survives into the C file as a stray backslash. Build it from a
`set(_SEMI ";")` variable.

### 88b. Two format bugs found by fr-CA

**ckls `unk0` is conditional.** Felix failed to load with a bare "format
error". His `_WORD_` chunk group is genuinely EMPTY (0 tokens) -- he ships
syllable chunks but no word chunks. Our parser read the group header as
`name, u32 token_count, u32 unk0` unconditionally, so for an empty group
it consumed the next group's name record and then ran off the chunk. The
bytes it swallowed decode as `0x535F0005` = `u16 len=5` + `"_S"`, i.e. the
start of `_SYL_`.

`unk0` is present ONLY when `token_count > 0`. With that fixed all five
voices consume their ckls payload exactly: 394375 / 355308 / 816044 /
1013449 / 5234800 bytes. Also guarded the zero-size `calloc`s, which can
return NULL and get misreported as OOM.

**Phoneme symbols can contain `~`.** fr-CA marks nasal vowels with a
trailing tilde (`a~`, `o~`, `oe~`, `E~`). `p_parse_ident` stopped at the
tilde and the phoneme parser then failed looking for `(`, giving
`[fe_parse] error at offset 50`. Added `p_parse_phone_ident` (ident plus
`~`), kept separate from the word-name tokeniser so en-US/es-MX are
provably unaffected -- their symbols are pure alnum.

### 88c. Result

| voice | FE image selected | output |
|-------|-------------------|--------|
| Tom | SWIttsFe-en-US.dll | 0.89 s |
| Jill | SWIttsFe-en-US.dll | 0.85 s |
| Felix | **SWIttsFe-fr-CA.dll** | 0.62 s |
| Javier | **SWIttsFe-es-MX.dll** | 0.89 s |
| Paulina | **SWIttsFe-es-MX.dll** | 0.85 s |

The foreign FEs emit genuine target-language phonemes:
`bonjour` -> `b o~ Z u r`, `comment` -> `k c m a~ t`, `allez` -> `a l e`;
`hola` -> `O l a`, `buenos` -> `b w E n o s`, `dias` -> `d i A s`.

Audits unchanged: **Tom 8532/8532 (100%)**, **Jill 8548/8592 (99.5%)**.

### 88d. Next step is identified and safe

The engine phone-id table is still en-US only (`data/en_us_engine_phone_ids.csv`
-> codegen), so fr-CA/es-MX phones log
`'o~' not in engine phone-id table; falling back to VCF id`. The correct
source is the VIN's own `feat["name"]` order -- the same table that
defines hp_class.

Verified the swap is safe: the en-US CSV is **exactly** the VIN feat
order -- 45 entries, **0 mismatches**, and feat additionally supplies the
one phone the CSV lacks. So replacing the compiled-in CSV with a
VIN-derived name->index lookup (via `spfy_phone_order_t.phone_names`) is a
provable no-op for Tom/Jill and the correct generalisation for the other
languages. Blocked only on giving fe_parse access to the per-voice phone
order (phone_names are non-NUL-terminated slices into the VIN today).

Also still open for Paulina specifically: v100005 has no on-disk
`phone_ctx[4]` and forces `flag_b = 1`, so her ccos S-cost and same-rec
join shortcut need alternate sourcing (Exp 80).

## Experiment 89: phone ids from the VIN, not a compiled-in en-US CSV (2026-07-20)

The "safe plumbing" identified in Exp 88. The FE resolved phone symbols
through `data/en_us_engine_phone_ids.csv` (ARPAbet only), so fr-CA and
es-MX phones fell back to VCF ids with a warning per phone.

### 89a. Why it was safe

The engine's phone id IS the VIN `feat["name"]` index -- the same
numbering hp_class is built from (Exp 80). Checked the CSV against Tom's
feat order before touching anything: **45 entries, 0 mismatches**, and
feat additionally supplies the one phone the CSV lacks. So the swap could
only be a no-op for en-US.

### 89b. What changed

- `spfy_phone_order_t.phone_names` now holds OWNED, NUL-terminated copies
  instead of counted slices into the VIN, since every consumer wants C
  strings. Added `spfy_phone_order_index(po, name)`.
- `fe_parse` takes an optional `fe_phone_names_t { names, n }` and
  resolves symbols through it, falling back to the compiled-in table when
  NULL. Deliberately a plain array, not `spfy_phone_order_t`, so fe_parse
  keeps no dependency on voice/.
- New `spfy_fe_set_phone_names()` on both FE backends (native + emulator),
  plus an accept-and-ignore stub on the in-house `fe/fe.c` so non-hosted
  builds link without callers branching.
- `spfy_voice_load` passes `v->phone_order.phone_names`, which it owns and
  which outlives the FE.

### 89c. Result

| | before | after |
|---|---|---|
| Tom  | 8684/8684, 8532/8532 | **unchanged** |
| Jill | 8664/8684, 8548/8592 | **unchanged** |
| Felix / Javier / Paulina phone-id warnings | many per phrase | **0** |

All three foreign voices still synthesize. The en-US no-op was confirmed
by re-running both full audits.

### 89d. Trace storage

Jill's 235 master traces were living in `C:\tmp\jill_traces_master` and
got cleaned up by a temp sweep mid-session. They now live at
**`spfy/test/oracle/traces_master_jill`** (81.7 MB), alongside Tom's
`traces_master` (75.4 MB). Both are covered by the `*.json*` .gitignore
rule and neither is tracked, so this only changes where they survive --
not what is committed. Pass `--traces-master spfy/test/oracle/traces_master_jill`
to master_compare2.py.
