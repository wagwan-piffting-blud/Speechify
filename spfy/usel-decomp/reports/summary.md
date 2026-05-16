# Triage Summary

**Program:** `SWIttsUSel.dll`
**Language:** `x86:LE:32:default`
**Compiler:** `windows`
**Image base:** `08e80000`
**Pointer size:** 4

**Functions:** 375
**Strings:** 611

## Tier distribution

| Tier | Count | Meaning |
|---|---|---|
| 0 | 111 | skip (junk) |
| 1 | 162 | auto-tag only |
| 2 | 85 | paragraph (LLM) |
| 3 | 17 | deep dive |

## Tag counts

| Tag | Count |
|---|---|
| `struct.small` | 184 |
| `struct.tiny` | 107 |
| `struct.no_decomp` | 105 |
| `math.heavy` | 91 |
| `struct.medium` | 78 |
| `struct.thunk` | 78 |
| `tts.phonetic_features` | 25 |
| `tts.engine` | 19 |
| `io.file` | 9 |
| `tts.concatenative` | 7 |
| `struct.large` | 6 |
| `tts.f0` | 6 |
| `name.runtime` | 5 |
| `tts.duration` | 4 |
| `tts.tone` | 4 |
| `io.string` | 3 |
| `tts.pos` | 3 |
| `tts.unit_db` | 3 |
| `tts.cost` | 2 |
| `name.dllmain` | 1 |

## Seed functions

| Address | Name | Size | Term | Match |
|---|---|---|---|---|
| `08e857a0` | `FUN_08e857a0` | 4838 | `f0` | `string:u"Loading F0 trees"` |
| `08e8de20` | `FUN_08e8de20` | 3810 | `f0` | `string:"TOTAL PATH %d units scores (S %f D %f DU %f SP %f J ` |
| `08e90dc0` | `FUN_08e90dc0` | 2481 | `halfphone` | `string:"tts.voiceCfg.HALFPHONE_CAND_MAX_UNITS"` |
| `08e84210` | `FUN_08e84210` | 2439 | `.pho` | `string:"tts.voiceCfg.proscost.phoneInSylCosts"` |
| `08e8d550` | `FUN_08e8d550` | 2241 | `f0` | `string:"%d F0Change %d %d : %d %d \n"` |
| `08e8adc0` | `FUN_08e8adc0` | 1973 | `candidate` | `string:u"WARNING MAX_CHUNK_CANDIDATES exceeded - pruning chu` |
| `08e84f00` | `FUN_08e84f00` | 1424 | `pitch` | `string:"pitch"` |
| `08e82c80` | `FUN_08e82c80` | 1228 | `.pho` | `string:"tts.voiceCfg.phones"` |
| `08e8edd0` | `FUN_08e8edd0` | 1077 | `diphone` | `string:u"Used bad score for diphone emulation: leftPhone %S ` |
| `08e8b620` | `FUN_08e8b620` | 1002 | `viterbi` | `string:u"UNIT SELECTION: USelGraph::ViterbiWithJoinCache"` |
| `08e8ce60` | `FUN_08e8ce60` | 921 | `candidate` | `string:u"%S %S found, %d candidates %d after context pruning` |
| `08e854a0` | `FUN_08e854a0` | 758 | `join_cost` | `string:u"load_join_cost_hash()"` |
| `08e83a40` | `FUN_08e83a40` | 436 | `f0` | `string:u"load_f0_prob_histos()"` |
| `08e8f9b0` | `FUN_08e8f9b0` | 357 | `duration` | `string:u"Warning: predicted closure offset %f on segment %S ` |
| `08e8fb20` | `FUN_08e8fb20` | 307 | `duration` | `string:u"Warning: predicted closure offset %f on segment %S ` |
| `08e917f0` | `FUN_08e917f0` | 155 | `triphone` | `string:u"Warning: Quinphone context not supported, backing o` |
| `08e817f0` | `Catch@08e817f0` | 13 | `f0` | `name:Catch@08e817f0` |
| `08e958f0` | `Unwind@08e958f0` | 9 | `f0` | `name:Unwind@08e958f0` |

## Tier 3 (deep dive)

_17 functions_

| Address | Name | Size | Reason |
|---|---|---|---|
| `08e857a0` | `FUN_08e857a0` | 4838 | seed |
| `08e8de20` | `FUN_08e8de20` | 3810 | seed |
| `08e90dc0` | `FUN_08e90dc0` | 2481 | seed |
| `08e84210` | `FUN_08e84210` | 2439 | seed |
| `08e8d550` | `FUN_08e8d550` | 2241 | seed |
| `08e8adc0` | `FUN_08e8adc0` | 1973 | seed |
| `08e84f00` | `FUN_08e84f00` | 1424 | seed |
| `08e83210` | `FUN_08e83210` | 1381 | 1-hop+tts |
| `08e82c80` | `FUN_08e82c80` | 1228 | seed |
| `08e8edd0` | `FUN_08e8edd0` | 1077 | seed |
| `08e8b620` | `FUN_08e8b620` | 1002 | seed |
| `08e8ce60` | `FUN_08e8ce60` | 921 | seed |
| `08e854a0` | `FUN_08e854a0` | 758 | seed |
| `08e83a40` | `FUN_08e83a40` | 436 | seed |
| `08e8f9b0` | `FUN_08e8f9b0` | 357 | seed |
| `08e8fb20` | `FUN_08e8fb20` | 307 | seed |
| `08e917f0` | `FUN_08e917f0` | 155 | seed |
