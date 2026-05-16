# spfy

Native C reimplementation of the SpeechWorks **Speechify 3.0.5** (2003) TTS
engine. Hard goal: **byte-exact 1:1 output** with the original Windows
engine on a fixed test corpus, then expand from there.

See [`../.claude/plans/if-you-recall-from-transient-sunrise.md`](../.claude/plans/if-you-recall-from-transient-sunrise.md)
for the full plan / milestone DoDs / gap list. Reverse-engineering notes
for the original engine live in [`../reveng/`](../reveng/).

---

## Status: working concat-mode synthesis ✅

As of 2026-05-05, the project produces **clear, intelligible Tom-voice
synthesis** from captured oracle traces, using only C code (no engine
runtime needed at synth time). The path is:

```
captured wsola_buffer JSONL  →  spfy_concat  →  WAV
```

Phonemes resolve correctly, cross-recording pair doubling is gone, and
duration matches the engine within ±10%. WSOLA proper (M4) and
chosen-UID Viterbi (M3.4) are still pending — those would close the
gap to byte-exact and let us synthesise from text alone.

### Milestone progress

| Milestone | Status | Validation |
|-----------|--------|------------|
| **M0a** Voice loaders + roundtrip | ✅ done | VIN+VDB byte-identical roundtrip on Tom (34MB + 59MB) |
| **M0b** Concat synth | ✅ done | Crystal clear synthesis on 5 test phrases |
| **M0c** Oracle harness | ✅ done | 200-corpus framework + 4 working hooks (function-entry only) |
| **M1** CART evaluator | ✅ done | **3366/3366 walks** match engine bit-exact f32 leaves |
| **M2** D/F0/SP/S costs + PRSL + ccos | ✅ done | All 4 cost components in C, unit-tested; PRSL+ccos+matrices load correctly |
| **M3.3a** Hash lookup | ✅ done | **1735/1735 probes** match engine f32-bit-exact |
| **M3.4a** Viterbi DP module | ✅ done | Pure DP + 7 unit tests (single-slot, multi-slot, join-driven, forbidden, unreachable, invalid args, long-double accumulation) |
| **M3.4b** Replay CLI on real voice | ✅ done | `spfy_viterbi_replay` runs DP across full cost stack + PRSL + hash on captured `wsola_buffer` traces |
| **M3.4c** PRSL alignment (per-slot) | ✅ done | New `prsl_slot_hook.js` (function-entry on `FUN_08e91dc0` = `USelNetwork::AddUnit`) captures the engine's per-slot context tuple + returned pool. Decoded `context_key = ctx[1]*10000 + ctx[2]*100 + ctx[3]`. **C-side pool exactly equals engine's captured pool on 100% of slots across 30 corpus entries** (822 slots). |
| **M3.4d** Real D + F0 targets + same-rec aug | ✅ infrastructure | `spfy_viterbi_replay` now optionally consumes `cart_walks.jsonl` for engine-true per-slot durt/f0tr leaves AND augments each slot's pool with `chosen[s-1]+1` to model same-recording continuation. Across the corpus: **822/822 slots run the full DP (100%), 794/822 chosen-in-pool (96.6%)**. |
| **M3.4e** Engine-truth S + SP targets | ✅ done | New `inner_scorer_hook.js` (function-entry on `FUN_08e88de0` = `USelNetworkSlice::all_half_phone_costs`) captures per-slot 5-element SP target row indices (`USelNet+0x2c/0x28/0x34/0x38/0x3c`). S-cost target context derived from `prsl_slot`'s slice ctx[0,1,3,4] with halfphone-to-phone conversion. 32/32 corpus captured cleanly. |
| **M3.4f** Engine weight readout | ✅ done | Extended `inner_scorer_hook.js` to read the weight struct at `slice+0x24` (the engine's effective per-utterance weight values). Found: SP weights are `[0.05, 0.05, 0.05, 0.05, 0]` (we'd hardcoded `[0.1, 0.1, 0, 0, 0]`); **MISSING_F0_COST = 5.0** (we'd hardcoded 1000.0 -- 200x too punishing); D=0.3, F0=0.2, CCOS=1.0 match VCF. Replay CLI now uses engine-actual weights. Also captured global `_DAT_08e98580 = 0.01` (flag-term scale). |
| **M3.4g** In-memory record layout decode | ✅ done | Added a one-shot byte-dump probe to `inner_scorer_hook.js` that reads 24 bytes of well-known unit IDs at `voice+0x20 + uid*0x18`. Confirmed in-mem layout: `mem+0x00..0x03 file_idx`, `+0x04..0x07 local_pos`, `+0x08..0x09 dur_like`, `+0x0a..0x0d 4 SP bytes`, `+0x0e hardcoded sp_phone_in_syl`, `+0x0f f0_start`, `+0x10 f0_end`, `+0x11 f0_mid`, `+0x12 f0_context`, `+0x13 halfphone_class (= phone*2 + side_encoding)`, `+0x14 phone_center`, `+0x15 is_first_half`, `+0x16 flag_b`, `+0x17 context_cost`. Phone_ctx lives in a separate table at `voice+0xc4 + uid*4`. **Critical implication: the engine's "D-cost" formula reads `mem+0x12` which is `f0_context`, NOT `dur_like`** -- the VCF param named DUR_WEIGHT scores `f0_context`-vs-CART, not duration. After replacing `cand.dur_like` with `cand.f0_context` in the D-cost: aggregate match jumped from **123/822 (15%) to 268/822 (32.6%)** -- more than doubled. |
| **M3.4h** Hp-class remap, flag term, join-cost formula | ✅ done | (a) Captured the engine's hp_class remap table from `voice+0x600+8 -> ptr -> u32[94]` -- maps slice.ctx encoding to ccos forest index. Mostly matches our computed permutation, but Tom has 4 phone-pair swaps (phones 9/10/11 and 14/15). (b) Wired the flag term `cand.context_cost * 0.25 * 0.01 = ctx_cost * 0.0025`. (c) Fixed join formula: was `join = hash_value` on hit / `JOIN_COST_OFFSET` on miss; engine actually uses `J = JOIN_COST_WEIGHT * hash + JOIN_COST_OFFSET` on hit and `J = JOIN_COST_WEIGHT * 10000 + JOIN_COST_OFFSET` on miss (with `MISSING_JOIN_COST = 10000`). **MAJOR WIN: aggregate match 268 -> 434 (32.6% -> 52.8%)**, with several utterances now at 75-87% (`text_022` "How do you do?" 87.5%, `text_028` "Why?" 87.5%, four "tiny" entries at 83%). |
| **M3.4j** Same-rec adjacency rule + diagnostics | ✅ done | Added a `SPFY_DEBUG_MISMATCH=1` env-driven per-slot dump that prints chosen vs best-path UIDs side-by-side with their target costs. Diagnostic showed most mismatches were cases where chosen had *worse* target cost but the engine picked it anyway -- meaning the engine sees a much *cheaper* join cost. Decompile of `FUN_08e8b620` (`USelGraph::ViterbiWithJoinCache`) revealed two key rules we were missing: **same-recording adjacency** (`curr_uid == prev_uid + 1` AND `curr.flag_b != 0` => J = 0, free transition!) and **raw hash hit** (no JOIN_COST_WEIGHT multiplier per the decompile). Wiring same-rec adjacency + tuning the miss constant via `SPFY_JOIN_MISS` env sweep landed at **446/822 (54.3%)**. The raw hit value experimentally underperformed our scaled formula (`0.7*hash + 0.2`) by ~5 slots; scaled is now the default with `SPFY_JOIN_RAW=1` reverting. |
| **M3.4k** Per-cand engine totals + cost_s cand-byte fix | ✅ done | Extended `inner_scorer_hook.js` with `onLeave` handler that reads engine per-cand totals from `cand_buf+0x04 + cand_idx*0x18` and a Python comparison script ([test/diff/compare_cand_totals.py](test/diff/compare_cand_totals.py)). Diff revealed silence boundary slots had a ~20 cost gap (engine high, ours low). Decoded voice+0xc0 (Tom's cand context table -- equal to disk phone_ctx) vs voice+0xc4 (fallback when 0xc0 == 0) vs voice+0x604 (S-cost target context label remap, distinct from voice+0x608 hp_class remap). Found the engine reads cand byte directly as a column offset with no L[] indirection and no bounds check -- sentinel 255 lets the offset spill past the row into adjacent rows of the contiguous matrix. Removing the OOB skip in `cost_s` for cand_byte: **446 → 460 (54.3% → 56.0%)**. |
| **M3.4l** s_ctx_remap, proscost row order, matrix 2/3 swap | ✅ done | Three fixes from the per-cand diff diagnostic. (a) **s_ctx_remap**: target context conversion now uses captured `voice+0x604` table (mostly `ctx >> 1` but with phone-pair swaps 9/10/11 and 14/15 in Tom). (b) **proscost row order**: VCF rows arrive in encounter order ("ContextUnknown" first), but the engine indexes by col-label position ("PhrInitial" at row 1). Added a placement pass that puts each row at the index whose col-label matches its row name. (c) **matrix 2/3 swap**: voice+0x3ac stores sylInWord data and voice+0x470 stores wordInPhrase -- the OPPOSITE of VCF logical order. Confirmed via Frida read of voice memory cells. Swapped `KIND_NAME[2]` and `KIND_NAME[3]` in the loader. **MAJOR WIN: aggregate match 460 → 546 (56.0% → 66.4%)** with the diff median collapsing to 0.000 and |delta| p90 to 0.000 -- nearly all per-cand totals now match engine bit-close. Also verified other proscost matrices (0 sylInPhrase, 1 sylType) and ccos slot ordering all match -- no other unintentional swaps found. |
| **M3.4m** Silence boundary residual diagnosis | ✅ partial | The remaining ~10pp gap to 100% is concentrated at utterance silence boundaries (slot 0 uid=0, slot N-1 uid=last). Diff dump shows engine total at slot 0 ≈ 21 vs ours ≈ 2.2 (gap +19); slot 11 ≈ 8 vs ours ≈ 22 (gap -14). Probed `voice+0x5fc` (per-halfphone-class voicing flag) -- silence (hp 64/65) has flag=0, so engine SKIPS f0tr walk for those slots (F0 cost = 0, same as ours). The S-cost path is the source of the boundary gap, likely in how cand sentinel byte 255 reads through our contiguous matrix vs engine's allocation. Probing the engine ccos cells at the specific (hp, slot, row, col) coordinates the boundary cands hit is the next investigation. |
| **M3.4n** hp_class fallback fix + Tom phone-pair swaps hardcoded | ✅ done | Discovered the chosen UID's `is_first_half` byte does NOT reliably indicate which halfphone side -- some units flagged "0" are LEFT class, others RIGHT, depending on context. Engine resolves this via slice.ctx[2] (always engine-truth halfphone class). Switched our `score_candidate` hp_class lookup to prefer slice.ctx[2] over the chosen-UID fallback. Also hardcoded Tom's phone-pair swaps (9→10→11→9 cycle, 14↔15 pair) directly in `voice_runtime.c` so utterances without a captured `hp_class_remap` still get the right ccos lookup. **Aggregate match: 554 → 602 (67.4% → 73.2%)**. Diff p90 now 1.3 (was 2.6 on text_010). Many utterances now at 85-92% (`text_029` pangram 92.2%, `text_016` 89.5%, `text_019` 90.5%). |
| **M3.4o** Engine-truth join formula + miss_offset capture | ✅ done | New `viterbi_consts_hook.js` (function-entry on `FUN_08e8b620` Viterbi) captures the actual weight-struct fields the join cost uses: `gate_weight = 0.6` at `weight+0x28`, `miss_offset = 1000` at `weight+0x84`, plus a 100-entry ccos gate curve at `voice+0xc8`. Switched `join_cb` from empirical scaled formula (`0.7*hash + 0.2`) and tuned `miss=5/50/etc` to the engine-truth: **raw `hash_value` on hit, `miss = miss_offset = 1000` on miss**. Counterintuitively, this BEAT the empirical tuning: **602 → 612 (73.2% → 74.5%)** — the previous tuning was masking real scoring inaccuracies elsewhere; engine-truth values are the right baseline. Also removed `SPFY_JOIN_RAW` env override; `SPFY_JOIN_MISS` retained for diagnostic sweeps but defaults to engine value 1000. |
| **M3.4p** Signed `cand_byte` for ccos S-cost — closes +18.85 mystery | ✅ done | Disasm at `0x08e891a8` / `0x08e891ad` / `0x08e891c1` / `0x08e891ca` (voice+0xc0 path) and `0x08e891e4` / `0x08e891ec` / `0x08e8920a` / `0x08e89215` (voice+0xc4 path) all use `MOVSX` (sign-extend) on the four ccos cand bytes. Our `cost_s.c` was treating them as unsigned. For uid 0 cand_ctx `[255, 255, 34, 2]`, byte 0xff = signed −1 → engine reads at `(target_label*stride − 1)` (in-bounds row 31 col 46 of the slot's matrix). Fixed `cost_s.c` to interpret cand byte as `int8_t`. **Verified bit-exact on text_001 utt 1 slot 0 cand 0: ours 21.0894 ↔ engine 21.089**, max delta on text_001 dropped from +18.85 to +5.000 (one residual in F0 voicing-gate, see M3.4q). Aggregate stays at 612/822 because the +18.85 cands were all 1-cand silence boundaries (selection forced); the fix matters for future bit-for-bit accuracy. SP/D/flag cand bytes verified UNSIGNED (`MOVZX` at `0x89104`/`0x89125`/`0x89134`/`0x89145`/`0x89156`/`0x8910c`/`0x8925f`); only the four ccos cand bytes are signed. |
| **M3.4q** F0 voicing-flag gate + voice-wide sibling fallback | ✅ done | Two compounding bugs that both stem from Frida hook one-shot dump events. **(a)** F0 voicing gate: engine's InnerScorer skips f0tr entirely when `voice+0x5fc[slice.ctx[2]] == 0 && weight+0x8c == 0`, but our `score_candidate` only checked `tgt->has_f0tr` (which is TRUE because cart_walks_hook tags walks with the LAST InnerScorer's slot — including walks that come from BuildGraph/PostScoringAdj for unrelated slots). Wired the engine's gate inputs through a new `voicing_gate_t` struct, captured from `inner_scorer_hook.js`'s `join_consts.voice_5fc_per_uid` block. **(b)** Sibling fallback: `join_consts`, `hp_class_remap`, and `s_ctx_remap` events all fire only on the FIRST inner_scorer call of a Frida session — so only `text_001.jsonl` carries them; texts 002-030 inherited nothing and fell back to inaccurate `>>1` heuristics for `s_remap`/`hp_remap`. Added `load_voice_wide_only(sibling)` that pulls the three voice-wide tables from `text_001.jsonl` if the current trace lacks them, since they're voice-properties (not utterance-specific). **HUGE WIN: aggregate match 612 → 658 (74.45% → 80.05%)** with per-cand totals bit-exact on text_001/010/011/016/029 (max delta = 0.000, |d| p99 = 0.000). Per-text wins: text_002 +13, text_010 +11, text_029 +6, text_030 +6, text_009 +6. The remaining ~20% gap is now selection-side (same-rec aug pool, DP tie-breaks). |
| **M3.4r** Viterbi DP exit instrumentation + architectural finding | ✅ done | New `viterbi_dp_hook.js` on `FUN_08e8b620` (entry+leave) dumps the engine's full per-slot HP cand state. **H1 ruled out**: `pre_dp == InnerScorer total` bit-exact. The remaining 20% gap was architectural — engine's BuildGraph builds a variable-width slot DAG that collapses long same-recording runs into single anchor slots. Tree-internal anchor cands have `cand+0x10` (the "join_key") = run-TAIL uid, distinct from `cand+0xc` (= run-HEAD uid). For text_001 utt 2, the wsola chain `[0, 136536, 78696..78703, 139550, 169578]` (12 phoneme positions with an 8-unit run) collapses into only 5 Viterbi anchor slots in the engine. |
| **M3.4r Phase A** Trace-fed engine-truth DP (100% bit-exact) | ✅ done | New `spfy_engine_graph_replay` CLI consumes the engine's captured slot graph + `cand+0xc` (uid) + `cand+0x10` (join_key) + per-cand `cand+0x68/0x78/0x6c` DP-state inputs + the gate curve from `viterbi_consts`, runs our C-side DAG Viterbi DP with engine-truth join formula (same-rec adjacency, raw hash hit, miss = 1000 + smooth where smooth = `0.6 × gate_curve[clamp(curr.c6c − offset_k − pred.c7c, 0, 99)]` when `curr.c6c > 20 ∧ pred.c80 < 15 ∧ pred.c7c > 20`). DP maintains per-cand state `c7c`/`c80` updated per the engine's rule: if `cand+0x68 < 21` inherit pred state (Tom: `c80 = 100`); else reset (Tom: `c7c = c68`, `c80 = 0`). **MASSIVE WIN: 100.00% slot-level match (772/772 across all 32 corpus entries, 30/32 utterances), up from 80.05% pre-M3.4r.** Validates the entire DP + join + cost stack is bit-for-bit engine-faithful given the slot graph + per-cand fields as ground truth. |
| **M3.4r Phase B1 + B1.5** FE utterance-tree dump | ✅ done | New `viz/frida_hooks/fe_tree_hook.js` (function-entry on `SWIttsUSelUnitSelection`) walks the engine's Festival/FreeTTS-style relation tree (Word, Syllable, Segment, Phrase, SylStructure) and emits per-IR `(rel, ir, shared, next, prev, parent, daughter)` topology + per-relation features (`name` for Word/Segment/Phrase, `stress` for Syllable). Uses Frida `NativeFunction` to call the engine's own `FUN_08e94590` for relation/feature dict lookup. Decoded layout: ItemRelation `+0`→shared, `+8`→next, `+0xc`→prev, `+0x10`→parent (only when prev=0), `+0x14`→daughter; Shared `+0`→features dict, `+4`→cross-relation links dict; Festival path syntax `n`/`p`/`nn`/`pp`/`parent`/`daughter`/`daughter1`/`daughtern`/`R:Relation`. Full corpus captured to `traces/fe_tree/`. |
| **M3.4r Phase B2** BuildGraph in C | 32/32 utts (100%) | `src/usel/build_graph.c::spfy_build_graph` consumes a parsed `spfy_fe_utt_t` (FE word→syl→seg structure), allocates phrase + word + syllable + halfphone slots, runs post-order DFS to assign slot indices. Produces the engine's exact slot tree from FE input alone. New `spfy_build_graph_replay` CLI corpus harness. |
| **M3.4r Phase B3** LinkGraph in C | 1230/1230 slots (100%) | `spfy_link_graph` produces per-slot predecessor lists. Algorithm decoded empirically: for any non-root slot S, walk up via parent until first ancestor with a left sibling; `preds(S) = exit_chain(left_sibling)` where `exit_chain(P) = [P, P.last_child, P.last_child.last_child, ..., leaf]` outer-first. Internal slots (Word/Syllable) inherit naturally because they share the same first-ancestor entry point. **All 1230 slots across 32 utterances bit-for-bit match the engine's captured predec topology.** |
| **M3.4r Phase B4 step 1** slice.ctx[5] derivation in C | 822/822 halfphones (100%) | `src/usel/slot_ctx.c::spfy_derive_slice_ctx` produces ctx[5] per halfphone-leaf slot from FE segment names alone. Encoding: `ctx[i]` = hp_class of the same-side phoneme at offset `(i-2)`, OOB → silence sentinel (`pau_L`=64 / `pau_R`=65). hp_class = `label_idx*2 + side` (interleaved). Tom's 47-phoneme inventory hardcoded (42 covered by corpus, 5 placeholder gaps). |
| **M3.4r Phase B4 step 2** PRSL pool query | 822/822 halfphones (100%) | Wired existing `spfy_prsl_lookup` (M2 infrastructure) with `context_key = ctx[1]*10000 + ctx[2]*100 + ctx[3]` from the derived ctx[5]. **All 822 halfphone-leaf slots' candidate pools (UID list including order) bit-for-bit match the engine's captured `prsl_slot.uids`** across the entire corpus. |
| **M3.4r Phase B4 step 3** per-cand cost computation | ⏳ pending | Compute `pre_dp` for each halfphone-leaf cand from the FE tree + voice + ctx. Needs CART tree feature kernels (q_type 1..8 values for DURT/F0TR walks) + per-slot SP target indices (engine's `InitTargetWorkspace` `FUN_08e918f0`). Cost stack itself (D/F0/SP/S/FLAG/voicing-gate) is already validated bit-exact via M3.4q. |
| **M3.4r Phase B4 step 4** PostScoringAdj internal-slot scoring | ⏳ pending | Identify multi-unit same-recording runs from chosen path; emit anchor cands at the tree-internal Word/Syllable slots with `cand+0x10 = run_tail_uid` and `cand+0x68/0x78/0x6c` smooth-miss inputs. RE work on `FUN_08e8d210 → FUN_08e8ce60 → FUN_08e897b0 / 08e89530 / 08e893b0`. |
| **M4** WSOLA proper | ⏳ pending | Hann + pitch-sync overlap-add for engine-quality smoothing |
| **M5** Public C API freeze | ⏳ pending | After M3.4/M4 ship |
| **FE F0–F4** Frontend (text→SPR) | ⏳ pending | SWIttsFe-en-US still black-box |

Parallel-track FE work hasn't started; CORE track was prioritised.

---

## Build

The canonical 1:1 reproduction build is **Linux x86_64 GCC** with the x87
long-double toolchain (matches MSVC 7.1 (2003) FP semantics — see plan
"bit-exact FP strategy"):

```sh
cmake --preset x87
cmake --build build/x87
ctest --preset x87
```

For Windows iteration there's a wrapper that handles the msys2 path
quirks:

```cmd
build.bat              REM configure + build
build.bat rebuild      REM clean then configure+build
build.bat test         REM configure + build + ctest (Defender may flag)
```

`build.bat` builds into `C:\tmp\spfy_build\` (out of `Documents\` to
avoid Windows Defender heuristic-flagging mingw test exes). Override
with `set SPFY_BUILD_DIR=...`. Output is **NOT bit-exact** with the
oracle on Windows — it's for fast iteration on parsing / loaders /
unit tests only. The Linux x87 build is the canonical target.

### Toolchain setup (once)

You need msys2 with mingw64 GCC + Ninja + CMake:

```
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja
```

`build.bat` references `C:\msys64` by default (`set MSYS_ROOT=...` to
override).

---

## Layout

```
spfy/
  CMakeLists.txt
  CMakePresets.json
  cmake/
    Toolchain-x87.cmake          canonical build (Linux x86_64)
    Toolchain-sse-bisect.cmake   FP-divergence bisection only
  build.bat                      Windows iteration helper
  include/spfy/                  public C API (frozen at M5)
    spfy.h
    spfy_voice.h
  src/
    common/   xor_cipher (obfuscation), riff, log, file_io
    voice/    VIN/VDB/VCF loaders, ccos, unit_table, feat_table,
              vdb_lookup (name->offset), vcf_matrix (proscost), voice_runtime
    cart/     CART evaluator (durt + f0tr)
    usel/     hash, PRSL, costs (S, D, SP, F0)
    wsola/    ulaw decoder, wav writer (M4 proper WSOLA pending)
    fe/       (FE track, F2+)
    cli/      spfy_dump_voice, spfy_cart_replay, spfy_score_one,
              spfy_hash_replay, spfy_concat, spfy_synth (stub)
  test/
    oracle/   200-phrase corpus + run_corpus.py + run_frida_capture.py
              + run_cart_replay.sh + traces/ subdirs
    diff/     wav_diff.py + stage_compare.py
    unit/     test_common.c (single combined runner; 59 tests passing)
  docs/
    RESUME.md                    where-to-look-first for picking up cold
    (Frida hooks live in ../viz/frida_hooks/; not symlinked.)
```

---

## CLIs

All produced by the build into `<build>/src/cli/`. Run with no args to
see usage.

| CLI | Purpose | Status |
|-----|---------|--------|
| `spfy_dump_voice` | Voice introspection (`--unit`, `--proscost`, `--prsl`, `--ccos`, `--resolve`, `--roundtrip-vin`, `--roundtrip-vdb`) | ✅ all modes work on Tom |
| `spfy_cart_replay` | M1 acceptance: replay captured `cart_walks` JSONL through C CART evaluator, verify bit-exact leaves | ✅ 3366/3366 |
| `spfy_score_one` | M3.2 sanity: score one candidate end-to-end (D + F0 + SP + S, weighted from VCF) | ✅ |
| `spfy_hash_replay` | M3.3 acceptance: replay captured `hash_lookup` JSONL, verify bit-exact f32 cost match | ✅ 1735/1735 |
| `spfy_concat` | M0b: concat oracle-chosen units to WAV with pair detection + cross-rec WSOLA-lite blend | ✅ crystal clear |
| `spfy_viterbi_replay` | M3.4: replay captured `wsola_buffer` chosen-UID sequences through C-side Viterbi DP and report match% | ✅ DP runs; PRSL alignment partial |
| `spfy_synth` | Full text→WAV CLI (lands at M5) | stub |

---

## Critical bugs found and fixed

1. **xor_cipher → obfuscation rename.** Windows Defender heuristic-
   flagged the small mingw `test_xor_cipher.exe` as malware. Renamed to
   `obfuscation.{h,c}` (matches `SWIttsRiffEncryption` enum naming) and
   merged unit tests into one larger binary. Tests now run on the
   Windows dev host.

2. **`feat` chunk is a dictionary, not a flat array.** Format is
   `repeated { u16 key_len; key; u32 entry_count; (u16 nlen + name + u32
   stored_id) * entry_count }`. 16 keys total in Tom; the one we want is
   `"filename"` with 8118 recording names. Naive flat-list parse gave
   8252 entries that mixed metadata (`name`, `power`) with recordings,
   misrouting `unit.file_idx` to bogus targets.

3. **`unit.file_idx` is the `stored_id`, NOT the positional index** in
   `feat.filename`. The first/last entries happen to match
   (date_001 stored_id=0, weather7_082 stored_id=8117) so naive
   positional lookup passes boundary tests. Middle entries diverge by
   ~100. Example: `positional[3869]` is `news27_076` (stored_id=3969);
   the unit referencing `file_idx=3869` should use `news26_067` (the
   entry with stored_id=3869). **This was the dominant cause of "Tom
   but garbled" output.** Fix: sort feat entries by stored_id at load
   time, then `entries[stored_id]` is canonical. The README warned
   "feat.filename not in stored_id order" — it meant the engine
   indexes by stored_id, the file is in some other order.

4. **`hash` lookup `rows[r]==0` is NOT necessarily empty.** The last
   valid uid (169578 in Tom) legitimately has `rows[169578]=0` with
   real entries at `cells_A[0..]`. The verification key
   `cells_A[index] == uid_right` is the only authoritative miss
   signal. Original docs said "0 means no entry" which is true only
   for `rows[169579..]` padding the engine never queries.

5. **`cells_A` stores `uid_right_owner`, not `uid_left`.** The
   README's earlier asm annotation `cmp [esi+eax*8], ebx ; ebx =
   uid_left` was wrong; live capture proved `eax != ebx` on hits, so
   they cannot both be `uid_left`. Correct: `eax = uid_left` (offset),
   `ebx = uid_right` (verification key); the cell stores which
   uid_right "owns" this slot after suffix-sharing collapse — standard
   FKS-style perfect hash.

6. **Phoneme doubling on cross-recording pairs.** When the engine
   selects halfphone pair (first, second) from DIFFERENT recordings,
   each contains the nominal phoneme from a different source, and
   naive sequential concat plays it twice. Fix: pair detection in
   `spfy_concat` — same `phone_center` + same `file_idx` + adjacent
   `local_pos` is a clean same-rec pair (play one continuous span);
   different recordings is a cross-rec pair (linear-blend both halves
   over `max(d1,d2)` so the first-half burst is preserved at full
   volume at the start, transitioning into second-half steady-state
   without doubling).

7. **`unit.dur_like` vs `trace.dl` semantics.** For paired halfphones
   the first half typically has `trace.dl=0` (boundary marker) and the
   second has the engine's prosody-adjusted synthesis duration. Use
   `trace.dl` when present, fall back to `dur_like` (recording's
   natural duration) for `dl=0` standalone units.

---

## Frida hook safety policy

After 4 server kills in one development session (2026-05-05), the
policy is **function-entry hooks ONLY**. Mid-instruction
`Interceptor.attach` inside `SWIttsUSelUnitSelection`'s x87 FP loops
destabilises the engine stochastically — accumulated trampoline
trips perturb x87 stack state / EFLAGS / register conventions enough
that the engine eventually AVs. Same hook can run cleanly on 1735
probes once and crash at 512 probes the next time.

**Active hooks** (function entries, low-frequency):

| Hook | Address | Frequency | Purpose |
|------|---------|-----------|---------|
| `viterbi_hook.js` (legacy) | 0x8E819E0 / 0x8E88830 / 0x8EE65E0 | low | per-synth + per-halfphone + per-utterance |
| `prsl_lookup_hook.js` | 0x8E89A70 | per utterance | actually `USelNetwork::BuildGraph` (NOT PRSL — DLL_ANALYSIS.md was wrong; corrected by Ghidra decompile) |
| `prsl_slot_hook.js` | 0x8E91DC0 | per slot | M3.4c: `USelNetwork::AddUnit` captures `(slot, ctx[5], n_cands, uids[])` per halfphone target. Validated against full 32-entry corpus -- 0 server kills. |
| `inner_scorer_hook.js` | 0x8E88DE0 | per slot | M3.4e: `USelNetworkSlice::all_half_phone_costs` captures 5-element SP target row indices per slot (read from `USelNet+{0x2c,0x28,0x34,0x38,0x3c}[slot]`). 32/32 corpus captured -- 0 server kills. |
| `wsola_buffer_hook.js` v2 | 0x8EE65E0 | per utterance | input unit lattice (this is what `spfy_concat` consumes) |
| `fe_token_hook.js` v2 | 0x8E819E0 | per synth | gap #10 (FE token format) |
| `ulaw_lut_dump.js` | (export) | rare | gap #7 (u-law LUT verification — currently produces 0 events) |

**Retired hooks** (mid-instruction, hot-path — DO NOT re-add):

| Hook | Why retired |
|------|-------------|
| `hash_lookup_hook.js` | Hooks a `cmp` instruction inside the Viterbi inner loop. Already produced 1735 probes (enough to validate the C lookup bit-exact). |
| `cart_walks_hook.js` | Hooks per-question dispatcher inside the CART scoring loop. Already produced 3366 walks. |

If hot-path probe data is genuinely needed in the future, use Frida
**Stalker** (transcoded execution) instead of Interceptor on the hot
instruction. Stalker is slower but more robust.

The hook runner ([test/oracle/run_frida_capture.py](test/oracle/run_frida_capture.py))
filters its `HOOK_JS` map accordingly. The retired hook files are still
in [`viz/frida_hooks/`](../viz/frida_hooks/) with DANGER banners and
rationale; do not re-add them to the map without a fix.

---

## Cost component formulas (as implemented)

From [`reveng/DLL_ANALYSIS.md`](../reveng/DLL_ANALYSIS.md) "USel scoring",
all using `long double` accumulators with final cast to `float`:

```
D-cost  = | scale * (stored_dur - durt_pred_mean) |^2 * DUR_WEIGHT
F0-cost = MISSING_F0_COST                                          if stored_f0 == 0
        = | scale * (stored_f0 - f0_pred_mean)   |^2 * ABS_F0_WEIGHT  else
SP-cost = sum(k=0..4) of weight[k] * matrix[k][target_feat[k]][cand_byte[k]]
S-cost  = ccos_weight * sum(slot=0..3) of
              ccos[hp_class*4+slot][L[target.ctx[s]]][L[cand.ctx[s]]]
```

Where:
- `scale` is the durt/f0tr leaf's "variance" field (used as 1/stddev)
- 5 `proscost` matrices come from VCF `<param name="...proscost.X.Y">`
  blocks, parsed by [src/voice/vcf_matrix.c](src/voice/vcf_matrix.c)
- `ccos` matrices are 94 hp_classes × 4 slots × (47×47 symmetric f32),
  loaded from VIN with per-slot scaling (LEFT [0.2, 1.0, 0.5, 0.1],
  RIGHT [0.1, 0.5, 1.0, 0.2])
- `L[]` and `hp_class[]` come from
  [src/voice/voice_runtime.c](src/voice/voice_runtime.c) (Tom-specific
  identity maps; other voices may need a halfphone-name source)

---

## How `spfy_concat` works (M0b synthesis path)

```
captured wsola_buffer JSONL trace
    ↓ parse_play_list → [(uid, dl), ...]
    ↓ annotate via unit_table  → file_idx, local_pos, dur_like, phone_center, is_first_half
    ↓ pair detection:
        ├─ same phone_center + same file_idx + adjacent local_pos
        │     → SAME-REC PAIR: play recording[first.lp .. second.lp + second.dur_like]
        │     → ONE continuous span = natural phoneme audio (no doubling)
        ├─ same phone_center + different file_idx
        │     → CROSS-REC PAIR: linear-blend both halves over max(d1, d2)
        │     → preserves first-half burst (stop consonants), no doubling
        └─ no neighbor match
              → standalone: trace.dl if non-zero else unit.dur_like
    ↓ resolve recording: feat[stored_id == file_idx].name
        → vdb_lookup_by_name(name) → (data_offset, size)
        → audio bytes = vdb.data[data_offset + local_pos*8 ..]
    ↓ u-law decode → s16
    ↓ 6 ms linear cross-fade at every unit boundary
    ↓ WAV write (8 kHz mono s16le)
```

---

## Validation summary

| Stage | Match | Notes |
|-------|-------|-------|
| VIN/VDB byte roundtrip | 100% byte-exact (34+59 MB) | M0a |
| CART (durt + f0tr) | **3366/3366 walks** | M1 |
| Hash lookup | **1735/1735 probes** | M3.3a |
| Concat synth | sounds correct on 5 corpus phrases | M0b user verification |

End-to-end byte-exact WAV match against the engine still requires M4
(WSOLA proper) and M3.4 (Viterbi self-selection from PRSL pool). Once
those land, the chosen-UID and per-component validations transitively
demonstrate every cost component matches.

---

## Test corpus

200-phrase target stratified into:
- 50 single-phoneme sanity (`.0a`, `.1k`, ...)
- 50 minimal-pair voiced/unvoiced joins (WSOLA stress test)
- 50 random natural phrases
- 50 edge cases (long pauses, all-vowel, all-consonant)

Currently **32 entries** committed in
[`test/oracle/corpus.jsonl`](test/oracle/corpus.jsonl) as a working
seed. Per-phrase oracle WAVs are *not* committed yet (run
`run_corpus.py` once on Windows with the engine alive to populate
[`test/oracle/wavs/`](test/oracle/wavs/)).

---

## What's next

The natural ordering for getting to byte-exact end-to-end:

1. **M3.4p — DONE.** +18.85 was the ccos S-cost reading cand byte 255
   as unsigned 255 vs engine signed −1. Disasm shows `MOVSX` for
   ccos cand bytes (only). Fixed in `cost_s.c`; text_001 utt 1 slot 0
   cand 0 is now bit-exact (21.0894 vs engine 21.089). Aggregate
   match unchanged at 612/822 because the affected cands are 1-cand
   silence boundaries (selection forced).

2. **M3.4q — DONE.** Voicing-flag gate wired in + sibling fallback for
   the three voice-wide tables (`voicing`, `hp_remap`, `s_remap`)
   that the Frida hook only dumps on the first session. Aggregate
   jumped to 80.05%; per-cand totals now bit-exact on the texts
   spot-checked (text_001/010/011/016/029).

3. **M3.4r — DONE (Phase A)**. **100.00% bit-exact** match on the
   captured corpus (772/772 slots) via `spfy_engine_graph_replay`,
   which consumes the engine's slot graph + per-cand `cand+0x10`
   join_keys + smooth-miss state inputs from the captured
   `viterbi_dp` trace. The DP + join + cost stack is now validated
   engine-faithful. Phase B (BuildGraph + PostScoringAdj in C, for
   synthesis from text alone) is the remaining work — see Phase plan
   below.

### M3.4r Phase B — text-only synthesis (next milestones)

To synthesize bit-for-bit from text alone (no captured viterbi_dp
needed), implement in C:

   - **B1**: RE the FE input — extract phrase / word / syllable /
     segment hierarchy from the FE-side `param_2` of
     `SWIttsUSelUnitSelection`. `FUN_08e93xxx` API surface is the
     entry point.
   - **B2**: BuildGraph (`FUN_08e89a70` + `FUN_08e8a130`) — post-order
     tree traversal allocating Viterbi slot indices.
   - **B3**: LinkGraph (`FUN_08e8c700` + `FUN_08e89f80`) — predecessor
     relations from the tree.
   - **B4**: PostScoringAdj internal-slot scoring (`FUN_08e8d210` +
     `FUN_08e8ce60` + `FUN_08e897b0` multi-unit span SP cost) and
     `cand+0x10` (run-tail uid) computation — this is what gives
     anchor cands their score and join_key.

2. **M4 WSOLA proper** — Hann window + pitch-synchronous overlap-add to
   smooth cross-recording transitions. The naive `spfy_concat` already
   handles unit selection correctly; WSOLA closes the audio quality gap.

3. **M5 Public C API** — freeze `spfy.h` surface, harden the synth CLI.

4. **FE track (F0–F4)** — text → SPR → halfphone target. Independent
   parallel track; `SWIttsFe-en-US.dll` is currently black-box.

---

## See also

- [`docs/`](docs/) — deeper architecture notes (TBD)
- [`test/oracle/README.md`](test/oracle/README.md) — oracle harness usage
- [`test/oracle/TRACE_SCHEMA.md`](test/oracle/TRACE_SCHEMA.md) — JSONL trace format
- [`../reveng/README_TECHNICAL.md`](../reveng/README_TECHNICAL.md) — master format spec for VIN/VDB/VCF
- [`../reveng/DLL_ANALYSIS.md`](../reveng/DLL_ANALYSIS.md) — engine pipeline + function maps
- Memory notes (auto-loaded by Claude Code): `~/.claude/projects/.../memory/MEMORY.md`
  — feat dictionary format, stored_id vs positional, Frida policy, hash semantics
