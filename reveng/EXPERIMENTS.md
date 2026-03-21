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
- Craig voice: next (same pipeline, new RVC model)
- All reverse engineering goals achieved: VIN/VDB/VCF formats fully understood
- Build pipeline proven and repeatable for new voices
