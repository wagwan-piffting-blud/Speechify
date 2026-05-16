# `FUN_08e8adc0` @ `08e8adc0`

- **Tier reason:** seed
- **Size:** 1973 bytes / 530 instr / 65 BBs
- **Params:** 9 / `__fastcall`
- **Signature:** `undefined __fastcall FUN_08e8adc0(int param_1, int param_2, undefined4 param_3, int param_4, int param_5, int param_6, int param_7, int param_8, int param_9)`
- **Tags:** `math.heavy`, `struct.medium`, `tts.concatenative`

## Callers

- `08e8cf8c`  `?`

## Callees

- `08e94e1e`  `logError`
- `08e94e24`  `logDiag`
- `08e94fcd`  `FUN_08e94fcd`
- `08e94fdc`  `_CxxThrowException`
- `08e9504c`  `FUN_08e9504c`

## String references

- `08e9d558` (ascii?): "N?@?"
- `08e98a80` (unicode): u"WARNING MAX_CHUNK_CANDIDATES exceeded - pruning chunk type %d\n"
- `08e98a28` (unicode): u"CHUNK Pruning total %d dynamic pruning %d "
- `08e971d8` (ascii?): "???=l"
- `08e989ec` (unicode): u" prune %f scaled %f num %d\n"
- `08e9845c` (unicode): u"num_labels"
- `08e96fc0` (unicode): u"label"
- `08e98448` (unicode): u"%s%d%s%d"

## Decompiled

