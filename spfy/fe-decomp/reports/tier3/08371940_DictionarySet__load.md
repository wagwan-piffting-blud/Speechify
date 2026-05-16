# `DictionarySet::load` @ `08371940`

- **Tier reason:** seed
- **Size:** 253 bytes / 86 instr / 10 BBs
- **Params:** 3 / `unknown`
- **Signature:** `undefined DictionarySet::load(undefined4 param_1, undefined4 param_2, undefined4 param_3)`
- **Tags:** `struct.medium`, `tts.dictionary`

## Callers

- `0836ba09`  `?`

## Callees

- `0836c8f0`  `FUN_0836c8f0`
- `0836f4e0`  `FUN_0836f4e0`
- `083704d0`  `DictionarySet::free`
- `08370bc0`  `FUN_08370bc0`
- `083716e0`  `UserDict::load`
- `0838e1a0`  `FUN_0838e1a0`

## String references

- `08396bd8` (string): "Entering DictionarySet::load\n"
- `08395cfc` (string): "Name is invalid\n"
- `08396bb8` (string): "Leaving DictionarySet::load\n"
- `08396b8c` (string): "Couldn't create user dictionary name: %s\n"
- `08396b54` (string): "User dictionary name: %s type: %d is already loaded.\n"

## Decompiled

```c

int DictionarySet__load(int param_1,undefined4 param_2,undefined4 param_3)

{
  int iVar1;
  int in_ECX;
  
  FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Entering DictionarySet::load\n");
  if (param_1 == 0) {
    FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Name is invalid\n");
    FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Leaving DictionarySet::load\n");
    return -0xc;
  }
  iVar1 = FUN_0836f4e0(*(undefined4 *)(in_ECX + 0x8e),param_1);
  if (iVar1 == 0) {
    iVar1 = FUN_0838e1a0(*(undefined4 *)(in_ECX + 1),param_1);
    if (iVar1 == 0) {
      iVar1 = FUN_08370bc0(param_1);
      if (iVar1 == 0) {
        FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Couldn\'t create user dictionary name: %s\n",
                     param_1);
        iVar1 = -2;
      }
      else {
        iVar1 = UserDict__load(param_2,param_3);
        if (iVar1 != 0) {
          DictionarySet__free(param_1);
        }
      }
    }
    else {
      FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),
                   "User dictionary name: %s type: %d is already loaded.\n",
                   *(undefined4 *)(iVar1 + 0x1e),*(undefined4 *)(iVar1 + 0x18));
      iVar1 = -0x14;
    }
  }
  FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Leaving DictionarySet::load\n");
  return iVar1;
}


```
