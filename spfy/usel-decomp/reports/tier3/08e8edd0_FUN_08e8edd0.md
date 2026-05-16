# `FUN_08e8edd0` @ `08e8edd0`

- **Tier reason:** seed
- **Size:** 1077 bytes / 342 instr / 61 BBs
- **Params:** 2 / `__thiscall`
- **Signature:** `undefined4 __thiscall FUN_08e8edd0(void * this, int param_1)`
- **Tags:** `math.heavy`, `struct.medium`, `tts.concatenative`, `tts.phonetic_features`

## Callers

- `08e81d5d`  `?`

## Callees

- `08e8b580`  `FUN_08e8b580`
- `08e8ed20`  `FUN_08e8ed20`
- `08e94e24`  `logDiag`

## String references

- `08e99948` (unicode): u"UNIT SELECTION: USelGraph::Viterbi"
- `08e99880` (unicode): u"Used bad score for diphone emulation: leftPhone %S leftEndUnit %d, rightPhone %S rightStartUnit %d"
- `08e98b00` (ascii?): "R??}"

## Decompiled

```c

undefined4 __thiscall FUN_08e8edd0(void *this,int param_1)

{
  char cVar1;
  char cVar2;
  int iVar3;
  int *piVar4;
  float fVar5;
  int *piVar6;
  uint uVar7;
  int iVar8;
  uint uVar9;
  int iVar10;
  int iVar11;
  int *piVar12;
  float10 fVar13;
  float10 fVar14;
  float local_34;
  float local_30;
  float local_2c;
  float local_28;
  int local_24;
  int local_18;
  int local_14;
  int local_10;
  
  logDiag(*(undefined4 *)((int)this + 0x30),0xfa1,L"UNIT SELECTION: USelGraph::Viterbi");
  iVar3 = **(int **)((int)this + 0x18);
  iVar11 = 0;
  if (0 < *(int *)(iVar3 + 0x2c)) {
    do {
      iVar10 = *(int *)(*(int *)(iVar3 + 0x34) + iVar11 * 4);
      *(undefined4 *)(iVar10 + 0x20) = *(undefined4 *)(iVar10 + 0x2c);
      *(undefined4 *)(iVar10 + 0x24) = 0;
      *(undefined4 *)(iVar10 + 0x18) = *(undefined4 *)(iVar10 + 0x14);
      *(undefined4 *)(iVar10 + 0x1c) = 1;
      *(undefined4 *)(iVar10 + 0x7c) = 0;
      *(undefined4 *)(iVar10 + 0x80) = 0;
      *(undefined4 *)(iVar10 + 0x30) = 0;
      *(undefined4 *)(iVar10 + 0x34) = 0;
      if (*(char *)(*(int *)((int)this + 4) + 0xc) != '\0') {
        *(undefined4 *)(iVar10 + 0x54) = *(undefined4 *)(iVar10 + 0x40);
        *(undefined4 *)(iVar10 + 0x58) = *(undefined4 *)(iVar10 + 0x44);
        *(undefined4 *)(iVar10 + 0x5c) = *(undefined4 *)(iVar10 + 0x4c);
        *(undefined4 *)(iVar10 + 0x50) = *(undefined4 *)(iVar10 + 0x3c);
        *(undefined4 *)(iVar10 + 0x60) = 0;
        *(undefined4 *)(iVar10 + 100) = 0;
      }
      iVar11 = iVar11 + 1;
    } while (iVar11 < *(int *)(iVar3 + 0x2c));
  }
  FUN_08e8b580(*(int *)(iVar3 + 0x2c));
                    /* WARNING: Load size is inaccurate */
  piVar4 = *(int **)(*this + 200);
  fVar5 = *(float *)(*(int *)((int)this + 4) + 0x28);
  local_10 = 1;
  if (1 < *(int *)((int)this + 0xc)) {
    do {
      iVar3 = *(int *)(*(int *)((int)this + 0x18) + local_10 * 4);
      if (0 < *(int *)(iVar3 + 0x2c)) {
        local_14 = 0;
        if (0 < *(int *)(iVar3 + 0x2c)) {
          do {
            iVar11 = *(int *)(*(int *)(iVar3 + 0x34) + local_14 * 4);
                    /* WARNING: Load size is inaccurate */
            cVar2 = *(char *)(*(int *)(*this + 0x20) + 0x16 + *(int *)(iVar11 + 0xc) * 0x18);
            local_2c = 3e+37;
            local_18 = 0;
            if (0 < *(int *)(iVar3 + 0x38)) {
              piVar12 = (int *)(iVar3 + 0x3c);
              do {
                if (param_1 != 0) {
                  iVar10 = *(int *)(param_1 + 0x48) +
                           *(int *)(*piVar12 + 0x2c) * *(int *)(iVar3 + 0x2c);
                  *(int *)(param_1 + 0x48) = iVar10;
                  *(int *)(param_1 + 0x4c) = iVar10;
                }
                iVar10 = *piVar12;
                local_24 = 0;
                if (0 < *(int *)(iVar10 + 0x2c)) {
                  do {
                    piVar6 = *(int **)(*(int *)(iVar10 + 0x34) + local_24 * 4);
                    local_34 = (float)piVar6[8];
                    if (local_2c < local_34) break;
                    uVar7 = piVar6[4];
                    uVar9 = *(int *)(iVar11 + 0xc) - 1;
                    if ((uVar7 == uVar9) && (cVar2 != '\0')) {
                      local_30 = 0.0;
                      local_28 = 0.0;
                    }
                    else {
                      cVar1 = *(char *)(*(int *)((int)this + 4) + 0xc4);
                      uVar9 = CONCAT31((int3)(uVar9 >> 8),cVar1);
                      if (cVar1 == '\0') {
LAB_08e8efed:
                        fVar14 = FUN_08e8ed20(uVar9,piVar6);
                        local_30 = (float)fVar14;
                        fVar14 = fVar14 + (float10)local_34;
                      }
                      else {
                        uVar9 = uVar7 & 0x80000001;
                        if ((int)uVar9 < 0) {
                          uVar9 = (uVar9 - 1 | 0xfffffffe) + 1;
                        }
                        if (uVar9 != 1) goto LAB_08e8efed;
                        iVar10 = *(int *)(iVar11 + 0x28);
                        if (*(uint *)(iVar10 + 0x1c) < 0x10) {
                          iVar10 = iVar10 + 8;
                        }
                        else {
                          iVar10 = *(int *)(iVar10 + 8);
                        }
                        iVar8 = piVar6[10];
                        if (*(uint *)(iVar8 + 0x1c) < 0x10) {
                          iVar8 = iVar8 + 8;
                        }
                        else {
                          iVar8 = *(int *)(iVar8 + 8);
                        }
                        logDiag(*(undefined4 *)((int)this + 0x30),0xfa5,
                                L"Used bad score for diphone emulation: leftPhone %S leftEndUnit %d, rightPhone %S rightStartUnit %d"
                                ,iVar8,uVar7,iVar10,*(int *)(iVar11 + 0xc));
                        fVar14 = (float10)local_34 + (float10)10000.0;
                        local_30 = 10000.0;
                      }
                      fVar13 = (float10)0.0;
                      if (((0x14 < *(int *)(iVar11 + 0x6c)) && (piVar6[0x20] < 0xf)) &&
                         (0x14 < piVar6[0x1f])) {
                        iVar10 = (*(int *)(iVar11 + 0x6c) - piVar4[1]) - piVar6[0x1f];
                        if (iVar10 < 0) {
                          iVar10 = 0;
                        }
                        else if (*piVar4 <= iVar10) {
                          iVar10 = *piVar4 + -1;
                        }
                        fVar13 = (float10)fVar5 * (float10)*(float *)(piVar4[2] + iVar10 * 4);
                      }
                      local_28 = (float)fVar13;
                      local_34 = (float)(fVar13 + fVar14);
                    }
                    if (local_34 < local_2c != (NAN(local_34) || NAN(local_2c))) {
                      local_2c = local_34;
                      *(float *)(iVar11 + 0x20) = local_34;
                      *(float *)(iVar11 + 0x34) = local_28;
                      *(float *)(iVar11 + 0x30) = local_30;
                      fVar14 = FUN_08e8ed20(local_30,piVar6);
                      *(float *)(iVar11 + 0x38) = (float)fVar14;
                      if (*(char *)(*(int *)((int)this + 4) + 0xc) != '\0') {
                        *(float *)(iVar11 + 0x50) = (float)piVar6[0x14] + *(float *)(iVar11 + 0x3c);
                        *(float *)(iVar11 + 0x54) = (float)piVar6[0x15] + *(float *)(iVar11 + 0x40);
                        *(float *)(iVar11 + 0x58) = (float)piVar6[0x16] + *(float *)(iVar11 + 0x44);
                        *(float *)(iVar11 + 0x5c) = (float)piVar6[0x17] + *(float *)(iVar11 + 0x4c);
                        *(float *)(iVar11 + 0x60) = local_30 + (float)piVar6[0x18];
                        *(float *)(iVar11 + 100) = local_28 + (float)piVar6[0x19];
                      }
                      *(int *)(iVar11 + 0x18) = piVar6[6];
                      *(int *)(iVar11 + 0x1c) = piVar6[7];
                      *(int **)(iVar11 + 0x24) = piVar6;
                      if (*(int *)(iVar11 + 0x68) < 0x15) {
                        *(int *)(iVar11 + 0x7c) = piVar6[0x1f];
                        if (*(int *)(*(int *)((int)this + 4) + 0x94) == 0) {
                          iVar10 = piVar6[0x20] + 1;
                          goto LAB_08e8f127;
                        }
                        *(undefined4 *)(iVar11 + 0x80) = 100;
                      }
                      else {
                        *(int *)(iVar11 + 0x7c) = *(int *)(iVar11 + 0x68);
                        if (*(int *)(*(int *)((int)this + 4) + 0x94) == 0) {
                          iVar10 = *(int *)(iVar11 + 0x78);
LAB_08e8f127:
                          *(int *)(iVar11 + 0x80) = iVar10;
                        }
                        else {
                          *(undefined4 *)(iVar11 + 0x80) = 0;
                        }
                      }
                    }
                    iVar10 = *piVar12;
                    local_24 = local_24 + 1;
                  } while (local_24 < *(int *)(iVar10 + 0x2c));
                }
                local_18 = local_18 + 1;
                piVar12 = piVar12 + 1;
              } while (local_18 < *(int *)(iVar3 + 0x38));
            }
            *(int *)(iVar11 + 0x18) = *(int *)(iVar11 + 0x18) + *(int *)(iVar11 + 0x14);
            *(float *)(iVar11 + 0x20) = *(float *)(iVar11 + 0x2c) + *(float *)(iVar11 + 0x20);
            *(int *)(iVar11 + 0x1c) = *(int *)(iVar11 + 0x1c) + 1;
            local_14 = local_14 + 1;
          } while (local_14 < *(int *)(iVar3 + 0x2c));
        }
        *(int *)((int)this + 0x10) = local_10;
        FUN_08e8b580(*(int *)(iVar3 + 0x2c));
      }
      local_10 = local_10 + 1;
    } while (local_10 < *(int *)((int)this + 0xc));
  }
  fVar5 = 3e+37;
  iVar3 = *(int *)(*(int *)((int)this + 0x18) + *(int *)((int)this + 0x10) * 4);
  iVar11 = *(int *)(iVar3 + 0x2c);
  iVar10 = 0;
  if (0 < iVar11) {
    do {
      iVar11 = *(int *)(*(int *)(iVar3 + 0x34) + iVar10 * 4);
      if (*(float *)(iVar11 + 0x20) < fVar5) {
        fVar5 = *(float *)(iVar11 + 0x20);
        *(int *)((int)this + 0x2c) = iVar10;
      }
      iVar11 = *(int *)(iVar3 + 0x2c);
      iVar10 = iVar10 + 1;
    } while (iVar10 < iVar11);
  }
  return CONCAT31((int3)((uint)iVar11 >> 8),1);
}


```
