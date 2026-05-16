# Stage trace JSONL schema

The native build (compiled with `-DSPFY_TRACE=1`) and the Frida hooks both
emit JSONL traces in a shared schema. Each line is one record. `type`
identifies the stage; the rest of the fields are stage-specific.

Files live under `spfy/test/oracle/traces/<hook_name>/<entry_id>.jsonl`
on the oracle side; the native side writes to whatever path is passed to
`spfy_synth_set_trace`.

## Common envelope

Every record has at least:

```json
{ "type": "<stage>", ... }
```

Batched send() messages from Frida (`hash_probe_batch` etc.) are
unfurled by `run_frida_capture.py` into per-sample `<stage>` records
when the JSONL is written. Hosts reading these files see only the
unfurled form.

## hash_probe (closes gap #1)

One record per join-cost hash table probe.

```json
{
  "type": "hash_probe",
  "n":         12345,           // running probe count this utterance
  "uid_left":  139245,          // candidate predecessor (eax)
  "uid_right": 128126,          // candidate successor   (ebx, verification key)
  "esi":       "0x4c44308",     // hex; (esi - cellsBase) / 8 = rows[uid_right]
  "stored":    128126,          // cells_A[rows[uid_right] + uid_left]
  "hit":       true,            // stored === uid_right
  "cost":      0.5812           // f32 join cost on hit; null on miss
}
```

Ground truth for verifying our C `usel/hash.c` matches the engine.

## prsl_lookup (closes gaps #5, #6)

One record per PRSL triphone-context query.

```json
{
  "type": "prsl_lookup",
  "n":      1,
  "args": {
    "ecx":     266533104,   // PRSL object (this)
    "edx":     4001,        // possibly the triphone key
    "esp+4":   58257328,    // ... raw stack args ...
    "esp+8":   46886936,
    "esp+12":  149429405,
    "esp+16":  58257328,
    "esp+20":  58257328,
    "esp+24":  46484984
  },
  "retval": 44356353        // probably output buffer ptr; TBD
}
```

## wsola_in / wsola_out (gap #3)

Per WSOLA call:

```json
{
  "type": "wsola_in",
  "utt":  1,
  "n_units": 12,
  "units": [
    {"uid": 0,      "lp": 1, "dl":   0},   // leading silence
    {"uid": 87072,  "lp": 1, "dl":   0},
    {"uid": 158634, "lp": 2, "dl":   0},
    {"uid": 158635, "lp": 0, "dl":  31},
    ...
    {"uid": 169578, "lp": 1, "dl":   0}    // terminal silence
  ]
}
{
  "type": "wsola_out",
  "utt":  1,
  "retval": 58412136,       // output buffer ptr (suspected)
  "obj_probe": { "+0x0": ..., "+0x4": ... }  // candidate field offsets
}
```

## fe_tokens (FE track gap #10)

Per USel orchestrator entry; raw arg dump for offline decode.

```json
{
  "type": "fe_tokens",
  "probe": {
    "call":  1,
    "ecx":   46656512,
    "stack": { "esp+4": ..., "esp+8": ..., ... },
    "blocks": {
      "@0x2c7ec00": [200,164,161,2, ...],   // 0x40 byte hex dump
      ...
    }
  }
}
```

## ulaw_pair (gap #7) -- KNOWN ZERO ON CURRENT ENGINE

When generated:

```json
{
  "type": "ulaw_pair",
  "sym":  "SWIttsAudioCvtUlawToL16",
  "pairs": [
    { "in_byte": 0,   "out_s16": -32124 },
    { "in_byte": 255, "out_s16":      0 },
    ...
  ]
}
```

See [ulaw_lut_dump.js](../../../viz/frida_hooks/ulaw_lut_dump.js) for the
known-issue note: the export is exported but not called during typical
synthesis paths. Likely needs an alternate hook inside SWIttsWsola.dll.

## prsl_slot [ACTIVE — covers all 200 phrases]

One record per PRSL preselection (`FUN_08e91dc0` = `USelNetwork::AddUnit` entry).

```json
{
  "type":      "prsl_slot",
  "utt":       1,
  "slot":      0,
  "ctx":       [123, 45, 67],
  "n_pool":    47,
  "pool":      [169578, 158634, ...],
  "sp_target": [0.123, 0.456, ...]
}
```

Required fields (typed): `utt: int`, `slot: int`, `ctx: list[int] (len 3)`,
`n_pool: int`, `pool: list[int]`, `sp_target: list[float] | null`.

