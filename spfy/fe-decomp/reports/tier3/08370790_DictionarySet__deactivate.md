# `DictionarySet::deactivate` @ `08370790`

- **Tier reason:** seed
- **Size:** 54 bytes / 17 instr / 1 BBs
- **Params:** 0 / `unknown`
- **Signature:** `undefined DictionarySet::deactivate(void)`
- **Tags:** `struct.small`, `tts.dictionary`

## Callers

- `0836baaa`  `?`

## Callees

- `0836c8f0`  `FUN_0836c8f0`
- `0836f630`  `FUN_0836f630`

## String references

- `083964bc` (string): "Entering DictionarySet::deactivate\n"
- `08396498` (string): "Leaving DictionarySet::deactivate\n"

## Decompiled

```c

undefined4 DictionarySet__deactivate(void)

{
  int in_ECX;
  
  FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Entering DictionarySet::deactivate\n");
  FUN_0836f630();
  FUN_0836c8f0(*(undefined4 *)(in_ECX + 0x8e),"Leaving DictionarySet::deactivate\n");
  return 0;
}


```
