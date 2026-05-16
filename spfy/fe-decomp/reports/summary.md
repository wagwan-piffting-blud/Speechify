# Triage Summary

**Program:** `SWIttsFe-en-US.dll`
**Language:** `x86:LE:32:default`
**Compiler:** `windows`
**Image base:** `07dd0000`
**Pointer size:** 4

**Functions:** 2,176
**Strings:** 438

## Tier distribution

| Tier | Count | Meaning |
|---|---|---|
| 0 | 127 | skip (junk) |
| 1 | 1,825 | auto-tag only |
| 2 | 175 | paragraph (LLM) |
| 3 | 49 | deep dive |

## Tag counts

| Tag | Count |
|---|---|
| `struct.medium` | 1,192 |
| `struct.small` | 767 |
| `struct.no_decomp` | 431 |
| `struct.tiny` | 126 |
| `struct.large` | 83 |
| `struct.thunk` | 65 |
| `io.string` | 19 |
| `tts.dictionary` | 19 |
| `speechify.delta` | 17 |
| `io.file` | 13 |
| `math.heavy` | 13 |
| `tts.klatt` | 10 |
| `struct.huge` | 8 |
| `name.runtime` | 4 |
| `speechify.core` | 3 |
| `tts.concatenative` | 2 |
| `tts.unit_db` | 2 |
| `eloquence.eci` | 1 |
| `name.dllmain` | 1 |
| `tts.engine` | 1 |
| `tts.phonetic_features` | 1 |

## Seed functions

