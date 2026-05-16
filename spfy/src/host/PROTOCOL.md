# SWIttsFe-en-US.dll — empirical synthesis protocol

Captured 2026-05-10 via `viz/frida_hooks/fe_vtable_trace.js` against a
running `Speechify.exe`. Raw traces:
`spfy/test/oracle/traces/fe_vtable_trace/vt_001.jsonl` ("Hello, world.")
and `vt_long.jsonl` ("The quick brown fox..."). The vtable inventory
(`host/vtable_inventory.md`) named the slots; this doc pins down how
Speechify.exe actually *drives* them per utterance.

## Per-utterance call sequence (Speechify.exe → FE)

```
1. getObject(kind=2, &fe)            -- kind=2, NOT kind=1 (engine uses
                                        the secondary interface)
2. fe->installHookA(p1, p2, p3)      -- slot 47: install callback A
3. fe->installHookB(p1, p2, p3)      -- slot 48: install callback B
4. fe->setPair_E(p, p)               -- slot 41
5. fe->setPair_F(p, p)               -- slot 43
6. fe->setPair_G(p, p)               -- slot 45
7. fe->setPair_H(p, p)               -- slot 46
8. fe->initStage1()                  -- slot 3: NOW state[+0x2d4] and
                                        state[+0x2dc] populate (delegates A/B)
9. fe->getKind(&out_kind_tuple)      -- slot 33  (called twice)
10. fe->getKind(&out_kind_tuple)
11. fe->feedConfigA("\\\\!SWIcv3.0.0.")  -- slot 5: control header
12. fe->feedConfigB(<config_ptr>)         -- slot 6: small UTF-16-flavored
                                              binary blob, identical across
                                              all calls (likely an options struct)
13. fe->delegateB_call(buf=&buf, max=100,
                       &out_len)     -- slot 42: pulls "%% \0" (length 3)
                                        first call is just the stream
                                        prologue marker
14. fe->feedConfigA("Hello, world.") -- slot 5: ACTUAL text input
15. fe->feedConfigB(<same_config>)   -- slot 6: trigger
16-21. fe->delegateB_call(...) x N   -- slot 42: pulls TAGGED OUTPUT in
                                        100-byte chunks until exhausted
22. fe->runOrAbort(0)                -- slot 11(0) = synth driver
                                        (delegates to slot-8 inner = full FE pass)
23. fe->initStage2()                 -- slot 4: TEARS DOWN delegates
24. fe->reset()                      -- slot 26
25. fe->Release()                    -- slot 2
```

## The tagged output format (slot 42 stream contents)

For input `"The quick brown fox jumps over the lazy dog."` the FE emits
(concatenated across `delegateB_call` chunks):

```
%% #{. pau(p25)
  <the   (0,3)  det,0          [.0    dh(p100) ix(p100)              ] >
  <quick (4,5)  adj,1          [.1,H* k(p100)  w(p100) ih(p100) k(p100) ] >
  <brown (10,5) adj,1          [.1,H* b(p100)  r(p100) aw(p100) n(p100) ] >
  <fox   (16,3) noun,1         [.1,H* f(p100)  aa(p100) k(p100) s(p100) ] >
  <jumps (20,5) noun_verb,1    [.1,H* jh(p100) ah(p100) m(p100) p(p100) s(p100) ] >
  <over  (26,4) prep,1         [.1,H* ow(p100) .0 v(p100) er(p100)   ] >
  <the   (31,3) det,0          [.0    dh(p100) ix(p100)              ] >
  <lazy  (35,4) adj,1          [.1,H* l(p100)  ey(p100) .0 z(p100) iy(p100) ] >
  <dog   (40,4) noun,2         [.1,H*;L-L% d(p100) ao(p100) g(p100)  ] >
pau(p50) } %%
```

Grammar (recoverable by recursive descent):

| token / pattern              | meaning                                                                          |
|------------------------------|----------------------------------------------------------------------------------|
| `%%`                         | stream begin / end                                                               |
| `#{` … `}`                   | utterance boundary (per sentence)                                                |
| `pau(pNNN)`                  | inter-word pause, NNN = duration scale (100 = default; 25 / 50 / 100 observed) |
| `<word (start, len) POS,sl`  | word span begin: `start`+`len` are char offsets into input; `sl` = stress level (0/1/2) |
| `[.X,ACCENT phn(pNNN) … ]`   | syllable: `.0` = unstressed / `.1` = stressed; `ACCENT` = ToBI-style pitch accent (`H*`, `L*`, `L-L%`, …); contents = ARPAbet phoneme(duration) tokens |
| `.0` mid-syllable            | unstressed-syllable boundary inside the `[ ]` (multi-syllable words)             |
| `>`                          | close word                                                                       |
| `;`                          | separator inside accent string (`H*;L-L%` = phrase accent + boundary tone)        |
| POS tags observed             | `det`, `adj`, `noun`, `noun_verb`, `prep`, `verb`, `adv`, `conj`, … (matches the 25-tag set in `spfy/data/baked_pos.csv`) |