Producer hook: `viz/frida_hooks/prsl_slot_hook.js`.

## cart_walks [RETIRED — seed-32 only]

Historical per-question CART walk dispatch records, retained for the
seed-32 traces only.

```json
{
  "type":      "cart_walks",
  "utt":       1,
  "slot":      0,
  "tree":      "durt"|"f0tr",
  "leaf_id":   123,
  "questions": [{"qid": 7, "ans": 1}, ...]
}
```

Required fields (typed): `utt: int`, `slot: int`, `tree: str`,
`leaf_id: int`, `questions: list[dict]`.

Hook RETIRED 2026-05-05 (mid-instruction hot-path; 4 historical
Speechify.exe kills). Schema retained for the seed-32 traces; new corpus
phrases are NOT captured with this hook. New phrases use
`cart_walker_args` instead — see next section. Per CONTEXT.md D-15.

Producer hook: `viz/frida_hooks/cart_walks_hook.js` (retired; seed-32
captures only).

## cart_walker_args [ACTIVE — covers all 200 phrases; substitute for cart_walks per D-15]

Per-slot CART feature inputs. Active substitute for the retired
`cart_walks` hook on the 168 new corpus phrases.

```json
{
  "type":  "cart_walker_args",
  "utt":   1,
  "slot":  0,
  "tree":  "durt"|"f0tr",
  "args":  {"phone_id": 23, "stress": 1, "is_voiced": 1, ...}
}
```

Required fields (typed): `utt: int`, `slot: int`, `tree: str`,
`args: dict`.

Producer hook: `viz/frida_hooks/cart_walker_args_hook.js`.

## inner_scorer [ACTIVE — covers all 200 phrases]

Per-slot SP target hook from M3.4e. One record per
`USelNetworkSlice::all_half_phone_costs` entry.

```json
{
  "type":      "inner_scorer",
  "utt":       1,
  "slot":      0,
  "uid":       169578,
  "tc":        0.5812,
  "components":{"f0": 0.12, "dur": 0.34, "sp": 0.41, "anchor": 0.0}
}
```

Required fields (typed): `utt: int`, `slot: int`, `uid: int`,
`tc: float`, `components: dict | null`.

Producer hook: `viz/frida_hooks/inner_scorer_hook.js`.

## inner_scorer_durt [ACTIVE — added by plan 02-05 D-17 Path R]

One record per call to `FUN_08e87d90` (the binary CART walker the engine
uses INSIDE the inner-scorer for duration target prediction). Captures
the leaf-node `(mean, inv_std)` floats — the engine's preselect-time
durt CART output, which is conceptually the same prediction
`cart_walks_hook.js` instruments for synth-time, but evaluated at the
preselect call site (`FUN_08e88de0`).

```json
{
  "type":      "inner_scorer_durt",
  "n":         132,
  "utt":       0,
  "slot":      65,
  "node_addr": "0x4c44308",
  "mean":      136.918,
  "inv_std":   0.0693761
}
```

Required fields (typed): `utt: int`, `slot: int`, `mean: float`,
`inv_std: float`.

`node_addr` is the leaf-node address (for cross-reference with voice
memory). `n` is the running call count.

Slot tracking is via a parallel `Interceptor.attach` on `FUN_08e88de0`
(the InnerScorer caller) inside the same hook script — its `esp+8` arg
is the slot, set on entry and read by the immediately-following
durt-CART call.

Producer hook: `viz/frida_hooks/inner_scorer_durt_hook.js`.

## fe_tree [ACTIVE — covers all 200 phrases]

Utterance tree dump (Word/Syl/Phone IRs + POS) from M3.4r Phase B1.
One record per utterance.

```json
{
  "type": "fe_tree",
  "utt":  1,
  "words": [
    {"text": "hello", "pos": "interj", "syllables": [
       {"phones": [{"name": "hh", "stress": 0, "accent": 0}, ...]}
    ]},
    ...
  ]
}
```

Required fields (typed): `utt: int`, `words: list[dict]`.

Producer hook: `viz/frida_hooks/fe_tree_hook.js`.

## viterbi_dp [ACTIVE — covers all 200 phrases]

Per-utterance DP entry/leave + per-slot best_idx records from M3.4r.
Two record subtypes are interleaved in the same file, distinguished by
`phase`:

