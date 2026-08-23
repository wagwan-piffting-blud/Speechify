# Voice builder spine

Seven stages, derived from Festival's clunits build as it maps onto the shipped
VIN chunks. Evidence and derivation: `reveng/README_TECHNICAL.md`, section
"Festival 1.4.2/1.4.3 provenance sweep".

The interface is `vb.h`. Nothing is implemented yet - this directory currently
defines the contract and records what already exists in Python, so the port has
a target and the gaps are visible rather than discovered halfway through.

## Why a spine at all

There are 382 Python tools in `reveng/spfy4/tools/`, and the *build path*
through them is about 2,500 lines (`vb_build1.py` 1,485, `vb_vin.py` 297,
`vb_vinpatch.py` 300, `vb_corpus.py` 239, `vb_select.py` 164). The rest are
instruments and should stay in Python.

The friction is not the language. It is that the build has no declared stage
order, so every tool re-derives what it needs and no tool can say what it
depends on. Festival's `do_all` is that declaration, and SpeechWorks demonstrably
worked from it.

## Stage → chunk → what exists today

| # | Stage | Festival origin | Chunks | Bytes (tom) | Today | Status |
|---|-------|-----------------|--------|-------------|-------|--------|
| S1 | CORPUS | `db_utts_load` + `find_same_types` + `name_units` | `ckls` `cklx` | 394 K + 81 K | `vb_corpus` `vb_render` `vb_relabel` `vb_ckls` `vb_build1` | **generates** |
| S2 | FEATURES | `acost:dump_features` | `feat` | 134 K | `vb_build1` `vb_f0` | **generates** |
| S3 | NORM | `utts_load_coeffs` + EST `meansd()` | `mean` | 2.9 K | `vb_mean` ("confirm, then generate") | **generates** |
| S4 | JOIN | `acost:build_disttabs` | `hash` | **22.1 M** | `hash_build.c` · `spfy_vb_hashgen` · `spfy_hash_roundtrip` · `vb_hashio` `vb_hash_apply` | **generates** |
| S5 | PRESEL | *replaces* `acost:find_clusters` | `prsl` | 4.97 M | `vb_prslbuild` ("vendors' admission rule") | **generates** |
| S6 | TREES | `acost:collect_trees` (wagon) | `durt` `f0tr` `ccos` `hist` | 31.9 K + 2.4 K + 1.63 M + 424 B | `vb_trees_apply` (leaves only) · `vb_ctxcost` (derives meaning) | ⚠ **partial** |
| S7 | PACK | `acost:save_catalogue` + container | `unit` `vers` `cnts` + `.vdb` `.vcf` | 4.92 M | `vb_build1` `vb_vin` `vb_vinpatch` `vb_compact` | **generates** |

## Implemented so far

- `join_cost.{c,h}` - the S4 join cost, ported from the shipped `edgeFrames`
  scorer in `SWIttsUSel.dll`. Kernel, inverse-SD weight derivation, 3-point
  boundary distance with the seam doubled, the affine map, and the hard-zero
  rule for natural continuations. Covered by four tests in
  `test/unit/test_common.c` that pin the vendor-specific behaviours rather than
  generic distance behaviour.
- `../usel/hash_build.{c,h}` + `spfy_hash_roundtrip` - packing and the S4
  acceptance gate.
- `../common/riff_write.{c,h}` + `spfy_riff_roundtrip` - the container writer,
  byte-identical on four vendor containers.

- `edge_frames.{c,h}` - builds the two frames per unit out of the VDB: dim 0
  from the unit record's `f0_start`/`f0_end`, dim 1 dead, dims 2–13 as 12 MFCC.
  31 of jill's 185,475 units fail to resolve (0.02%).
- `spfy_vb_joincost` - calibration harness. On jill the derived weights come out
  at exactly `1/(2*dim-4)` and the cost shape matches the vendor to **4%** on
  `p99/p50`; scale is a free gauge carried by `spfy_jc_t.raw_scale`.

**S4 is generative, and the formula is fully derived** - the 3-point
combination was resolved by tracking ESP through `FUN_08e8d3a0`: it is
`kernel(end(L),end(R-1)) + 2*kernel(end(L),start(R)) + kernel(start(L+1),start(R))`,
a **sum**, with no scaling anywhere in the function.

⛔ The claim that byte-identity is "unreachable for `hash` for informational
reasons" is **RETRACTED** (2026-08-16). It reasoned about the reader; `hash` is
1.71 M measurements of the vendor's own metric, and once the 3-point
combination is known, recovering the spectral representation is a well-posed
least-squares problem - one quadratic form, 352 unknowns against 1,711,648
observations. It is fitted, not guessed, by `spfy_vb_jcfit`:

```
hand-picked 12 MFCC, per pair       0.379      <- what "shape agrees to 4%" hid
fitted quadratic form, held out     0.562
same form on a voice never fitted   0.547      <- jill -> tom
CEILING control (known M)           1.0000     FLOOR control (shuffled) 0.0004
```