## Buffer-fill semantics (slot 42 / FUN_0836c420)

`FUN_0836c420(delegate, buf, max, &out_len)` reads from the delegate's
internal stream:

```c
int n = stream_length(delegate->stream);  // current bytes available
*out_len = min(n, max - 1);                // cap at max-1
memcpy(buf, stream + 0, *out_len);         // copy from stream head
stream_seek(delegate->stream, 0);          // reset
stream_consume(delegate->stream, *out_len);// drop those bytes from stream
*out_len += 1;                             // returned length is bytes+1
return 1;
```

So the engine pulls chunks of at most `max-1` bytes; the loop continues
until `out_len < max` (i.e., the stream had fewer bytes than the
buffer could hold).

## Implementation guidance for `fe_host.c`

1. **Open**: `host_dll_load` → `getObject(2, &iobj)`. The engine uses
   `kind=2`; our open path should match (the inventory says either 1 or
   2 works, but staying on 2 maximizes compatibility).
2. **Init**: skip slots 47/48/41/43/45/46 — they pass engine-internal
   pointers (callback addresses in Speechify.exe's address space) that
   wouldn't make sense from our process. Verify empirically that
   `initStage1` alone is enough; the FE's default delegates should
   accept missing callbacks gracefully (the err_flag stays 0 in the
   capture even before installHook are called).
3. **Per-utterance synth**:
   ```c
   slot5(this, "\\\\!SWIcv3.0.0.");   // header
   /* slot6 is a no-op for us — its arg is an engine-internal struct;
    * we'll attempt without and observe. */
   slot5(this, text);                  // the input
   /* Drain output. */
   char buf[256];
   int out_len;
   for (;;) {
       slot42(this, buf, sizeof(buf), &out_len);
       if (out_len <= 1) break;        // out_len includes the +1
       append_to_output_stream(buf, out_len - 1);
   }
   /* runOrAbort triggers the synth pass — but since we're consuming
    * the tagged FE output ourselves, this may or may not be needed
    * depending on whether the deep FE has any work that happens AFTER
    * the tagged text is emitted. The capture shows runOrAbort is
    * called AFTER all drains. */
   slot11(this, 0);                    // synth / commit
   ```
4. **Parse** the tagged output stream into `spfy_fe_utterance_t.slots`.
   The parser is straightforward recursive-descent — one slot per
   phoneme `phn(pNNN)`. POS / stress / accent attach to enclosing word
   / syllable. Char-span tuples `(start, len)` map slots back to input
   text positions for prosody hint application.
5. **Close**: `slot4 (initStage2)` → `slot26 (reset)` → `slot2 (Release)`
   → `host_dll_free`.

## Still unknown (low-priority — not blocking task 4)

- **What does `feedConfigB`'s argument actually point to?** The blob
  reads as something like `\0\0Exiting\0\0\0E\0x\0i\0t\0i\0n\0g\0` — looks
  like a Microsoft BSTR (4-byte length prefix immediately before the
  UTF-16LE string at offset -4). The value is the SAME pointer across
  all calls in a session — likely a static struct in Speechify.exe's
  `.data`. Skipping `feedConfigB` from our hosted side may simply
  bypass an optional config path; needs runtime test to confirm.
- **What do `installHookA` / `installHookB` install?** First arg points
  to executable bytes (`8b 44 24 04 85 c0 74 1b …` = function prologue),
  so they install **callback function pointers**. These are probably
  status-update callbacks (progress, log, error) that we can safely
  skip if our usage doesn't need them.
- **What do `setPair_E..H` actually configure?** They write to
  `ctrl[+0x25..+0x41]` in the state. The blobs look like more
  code-pointer bytes — probably more callbacks parked in the FE state
  for the FE to call when generating output. The output stream we
  captured works without us replicating these.

The minimum viable hosted-FE drive sequence is therefore:
`getObject(2) → initStage1 → feedConfigA(header) → feedConfigA(text) →
loop slot42 → slot11(0) → initStage2 → reset → Release`.
