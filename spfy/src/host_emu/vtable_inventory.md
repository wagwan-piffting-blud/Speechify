# Vtable slot inventory — `SWIttsFe-en-US.dll`

**Vtable VA:** `0x08395980` (`.rdata`, file offset `0x005c5980`). **49 × 4-byte slots.**

**Image base:** `0x07dd0000`. **Sections:** `.text` `0x07dd1000..0x08392402` · `.rdata` `0x08393000..0x08398334` · `.data` `0x08399000..0x084aE724`.

**Object layout (verified from slot 1 / slot 2 / slot 3 wrappers):**

| offset | width | meaning                                                                 |
|-------:|------:|-------------------------------------------------------------------------|
| `+0x0` |     4 | vtable pointer (= `&PTR_FUN_08395980` after Release zeros it)           |
| `+0x4` |     4 | refcount                                                                |
| `+0x8` |     4 | FE state pointer (returned by `FUN_0835dfdf` constructor, ~0x1285 B)    |
| `+0xc` |     1 | `init_flag` — set true after a one-shot method succeeded                 |
| `+0xd` |     1 | `err_flag` — sticky error                                                |

Total 14 B (`getObject` allocates 14; `operator_delete` frees 14). FE_CLAUDE.md's "padding before err_flag" is unnecessary — `init_flag` and `err_flag` are adjacent at +0xc / +0xd.

**Wrapper pattern (slots 3..48, except DictionarySet 27–38 which skip the gate):**

```c
uint wrap(this, arg2, arg3, ...) {
    uint ret = 1;
    if (this->init_flag == 0) {
        ret = inner(this->state, arg2, arg3, ...);
        if (ret == 1) this->init_flag = 1;
    }
    return ret & ((this->err_flag != 0) - 1);   // masked to 0 on err
}
```

**State sub-struct (the "ctrl block" at `state[+0x6c]`):**

| ctrl offset | meaning                                                                                  |
|------------:|------------------------------------------------------------------------------------------|
|         `+0` | mode byte (slot 25 setter writes here)                                                  |
|         `+1` | stage-flag 1 (slot 3 inner gates on this)                                               |
|         `+2` | stage-flag 2 (slot 4 inner gates on this)                                               |
|         `+3` | "blocking" byte (slot 11 writes; slot 8 inner clears)                                   |
|         `+4` | busy-flag (slots 5, 6, 8 gate on this; cleared at end)                                  |
|     `+0x05` | float-pair A (slot 19 writes 2 × 4 B)                                                   |
|     `+0x0d` | float-pair B (slot 39 setter)                                                           |
|     `+0x15` | float-pair C (slot 40 setter)                                                           |
|     `+0x1d` | float-pair D (slot 22 setter)                                                           |
|     `+0x25` | float-pair E (slot 41 setter)                                                           |
|     `+0x2d` | float-pair F (slot 43 setter)                                                           |
|     `+0x35` | float-pair G (slot 45 setter)                                                           |
|     `+0x3d` | float-pair H (slot 46 setter)                                                           |
|    `+0x2d4` | delegate-A pointer (slot 9 / slot 10 read via `FUN_0836c420`)                           |
|    `+0x2d8` | delegate-A state                                                                         |
|    `+0x2dc` | delegate-B pointer (slot 42 / slot 44 read)                                              |
|    `+0x2e0` | delegate-B state                                                                         |

## The table

`inner` = the immediate `FUN_0836bXXX` the wrapper dispatches to (after init/err gate). `deep` = the wrapper-stripped FE function it ultimately runs (where present). `conf` = confidence per method rules: **A** walked decomp end-to-end, **B** inferred from decomp + helper-name + signature, **C** guess (needs runtime probe).

