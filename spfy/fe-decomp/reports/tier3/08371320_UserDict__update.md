# `UserDict::update` @ `08371320`

- **Tier reason:** seed
- **Size:** 414 bytes / 163 instr / 17 BBs
- **Params:** 2 / `unknown`
- **Signature:** `undefined UserDict::update(undefined4 param_1, undefined4 param_2)`
- **Tags:** `struct.medium`, `tts.dictionary`

## Callers

- `083716be`  `?`

## Callees

- `0836c8f0`  `FUN_0836c8f0`
- `083710b0`  `FUN_083710b0`
- `0838de80`  `FUN_0838de80`
- `0838e1a0`  `FUN_0838e1a0`
- `0838e210`  `FUN_0838e210`

## String references

- `0839693c` (string): "Entering UserDict::update\n"
- `08395f88` (string): "Parameters are invalid\n"
- `08396920` (string): "Leaving UserDict::update\n"
- `08396240` (string): "Could not build table for user dictionary\n"
- `08396904` (string): "Could not create hashtable\n"
- `083968b8` (string): "Updated key: %s in user dictionary name: %s type: %d with translation: %s\n"
- `08396860` (string): "Error updating key: %s in user dictionary name: %s type: %d with translation: %s\n"
- `08396824` (string): "Deleting key: %s from user dictionary name: %s type: %d\n"
- `083967d8` (string): "Added key: %s in user dictionary name: %s type: %d with translation: %s\n"
- `08396788` (string): "Error adding key: %s in user dictionary name: %s type: %d with translation: %s\n"

## Decompiled

```c

int UserDict__update(int param_1,int param_2)

{
  int iVar1;
  int *in_ECX;
  char *pcVar2;
  
  FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Entering UserDict::update\n");
  if (param_1 == 0) {
    FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Parameters are invalid\n");
    FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Leaving UserDict::update\n");
    return -0xc;
  }
  if (*in_ECX == 0) {
    iVar1 = FUN_0838de80(0x100,1,1);
    *in_ECX = iVar1;
    if (iVar1 == 0) {
      FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Could not build table for user dictionary\n"
                  );
      FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Could not create hashtable\n");
      FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Leaving UserDict::update\n");
      return -2;
    }
  }
  iVar1 = FUN_0838e1a0(*in_ECX,param_1);
  if (iVar1 == 0) {
    if (param_2 != 0) {
      iVar1 = FUN_083710b0(param_1,param_2);
      if (iVar1 == 0) {
        FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),
                     "Added key: %s in user dictionary name: %s type: %d with translation: %s\n",
                     param_1,*(undefined4 *)((int)in_ECX + 0x1e),in_ECX[6],param_2);
        return 0;
      }
      FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),
                   "Error adding key: %s in user dictionary name: %s type: %d with translation: %s\n"
                   ,param_1,*(undefined4 *)((int)in_ECX + 0x1e),in_ECX[6],param_2);
      return iVar1;
    }
  }
  else {
    if (param_2 != 0) {
      FUN_0838e210(*in_ECX,param_1,1,1);
      iVar1 = FUN_083710b0(param_1,param_2);
      if (iVar1 == 0) {
        pcVar2 = "Updated key: %s in user dictionary name: %s type: %d with translation: %s\n";
      }
      else {
        pcVar2 = 
        "Error updating key: %s in user dictionary name: %s type: %d with translation: %s\n";
      }
      FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),pcVar2,param_1,
                   *(undefined4 *)((int)in_ECX + 0x1e),in_ECX[6],param_2);
      FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Leaving UserDict::update\n");
      return iVar1;
    }
    FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),
                 "Deleting key: %s from user dictionary name: %s type: %d\n",param_1,
                 *(undefined4 *)((int)in_ECX + 0x1e),in_ECX[6]);
    FUN_0838e210(*in_ECX,param_1,1,1);
    FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Leaving UserDict::update\n");
  }
  return 0;
}


```
