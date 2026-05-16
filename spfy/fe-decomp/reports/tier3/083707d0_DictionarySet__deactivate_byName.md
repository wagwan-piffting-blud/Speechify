# `DictionarySet::deactivate_byName` @ `083707d0`

- **Tier reason:** seed
- **Size:** 224 bytes / 75 instr / 7 BBs
- **Params:** 1 / `unknown`
- **Signature:** `undefined DictionarySet::deactivate_byName(undefined4 param_1)`
- **Tags:** `struct.medium`, `tts.dictionary`

## Callers

- `0836ba8f`  `?`

## Callees

- `0836c8f0`  `FUN_0836c8f0`
- `0836f4e0`  `FUN_0836f4e0`
- `0836f7e0`  `FUN_0836f7e0`
- `0838e1a0`  `FUN_0838e1a0`

## String references

- `08396518` (string): "Entering DictionarySet::deactivate(const char *name)\n"
- `08395cfc` (string): "Name is invalid\n"
- `083964e0` (string): "Leaving DictionarySet::deactivate(const char *name)\n"
- `08396498` (string): "Leaving DictionarySet::deactivate\n"
- `083963d0` (string): "Can't find user dictionary name: %s\n"

## Decompiled

```c

int DictionarySet__deactivate_byName(int param_1)

{
  int iVar1;
  int in_ECX;
  
  FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),
               "Entering DictionarySet::deactivate(const char *name)\n");
  if (param_1 == 0) {
    FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Name is invalid\n");
    FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),
                 "Leaving DictionarySet::deactivate(const char *name)\n");
  }
  else {
    iVar1 = FUN_0836f4e0(*(undefined4 *)(in_ECX + 0x8e),param_1);
    if (iVar1 != 0) {
      FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Leaving DictionarySet::deactivate\n");
      return iVar1;
    }
    iVar1 = FUN_0838e1a0(*(undefined4 *)(in_ECX + 1),param_1);
    if (iVar1 != 0) {
      FUN_0836f7e0(iVar1);
      FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),
                   "Leaving DictionarySet::deactivate(const char *name)\n");
      return 0;
    }
  }
  FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Can\'t find user dictionary name: %s\n",param_1);
  FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),
               "Leaving DictionarySet::deactivate(const char *name)\n");
  return -0x13;
}


```
