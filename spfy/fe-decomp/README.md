# New static-analysis dataset for SWIttsFe-en-US.dll

A new pipeline (Ghidra parallel-decompile → JSONL → SQLite + triage) produced
a queryable dataset of the FE DLL. Use this in place of `reveng/ddl/*` for
static exploration; treat the DDL clusters / handlers.csv as stale per user
instruction.

## Artifacts (CWD)

- `analysis.db` — SQLite (~18 MB). 2,176 functions with decompilation, 438
  strings, 110k caller edges, 22k callee edges, 3.6k string refs.
- `reports/summary.md` — triage summary.
- `reports/tier3/*.md` — 38 per-function briefs (only seed-term matches; see
  caveat below).
- `tts_triage.py` — query CLI. Useful: `show <db> <addr>`, `grep <db> <term>`,
  `stats <db>`.

## Schema

```sql
functions(address PK, name, size, instructions, basic_blocks, params,
          calling_convention, signature, frame_size, decompile_ok, decompiled)
callers(callee_addr, caller_addr)
callees(caller_addr, callee_name, callee_addr)
string_refs(func_addr, instr_addr, target_addr, encoding, value)
strings(address PK, length, type, xrefs, value)
tags(func_addr, tag);  tiers(func_addr, tier, reason);  seeds(...)
```

Addresses: lowercase hex, no `0x` prefix.

## Triage findings worth knowing (NEW info from this pipeline)

- Embedded **IBM KlattID v4.0 (1996/97)** formant synth at 0838xxxx–0838axxx.
  Main loop FUN_0838a410 (7291 bytes). 317-byte default param struct at
  `&DAT_084ad798` — likely the eciTSParam-style table for Klatt.
- **DictionarySet / UserDict C++ class hierarchy.** All 19 method names leak
  via `"Entering ClassName::method\n"` debug strings — see tier3 briefs.
- **Voice constructor at FUN_0836dba0** builds an `audio_state` at
  `voice+0x64` containing the literal `"audio.cdv"` filename and the 317-byte
  Klatt param struct copied in. Single caller, near 08373378.
- **Two-tier A/B declination prosody** (ADeclnScale/Level + BDeclnScale/Level)
  + ToBI tones (bound_tone/phr_tone/nuc_tone) — strings present, but the
  **consumers are NOT in tier3** (triage seed terms didn't include prosody
  vocab). See unblock path below.
- **Internal "Delta" subsystem** at FUN_0837f380 / 0837f580 / 0837fcc0 /
  08380160 — strings include `DELTIO`, `"\ndelta insert [..."`, `delta project`.
  Whether this interprets `enu.ddl` is unverified.

## Unblocking 03-05 (intonation analyzer)

The triage missed it — seed list didn't include intonation/declination
vocabulary. Direct query:

```sql
SELECT f.address, f.size, COUNT(*) AS hits
FROM functions f JOIN string_refs s ON s.func_addr = f.address
WHERE LOWER(s.value) LIKE '%inton%'
   OR LOWER(s.value) LIKE '%bound_tone%'
   OR LOWER(s.value) LIKE '%phr_tone%'
   OR LOWER(s.value) LIKE '%nuc_tone%'
   OR LOWER(s.value) LIKE '%adecln%'
   OR LOWER(s.value) LIKE '%bdecln%'
   OR LOWER(s.value) LIKE '%midline%'
   OR LOWER(s.value) LIKE '%rangeval%'
GROUP BY f.address ORDER BY hits DESC, f.size DESC;
```

Largest functions in the binary, many string refs, all currently tier-1
because of the seed-term gap (overlaps your previous cluster 38–41
candidates):

| Address | Size | String refs |
|---|---:|---:|
| 07e11857 | 23861 | many |
| 07e2f95e | 23415 | 55 |
| 07ed1f64 | 14170 | 30 |
| 07ecb79a | 12525 | 28 |
| 07ececd3 | 11757 | 24 |
| 07ec8b4f | 10431 | 26 |
| 07ed5bb2 |  8742 | 22 |
| 07ec6f07 |  6680 | 19 |

Cross-check candidates against `state_writes_full.csv` for writes to the
prosody offsets (`0x86b`/`0x86f`/`0x873`/`0x883`/`0x887`) to filter.

If no single function emerges, expect the analyzer to be split — common in
this engine (the dictionary subsystem is fragmented across 19 functions).
Build the local call-graph cluster around the strongest string-ref hit and
look for a coherent set of co-callers.
