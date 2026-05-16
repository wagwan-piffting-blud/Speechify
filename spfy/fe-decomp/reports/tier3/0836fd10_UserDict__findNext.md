# `UserDict::findNext` @ `0836fd10`

- **Tier reason:** seed
- **Size:** 225 bytes / 83 instr / 6 BBs
- **Params:** 2 / `unknown`
- **Signature:** `undefined UserDict::findNext(undefined4 param_1, undefined4 param_2)`
- **Tags:** `struct.medium`, `tts.dictionary`

## Callers

- `083709ae`  `?`

## Callees

- `0836c8f0`  `FUN_0836c8f0`
- `0838dd60`  `FUN_0838dd60`
- `0838dd70`  `FUN_0838dd70`
- `0838dd80`  `FUN_0838dd80`

## String references

- `0839606c` (string): "Entering UserDict::findNext\n"
- `08395fa0` (string): "Found key: %s translation: %s in user dictionary name: %s type: %d\n"
- `08396050` (string): "Leaving UserDict::findNext\n"
- `08396004` (string): "User dictionary name: %s type: %d is empty\n"
- `08395f88` (string): "Parameters are invalid\n"

## Decompiled

```c

undefined4 UserDict__findNext(undefined4 *param_1,undefined4 *param_2)

{
  int iVar1;
  char cVar2;
  undefined4 uVar3;
  int in_ECX;
  
  FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x22),"Entering UserDict::findNext\n");
  if ((param_1 != (undefined4 *)0x0) && (param_2 != (undefined4 *)0x0)) {
    iVar1 = in_ECX + 4;
    cVar2 = FUN_0838dd80(iVar1);
    if (cVar2 != '\0') {
      uVar3 = FUN_0838dd60(iVar1);
      *param_1 = uVar3;
      uVar3 = FUN_0838dd70(iVar1);
      *param_2 = uVar3;
      FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x22),
                   "Found key: %s translation: %s in user dictionary name: %s type: %d\n",*param_1,
                   uVar3,*(undefined4 *)(in_ECX + 0x1e),*(undefined4 *)(in_ECX + 0x18));
      FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x22),"Leaving UserDict::findNext\n");
      return 0;
    }
    FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x22),"User dictionary name: %s type: %d is empty\n",
                 *(undefined4 *)(in_ECX + 0x1e),*(undefined4 *)(in_ECX + 0x18));
    FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x22),"Leaving UserDict::findNext\n");
    return 0xffffffef;
  }
  FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x22),"Parameters are invalid\n");
  FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x22),"Leaving UserDict::findNext\n");
  return 0xfffffff4;
}


```
