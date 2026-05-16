# Anchor pre_dp scorer (Python reference, 94.41% bit-exact)

Reference Python implementation of the engine's anchor cand pre_dp formula
(M3.4r Phase B4.4). Reproduces the engine's selection on 169/179 = 94.41%
of corpus anchor slots. C port of this scorer is the path to text-only
synthesis (Phase C).

## Files

- `anchor_predp_v7.py` -- main reference implementation. Uses
  `../../data/tom_hpclass.bin` for engine-truth halfphone_class lookup
  (the critical breakthrough; 53.6% → 94.41% jump).
- `anchor_predp_v6.py` -- earlier round, kept for diff.
- `vcf_parse.py` -- VCF nibble cipher decoder + proscost matrix loader.
- `ccos_parse.py` -- ccos chunk parser to flat float table.
- `cklx_ckls_parse.py` -- chunk-table parser (cklx + ckls).
- `build_hpclass_table.py` -- rebuilds `tom_hpclass.bin` from a fresh
  Frida `unit_hpclass_dump` capture (only needed if .bin is lost).

## Run

```
python anchor_predp_v7.py
```

Expected output:
```
slots: 179
top-N pre_dp == actual: 169/179  (94.41%)
```

## Algorithm summary (decoded from FUN_08e8ce60 + FUN_08e8adc0/897b0/89530/893b0)

1. Per posting: 4-cell ccos at boundary (first/last hp ctx vs ss/se phone_ctx).
2. Phase 1 dynamic-threshold pruning (drop cands with cost >= norm + best_so_far).
3. 50-bin histogram pruning (DAT_971d8 = 0.1 bin width, DAT_98a24 = 50 norm).
4. For surviving cands: full cost = ccos_4cell + FLAG_sum*w_3c*0.01 +
   SP-span + D-span (POOLED Mahalanobis) + F0-span (per-unit Mahalanobis,
   voicing-gated by `unit_mem+0x13`).

## Critical engine-truth dependencies

- `../../data/tom_hpclass.bin` (169 KB): per-uid mem+0x13. Required.
- VCF / VIN / ccos / hash chunks parsed at runtime from the voice files.

## Re-dump tom_hpclass.bin if lost

```powershell
& "c:/program files/python312/python.exe" `
  "spfy/test/oracle/run_frida_capture.py" `
  --hook unit_hpclass_dump --filter text_002

python build_hpclass_table.py   # produces tom_hpclass_table.bin in c:/tmp
# Then move/rename to ../../data/tom_hpclass.bin
```

## Path to C port

Port `compute_4cell_ccos`, `histogram_prune`, and `compute_anchor_full_cost`
from v7 into `spfy/src/usel/anchor_score.c`. Embed `tom_hpclass.bin` as
static data or load via `spfy_voice_open`. Wire into a new
`spfy_synth_from_text` pipeline (FE → BuildGraph → PRSL → AnchorScore →
PostScoringAdj → Viterbi → WSOLA → WAV).