| slot | wrap_va    | inner       | params (incl `this`) | name                       | semantics                                                                                                              | conf |
|-----:|-----------:|------------:|---------------------:|----------------------------|------------------------------------------------------------------------------------------------------------------------|------|
|    0 | 0x0836ca90 | (inline)    |                    3 | `QueryInterface`           | `if (kind ∈ {1,2}) { *out = this; AddRef(this); } else *out = NULL;` returns bool.                                       | A    |
|    1 | 0x0836cac0 | (inline)    |                    1 | `AddRef`                   | `return ++refcount;`                                                                                                   | A    |
|    2 | 0x0836d2e0 | (inline)    |                    1 | `Release`                  | `if (--refcount == 0) { vtable = PTR_FUN_08395980; FUN_0835df63(state); state = NULL; operator_delete(this); }`         | A    |
|    3 | 0x0836cb10 | FUN_0836b270|                    1 | `initStage1`               | `mutex`; if `ctrl[+1]==0`: set `ctrl[+1]=1`, run `FUN_083798d0` → `FUN_0836abd0` → `FUN_07df2926` (deep FE init).        | B    |
|    4 | 0x0836cb50 | FUN_0836b2e0|                    1 | `initStage2`               | `mutex`; if `ctrl[+2]==0`: run `FUN_07df2e06` + `FUN_0838e7a0(state,0)`. Companion to slot 3.                            | B    |
|    5 | 0x0836cb90 | FUN_0836b340|                    2 | `feedConfigA(s)`           | gates on `ctrl[+4]` (busy); calls `FUN_0836c3d0(delegate_A, s)` then `FUN_07df4173` then `FUN_08372760`. 1 user arg.     | B    |
|    6 | 0x0836cbd0 | FUN_0836b3e0|                    2 | `feedConfigB(s)`           | identical structure to slot 5 but calls `FUN_07df40b6` and includes `FUN_0836d8f0` cleanup. 1 user arg.                  | B    |
|    7 | 0x0836cc10 | FUN_0836b490|                    3 | `logEvent(a,b)`            | `FUN_0836c9d0(a,b,c)` + `FUN_0836ca20(a)`. No state touch — looks like log/notification with 2 user args.                | B    |
|    8 | 0x0836cc50 | FUN_0836b4c0|                    1 | **`synth`**                | The synthesise entry. `mutex` → `FUN_08373260` → `FUN_0836d960` → `FUN_0836d8f0` → `FUN_08382010` → `FUN_08372af0` → handler lookup `"wordsin"` (`FUN_0838ebc0`+`FUN_0838ee90`) → `FUN_0836e8b0` → `FUN_07df2926`; clears `ctrl[+3]`/`+4`. **This is what to call after slot-3/4 init.** | B    |
|    9 | 0x0836cc90 | FUN_0836b560|                    4 | `delegateA_call(a,b,c)`    | If `state[+0x2d4]` != 0: `FUN_0836c420(state[+0x2d4], a, b, c)`. Generic 3-arg delegate dispatch.                        | B    |
|   10 | 0x0836cce0 | FUN_0836b650|                    4 | `getErrorMessage(buf,n,_)` | Calls delegate-A's `FUN_0836c420(state[+0x2d8], buf, n, _)`; on failure copies `"Unable to get error message from Eloquence."` into `buf`. | A    |
|   11 | 0x0836cd30 | FUN_0836b6b0|                    2 | `runOrAbort(b)`            | `ctrl[+3] = (char)b`; if `b != 0`: cleanup (cancel); else call slot-8 inner (= synth).                                  | B    |
|   12 | 0x0836cd60 | FUN_0836b700|                    1 | `notifyEvent`              | `FUN_0836c3d0(delegate_A, &DAT_0839334a)` then `FUN_07dd526d`. Posts an event name to the delegate.                      | B    |
|   13 | 0x0836cda0 | FUN_0836b760|                    1 | `cancel`                   | `FUN_0836c970(this, -7)` (set err) + `FUN_08372740`. Forces an abort.                                                   | B    |
|   14 | 0x0836cde0 | FUN_0836b780|                    1 | `isReady`                  | If `ctrl[+4]==0` (not busy): return `FUN_0836d640(state) != 0`; else return 1.                                          | B    |
|   15 | 0x0836ce20 | FUN_0836b7b0|                    2 | `predicateA(x)`            | `return FUN_0836d650() == 0` (Ghidra mis-typed — signature suggests 1 arg passed but callee `FUN_0836d650` shows void). | C    |
|   16 | 0x0836ce60 | FUN_0836b7d0|                    2 | `predicateB(x)`            | `return FUN_0836d6a0(state, x) == 0`. 1 user arg, bool.                                                                 | B    |
|   17 | 0x0836cea0 | FUN_0836b7f0|                    3 | `predicateC(x,y)`          | `return FUN_0836d720(state, x, y) == 0`. 2 user args, bool.                                                             | B    |
|   18 | 0x0836cee0 | FUN_0836b810|                    1 | `setterA(x,y)`             | `FUN_0836d790(state, x, y)` — 2 user args, void.                                                                        | B    |
|   19 | 0x0836cf00 | FUN_0836b8b0|                    1 | `setPair_A(a,b)`           | `ctrl[+5] = a; ctrl[+9] = b;` — typed float-pair setter A. 2 user args.                                                 | A    |
|   20 | 0x0836cf60 | FUN_0836b930|                    1 | `setterB(x,y)`             | `FUN_0836d7b0(state, x, y)` — 2 args, void.                                                                             | B    |
|   21 | 0x0836cf80 | FUN_0836b950|                    1 | `setterC(x,y)`             | `FUN_0836d7d0(state, x, y)` — 2 args, void.                                                                             | B    |
|   22 | 0x0836cfa0 | FUN_0836b910|                    1 | `setPair_D(a,b)`           | `ctrl[+0x1d] = a; ctrl[+0x21] = b;` — float-pair D.                                                                     | A    |
|   23 | 0x0836cfc0 | FUN_0836b970|                    2 | `predicateD(x)`            | `return FUN_0836d830(state, x, 0) == 0`. 1 user arg.                                                                    | B    |
|   24 | 0x0836d000 | FUN_0836b990|                    3 | `predicateD_full(x,y)`     | `return FUN_0836d830(state, x, y) == 0`. Same callee as slot 23 with 2 args.                                            | B    |
|   25 | 0x0836d040 | FUN_0836b9b0|                    1 | `setMode(b)`               | `ctrl[+0] = (uint8_t)b` — single-byte mode/language setter.                                                              | A    |
|   26 | 0x0836d060 | FUN_0836b9c0|                    1 | `reset`                    | `FUN_0836e8b0(state); FUN_0836ab10(state); FUN_0836db70(state);` — clear pipeline buffers.                              | B    |
|   27 | 0x0836d090 | FUN_0836b9f0|                    1 | `DictionarySet_load`       | Slot 27 inner wraps `DictionarySet::load` (labeled in db).                                                              | A    |
|   28 | 0x0836d0a0 | FUN_0836ba20|                    1 | `DictionarySet_fileLoad`   | Wraps `DictionarySet::fileLoad`.                                                                                        | A    |
|   29 | 0x0836d0b0 | FUN_0836ba40|                    1 | `DictionarySet_free`       | Wraps `DictionarySet::free`.                                                                                            | A    |
|   30 | 0x0836d0c0 | FUN_0836ba60|                    1 | `DictionarySet_activate`   | Wraps `DictionarySet::activate`.                                                                                        | A    |
|   31 | 0x0836d0d0 | FUN_0836ba80|                    1 | `DictionarySet_deactByName`| Wraps `DictionarySet::deactivate_byName`.                                                                               | A    |
|   32 | 0x0836d0e0 | FUN_0836baa0|                    1 | `DictionarySet_deactivate` | Wraps `DictionarySet::deactivate`.                                                                                      | A    |
|   33 | 0x0836d0f0 | FUN_0836bac0|                    1 | `getKind`                  | `inner` → `FUN_0836f5f0` writes `out1 = &PTR_DAT_084ad8e4` and `out2 = 3`. Returns vtable + kind to caller-supplied pointers. | B    |
|   34 | 0x0836d100 | FUN_0836bae0|                    1 | `DictionarySet_update`     | Wraps `DictionarySet::update`.                                                                                          | A    |
|   35 | 0x0836d110 | FUN_0836bb10|                    1 | `DictionarySet_findFirst`  | Wraps `DictionarySet::findFirst`.                                                                                       | A    |
|   36 | 0x0836d120 | FUN_0836bb40|                    1 | `DictionarySet_findNext`   | Wraps `DictionarySet::findNext`.                                                                                        | A    |
|   37 | 0x0836d130 | FUN_0836bb70|                    1 | `DictionarySet_lookup`     | Wraps `DictionarySet::lookup`.                                                                                          | A    |
|   38 | 0x0836d140 | FUN_0836bba0|                    1 | `DictionarySet_prioLookup` | Wraps `DictionarySet::prioritizedLookup`.                                                                               | A    |
|   39 | 0x0836cf20 | FUN_0836b8d0|                    1 | `setPair_B(a,b)`           | `ctrl[+0xd] = a; ctrl[+0x11] = b;`                                                                                      | A    |
|   40 | 0x0836cf40 | FUN_0836b8f0|                    1 | `setPair_C(a,b)`           | `ctrl[+0x15] = a; ctrl[+0x19] = b;`                                                                                     | A    |
|   41 | 0x0836d150 | FUN_0836b830|                    1 | `setPair_E(a,b)`           | `ctrl[+0x25] = a; ctrl[+0x29] = b;`                                                                                     | A    |
|   42 | 0x0836d170 | FUN_0836b5b0|                    4 | `delegateB_call(a,b,c)`    | If `state[+0x2dc]` != 0: `FUN_0836c420(state[+0x2dc], a, b, c)`. 3 user args.                                            | B    |
|   43 | 0x0836d1c0 | FUN_0836b850|                    1 | `setPair_F(a,b)`           | `ctrl[+0x2d] = a; ctrl[+0x31] = b;`                                                                                     | A    |
|   44 | 0x0836d1e0 | FUN_0836b600|                    4 | `delegateB_call2(a,b,c)`   | If `state[+0x2dc]` != 0: `FUN_0836c420(state[+0x2e0], a, b, c)`. Same delegate, different "method".                      | B    |
|   45 | 0x0836d230 | FUN_0836b870|                    1 | `setPair_G(a,b)`           | `ctrl[+0x35] = a; ctrl[+0x39] = b;`                                                                                     | A    |
|   46 | 0x0836d250 | FUN_0836b890|                    1 | `setPair_H(a,b)`           | `ctrl[+0x3d] = a; ctrl[+0x41] = b;`                                                                                     | A    |
|   47 | 0x0836d270 | FUN_0836bbd0|                    1 | `installHookA`             | `FUN_0836c8d0(a,b,c)` — 3 args, no state. Probably a callback installer.                                                | C    |
|   48 | 0x0836d290 | FUN_0836bbf0|                    1 | `installHookB`             | `FUN_0836c920(a,b,c)` — 3 args, no state. Probably another callback installer.                                          | C    |

