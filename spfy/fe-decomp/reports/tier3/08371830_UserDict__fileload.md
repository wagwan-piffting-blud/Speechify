# `UserDict::fileload` @ `08371830`

- **Tier reason:** seed
- **Size:** 263 bytes / 93 instr / 9 BBs
- **Params:** 2 / `unknown`
- **Signature:** `undefined UserDict::fileload(undefined4 param_1, undefined4 param_2)`
- **Tags:** `struct.medium`, `tts.dictionary`

## Callers

- `08371aca`  `?`

## Callees

- `0836c8f0`  `FUN_0836c8f0`
- `083700c0`  `FUN_083700c0`
- `083714c0`  `FUN_083714c0`

## String references

- `08396b34` (string): "Entering UserDict::fileload\n"
- `08396a94` (string): "Loading user dictionary name: %s type: %d\n"
- `08396904` (string): "Could not create hashtable\n"
- `08396ac0` (string): "Leaving UserDict::load\n"
- `08396a78` (string): "Could not load hashtable\n"
- `08396b18` (string): "Leaving UserDict::fileload\n"
- `08395f88` (string): "Parameters are invalid\n"

## Decompiled

```c

undefined4 UserDict__fileload(int param_1,uint param_2)

{
  char cVar1;
  int *in_ECX;
  
  FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Entering UserDict::fileload\n");
  if ((param_1 == 0) || (param_2 == 0)) {
    FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Parameters are invalid\n");
    FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Leaving UserDict::fileload\n");
    return 0xfffffff4;
  }
  FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Loading user dictionary name: %s type: %d\n",
               *(undefined4 *)((int)in_ECX + 0x1e),in_ECX[6]);
  if (*in_ECX == 0) {
    cVar1 = FUN_083700c0(param_2 / 0xf);
    if (cVar1 == '\0') {
      FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Could not create hashtable\n");
      FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Leaving UserDict::load\n");
      return 0xfffffffe;
    }
  }
  cVar1 = FUN_083714c0(param_1,param_2);
  if (cVar1 == '\0') {
    FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Could not load hashtable\n");
    FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Leaving UserDict::fileload\n");
    return 0xffffffeb;
  }
  FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Leaving UserDict::fileload\n");
  return 0;
}


```
