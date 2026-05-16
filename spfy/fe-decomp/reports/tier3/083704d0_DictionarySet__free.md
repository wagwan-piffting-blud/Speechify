# `DictionarySet::free` @ `083704d0`

- **Tier reason:** seed
- **Size:** 344 bytes / 117 instr / 9 BBs
- **Params:** 1 / `unknown`
- **Signature:** `undefined DictionarySet::free(undefined4 param_1)`
- **Tags:** `struct.medium`, `tts.dictionary`

## Callers

- `0836ba4f`  `?`
- `083719dd`  `?`

## Callees

- `0836c8f0`  `FUN_0836c8f0`
- `0836f4e0`  `FUN_0836f4e0`
- `08370360`  `FUN_08370360`
- `0838e1a0`  `FUN_0838e1a0`
- `0838e210`  `FUN_0838e210`

## String references

- `08396418` (string): "Entering DictionarySet::free\n"
- `08395cfc` (string): "Name is invalid\n"
- `083963f8` (string): "Leaving DictionarySet::free\n"
- `083963d0` (string): "Can't find user dictionary name: %s\n"
- `08396388` (string): "User dictionary name: %s type: %d is already active at priority: %d\n"
- `0839635c` (string): "Freeing user dictionary name: %s type: %d\n"

## Decompiled

```c

int DictionarySet__free(int param_1)

{
  int iVar1;
  int in_ECX;
  
  FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Entering DictionarySet::free\n");
  if (param_1 == 0) {
    FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Name is invalid\n");
    FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Leaving DictionarySet::free\n");
    return -0xc;
  }
  iVar1 = FUN_0836f4e0(*(undefined4 *)(in_ECX + 0x8e),param_1);
  if (iVar1 != 0) {
    FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Leaving DictionarySet::free\n");
    return iVar1;
  }
  iVar1 = FUN_0838e1a0(*(undefined4 *)(in_ECX + 1),param_1);
  if (iVar1 == 0) {
    FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Can\'t find user dictionary name: %s\n",param_1);
    FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Leaving DictionarySet::free\n");
    return -0x13;
  }
  if (*(char *)(iVar1 + 0x1c) != '\0') {
    FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),
                 "User dictionary name: %s type: %d is already active at priority: %d\n",
                 *(undefined4 *)(iVar1 + 0x1e),*(undefined4 *)(iVar1 + 0x18),
                 *(undefined4 *)(iVar1 + 0x10));
    FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Leaving DictionarySet::free\n");
    return -0x12;
  }
  FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Freeing user dictionary name: %s type: %d\n",
               *(undefined4 *)(iVar1 + 0x1e),*(undefined4 *)(iVar1 + 0x18));
  FUN_08370360(1);
  FUN_0838e210(*(undefined4 *)(in_ECX + 1),param_1,1,0);
  FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Leaving DictionarySet::free\n");
  return 0;
}


```