```json
{"type": "viterbi_dp", "phase": "enter", "utt": 1, "n_slots": 47}
{"type": "viterbi_dp", "phase": "leave", "utt": 1, "best_score": -123.4}
{"type": "viterbi_dp", "phase": "slot",  "utt": 1, "slot": 0, "best_uid": 169578, "best_idx": 3, "best_cost": 1.23}
```

Required fields (typed): `utt: int`, `phase: str (enter|leave|slot)`.
Per-phase additional required fields:
- `phase == "enter"`: `n_slots: int`.
- `phase == "leave"`: `best_score: float`.
- `phase == "slot"`:  `slot: int`, `best_uid: int`, `best_idx: int`,
  `best_cost: float`.

Producer hook: `viz/frida_hooks/viterbi_dp_hook.js`.

## ccos_apply [ACTIVE — added by plan 02-02 D-05]

One record per call to `FUN_08e8adc0` (USelNetwork CCOS context-cost
reduction). Function-entry hook captures inputs; function-leave hook
walks the populated `cost_array_out` / `idx_array_out` arrays for the
n_kept survivors after the engine's histogram-based dynamic prune.

```json
{
  "type":           "ccos_apply",
  "n_call":         42,
  "type_arg":       4,                 // 2=Syl, 4=Word
  "first_hp":       2,                 // anchor first HP idx (param_9)
  "last_hp":        5,                 // anchor last HP idx (ECX)
  "group_idx":      0,                 // per-anchor group iter (param_8)
  "cklx_match_idx": 1,                 // lex-match iter (EDX)
  "n_input":        87,                // candidates before histogram prune
  "n_kept":         23,                // EAX = local_114 = survivors
  "cost_w_44":      0.5,               // weight+0x44 (CCOS_WEIGHT)
  "anchor_max_syl":    20.0,           // weight+0x54 = local_10c when type_arg=2
  "anchor_slope_syl":  0.5,            // weight+0x58 = local_108 when type_arg=2
  "anchor_max_word":   30.0,           // weight+0x5c = local_10c when type_arg=4
  "anchor_slope_word": 0.5,            // weight+0x60 = local_108 when type_arg=4
  "cands": [
    {"idx": 0, "cost": 0.012345, "s_first_uid": 26214, "s_last_uid": 26217},
    {"idx": 3, "cost": 10000.0,  "s_first_uid": 46287, "s_last_uid": 46290},
    ...
  ]
}
```

Per-cand `s_first_uid` / `s_last_uid` are the engine cand-pair endpoints
(`-1` if any pointer in the resolution chain is unreadable). Resolved via:
```
pair_idx   = match_list[cand.idx]                   // match_list = *(int*)(voice+0x9c[group_idx*0x1c+0x18] + cklx_match_idx*0xc + 8)
pair_entry = pair_table + pair_idx * 0xc            // pair_table = *(int*)(voice+0x98 + group_idx*4)
s_first    = *(int*)(pair_entry + 4)
s_last     = *(int*)(pair_entry + 8)
```

Per-cand `cost` is the engine's CCOS reduction value applied to that
candidate's TC at offset +0x2c by the caller (`FUN_08e8ce60`):
- HP-class match: `cost = (cell_a + cell_b + cell_c + cell_d) * cost_w_44`
  where the 4 cells are looked up from per-(hp_class, slot) tables addressed
  via `voice+0x610` (ccos_meta) and ctx-byte indices from `voice+0xc0` /
  `voice+0xc4` (one of the two paths is taken based on `voice+0xc0` null
  gating).
- HP-class mismatch: `cost = 10000.0f` (sentinel; the histogram prune
  typically drops these entries).

Required fields (typed): `n_call: int`, `type_arg: int`, `first_hp: int`,
`last_hp: int`, `group_idx: int`, `cklx_match_idx: int`, `n_input: int`,
`n_kept: int`, `cost_w_44: float`, `cands: list[dict]`.

Producer hook: `viz/frida_hooks/ccos_apply_hook.js`.

*Last updated: 2026-05-09 — added ccos_apply for plan 02-02 D-05/D-07.*

## How the native side emits this

Native build with `-DSPFY_TRACE=1` calls `spfy_trace_event(stage, json)`
from `src/common/log.c`. Each call produces one JSONL line via the FILE*
set with `spfy_trace_set_sink(fp)`. As stages get implemented, they
will emit these record types so `test/diff/stage_compare.py` can
field-by-field compare native vs oracle.
