'use strict';
/*
 * inner_scorer_hook.js -- per-slot target SP (position) feature capture.
 *
 * Closes the M3.4e blocker: spfy_viterbi_replay's S-cost is now wired to
 * engine-truth context (from prsl_slot's slice.ctx), but SP cost still
 * uses a "chosen-UID-as-target" proxy because we have no engine-side
 * capture for the FE-emitted halfphone target struct's SP fields. This
 * hook fixes that by reading the 5 per-slot SP target indices straight
 * from the USelNetwork workspace where InnerScorer reads them.
 *
 * --- Reverse engineering basis (Ghidra MCP) ---
 *
 * Function:  FUN_08e88de0  @  0x08E88DE0  in SWIttsUSel.dll
 *            "USelNetworkSlice::all_half_phone_costs" (the InnerScorer
 *            from DLL_ANALYSIS.md). Called once per target slot from
 *            USelNetwork::AddUnits' inner loop, immediately after
 *            USelNetwork::AddUnit (FUN_08e91dc0 -- our prsl_slot site).
 *
 * Calling convention: __thiscall
 *   ecx        = this (slice scratch struct, same one prsl_slot used)
 *   [esp+ 4]   = param_1 (USelNetwork workspace ptr)
 *   [esp+ 8]   = param_2 (slot index)
 *   [esp+12]   = param_3 (some flag, ignored)
 *
 * The decompile shows the 5 SP target indices per slot are read as:
 *
 *   pvVar6 = *(void  **)(*(int *)(param_1 + 0x2c) + slot * 4);  // sp[0]
 *   iVar16 = *(int   *)(*(int *)(param_1 + 0x28) + slot * 4);   // sp[1]
 *   iVar7  = *(int   *)(*(int *)(param_1 + 0x34) + slot * 4);   // sp[2]
 *   iVar8  = *(int   *)(*(int *)(param_1 + 0x38) + slot * 4);   // sp[3]
 *   iVar9  = *(int   *)(*(int *)(param_1 + 0x3c) + slot * 4);   // sp[4]
 *
 * Each is then used as the row index into one of the 5 proscost matrices
 * (voice+0xd8/0x268/0x3ac/0x470/0x534 with strides 0x28/0x24/0x1c each).
 * The order in the cost formula is (sp[0], sp[1], sp[2], sp[3], sp[4]) =
 * (matrix-0-row, matrix-1-row, matrix-2-row, matrix-3-row, matrix-4-row).
 *
 * Cost weights are read from `slice+0x24` -> some weight struct, at
 * offsets:
 *   +0x10..+0x20 : 5 SP weights (one per matrix, in cost-formula order)
 *   +0x24        : F0 weight
 *   +0x34        : D weight
 *   +0x38        : flag bonus weight (multiplied by const _DAT_08e98580)
 *   +0x44        : CCOS / S-cost weight
 *   +0x4c        : another weight (loaded as fVar4 at function entry)
 *   +0x80        : MISSING_F0_COST (used when cand byte +0xf == 0)
 * We capture these per call so we can verify against VCF and use real
 * values in the C-side replay scorer.
 *
 * --- Safety ---
 *
 * Function-entry only. All reads guarded with Process.findRangeByAddress
 * (v2 hardening pattern). Output is the same batched-send format as
 * prsl_slot. Capped to TOTAL_CAP per session.
 */

var ADDR_INNER_SCORER = ptr('0x08E88DE0');

var BATCH_N    = 64;
var TOTAL_CAP  = 50000;
var stats      = { calls: 0, sent: 0, dropped: 0, ptr_invalid: 0,
                   read_errors: 0 };
var batch      = [];
var probedLayout = false;     /* dump in-mem unit record layout once */
var debugFlagSet = false;     /* weight+0xc flipped to 1 once per session
                                 (D-17 path A: enables per-component
                                  storage in cand_buf+0x08/0x0c/0x10/0x14
                                  inside FUN_08e88de0 — pure debug write
                                  on a per-utterance scratch field, no
                                  effect on engine correctness). */

