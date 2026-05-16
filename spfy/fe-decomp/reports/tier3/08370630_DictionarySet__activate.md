# `DictionarySet::activate` @ `08370630`

- **Tier reason:** seed
- **Size:** 337 bytes / 113 instr / 12 BBs
- **Params:** 2 / `unknown`
- **Signature:** `undefined DictionarySet::activate(undefined4 param_1, undefined4 param_2)`
- **Tags:** `struct.medium`, `tts.dictionary`

## Callers

- `0836ba74`  `?`

## Callees

- `0836c8f0`  `FUN_0836c8f0`
- `0836f4e0`  `FUN_0836f4e0`
- `0836f6b0`  `FUN_0836f6b0`
- `0838e1a0`  `FUN_0838e1a0`

## String references

- `08396474` (string): "Entering DictionarySet::activate\n"
- `08395cfc` (string): "Name is invalid\n"
- `08396450` (string): "Leaving DictionarySet::activate\n"
- `083963d0` (string): "Can't find user dictionary name: %s\n"
- `08396388` (string): "User dictionary name: %s type: %d is already active at priority: %d\n"
- `08396438` (string): "Priority is invalid\n"

## Decompiled

```c

int DictionarySet__activate(int param_1,int param_2)

{
  int iVar1;
  int in_ECX;
  
  FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Entering DictionarySet::activate\n");
  if (param_1 == 0) {
    FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Name is invalid\n");
    FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Leaving DictionarySet::activate\n");
    return -0xc;
  }
  iVar1 = FUN_0836f4e0(*(undefined4 *)(in_ECX + 0x8e),param_1);
  if (iVar1 != 0) {
    FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Leaving DictionarySet::activate\n");
    return iVar1;
  }
  iVar1 = FUN_0838e1a0(*(undefined4 *)(in_ECX + 1),param_1);
  if (iVar1 == 0) {
    FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Can\'t find user dictionary name: %s\n",param_1);
    FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Leaving DictionarySet::activate\n");
    return -0x13;
  }
  if (*(char *)(iVar1 + 0x1c) != '\0') {
    FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),
                 "User dictionary name: %s type: %d is already active at priority: %d\n",
                 *(undefined4 *)(iVar1 + 0x1e),*(undefined4 *)(iVar1 + 0x18),
                 *(undefined4 *)(iVar1 + 0x10));
    FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Leaving DictionarySet::activate\n");
    return -0x12;
  }
  if (param_2 == 0) {
    FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Priority is invalid\n");
    iVar1 = -0x16;
  }
  else {
    iVar1 = FUN_0836f6b0(iVar1,param_2);
  }
  FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Leaving DictionarySet::activate\n");
  return iVar1;
}


```
