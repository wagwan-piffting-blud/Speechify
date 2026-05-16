'use strict';
/*
 * ccos_cell_probe_hook.js -- on FUN_08e8adc0 entry, walk voice memory
 * to dump the EXACT 4-cell ccos cell addresses and float values that
 * the engine accesses for specific (anchor, cand_uid) pairs.
 *
 * Goal: localize the divergence between v7's computed cost and engine's
 * captured pre_dp for the "the" anchor at slot 9 (first_hp=2, last_hp=5,
 * type=4, cand uids 26214 and 46287).
 *
 * Target: FUN_08e8adc0 @ 0x08E8ADC0 in SWIttsUSel.dll
 *   __fastcall, args:
 *     ECX        = last_hp_idx
 *     EDX        = cklx_match_idx
 *     [ESP+4]    = log_ctx
 *     [ESP+8]    = USelNetwork ptr
 *     [ESP+0xC]  = cost_array_out
 *     [ESP+0x10] = idx_array_out
 *     [ESP+0x14] = type (2=Syl, 4=Word)
 *     [ESP+0x18] = group_idx
 *     [ESP+0x1C] = first_hp_idx
 *
 * Filter: only fire for type=4, first_hp=2, last_hp=5 (the "the" Word
 * anchor at slot 9 in text_002 utt 0).
 *
 * Probe targets: cand_uids 26214 (engine-kept) and 46287 (my v7's #1).
 *
 * Safety: function-entry only.
 */

var ADDR_FUN = ptr('0x08E8ADC0');
var TARGET_TYPE     = 4;
var TARGET_FIRST_HP = 2;
var TARGET_LAST_HP  = 5;
// (ss, se) pairs for the "the" anchor. Engine uses ss phone_ctx for
// slots 0,1 and se phone_ctx for slots 2,3.
var PROBE_PAIRS = [
    [26214, 26217],   // engine-kept
    [46287, 46290],   // v7's incorrect top-1
    [54549, 54552]    // v7's surviving cand at 1.431
];

var stats = { calls: 0, matches: 0, dumps: 0 };
var sentReady = false;

function rangeOK(addr) {
    try {
        var r = Process.findRangeByAddress(addr);
        return r !== null && r.protection.indexOf('r') !== -1;
    } catch (e) { return false; }
}

function safeReadU32(addr) {
    if (!rangeOK(addr)) return null;
    try { return addr.readU32(); }
    catch (e) { return null; }
}

function safeReadS32(addr) {
    if (!rangeOK(addr)) return null;
    try { return addr.readS32(); }
    catch (e) { return null; }
}

function safeReadF32(addr) {
    if (!rangeOK(addr)) return null;
    try { return addr.readFloat(); }
    catch (e) { return null; }
}

function safeReadS8(addr) {
    if (!rangeOK(addr)) return null;
    try { return addr.readS8(); }
    catch (e) { return null; }
}