```c

void __fastcall
FUN_08e8adc0(int param_1,int param_2,undefined4 param_3,int param_4,int param_5,int param_6,
            int param_7,int param_8,int param_9)

{
  char cVar1;
  char cVar2;
  char cVar3;
  int iVar4;
  float fVar5;
  int iVar6;
  int iVar7;
  int iVar8;
  int iVar9;
  int *piVar10;
  ulonglong uVar11;
  int local_114;
  float local_110;
  float local_10c;
  float local_108;
  int local_104;
  int local_100;
  float local_fc;
  float local_f8;
  int local_f4;
  int local_f0;
  int local_ec;
  int local_e8;
  int local_e4 [50];
  undefined4 local_1c;
  int local_14;
  char local_10;
  float local_c;
  int local_8;
  int local_4;
  
  local_4 = DAT_08e9d558;
  local_8 = *(int *)(param_4 + 8);
  iVar7 = *(int *)(param_4 + 4);
  local_114 = 0;
  local_10c = 0.0;
  local_108 = 0.0;
  if (param_7 == 2) {
    local_10c = *(float *)(local_8 + 0x54);
    local_108 = *(float *)(local_8 + 0x58);
  }
  else if (param_7 == 4) {
    local_10c = *(float *)(local_8 + 0x5c);
    local_108 = *(float *)(local_8 + 0x60);
  }
  local_ec = param_8 * 0x1c;
  local_e8 = param_2 * 0xc;
  iVar9 = *(int *)(param_4 + 0x14);
  local_f4 = *(int *)(*(int *)(local_ec + 0x18 + *(int *)(iVar7 + 0x9c)) + 4 + local_e8);
  iVar6 = *(int *)(iVar7 + 0x604);
  cVar1 = *(char *)(iVar9 + 4 + param_9 * 0x28);
  cVar2 = *(char *)(iVar9 + 8 + param_9 * 0x28);
  cVar3 = *(char *)(iVar9 + 0x10 + param_1 * 0x28);
  local_10 = *(char *)(iVar9 + 0x14 + param_1 * 0x28);
  local_14 = CONCAT13(cVar3,(undefined3)local_14);
  local_c = *(float *)(iVar9 + 0xc + param_9 * 0x28);
  local_110 = *(float *)(iVar9 + 0xc + param_1 * 0x28);
  local_104 = iVar6;
  if (-1 < (int)local_c) {
    iVar9 = *(int *)(iVar7 + 0x600);
    fVar5 = (float)(iVar9 * 2);
    if (local_c == fVar5 || SBORROW4((int)local_c,(int)fVar5) != (int)local_c + iVar9 * -2 < 0) {
      iVar8 = *(int *)(*(int *)(iVar7 + 0x608) + (int)local_c * 4) * 0x30 + *(int *)(iVar7 + 0x610);
      if (((int)local_110 < 0) ||
         (local_110 != fVar5 &&
          SBORROW4((int)local_110,(int)fVar5) == (int)local_110 + iVar9 * -2 < 0)) {
        logError(param_3,0x1b95,L"%s%d%s%d",L"label",local_110,L"num_labels",fVar5);
        local_110 = 9.894568e-42;
                    /* WARNING: Subroutine does not return */
        _CxxThrowException(&local_110,(ThrowInfo *)&DAT_08e9b304);
      }
      iVar9 = *(int *)(*(int *)(iVar7 + 0x608) + (int)local_110 * 4) * 0x30 +
              *(int *)(iVar7 + 0x610);
      if (*(int *)(iVar8 + 8) == 0) {
        local_f0 = 0;
      }
      else {
        local_f0 = *(int *)(iVar8 + 8) + *(int *)(iVar6 + cVar1 * 4) * *(int *)(iVar8 + 4) * 4;
      }
      if (*(int *)(iVar8 + 0x14) == 0) {
        local_104 = 0;
      }
      else {
        local_104 = *(int *)(iVar8 + 0x14) +
                    *(int *)(iVar6 + cVar2 * 4) * *(int *)(iVar8 + 0x10) * 4;
      }
      if (*(int *)(iVar9 + 0x20) == 0) {
        local_1c = 0;
      }
      else {
        local_1c = *(int *)(iVar9 + 0x20) + *(int *)(iVar6 + cVar3 * 4) * *(int *)(iVar9 + 0x1c) * 4
        ;
      }
      if (*(int *)(iVar9 + 0x2c) == 0) {
        local_14 = 0;
      }
      else {
        local_14 = *(int *)(iVar9 + 0x2c) +
                   *(int *)(iVar6 + local_10 * 4) * *(int *)(iVar9 + 0x28) * 4;
      }
      local_f8 = local_10c + 10000.0;
      local_fc = 10000.0;
      local_100 = 0;
      if (0 < local_f4) {
        do {
          iVar9 = *(int *)(*(int *)(iVar7 + 0x98) + param_8 * 4) +
                  *(int *)(*(int *)(*(int *)(local_ec + 0x18 + *(int *)(iVar7 + 0x9c)) + 8 +
                                   local_e8) + local_100 * 4) * 0xc;
          iVar8 = *(int *)(iVar9 + 4);
          iVar9 = *(int *)(iVar9 + 8);
          if (((float)(uint)*(byte *)(*(int *)(iVar7 + 0x20) + 0x13 + iVar8 * 0x18) == local_c) &&
             ((float)(uint)*(byte *)(*(int *)(iVar7 + 0x20) + 0x13 + iVar9 * 0x18) == local_110)) {
            iVar4 = *(int *)(iVar7 + 0xc0);
            if (iVar4 == 0) {
              iVar4 = *(int *)(iVar7 + 0xc4);
              fVar5 = (*(float *)(local_14 +
                                 *(int *)(iVar6 + *(char *)(iVar4 + 3 + iVar9 * 4) * 4) * 4) +
                       *(float *)(local_1c +
                                 *(int *)(iVar6 + *(char *)(iVar4 + 2 + iVar9 * 4) * 4) * 4) +
                       *(float *)(local_104 +
                                 *(int *)(iVar6 + *(char *)(iVar4 + 1 + iVar8 * 4) * 4) * 4) +
                      *(float *)(local_f0 + *(int *)(iVar6 + *(char *)(iVar4 + iVar8 * 4) * 4) * 4))
                      * *(float *)(local_8 + 0x44);
            }
            else {
              fVar5 = (*(float *)(local_14 + *(char *)(iVar4 + 3 + iVar9 * 4) * 4) +
                       *(float *)(local_1c + *(char *)(iVar4 + 2 + iVar9 * 4) * 4) +
                       *(float *)(local_104 + *(char *)(iVar4 + 1 + iVar8 * 4) * 4) +
                      *(float *)(local_f0 + *(char *)(iVar4 + iVar8 * 4) * 4)) *
                      *(float *)(local_8 + 0x44);
            }
            if (fVar5 < local_f8 != (NAN(fVar5) || NAN(local_f8))) {
              if (fVar5 < local_fc != (NAN(fVar5) || NAN(local_fc))) {
                local_f8 = local_10c + fVar5;
                local_fc = fVar5;
              }
              *(float *)(param_5 + local_114 * 4) = fVar5;
              *(int *)(param_6 + local_114 * 4) = local_100;
              local_114 = local_114 + 1;
              if (9999 < local_114) {
                logDiag();
                break;
              }
            }
          }
          else {
            *(undefined4 *)(param_5 + local_114 * 4) = 0x461c4000;
            *(int *)(param_6 + local_114 * 4) = local_100;
            local_114 = local_114 + 1;
            if (9999 < local_114) {
              logDiag();
              break;
            }
          }
          local_100 = local_100 + 1;
        } while (local_100 < local_f4);
      }
      logDiag(param_3,0x1854,L"CHUNK Pruning total %d dynamic pruning %d ",local_f4,local_114);
      local_110 = 50.0 / local_10c;
      iVar9 = 0;
      piVar10 = local_e4;
      for (iVar7 = 0x32; iVar7 != 0; iVar7 = iVar7 + -1) {
        *piVar10 = 0;
        piVar10 = piVar10 + 1;
      }
      if (0 < local_114) {
        do {
          uVar11 = FUN_08e9504c();
          iVar7 = (int)uVar11;
          if (iVar7 < 0x32) {
            local_e4[iVar7] = local_e4[iVar7] + 1;
          }
          iVar9 = iVar9 + 1;
        } while (iVar9 < local_114);
      }
      iVar7 = 0;
      local_c = 2.802597e-45;
      piVar10 = local_e4 + 1;
      do {
        local_110 = (float)((int)local_c + -1);
        fVar5 = (float)(int)local_110 * 0.1;
        if (local_10c - (float)(iVar7 + piVar10[-1]) * local_108 < fVar5) break;
        iVar7 = iVar7 + piVar10[-1] + *piVar10;
        fVar5 = (float)(int)local_c * 0.1;
        if (local_10c - (float)iVar7 * local_108 < fVar5) break;
        iVar7 = iVar7 + piVar10[1];
        local_110 = (float)((int)local_c + 1);
        fVar5 = (float)(int)local_110 * 0.1;
        if (local_10c - (float)iVar7 * local_108 < fVar5) break;
        iVar7 = iVar7 + piVar10[2];
        local_110 = (float)((int)local_c + 2);
        fVar5 = (float)(int)local_110 * 0.1;
        if (local_10c - (float)iVar7 * local_108 < fVar5) break;
        iVar7 = iVar7 + piVar10[3];
        local_110 = (float)((int)local_c + 3);
        fVar5 = (float)(int)local_110 * 0.1;
        if (local_10c - (float)iVar7 * local_108 < fVar5) break;
        iVar7 = iVar7 + piVar10[4];
        local_110 = (float)((int)local_c + 4);
        fVar5 = (float)(int)local_110 * 0.1;
        if (local_10c - (float)iVar7 * local_108 < fVar5) break;
        iVar7 = iVar7 + piVar10[5];
        local_110 = (float)((int)local_c + 5);
        fVar5 = (float)(int)local_110 * 0.1;
        if (local_10c - (float)iVar7 * local_108 < fVar5) break;
        iVar7 = iVar7 + piVar10[6];
        local_110 = (float)((int)local_c + 6);
        fVar5 = (float)(int)local_110 * 0.1;
        if (local_10c - (float)iVar7 * local_108 < fVar5) break;
        iVar7 = iVar7 + piVar10[7];
        local_110 = (float)((int)local_c + 7);
        fVar5 = (float)(int)local_110 * 0.1;
        if (local_10c - (float)iVar7 * local_108 < fVar5) break;
        iVar7 = iVar7 + piVar10[8];
        local_110 = (float)((int)local_c + 8);
        fVar5 = (float)(int)local_110 * 0.1;
        if (local_10c - (float)iVar7 * local_108 < fVar5) break;
        iVar9 = (int)local_c + 8;
        piVar10 = piVar10 + 10;
        local_c = (float)((int)local_c + 10);
      } while (iVar9 < 0x32);
      iVar9 = 0;
      local_f8 = local_fc + fVar5;
      iVar7 = 0;
      if (0 < local_114) {
        do {
          iVar6 = (iVar9 + iVar7) * 4;
          *(undefined4 *)(param_5 + iVar7 * 4) = *(undefined4 *)(iVar6 + param_5);
          *(undefined4 *)(param_6 + iVar7 * 4) = *(undefined4 *)(iVar6 + param_6);
          if (local_f8 < *(float *)(param_5 + iVar7 * 4)) {
            iVar9 = iVar9 + 1;
            local_114 = local_114 + -1;
            iVar7 = iVar7 + -1;
          }
          iVar7 = iVar7 + 1;
        } while (iVar7 < local_114);
      }
      logDiag(param_3,0x1854,L" prune %f scaled %f num %d\n",(double)local_10c,(double)fVar5,
              local_114);
      FUN_08e94fcd(local_4);
      return;
    }
  }
  logError(param_3,0x1b95,L"%s%d%s%d",L"label",local_c,L"num_labels",*(int *)(iVar7 + 0x600) << 1);
  local_110 = 9.894568e-42;
                    /* WARNING: Subroutine does not return */
  _CxxThrowException(&local_110,(ThrowInfo *)&DAT_08e9b304);
}


```