The fit also corroborates `FUN_08e8d3a0` from data the disassembly never saw:
with edge-anchored frames the 3-point form beats seam-only by +0.107, and with
centre-anchored frames - where `end(R-1)` and `start(R)` are literally the same
frame - it does not, exactly as the degeneracy predicts. Byte-identity is still
not demonstrated, but the gap is bounded on every axis tested and is **not**
explained by our holding u-law audio. See `SPEC_S4_hash.md`.

⛔⛔ **AND THE COST MATTERS MORE THAN THIS FILE USED TO SAY.** The guidance
"selection is insensitive to the exact metric, calibrate the distribution
instead" is **REFUTED**. Measured in the only currency that counts - units the
engine actually picks differently, over 6,070 slots:

```
vendor (no-op control)   0.00%     0 differing slots, 15/15 byte-identical
const  (no cost at all) 17.07%
mfcc   (scale-calibrated guess) 21.22%     <- WORSE than having no cost
shuffle (actively wrong) 27.02%
```

Break-even against a flat constant sits at per-pair **r ≈ 0.67**. A guessed
metric below that is worse than shipping no metric. The old advice survived
because Exp 65 reported "byte-identical output" from a harness that was never
shown able to detect a change.

## Stage specs

- [`SPEC_S2_feat.md`](SPEC_S2_feat.md) - the feature registry. `feat` is the
  *schema* (wagon's `.desc`), not the per-unit rows; those live in `unit`.
- [`SPEC_S4_hash.md`](SPEC_S4_hash.md) - the join-cost table. Format closed and
  verified against jill.vin; generation decomposed into domain / packing / cost.

## S4 has been run on a voice of ours

`spfy_vb_hashgen --fix-domain --rows-units --mode const` over **donnart**, the
first application of any of this to a voice we built. It found a defect the
Python packer had been shipping silently: `vb_hash_pack.c` leaves its cell array
uninitialised, an unwritten cell reads back as key **0** - a valid `uid_right` -
and the ones landing inside row 0's window **resolve**, handing 196,850 left
units a free join into one specific unit. A further 5.10 M live cells (38.9 MB)
were unreachable by any lookup. Both vendors carry zero of either, which is the
control.

The vendor invariant `cost == 0 ⟺ r == l+1` removes them with no heuristic:
`uid_right == 0` cannot be a continuation, so every zero-cost non-adjacent pair
is definitionally not a pair. `--fix-domain` drops those and adds back every
genuine continuation from the engine's own same-rec test.

```
                    hash      n_cells     fill   S4 gate         slots moved
donnart           78.7 MB   9,668,472   43.7%   SEMANTIC FAIL         -
donnafix          60.7 MB   7,417,121   58.3%   SEMANTIC PASS    267 (3.16%)
donnas4           60.7 MB   7,417,121   58.3%   SEMANTIC PASS  1,011 (11.96%)
```

Container **100.5 MB → 82.4 MB**. `donnafix` fixes only the domain; `donnas4`
also ships the constant this spec recommends. Not yet judged by ear.

**Remaining in S4:** generating the domain from prsl + adjacency rather than
recovering it from an existing table, and the density gap - 58% fill against the
vendor's 67–72%. See `SPEC_S4_hash.md`.

## The remaining real gap

**S6 topology.** `vb_trees_apply` recomputes `durt` leaves but keeps Tom's tree
structure, and deliberately leaves `f0tr` alone. `ccos` (1.6 MB) has been
*characterised* by `vb_ctxcost` but not generated. So the prosody models are
still substantially Tom's.

It has the shape S4 used to have: the chunk is understood well enough to read
and patch, but not yet produced from our own corpus. That is worth naming,
because it means a voice built today is partly **the base voice wearing our
audio** (Jill, for current work) - which is a plausible contributor to the
friction of "building from scratch". S4 was ~65% of the container by bytes and
is now ours; `ckls`/`cklx`/`mean`/`f0tr`/`durt` still are not.

## Rules the spine enforces

1. **No stage reaches backwards.** Each is a pure function of its inputs. A
   stage may be recomputed without re-running its predecessors.
2. **Linguistic features come from `src/fe`.** Never from MFA, never from a
   separate FE harness. Training and synthesis then read the same values by
   construction rather than by discipline.
3. **Boundaries come from the engine** (`SPFY_UID_DUMP`) for voices built from
   our own renders. See `project_segmentation_from_engine`.
4. **A stage is COMPLETE only if it generates.** Patching a vendor chunk into
   place does not count, and the table above says so explicitly.

## Not ported

Diagnostics stay in Python: `vb_why*`, `vb_*check`, `vb_*audit`, `vb_asr*`,
alignment, RVC, everything using parselmouth / sklearn / MFA. There is no C
version of `vb_asrsig.py` worth having.