Interceptor.attach(ADDR_FUN, {
    onEnter: function () {
        stats.calls++;

        var esp = this.context.esp;
        var last_hp = this.context.ecx.toUInt32();
        var net_ptr = safeReadU32(esp.add(8));
        var type    = safeReadS32(esp.add(0x14));
        var first_hp = safeReadS32(esp.add(0x1c));

        if (type !== TARGET_TYPE || first_hp !== TARGET_FIRST_HP ||
            last_hp !== TARGET_LAST_HP) {
            return;
        }
        stats.matches++;

        if (net_ptr === null || net_ptr < 0x100000) return;
        var net = ptr(net_ptr);

        // voice ptr at USelNet+0x4
        var voice_ptr = safeReadU32(net.add(4));
        if (voice_ptr === null || voice_ptr < 0x100000) return;
        var voice = ptr(voice_ptr);

        // workspace at USelNet+0x14, stride 0x28 per hp
        var ws_block = safeReadU32(net.add(0x14));
        if (ws_block === null || ws_block < 0x100000) return;
        var ws = ptr(ws_block);

        // Read first_hp's ctx[0..4] and last_hp's ctx[0..4]
        // Layout: [type_byte, ctx[0], ctx[1], ctx[2], ctx[3], ctx[4]] u32s
        function readCtx(hp_idx) {
            var base = ws.add(hp_idx * 0x28);
            return {
                type_byte: safeReadU32(base.add(0)),
                c0: safeReadU32(base.add(4)),
                c1: safeReadU32(base.add(8)),
                c2: safeReadU32(base.add(0xc)),
                c3: safeReadU32(base.add(0x10)),
                c4: safeReadU32(base.add(0x14))
            };
        }
        var first_ctx = readCtx(first_hp);
        var last_ctx = readCtx(last_hp);

        // hp_class_remap (de-interleaved)
        var hp_remap_ptr = safeReadU32(voice.add(0x608));
        var s_ctx_ptr = safeReadU32(voice.add(0x604));
        var ccos_meta_base = safeReadU32(voice.add(0x610));
        var phone_ctx_base = safeReadU32(voice.add(0xc0));
        if (hp_remap_ptr === null || s_ctx_ptr === null ||
            ccos_meta_base === null || phone_ctx_base === null) return;

        var hp_remap = ptr(hp_remap_ptr);
        var s_ctx = ptr(s_ctx_ptr);
        var ccos_meta = ptr(ccos_meta_base);
        var pc_base = ptr(phone_ctx_base);

        var first_hpc_remap = safeReadU32(hp_remap.add(first_ctx.c2 * 4));
        var last_hpc_remap = safeReadU32(hp_remap.add(last_ctx.c2 * 4));

        // ccos_meta entry per hpc_remap: 0x30 bytes; slot k offsets:
        //   slot 0: stride@+4, data@+8
        //   slot 1: stride@+0x10, data@+0x14
        //   slot 2: stride@+0x1c, data@+0x20
        //   slot 3: stride@+0x28, data@+0x2c
        var meta_first = ccos_meta.add(first_hpc_remap * 0x30);
        var meta_last = ccos_meta.add(last_hpc_remap * 0x30);
        var slot_0_stride = safeReadU32(meta_first.add(4));
        var slot_0_data   = safeReadU32(meta_first.add(8));
        var slot_1_stride = safeReadU32(meta_first.add(0x10));
        var slot_1_data   = safeReadU32(meta_first.add(0x14));
        var slot_2_stride = safeReadU32(meta_last.add(0x1c));
        var slot_2_data   = safeReadU32(meta_last.add(0x20));
        var slot_3_stride = safeReadU32(meta_last.add(0x28));
        var slot_3_data   = safeReadU32(meta_last.add(0x2c));

        // Row indices: s_ctx_remap[first_ctx[0]] for slot 0, etc.
        var row_0 = safeReadU32(s_ctx.add(first_ctx.c0 * 4));
        var row_1 = safeReadU32(s_ctx.add(first_ctx.c1 * 4));
        var row_2 = safeReadU32(s_ctx.add(last_ctx.c3 * 4));
        var row_3 = safeReadU32(s_ctx.add(last_ctx.c4 * 4));

        // Read weight+0x44 (CCOS_WEIGHT) for verification
        var weight_ptr = safeReadU32(net.add(8));
        var w_44 = (weight_ptr && weight_ptr >= 0x100000)
                   ? safeReadF32(ptr(weight_ptr).add(0x44))
                   : null;

        // unit_table_base for boundary check
        var unit_tbl = safeReadU32(voice.add(0x20));

        // Helper: probe a cell for an (ss, se) pair (anchor cand).
        // Engine uses voice+0xc0[ss*4+0..1] for slots 0,1 and
        // voice+0xc0[se*4+2..3] for slots 2,3.
        function probePair(ss, se) {
            var ss_addr = pc_base.add(ss * 4);
            var se_addr = pc_base.add(se * 4);
            var pc_ss = [
                safeReadS8(ss_addr.add(0)),
                safeReadS8(ss_addr.add(1)),
                safeReadS8(ss_addr.add(2)),
                safeReadS8(ss_addr.add(3))
            ];
            var pc_se = [
                safeReadS8(se_addr.add(0)),
                safeReadS8(se_addr.add(1)),
                safeReadS8(se_addr.add(2)),
                safeReadS8(se_addr.add(3))
            ];
            function cellInfo(slot_data, slot_stride, row, col) {
                if (slot_data === null || slot_data === 0) {
                    return { addr: null, value: null,
                             reason: 'slot_data is null/0' };
                }
                var addr = ptr(slot_data).add(
                    row * slot_stride * 4 + col * 4);
                var v = safeReadF32(addr);
                return { addr: addr.toString(),
                         row: row, stride: slot_stride, col: col,
                         offset_bytes: row * slot_stride * 4 + col * 4,
                         value: v };
            }
            var c0 = cellInfo(slot_0_data, slot_0_stride, row_0, pc_ss[0]);
            var c1 = cellInfo(slot_1_data, slot_1_stride, row_1, pc_ss[1]);
            var c2 = cellInfo(slot_2_data, slot_2_stride, row_2, pc_se[2]);
            var c3 = cellInfo(slot_3_data, slot_3_stride, row_3, pc_se[3]);
            var sum = (c0.value || 0) + (c1.value || 0) +
                      (c2.value || 0) + (c3.value || 0);
            // Boundary check: mem[uid*0x18 + 0x13] for ss + se
            var ss_mem_13 = (unit_tbl && unit_tbl >= 0x100000)
                ? safeReadS8(ptr(unit_tbl).add(ss * 0x18 + 0x13))
                : null;
            var se_mem_13 = (unit_tbl && unit_tbl >= 0x100000)
                ? safeReadS8(ptr(unit_tbl).add(se * 0x18 + 0x13))
                : null;
            // Read bytes 0x12..0x17 of mem record for both
            function dumpMem(uid) {
                if (!unit_tbl) return null;
                var rec = ptr(unit_tbl).add(uid * 0x18);
                var bytes = [];
                for (var i = 0; i < 0x18; ++i) {
                    bytes.push(safeReadS8(rec.add(i)));
                }
                return bytes;
            }
            return {
                ss: ss, se: se,
                pc_ss: pc_ss, pc_se: pc_se,
                ss_mem13_hpclass: ss_mem_13,
                se_mem13_hpclass: se_mem_13,
                ss_mem_bytes: dumpMem(ss),
                se_mem_bytes: dumpMem(se),
                cell_0: c0, cell_1: c1, cell_2: c2, cell_3: c3,
                sum_4cell: sum,
                cost_with_w_44: sum * (w_44 || 1.0)
            };
        }

        var cand_data = [];
        for (var i = 0; i < PROBE_PAIRS.length; ++i) {
            cand_data.push(probePair(PROBE_PAIRS[i][0], PROBE_PAIRS[i][1]));
        }

        send({
            type: 'ccos_cell_probe',
            n_call: stats.calls,
            type_arg: type,
            first_hp: first_hp,
            last_hp: last_hp,
            first_ctx: first_ctx,
            last_ctx: last_ctx,
            first_hpc_remap: first_hpc_remap,
            last_hpc_remap: last_hpc_remap,
            slot_meta: {
                slot_0: { stride: slot_0_stride, data: slot_0_data },
                slot_1: { stride: slot_1_stride, data: slot_1_data },
                slot_2: { stride: slot_2_stride, data: slot_2_data },
                slot_3: { stride: slot_3_stride, data: slot_3_data }
            },
            slot_rows: { row_0: row_0, row_1: row_1, row_2: row_2, row_3: row_3 },
            w_44: w_44,
            cands: cand_data
        });
        stats.dumps++;
    }
});

if (!sentReady) {
    send({ type: 'ready', stats: stats });
    sentReady = true;
}

rpc.exports = {
    flush: function () { return stats; },
    drain: function () { return stats; },
    reset: function () { stats = { calls: 0, matches: 0, dumps: 0 }; }
};