| Address | Name | Size | Term | Match |
|---|---|---|---|---|
| `0838a410` | `FUN_0838a410` | 7291 | `klatt` | `string:"

KlattID version 4.0 ? International Business Machi` |
| `0836abd0` | `FUN_0836abd0` | 935 | `concatenative` | `string:"Concatenative ECI Output"` |
| `0837f580` | `FUN_0837f580` | 905 | `delta insert` | `string:"\ndelta insert [%s%s "` |
| `08380160` | `FUN_08380160` | 899 | `deltio` | `string:"DELTIO"` |
| `0837fcc0` | `FUN_0837fcc0` | 431 | `deltio` | `string:"DELTIO"` |
| `08371320` | `UserDict::update` | 414 | `userdict` | `string:"Entering UserDict::update\n"` |
| `08370de0` | `FUN_08370de0` | 404 | `maindict` | `string:"maindict"` |
| `0838c110` | `FUN_0838c110` | 348 | `klatt` | `string:"

KlattID version 4.0 ? International Business Machi` |
| `083704d0` | `DictionarySet::free` | 344 | `dictionaryset` | `string:"Entering DictionarySet::free\n"` |
| `08370630` | `DictionarySet::activate` | 337 | `dictionaryset` | `string:"Entering DictionarySet::activate\n"` |
| `083716e0` | `UserDict::load` | 325 | `userdict` | `string:"Entering UserDict::load\n"` |
| `0836fbf0` | `UserDict::findFirst` | 282 | `userdict` | `string:"Entering UserDict::findFirst\n"` |
| `0838a190` | `FUN_0838a190` | 275 | `klatt` | `string:"

KlattID version 4.0 ? International Business Machi` |
| `08371830` | `UserDict::fileload` | 263 | `userdict` | `string:"Entering UserDict::fileload\n"` |
| `0836fe00` | `UserDict::lookup` | 261 | `userdict` | `string:"Entering UserDict::lookup\n"` |
| `08371940` | `DictionarySet::load` | 253 | `dictionaryset` | `string:"Entering DictionarySet::load\n"` |
| `0836fd10` | `UserDict::findNext` | 225 | `userdict` | `string:"Entering UserDict::findNext\n"` |
| `0836835a` | `FUN_0836835a` | 225 | `enu.ddl` | `string:"enu.ddl"` |
| `083707d0` | `DictionarySet::deactivate_byName` | 224 | `dictionaryset` | `string:"Entering DictionarySet::deactivate(const char *name)` |
| `08370a60` | `DictionarySet::prioritizedLookup` | 220 | `dictionaryset` | `string:"Entering DictionarySet::prioritizedLookup\n"` |
| `0836dba0` | `FUN_0836dba0` | 208 | `audio.cdv` | `string:"audio.cdv"` |
| `08371a40` | `DictionarySet::fileLoad` | 201 | `dictionaryset` | `string:"Entering DictionarySet::fileLoad\n"` |
| `0838eef0` | `FUN_0838eef0` | 199 | `deltatools` | `string:"There are too many interactive lfiles; use a bigger ` |
| `08371650` | `DictionarySet::update` | 144 | `dictionaryset` | `string:"Entering DictionarySet::update\n"` |
| `083709d0` | `DictionarySet::lookup` | 144 | `dictionaryset` | `string:"Entering DictionarySet::lookup\n"` |
| `08370940` | `DictionarySet::findNext` | 144 | `dictionaryset` | `string:"Entering DictionarySet::findNext\n"` |
| `083708b0` | `DictionarySet::findFirst` | 144 | `dictionaryset` | `string:"Entering DictionarySet::findFirst\n"` |
| `0837f380` | `FUN_0837f380` | 108 | `deltio` | `string:"DELTIO"` |
| `0836f504` | `FUN_0836f504` | 108 | `maindict` | `string:"maindict"` |
| `0838a0d0` | `FUN_0838a0d0` | 94 | `klatt` | `string:"

KlattID version 4.0 ? International Business Machi` |
| `0838a3b0` | `FUN_0838a3b0` | 81 | `klatt` | `string:"

KlattID version 4.0 ? International Business Machi` |
| `0838a350` | `FUN_0838a350` | 81 | `klatt` | `string:"

KlattID version 4.0 ? International Business Machi` |
| `0838a2b0` | `FUN_0838a2b0` | 79 | `klatt` | `string:"

KlattID version 4.0 ? International Business Machi` |
| `0838a130` | `FUN_0838a130` | 79 | `klatt` | `string:"

KlattID version 4.0 ? International Business Machi` |
| `0838c0c0` | `FUN_0838c0c0` | 76 | `klatt` | `string:"

KlattID version 4.0 ? International Business Machi` |
| `0838a300` | `FUN_0838a300` | 72 | `klatt` | `string:"

KlattID version 4.0 ? International Business Machi` |
| `0837f4f0` | `FUN_0837f4f0` | 66 | `delta insert` | `string:"\ndelta insert [%s%s "` |
| `08370790` | `DictionarySet::deactivate` | 54 | `dictionaryset` | `string:"Entering DictionarySet::deactivate\n"` |
| `0836bba0` | `FUN_0836bba0` | 33 | `dictionaryset` | `callee:DictionarySet::prioritizedLookup` |
| `0836bb70` | `FUN_0836bb70` | 33 | `dictionaryset` | `callee:DictionarySet::lookup` |
| `0836bb40` | `FUN_0836bb40` | 33 | `dictionaryset` | `callee:DictionarySet::findNext` |
| `0836bb10` | `FUN_0836bb10` | 33 | `dictionaryset` | `callee:DictionarySet::findFirst` |
| `0836bae0` | `FUN_0836bae0` | 33 | `dictionaryset` | `callee:DictionarySet::update` |
| `0836b9f0` | `FUN_0836b9f0` | 33 | `dictionaryset` | `callee:DictionarySet::load` |
| `0836ba60` | `FUN_0836ba60` | 28 | `dictionaryset` | `callee:DictionarySet::activate` |
| `0836ba20` | `FUN_0836ba20` | 28 | `dictionaryset` | `callee:DictionarySet::fileLoad` |
| `0836ba80` | `FUN_0836ba80` | 23 | `dictionaryset` | `callee:DictionarySet::deactivate_byName` |
| `0836ba40` | `FUN_0836ba40` | 23 | `dictionaryset` | `callee:DictionarySet::free` |
| `0836baa0` | `FUN_0836baa0` | 18 | `dictionaryset` | `callee:DictionarySet::deactivate` |
| `083923f0` | `Unwind@083923f0` | 8 | `f0` | `name:Unwind@083923f0` |

