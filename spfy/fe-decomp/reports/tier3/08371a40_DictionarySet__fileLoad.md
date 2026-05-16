# `DictionarySet::fileLoad` @ `08371a40`

- **Tier reason:** seed
- **Size:** 201 bytes / 66 instr / 6 BBs
- **Params:** 2 / `unknown`
- **Signature:** `undefined DictionarySet::fileLoad(undefined4 param_1, undefined4 param_2)`
- **Tags:** `struct.medium`, `tts.dictionary`

## Callers

- `0836ba34`  `?`

## Callees

- `0836c8f0`  `FUN_0836c8f0`
- `0836f3f0`  `FUN_0836f3f0`
- `0836f480`  `FUN_0836f480`
- `0836f880`  `FUN_0836f880`
- `08371830`  `UserDict::fileload`
- `08390cac`  `operator_delete`

## String references

- `08396c1c` (string): "Entering DictionarySet::fileLoad\n"
- `08396574` (string): "Type is invalid\n"
- `08396bf8` (string): "Leaving DictionarySet::fileLoad\n"

## Decompiled

```c

undefined4 DictionarySet__fileLoad(undefined4 param_1,undefined4 param_2)

{
  char cVar1;
  undefined4 uVar2;
  void *in_ECX;
  void *pvVar3;
  undefined4 uVar4;
  void *local_4;
  
  local_4 = in_ECX;
  FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x8e),"Entering DictionarySet::fileLoad\n");
  uVar2 = param_1;
  cVar1 = FUN_0836f480(param_1);
  if (cVar1 == '\0') {
    FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x8e),"Type is invalid\n");
    FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x8e),"Leaving DictionarySet::fileLoad\n");
    return 0xffffffeb;
  }
  cVar1 = FUN_0836f880(param_2,&local_4,&param_1);
  if (cVar1 == '\0') {
    uVar2 = 1;
  }
  else {
    pvVar3 = local_4;
    uVar4 = param_1;
    FUN_0836f3f0(uVar2,local_4,param_1);
    uVar2 = UserDict__fileload(pvVar3,uVar4);
    operator_delete(local_4);
    local_4 = (void *)0x0;
  }
  FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x8e),"Leaving DictionarySet::fileLoad\n");
  return uVar2;
}


```
