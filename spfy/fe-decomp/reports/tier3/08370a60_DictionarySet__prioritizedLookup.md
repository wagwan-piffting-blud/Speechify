# `DictionarySet::prioritizedLookup` @ `08370a60`

- **Tier reason:** seed
- **Size:** 220 bytes / 78 instr / 11 BBs
- **Params:** 3 / `unknown`
- **Signature:** `undefined DictionarySet::prioritizedLookup(undefined4 param_1, undefined4 param_2, undefined4 param_3)`
- **Tags:** `struct.medium`, `tts.dictionary`

## Callers

- `0836bbb9`  `?`

## Callees

- `0836c8f0`  `FUN_0836c8f0`
- `0836f3f0`  `FUN_0836f3f0`
- `0836f480`  `FUN_0836f480`
- `0836fe00`  `UserDict::lookup`

## String references

- `08396660` (string): "Entering DictionarySet::prioritizedLookup\n"
- `08396574` (string): "Type is invalid\n"
- `08396634` (string): "Leaving DictionarySet::prioritizedLookup\n"

## Decompiled

```c

/* WARNING: Removing unreachable block (ram,0x08370afa) */

int DictionarySet__prioritizedLookup(undefined4 param_1,undefined4 param_2,undefined4 param_3)

{
  char cVar1;
  int iVar2;
  int iVar3;
  int in_ECX;
  
  FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Entering DictionarySet::prioritizedLookup\n");
  cVar1 = FUN_0836f480(param_1);
  if (cVar1 == '\0') {
    FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Type is invalid\n");
    FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Leaving DictionarySet::prioritizedLookup\n");
    return -0x15;
  }
  iVar2 = FUN_0836f3f0(param_1);
  for (iVar2 = *(int *)(*(int *)(in_ECX + 9) + iVar2 * 4); iVar2 != 0;
      iVar2 = *(int *)(iVar2 + 0x14)) {
    iVar3 = UserDict__lookup(param_2,param_3);
    if ((iVar3 == 0) || (iVar3 != -0x11)) goto LAB_08370b1a;
  }
  FUN_0836f3f0(param_1,param_2,param_3);
  iVar3 = UserDict__lookup(param_2,param_3);
LAB_08370b1a:
  FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Leaving DictionarySet::prioritizedLookup\n");
  return iVar3;
}


```