## Tier 3 (deep dive)

_49 functions_

| Address | Name | Size | Reason |
|---|---|---|---|
| `0838a410` | `FUN_0838a410` | 7291 | seed |
| `0836abd0` | `FUN_0836abd0` | 935 | seed |
| `0837f580` | `FUN_0837f580` | 905 | seed |
| `08380160` | `FUN_08380160` | 899 | seed |
| `0837fcc0` | `FUN_0837fcc0` | 431 | seed |
| `08371320` | `UserDict::update` | 414 | seed |
| `08370de0` | `FUN_08370de0` | 404 | seed |
| `0838c110` | `FUN_0838c110` | 348 | seed |
| `083704d0` | `DictionarySet::free` | 344 | seed |
| `08370630` | `DictionarySet::activate` | 337 | seed |
| `083716e0` | `UserDict::load` | 325 | seed |
| `0836fbf0` | `UserDict::findFirst` | 282 | seed |
| `0838a190` | `FUN_0838a190` | 275 | seed |
| `08371830` | `UserDict::fileload` | 263 | seed |
| `0836fe00` | `UserDict::lookup` | 261 | seed |
| `08371940` | `DictionarySet::load` | 253 | seed |
| `0836835a` | `FUN_0836835a` | 225 | seed |
| `0836fd10` | `UserDict::findNext` | 225 | seed |
| `083707d0` | `DictionarySet::deactivate_byName` | 224 | seed |
| `08370a60` | `DictionarySet::prioritizedLookup` | 220 | seed |
| `0836dba0` | `FUN_0836dba0` | 208 | seed |
| `08371a40` | `DictionarySet::fileLoad` | 201 | seed |
| `0838eef0` | `FUN_0838eef0` | 199 | seed |
| `083708b0` | `DictionarySet::findFirst` | 144 | seed |
| `08370940` | `DictionarySet::findNext` | 144 | seed |
| `083709d0` | `DictionarySet::lookup` | 144 | seed |
| `08371650` | `DictionarySet::update` | 144 | seed |
| `0836f504` | `FUN_0836f504` | 108 | seed |
| `0837f380` | `FUN_0837f380` | 108 | seed |
| `0838a0d0` | `FUN_0838a0d0` | 94 | seed |
| `0838a350` | `FUN_0838a350` | 81 | seed |
| `0838a3b0` | `FUN_0838a3b0` | 81 | seed |
| `0838a130` | `FUN_0838a130` | 79 | seed |
| `0838a2b0` | `FUN_0838a2b0` | 79 | seed |
| `0838c0c0` | `FUN_0838c0c0` | 76 | seed |
| `0838a300` | `FUN_0838a300` | 72 | seed |
| `0837f4f0` | `FUN_0837f4f0` | 66 | seed |
| `08370790` | `DictionarySet::deactivate` | 54 | seed |
| `0836b9f0` | `FUN_0836b9f0` | 33 | seed |
| `0836bae0` | `FUN_0836bae0` | 33 | seed |
| `0836bb10` | `FUN_0836bb10` | 33 | seed |
| `0836bb40` | `FUN_0836bb40` | 33 | seed |
| `0836bb70` | `FUN_0836bb70` | 33 | seed |
| `0836bba0` | `FUN_0836bba0` | 33 | seed |
| `0836ba20` | `FUN_0836ba20` | 28 | seed |
| `0836ba60` | `FUN_0836ba60` | 28 | seed |
| `0836ba40` | `FUN_0836ba40` | 23 | seed |
| `0836ba80` | `FUN_0836ba80` | 23 | seed |
| `0836baa0` | `FUN_0836baa0` | 18 | seed |
