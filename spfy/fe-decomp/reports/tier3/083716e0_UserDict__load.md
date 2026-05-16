# `UserDict::load` @ `083716e0`

- **Tier reason:** seed
- **Size:** 325 bytes / 110 instr / 11 BBs
- **Params:** 2 / `unknown`
- **Signature:** `undefined UserDict::load(undefined4 param_1, undefined4 param_2)`
- **Tags:** `struct.medium`, `tts.dictionary`

## Callers

- `083719cf`  `?`

## Callees

- `0836c8f0`  `FUN_0836c8f0`
- `0836fb10`  `FUN_0836fb10`
- `083700c0`  `FUN_083700c0`
- `083714c0`  `FUN_083714c0`

## String references

- `08396afc` (string): "Entering UserDict::load\n"
- `08396ad8` (string): "Could not extract dictionary type\n"
- `08396ac0` (string): "Leaving UserDict::load\n"
- `08396904` (string): "Could not create hashtable\n"
- `08396a94` (string): "Loading user dictionary name: %s type: %d\n"
- `08396a78` (string): "Could not load hashtable\n"
- `08395f88` (string): "Parameters are invalid\n"

## Decompiled

```c

int UserDict__load(int param_1,uint param_2)

{
  uint uVar1;
  char cVar2;
  int iVar3;
  int *in_ECX;
  
  FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Entering UserDict::load\n");
  if ((param_1 == 0) || (param_2 == 0)) {
    FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Parameters are invalid\n");
    FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Leaving UserDict::load\n");
    return -0xc;
  }
  iVar3 = FUN_0836fb10(&param_1,&param_2);
  uVar1 = param_2;
  if (iVar3 != 0) {
    FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Could not extract dictionary type\n");
    FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Leaving UserDict::load\n");
    return iVar3;
  }
  if (*in_ECX == 0) {
    cVar2 = FUN_083700c0(param_2 / 0xf);
    if (cVar2 == '\0') {
      FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Could not create hashtable\n");
      FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Leaving UserDict::load\n");
      return -2;
    }
  }
  FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Loading user dictionary name: %s type: %d\n",
               *(undefined4 *)((int)in_ECX + 0x1e),in_ECX[6]);
  cVar2 = FUN_083714c0(param_1,uVar1);
  if (cVar2 == '\0') {
    FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Could not load hashtable\n");
    FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Leaving UserDict::load\n");
    return -0x15;
  }
  FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Leaving UserDict::load\n");
  return 0;
}


```