**Counts:** A = **24**, B = **22**, C = **3**. Easily clears the >15 A/B bar.

## How to read this for Task 4

The "synth_text" entry is **slot 8**, not slot 3. Slot 3 / slot 4 are one-shot initializers; slot 8 is where the FE pipeline runs each utterance (it clears `ctrl[+3]`/`ctrl[+4]` to allow re-entry and is the one that invokes the per-utterance `FUN_07df2926` after running text normalization helpers and the "wordsin" handler lookup). FE_CLAUDE.md's guess "slot 3 is most likely synth" appears to be wrong; slot 3 is the FE *init* stage.

Confirm with the runtime probe (`host/probe_vtable.c` — also in this directory) before wiring Task 4.

## How input text reaches the FE

There is no `setText` slot taking a `char *` directly in the table above. The text-input path appears to go through one of:

- `feedConfigA(s)` / `feedConfigB(s)` (slots 5, 6) which pass `s` to delegate-A via `FUN_0836c3d0` — this is consistent with the Eloquence convention where the host pushes input via a "text source" delegate that the FE pulls from.
- The `"wordsin"` handler lookup inside slot 8 — `FUN_0838ebc0(state, "wordsin")` resolves a registered handler ID; `FUN_0838ee90(state, id)` selects it; then the deep FE reads from whatever the delegate provides.

