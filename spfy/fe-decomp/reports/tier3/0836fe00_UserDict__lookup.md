# `UserDict::lookup` @ `0836fe00`

- **Tier reason:** seed
- **Size:** 261 bytes / 94 instr / 9 BBs
- **Params:** 2 / `unknown`
- **Signature:** `undefined UserDict::lookup(undefined4 param_1, undefined4 param_2)`
- **Tags:** `struct.medium`, `tts.dictionary`

## Callers

- `08370a3e`  `?`
- `08370adc`  `?`
- `08370b13`  `?`

## Callees

- `0836c8f0`  `FUN_0836c8f0`
- `0838e1a0`  `FUN_0838e1a0`

## String references

- `08396118` (string): "Entering UserDict::lookup\n"
- `08396004` (string): "User dictionary name: %s type: %d is empty\n"
- `083960fc` (string): "Leaving UserDict::lookup\n"
- `083960d8` (string): "Translation for key: %s not found.\n"
- `08396090` (string): "Found translation: %s for key: %s in user dictionary name: %s type: %d\n"
- `08395f88` (string): "Parameters are invalid\n"

## Decompiled

```c

undefined4 UserDict__lookup(char *param_1,int *param_2)

{
  int iVar1;
  int *in_ECX;
  
  FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Entering UserDict::lookup\n");
  if (((param_1 == (char *)0x0) || (*param_1 == '\0')) || (param_2 == (int *)0x0)) {
    FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Parameters are invalid\n");
    FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Leaving UserDict::lookup\n");
    return 0xfffffff4;
  }
  if (*in_ECX == 0) {
    FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"User dictionary name: %s type: %d is empty\n",
                 *(undefined4 *)((int)in_ECX + 0x1e),in_ECX[6]);
    FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Leaving UserDict::lookup\n");
    return 0xffffffef;
  }
  iVar1 = FUN_0838e1a0(*in_ECX,param_1);
  *param_2 = iVar1;
  if (iVar1 == 0) {
    FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Translation for key: %s not found.\n",param_1)
    ;
    FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Leaving UserDict::lookup\n");
    return 0xffffffef;
  }
  FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),
               "Found translation: %s for key: %s in user dictionary name: %s type: %d\n",iVar1,
               param_1,*(undefined4 *)((int)in_ECX + 0x1e),in_ECX[6]);
  FUN_0836c8f0(*(undefined4 *)((int)in_ECX + 0x22),"Leaving UserDict::lookup\n");
  return 0;
}


```