/* Probe a handful of well-known unit_ids whose disk fields we already
 * know from spfy_dump_voice. The hex dump in the trace lets us decode
 * the in-memory unit record byte layout offline (see RESUME.md). */
var PROBE_UIDS = [0, 87072, 139585, 27135, 124625, 169578];

/* Read the global float constant _DAT_08e98580, which scales the "flag"
 * term contribution in the inner scorer:
 *   fVar14 += cand.byte+0x17 * weight_0x38 * _DAT_08e98580
 * The constant is at a fixed module address; resolve once per session.
 * NOTE: 0x08e98580 is the memory address; we read it as f32. */
var ADDR_FLAG_CONST = ptr('0x08e98580');
var addrConstSent = false;

/* Field offsets in the USelNetwork workspace, in cost-formula order
 * (matrix 0 row, matrix 1 row, ..., matrix 4 row). */
var SP_FIELD_OFFSETS = [0x2c, 0x28, 0x34, 0x38, 0x3c];

function rangeOK(addr) {
    try {
        var r = Process.findRangeByAddress(addr);
        return r !== null && r.protection.indexOf('r') !== -1;
    } catch(e) { return false; }
}

function safeReadU32(addr) {
    if (!rangeOK(addr)) { stats.ptr_invalid++; return null; }
    try { return addr.readU32(); }
    catch(e) { stats.read_errors++; return null; }
}

function safeReadF32(addr) {
    if (!rangeOK(addr)) { stats.ptr_invalid++; return null; }
    try { return addr.readFloat(); }
    catch(e) { stats.read_errors++; return null; }
}

function flush() {
    if (batch.length === 0) return;
    send({ type: 'inner_scorer_batch', samples: batch });
    stats.sent += batch.length;
    batch = [];
}