So the **expected call sequence** is:

```
getObject(1, &fe);
fe->slot3();                          // initStage1
fe->slot4();                          // initStage2  (may be optional)
fe->slot27(...) or slot28(...);       // load dictionary
fe->slot30(...);                      // activate dictionary
fe->slot25('en');                     // setMode (language)
fe->slot19/22/39-46(a, b);            // set up float-pair parameters
fe->slot9(setup_delegate_args...);    // wire delegate-A (text source)
fe->slot8();                          // SYNTH — runs the pipeline
fe->slot42(read_results...);          // read back results via delegate-B
fe->slot26();                         // reset for next utterance
fe->Release();
```

This is a guess — the runtime probe + Frida trace of a known-good caller (`Speechify.exe` itself) will pin it down without ambiguity.

## What stays unknown without runtime data

- The exact bytes that `feedConfigA`/`feedConfigB` (slots 5, 6) expect for arg 2 (string? struct?). Decomp shows `FUN_0836c3d0(delegate, arg)` — delegate's vtable is the only way to know.
- Whether slot 9 / slot 42 / slot 44 are "push input" or "pull output" — depends on the delegate vtable behavior.
- The format of slot 15 (`predicateA`) — `FUN_0836d650(void)` is 3 bytes (just `xor eax, eax; ret`), so it always returns 0 i.e. `predicateA` always returns `true`. Probably a stub for backwards-compatibility.

These can be resolved by running `probe_vtable.c` on Windows (see next file) and by Frida-tracing a real `Speechify.exe` call.
