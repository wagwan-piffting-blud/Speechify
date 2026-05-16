# `UserDict::findFirst` @ `0836fbf0`

- **Tier reason:** seed
- **Size:** 282 bytes / 105 instr / 8 BBs
- **Params:** 2 / `unknown`
- **Signature:** `undefined UserDict::findFirst(undefined4 param_1, undefined4 param_2)`
- **Tags:** `struct.medium`, `tts.dictionary`

## Callers

- `0837091e`  `?`

## Callees

- `0836c8f0`  `FUN_0836c8f0`
- `0838dd60`  `FUN_0838dd60`
- `0838dd70`  `FUN_0838dd70`
- `0838e3f0`  `FUN_0838e3f0`

## String references

- `08396030` (string): "Entering UserDict::findFirst\n"
- `08396004` (string): "User dictionary name: %s type: %d is empty\n"
- `08395fe4` (string): "Leaving UserDict::findFirst\n"
- `08395fa0` (string): "Found key: %s translation: %s in user dictionary name: %s type: %d\n"
- `08395f88` (string): "Parameters are invalid\n"

## Decompiled

```c

undefined4 UserDict__findFirst(undefined4 *param_1,undefined4 *param_2)

{
  int *piVar1;
  char cVar2;
  undefined4 uVar3;
  int *in_ECX;
  
  FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Entering UserDict::findFirst\n");
  if ((param_1 == (undefined4 *)0x0) || (param_2 == (undefined4 *)0x0)) {
    FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Parameters are invalid\n");
    FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Leaving UserDict::findFirst\n");
    return 0xfffffff4;
  }
  if (*in_ECX == 0) {
    FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"User dictionary name: %s type: %d is empty\n",
                 *(undefined4 *)((int)in_ECX + 0x1e),in_ECX[6]);
    FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Leaving UserDict::findFirst\n");
    return 0xffffffef;
  }
  piVar1 = in_ECX + 1;
  cVar2 = FUN_0838e3f0(piVar1,*in_ECX);
  if (cVar2 != '\0') {
    uVar3 = FUN_0838dd60(piVar1);
    *param_1 = uVar3;
    uVar3 = FUN_0838dd70(piVar1);
    *param_2 = uVar3;
    FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),
                 "Found key: %s translation: %s in user dictionary name: %s type: %d\n",*param_1,
                 uVar3,*(undefined4 *)((int)in_ECX + 0x1e),in_ECX[6]);
    FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Leaving UserDict::findFirst\n");
    return 0;
  }
  FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"User dictionary name: %s type: %d is empty\n",
               *(undefined4 *)((int)in_ECX + 0x1e),in_ECX[6]);
  FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Leaving UserDict::findFirst\n");
  return 0xffffffef;
}


```
