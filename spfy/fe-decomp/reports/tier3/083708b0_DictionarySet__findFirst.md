# `DictionarySet::findFirst` @ `083708b0`

- **Tier reason:** seed
- **Size:** 144 bytes / 46 instr / 3 BBs
- **Params:** 3 / `unknown`
- **Signature:** `undefined DictionarySet::findFirst(undefined4 param_1, undefined4 param_2, undefined4 param_3)`
- **Tags:** `struct.small`, `tts.dictionary`

## Callers

- `0836bb29`  `?`

## Callees

- `0836c8f0`  `FUN_0836c8f0`
- `0836f3f0`  `FUN_0836f3f0`
- `0836f480`  `FUN_0836f480`
- `0836fbf0`  `UserDict::findFirst`

## String references

- `08396588` (string): "Entering DictionarySet::findFirst\n"
- `08396574` (string): "Type is invalid\n"
- `08396550` (string): "Leaving DictionarySet::findFirst\n"

## Decompiled

```c

undefined4 DictionarySet__findFirst(undefined4 param_1,undefined4 param_2,undefined4 param_3)

{
  char cVar1;
  undefined4 uVar2;
  int in_ECX;
  
  FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Entering DictionarySet::findFirst\n");
  cVar1 = FUN_0836f480(param_1);
  if (cVar1 == '\0') {
    FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Type is invalid\n");
    FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Leaving DictionarySet::findFirst\n");
    return 0xffffffeb;
  }
  FUN_0836f3f0(param_1,param_2,param_3);
  uVar2 = UserDict__findFirst(param_2,param_3);
  FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Leaving DictionarySet::findFirst\n");
  return uVar2;
}


```