Interceptor.attach(ADDR_INNER_SCORER, {
    onEnter: function () {
        stats.calls++;
        if (stats.calls > TOTAL_CAP) { stats.dropped++; this.skip = 1; return; }

        var ctx = this.context;
        var esp = ctx.esp;
        var thisPtrVal = ctx.ecx.toUInt32();
        var arg1Val    = safeReadU32(esp.add(4));    /* USelNetwork ptr */
        var slot       = safeReadU32(esp.add(8));    /* slot index      */

        /* Save the slice ptr for onLeave (to read cand totals AFTER the
         * scorer has populated them at cand_buf+0x04+i*0x18). */
        this.slice_ptr = thisPtrVal;

        var snap = {
            n        : stats.calls,
            slot     : (slot !== null) ? (slot >>> 0) : null,
            this_ptr : thisPtrVal >>> 0,
            net_ptr  : (arg1Val !== null) ? (arg1Val >>> 0) : null,
            sp_target: [null, null, null, null, null]
        };

        if (arg1Val !== null && slot !== null && arg1Val >= 0x100000) {
            var net = ptr(arg1Val);
            for (var i = 0; i < SP_FIELD_OFFSETS.length; ++i) {
                /* arr_ptr = *(u32*)(net + offset) */
                var arrPtrVal = safeReadU32(net.add(SP_FIELD_OFFSETS[i]));
                if (arrPtrVal === null || arrPtrVal < 0x100000) continue;
                /* val = *(u32*)(arr + slot * 4) */
                var v = safeReadU32(ptr(arrPtrVal).add(slot * 4));
                if (v !== null) snap.sp_target[i] = v >>> 0;
            }
        }

        /* Read flag term scale constant once. */
        if (!addrConstSent) {
            var k = safeReadF32(ADDR_FLAG_CONST);
            if (k !== null) {
                send({ type: 'flag_const', value: k,
                       addr: ADDR_FLAG_CONST.toString() });
            }
            /* Also capture join-cost-on-miss globals + slice fields used by
             * Viterbi (FUN_08e8b620). Decompile shows:
             *   miss_base   = _DAT_08e9852c   (global f32)
             *   miss_offset = *(float *)(slice+0x84)   ("join_offset")
             *   gate_weight = *(float *)(slice+0x28)   (ccos gate weight)
             *   _DAT_08e98b00 used as initial best (1e38 sentinel) */
            var miss_base   = safeReadF32(ptr('0x08e9852c'));
            var inf_sent    = safeReadF32(ptr('0x08e98b00'));
            var thisP1      = ptr(thisPtrVal);
            var miss_offset = safeReadF32(thisP1.add(0x84));
            var gate_weight = safeReadF32(thisP1.add(0x28));
            /* Also dump nearby globals - the silence-boundary constant
             * we're missing might be one of these. */
            var bd_consts = {};
            for (var dd = 0; dd < 16; ++dd) {
                var addr = 0x08e98520 + dd * 4;
                bd_consts['DAT_' + addr.toString(16)] =
                    safeReadF32(ptr(addr));
            }
            /* voice+0x614 is param_3 to InnerScorer (the boundary-mode
             * filter ptr per the decompile branch). voice+0x5fc is the
             * per-unit voicing-flag table (4-byte u32 per unit) the F0
             * block reads to decide whether to walk f0tr. */
            var voicePtrV = safeReadU32(thisP1.add(0x20));
            var voice_614 = null;
            var voice_5fc_per_uid = {};
            if (voicePtrV !== null && voicePtrV >= 0x100000) {
                voice_614 = safeReadU32(ptr(voicePtrV).add(0x614));
                var v5fc = safeReadU32(ptr(voicePtrV).add(0x5fc));
                if (v5fc !== null && v5fc >= 0x100000) {
                    /* Indexed by halfphone class (slice.ctx[2]), 0..93. */
                    var v5fc_arr = [];
                    for (var hc = 0; hc < 94; ++hc) {
                        var rv = safeReadU32(ptr(v5fc).add(hc * 4));
                        v5fc_arr.push(rv);
                    }
                    voice_5fc_per_uid['by_hp_class'] = v5fc_arr;
                }
                /* Also read weight struct field +0x8c (the second part
                 * of the f0tr-walk gate condition). */
                var ws = safeReadU32(thisP1.add(0x24));
                if (ws !== null && ws >= 0x100000) {
                    voice_5fc_per_uid['weight_8c'] = safeReadU32(ptr(ws).add(0x8c));
                }
            }
            send({ type: 'join_consts',
                   miss_base   : miss_base,
                   miss_offset : miss_offset,
                   gate_weight : gate_weight,
                   inf_sent    : inf_sent,
                   nearby      : bd_consts,
                   voice_614   : voice_614,
                   voice_5fc_per_uid : voice_5fc_per_uid });
            addrConstSent = true;
        }

        /* Probe voice+0x600 hp_class remap table. The engine calls
         * FUN_08e87b50(voice+0x600, ..., slice.ctx[2]) which returns
         * `*(u32*)(*(u32*)(voice+0x608) + slice.ctx[2]*4)` -- a
         * remap from interleaved hp_class (0..93) to whatever the ccos
         * forest uses for the CURRENT slot's hp_class.
         *
         * The S-cost target context lookup uses a DIFFERENT table at
         * `*(u32*)(voice+0x604)`, indexed by slice.ctx[k] (halfphone
         * class). Capture both. */
        if (!probedLayout) {
            var thisTmpForRemap = ptr(thisPtrVal);
            var voiceForRemap = safeReadU32(thisTmpForRemap.add(0x20));
            if (voiceForRemap !== null && voiceForRemap >= 0x100000) {
                var n_labels = safeReadU32(ptr(voiceForRemap).add(0x600));
                var remapTblVal = safeReadU32(ptr(voiceForRemap).add(0x600 + 8));
                if (n_labels !== null && remapTblVal !== null &&
                    n_labels > 0 && n_labels < 256 &&
                    remapTblVal >= 0x100000) {
                    var remap = [];
                    var n_hp = (n_labels * 2) >>> 0;
                    var ok = true;
                    for (var rr = 0; rr < n_hp; ++rr) {
                        var rv = safeReadU32(ptr(remapTblVal).add(rr * 4));
                        if (rv === null) { ok = false; break; }
                        remap.push(rv >>> 0);
                    }
                    if (ok) {
                        send({ type: 'hp_class_remap',
                               n_labels: n_labels >>> 0,
                               remap: remap });
                    }
                }
                /* Probe engine's hash chunk for known (prev, curr) pairs.
                 * Engine reads voice+0x80 (cells base) and voice+0x84
                 * (rows ptr). For each pair, compute index = rows[curr]
                 * + prev, then read cells_A[idx] and cells_B[idx]. */
                var hashCellsBase = safeReadU32(ptr(voiceForRemap).add(0x80));
                var hashRowsBase  = safeReadU32(ptr(voiceForRemap).add(0x84));
                if (hashCellsBase !== null && hashRowsBase !== null &&
                    hashCellsBase >= 0x100000 && hashRowsBase >= 0x100000) {
                    var pairs = [
                        [0,      87072],
                        [87072,  158634],
                        [158634, 158635],
                        [158635, 129379],
                        [129379, 129380],
                        [129380, 46793],
                    ];
                    var hash_probes = [];
                    for (var pi = 0; pi < pairs.length; ++pi) {
                        var prev = pairs[pi][0];
                        var curr = pairs[pi][1];
                        var rowOff = safeReadU32(ptr(hashRowsBase).add(curr * 4));
                        if (rowOff === null) continue;
                        var idx = rowOff + prev;
                        var cellA = safeReadU32(ptr(hashCellsBase).add(idx * 8));
                        var cellB = safeReadF32(ptr(hashCellsBase).add(idx * 8 + 4));
                        hash_probes.push({prev, curr, idx, cellA, cellB});
                    }
                    send({ type: 'hash_cell_probe', probes: hash_probes });
                }

                /* Probe ccos forest metadata + slot matrices for hp_class
                 * 32 (silence first half in Tom's labels). The Viterbi
                 * code reads `*(int *)(voice+0x610)` -> metadata base.
                 * Each hp_class entry is 0x30 bytes: 4 slot blocks of
                 * 12 bytes each (unknown +4, stride +4, ptr +4). */
                var ccosMetaBase = safeReadU32(ptr(voiceForRemap).add(0x610));
                if (ccosMetaBase !== null && ccosMetaBase >= 0x100000) {
                    /* Dump metadata for hp_class 32 and 33. */
                    for (var hi = 32; hi <= 33; ++hi) {
                        var meta = ptr(ccosMetaBase).add(hi * 0x30);
                        var slot_info = [];
                        for (var sl = 0; sl < 4; ++sl) {
                            var slot_off = sl * 12;
                            slot_info.push({
                                slot: sl,
                                f0:  safeReadU32(meta.add(slot_off + 0)),
                                stride: safeReadU32(meta.add(slot_off + 4)),
                                ptr: safeReadU32(meta.add(slot_off + 8)),
                            });
                        }
                        send({ type: 'ccos_meta_probe', hp_class: hi,
                               slots: slot_info });
                    }
                    /* Probe ccos cells unit 57061 at slot 18 of text_010
                     * reads. hp_class via hp_remap[62] = 31. */
                    var meta31 = ptr(ccosMetaBase).add(31 * 0x30);
                    var probe31 = {};
                    var t31 = [27, 34, 16, 27];   // s_ctx_remap[54,68,32,54]
                    var c31 = [21, 35, 0, 25];    // unit 57061 phone_ctx
                    for (var sl = 0; sl < 4; ++sl) {
                        var slot_off = sl * 12;
                        var sl_stride = safeReadU32(meta31.add(slot_off + 4));
                        var sl_ptr    = safeReadU32(meta31.add(slot_off + 8));
                        if (sl_ptr === null || sl_ptr < 0x100000 ||
                            sl_stride === null) continue;
                        var off = (t31[sl] * sl_stride + c31[sl]) * 4;
                        var v = safeReadF32(ptr(sl_ptr).add(off));
                        probe31['slot_' + sl + '_t' + t31[sl] +
                                 '_c' + c31[sl]] = v;
                    }
                    send({ type: 'ccos_cell_probe', hp_class: 31,
                           cells: probe31 });

                    /* Also dump the cells unit 0 reads at slot 0 for all 4
                     * S-cost slots. We compare these to our cost_s reads to
                     * isolate any layout difference. */
                    var meta32 = ptr(ccosMetaBase).add(32 * 0x30);
                    var probe_cells = {};
                    /* Unit 0 phone_ctx = [255, 255, 34, 2]; target labels
                     * (after s_ctx_remap of slice.ctx[0,1,3,4]=[64,64,38,24]
                     * = [32, 32, 19, 12]). */
                    var tgt_labels  = [32, 32, 19, 12];
                    var cand_bytes  = [255, 255, 34, 2];
                    for (var sl = 0; sl < 4; ++sl) {
                        var slot_off = sl * 12;
                        var sl_stride = safeReadU32(meta32.add(slot_off + 4));
                        var sl_ptr    = safeReadU32(meta32.add(slot_off + 8));
                        if (sl_ptr === null || sl_ptr < 0x100000 ||
                            sl_stride === null) continue;
                        var off = (tgt_labels[sl] * sl_stride + cand_bytes[sl]) * 4;
                        var v = safeReadF32(ptr(sl_ptr).add(off));
                        probe_cells['slot_' + sl + '_t' + tgt_labels[sl] +
                                    '_c' + cand_bytes[sl]] = v;
                    }
                    send({ type: 'ccos_cell_probe', hp_class: 32,
                           cells: probe_cells });
                }

                /* Probe proscost matrices at fixed voice offsets. */
                var voiceFP = ptr(voiceForRemap);
                var MAT_DESC = [
                  { name: 'sylInPhrase',  base: 0xd8,  stride: 0x28, rows: 10, cols: 10 },
                  { name: 'sylType',      base: 0x268, stride: 0x24, rows: 9,  cols: 9 },
                  { name: 'wordInPhrase', base: 0x3ac, stride: 0x1c, rows: 7,  cols: 7 },
                  { name: 'sylInWord',    base: 0x470, stride: 0x1c, rows: 7,  cols: 7 },
                ];
                for (var mi = 0; mi < MAT_DESC.length; ++mi) {
                    var md = MAT_DESC[mi];
                    var dat = [];
                    for (var rr = 0; rr < md.rows; ++rr) {
                        var row_data = [];
                        for (var cc = 0; cc < md.cols; ++cc) {
                            var off = md.base + rr * md.stride + cc * 4;
                            var v = safeReadF32(voiceFP.add(off));
                            row_data.push(v);
                        }
                        dat.push(row_data);
                    }
                    send({ type: 'proscost_matrix_probe', kind: md.name,
                           base: md.base, stride: md.stride,
                           n_rows: md.rows, n_cols: md.cols, data: dat });
                }

                /* Also probe voice+0x604: the S-cost target context
                 * lookup table (different from voice+0x608 hp remap). */
                var sCtxTblVal = safeReadU32(ptr(voiceForRemap).add(0x604));
                if (sCtxTblVal !== null && sCtxTblVal >= 0x100000 &&
                    n_labels !== null && n_labels > 0) {
                    var sctx = [];
                    var n_hp2 = (n_labels * 2) >>> 0;
                    var ok3 = true;
                    for (var rr2 = 0; rr2 < n_hp2; ++rr2) {
                        var rv2 = safeReadU32(ptr(sCtxTblVal).add(rr2 * 4));
                        if (rv2 === null) { ok3 = false; break; }
                        sctx.push(rv2 >>> 0);
                    }
                    if (ok3) {
                        send({ type: 's_ctx_remap',
                               n_labels: n_labels >>> 0,
                               remap: sctx });
                    }
                }
            }
        }

        /* Probe in-memory unit record byte layout once per run. Read
         * voice ptr from slice+0x20, then unit_table = voice+0x20, then
         * dump the first 24 bytes of each well-known UID. Done at most
         * once per run (probedLayout flag). */
        if (!probedLayout) {
            var thisPtrTmp = ptr(thisPtrVal);
            var voicePtrVal = safeReadU32(thisPtrTmp.add(0x20));
            if (voicePtrVal !== null && voicePtrVal >= 0x100000) {
                var unitTblBaseVal = safeReadU32(ptr(voicePtrVal).add(0x20));
                if (unitTblBaseVal !== null && unitTblBaseVal >= 0x100000) {
                    var probes = [];
                    for (var u = 0; u < PROBE_UIDS.length; ++u) {
                        var uid = PROBE_UIDS[u];
                        var rec = ptr(unitTblBaseVal).add(uid * 0x18);
                        if (!rangeOK(rec)) continue;
                        var bytes = [];
                        var ok = true;
                        for (var b = 0; b < 24; ++b) {
                            try {
                                bytes.push(rec.add(b).readU8());
                            } catch (e) { ok = false; break; }
                        }
                        if (ok) probes.push({ uid: uid, bytes: bytes });
                    }
                    if (probes.length > 0) {
                        send({ type: 'unit_layout_probe',
                               unit_table_base: unitTblBaseVal >>> 0,
                               probes: probes });
                        /* Engine S-cost reads cand context bytes from
                         * voice+0xc0 + uid*4 (Tom path; voice+0xc4 +
                         * uid*4 is fallback when 0xc0 is null). The data
                         * is 4 bytes per unit, may differ from disk
                         * phone_ctx. */
                        var voicePtrLocal = ptr(voicePtrVal);
                        var ctxC0 = safeReadU32(voicePtrLocal.add(0xc0));
                        var ctxC4 = safeReadU32(voicePtrLocal.add(0xc4));
                        send({ type: 'voice_field_probe',
                               voice_plus_0xc0: ctxC0,
                               voice_plus_0xc4: ctxC4 });
                        var ctxTblVal = (ctxC0 !== null && ctxC0 >= 0x100000)
                                        ? ctxC0 : ctxC4;
                        if (ctxTblVal !== null && ctxTblVal >= 0x100000) {
                            var ctxProbes = [];
                            for (var u = 0; u < PROBE_UIDS.length; ++u) {
                                var uid = PROBE_UIDS[u];
                                var rec = ptr(ctxTblVal).add(uid * 4);
                                if (!rangeOK(rec)) continue;
                                var bs = [];
                                var ok2 = true;
                                for (var b = 0; b < 4; ++b) {
                                    try { bs.push(rec.add(b).readU8()); }
                                    catch (e) { ok2 = false; break; }
                                }
                                if (ok2) ctxProbes.push({ uid: uid, ctx: bs });
                            }
                            if (ctxProbes.length > 0) {
                                send({ type: 'cand_ctx_probe',
                                       table_base: ctxTblVal >>> 0,
                                       probes: ctxProbes });
                            }
                        }
                        probedLayout = true;
                    }
                }
            }
        }

        /* Probe param_1 (USelNet) +0x30 array for current slot, and
         * weight+0x98 byte (emphasis flags). Per InnerScorer decompile:
         *   if (weight+0x98 != 0 && USelNet+0x30[slot] > 0):
         *     emphasis adjustment to durt and f0tr means. */
        if (arg1Val !== null && slot !== null && arg1Val >= 0x100000) {
            var net2 = ptr(arg1Val);
            var p1_30 = safeReadU32(net2.add(0x30));
            var emph_idx = null;
            if (p1_30 !== null && p1_30 >= 0x100000) {
                emph_idx = safeReadU32(ptr(p1_30).add(slot * 4));
            }
            var ws = safeReadU32(ptr(thisPtrVal).add(0x24));
            var emph_flag = null;
            var emph_8c = null;
            var dat_857c = safeReadF32(ptr('0x08e9857c'));
            if (ws !== null && ws >= 0x100000) {
                try { emph_flag = ptr(ws).add(0x98).readU8(); }
                catch (e) {}
                try { emph_8c = safeReadU32(ptr(ws).add(0x8c)); }
                catch (e) {}
            }
            snap.emph = { idx: emph_idx, flag: emph_flag,
                          weight_8c: emph_8c, dat_857c: dat_857c };
        }

        /* Read the weight struct at slice+0x24 -> ptr -> { ... }. Capture
         * once per call (values are constant per-utterance for any given
         * voice; we just want at least one valid sample). */
        var thisPtr = ptr(thisPtrVal);
        var wptrVal = safeReadU32(thisPtr.add(0x24));
        if (wptrVal !== null && wptrVal >= 0x100000) {
            var wp = ptr(wptrVal);
            /* D-17 path A: enable per-component storage in cand_buf.
             * The engine guards the 4 storage writes with
             *   if (*(char*)(weight+0xc) != '\0') { cand_buf[+0x08/+0x0c/+0x10/+0x14] = comp; }
             * Setting the flag once per session is sufficient (the byte
             * is read on every cand, written by us once). Done before
             * the engine executes the inner-scorer body. */
            if (!debugFlagSet) {
                try {
                    wp.add(0xc).writeU8(1);
                    debugFlagSet = true;
                    send({ type: 'comp_capture_enabled',
                           weight_ptr: wptrVal >>> 0,
                           addr: wp.add(0xc).toString() });
                } catch (e) {
                    send({ type: 'comp_capture_failed',
                           err: e.toString() });
                }
            }
            snap.weights = {
                sp:   [safeReadF32(wp.add(0x10)),
                       safeReadF32(wp.add(0x14)),
                       safeReadF32(wp.add(0x18)),
                       safeReadF32(wp.add(0x1c)),
                       safeReadF32(wp.add(0x20))],
                f0:    safeReadF32(wp.add(0x24)),
                d:     safeReadF32(wp.add(0x34)),
                flag:  safeReadF32(wp.add(0x38)),
                ccos:  safeReadF32(wp.add(0x44)),
                w_4c:  safeReadF32(wp.add(0x4c)),
                miss_f0: safeReadF32(wp.add(0x80))
            };
        }

        /* Stash for onLeave; do NOT push yet -- onLeave fills in
         * cand_totals after the scorer has populated cand_buf+0x04. */
        this.snap = snap;

        /* For debugging slot 0: also dump cand_buf at onEnter (before
         * scoring) -- should show uid (set by AddUnit) and an
         * UN-INITIALIZED total. After InnerScorer returns onLeave should
         * show fVar14 OR the 10000 sentinel. */
        if (snap.slot !== null && snap.slot === 0 && stats.calls < 50) {
            var slicePtrEnter = ptr(thisPtrVal);
            var n_cands_e = safeReadU32(slicePtrEnter);
            var cb_e = safeReadU32(slicePtrEnter.add(0x18));
            if (n_cands_e !== null && cb_e !== null && cb_e >= 0x100000 &&
                n_cands_e > 0) {
                var bs_e = [];
                for (var bb = 0; bb < 24; ++bb) {
                    try { bs_e.push(ptr(cb_e).add(bb).readU8()); }
                    catch (e) { break; }
                }
                snap.cand0_bytes_enter = bs_e;
            }
        }
    },
    onLeave: function () {
        if (this.skip) return;
        if (!this.snap) return;
        var snap = this.snap;

        /* Read cand_buf from slice+0x18, n_cands from slice+0, then
         * (uid, total_cost) for each cand entry (0x18-byte stride;
         * uid @ +0x00, total f32 @ +0x04). */
        var slicePtr  = ptr(this.slice_ptr);
        var n_cands   = safeReadU32(slicePtr);
        var cbVal     = safeReadU32(slicePtr.add(0x18));
        if (n_cands !== null && cbVal !== null && cbVal >= 0x100000 &&
            n_cands > 0 && n_cands <= 4096) {
            var cb = ptr(cbVal);
            var totals = [];
            /* Also do a full byte-dump of the FIRST cand entry per slot
             * to verify offset interpretation (uid @ +0, total @ +4,
             * then per-component costs at +8/+0xc/+0x10/+0x14 if
             * verbose). */
            if (snap.slot !== null && snap.slot <= 1) {
                var first_bytes = [];
                var ok_bytes = true;
                for (var bb = 0; bb < 24; ++bb) {
                    try { first_bytes.push(cb.add(bb).readU8()); }
                    catch (e) { ok_bytes = false; break; }
                }
                if (ok_bytes) snap.cand0_bytes = first_bytes;
            }
            /* Per-cand component capture (D-17 path A). Layout in
             * cand_buf when weight+0xc != 0 (set above):
             *   +0x00 uid (u32)
             *   +0x04 total f32 (always written)
             *   +0x08 D-cost contribution        = abs(cand_byte+0x12 - durt_mean) * (1/durt_var) squared * weight_d
             *   +0x0c F0 contribution            = squared error * weight_f0  OR  weight_0x80 (MISSING_F0_COST)  OR  0
             *   +0x10 SP cost contribution       = sum-of-5-prosodic-matrices
             *   +0x14 CCOS / S contribution      = sum-of-4-cells * weight_ccos
             *
             * Note: the "flag" term (cand_byte+0x17 * weight_flag * _DAT_08e98580)
             * is folded into total directly and not stored separately; it can
             * be reconstructed offline from {cand_byte_0x17, weights.flag,
             * flag_const} (all already captured).
             *
             * Note: components for cands that get pruned mid-cascade (when
             * fVar14 > local_50 ceiling) may be partially-populated — only
             * components computed before the prune are written. cand_totals
             * stays at the 0x461c4000 sentinel (10000.0) for those. */
            var components = [];
            for (var i = 0; i < n_cands; ++i) {
                var base = cb.add(i * 0x18);
                var uid = safeReadU32(base);
                var t   = safeReadF32(base.add(4));
                if (uid === null || t === null) break;
                totals.push([uid >>> 0, t]);
                components.push([
                    safeReadF32(base.add(0x08)),  /* d    */
                    safeReadF32(base.add(0x0c)),  /* f0   */
                    safeReadF32(base.add(0x10)),  /* sp   */
                    safeReadF32(base.add(0x14))   /* ccos */
                ]);
            }
            snap.cand_totals     = totals;
            snap.cand_components = components;
        }

        batch.push(snap);
        if (batch.length >= BATCH_N) flush();
    }
});

rpc.exports = {
    stats : function () { return stats; },
    flush : function () { flush(); return stats; },
    reset : function () {
        flush();
        stats = { calls: 0, sent: 0, dropped: 0, ptr_invalid: 0,
                  read_errors: 0 };
    }
};

send({ type: 'ready', hook: 'inner_scorer', addr: '0x08E88DE0',
       batch_n: BATCH_N, cap: TOTAL_CAP });
