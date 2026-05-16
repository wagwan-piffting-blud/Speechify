'use strict';
/*
 * ccos_apply_hook.js -- function-entry/leave hook on FUN_08e8adc0
 * (USelNetwork CCOS context-cost reduction site).
 *
 * Per Ghidra (verified 2026-05-09 in plan 02-02 Task 1 decode):
 *
 *   FUN_08e8adc0 is the producer of the per-candidate CCOS context-cost
 *   reduction. For each cand c in the input candidate group:
 *
 *     if HP-class match (mem[+0x13] of c.s_first AND c.s_last match
 *                        local_c (=local_10c label) AND local_110):
 *         cost[c] = (cell_a + cell_b + cell_c + cell_d) * weight+0x44
 *         where cell_{a..d} are looked up from per-(hp_class, slot)
 *         tables addressed via local_f0/local_104/local_1c/local_14
 *         and ctx-byte indices from voice+0xc0 (or voice+0xc4 path).
 *     else:
 *         cost[c] = 10000.0f   (HP-class mismatch sentinel)
 *
 *   Then a histogram-based dynamic prune drops cands with cost > threshold.
 *
 *   Outputs: param_5[i] (= cost_array_out)  -- per-survivor cost
 *            param_6[i] (= idx_array_out)   -- per-survivor original group offset
 *            return EAX                    -- n_survivors after histogram pruning
 *
 *   The caller FUN_08e8ce60 adds afStack_13880[i] (= cost_array_out[i])
 *   to the i'th surviving candidate's TC at struct offset +0x2c.
 *
 * Calling convention (verified by ccos_cell_probe_hook.js):
 *   ECX        = last_hp_idx        (param_1)
 *   EDX        = cklx_match_idx     (param_2)
 *   [esp+4]    = log_ctx            (param_3)
 *   [esp+8]    = USelNetwork ptr    (param_4)
 *   [esp+0xC]  = cost_array_out     (param_5)
 *   [esp+0x10] = idx_array_out      (param_6)
 *   [esp+0x14] = type               (param_7) [2=Syl, 4=Word]
 *   [esp+0x18] = group_idx          (param_8)
 *   [esp+0x1C] = first_hp_idx       (param_9)
 *
 *   Return EAX = local_114 = n_survivors (caller stores at param_2+0x2c).
 *
 * Schema (per slot -> schema_check.py / TRACE_SCHEMA.md):
 *   {
 *     "type": "ccos_apply",
 *     "n_call":    int,    // monotonically increasing call counter
 *     "type_arg":  int,    // 2 (Syl) or 4 (Word)
 *     "first_hp":  int,    // anchor first HP idx
 *     "last_hp":   int,    // anchor last HP idx
 *     "group_idx": int,    // per-anchor group iter (param_8)
 *     "cklx_match_idx": int, // param_2 (lex match iter)
 *     "n_input":   int,    // candidates in input group (local_f4)
 *     "n_kept":    int,    // return value -- after histogram prune
 *     "cost_w_44": float,  // weight+0x44 (CCOS_WEIGHT) for verification
 *     "cands":     list[ {"idx": int, "cost": float,
 *                         "s_first_uid": int, "s_last_uid": int} ]
 *                  // s_first_uid/s_last_uid map idx -> (s_first, s_last)
 *                  // unit pair via voice+0x98[group_idx] + idx*0xc + {4,8}.
 *                  // Per FUN_08e8adc0:
 *                  //   pair_table = *(int*)(voice+0x98 + group_idx*4)
 *                  //   pair_entry = pair_table + idx*0xc
 *                  //   s_first    = *(int*)(pair_entry + 4)
 *                  //   s_last     = *(int*)(pair_entry + 8)
 *                  // -1 if any pointer in the chain is unreadable.
 *
 * Safety: function-entry (onEnter) + function-return (onLeave) only.
 * No mid-instruction stalker placement. Honors run_frida_capture.py:47-65.
 *
 * D-07 parity bar: the (idx, cost) values per surviving cand are the
 * ground truth our spfy_ccos_reduction() must reproduce bit-exact on
 * the 200-phrase corpus.
 */

var ADDR_FUN = ptr('0x08E8ADC0');

var stats = { calls: 0, leaves: 0, kept_total: 0 };
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

Interceptor.attach(ADDR_FUN, {
    onEnter: function (args) {
        stats.calls++;

        var esp = this.context.esp;
        var last_hp        = this.context.ecx.toUInt32();
        var cklx_match_idx = this.context.edx.toUInt32();
        var log_ctx        = safeReadU32(esp.add(0x04));
        var net_ptr        = safeReadU32(esp.add(0x08));
        var cost_array_out = safeReadU32(esp.add(0x0C));
        var idx_array_out  = safeReadU32(esp.add(0x10));
        var type_arg       = safeReadS32(esp.add(0x14));
        var group_idx      = safeReadS32(esp.add(0x18));
        var first_hp       = safeReadS32(esp.add(0x1C));

        // Per FUN_08e8adc0 (Ghidra-decoded 2026-05-09):
        //   iVar7      = voice ptr (= *(int*)(net_ptr + 4))
        //   local_ec   = group_idx * 0x1c
        //   local_e8   = cklx_match_idx * 0xc
        //   grp_entry  = *(int*)(voice+0x9c + group_idx*0x1c + 0x18)
        //   match_list = *(int*)(grp_entry + cklx_match_idx*0xc + 8)
        //                -- int[] of length n_input, each entry a
        //                   per-cand pair-idx into voice+0x98[group_idx]
        //   n_input    = *(int*)(grp_entry + cklx_match_idx*0xc + 4)
        //   pair_table = *(int*)(voice+0x98 + group_idx*4)
        //                -- per-pair entry has s_first @+4, s_last @+8
        var n_input = null;
        var w_44 = null;
        // Anchor histogram-prune weights at weight+0x54..+0x60
        // (decoded plan 02-02 Task 4):
        //   +0x54 MAX_ANCHOR_COST_SYL  (local_10c when type_arg=2)
        //   +0x58 SLOPE_SYL            (local_108 when type_arg=2)
        //   +0x5c MAX_ANCHOR_COST_WORD (local_10c when type_arg=4)
        //   +0x60 SLOPE_WORD           (local_108 when type_arg=4)
        var w_54 = null, w_58 = null, w_5c = null, w_60 = null;
        var voice_ptr_u32 = null;
        var pair_table_u32 = null;
        var match_list_u32 = null;
        if (net_ptr && net_ptr >= 0x100000) {
            var net = ptr(net_ptr);
            voice_ptr_u32 = safeReadU32(net.add(4));
            var weight_ptr = safeReadU32(net.add(8));
            if (weight_ptr && weight_ptr >= 0x100000) {
                var wp = ptr(weight_ptr);
                w_44 = safeReadF32(wp.add(0x44));
                w_54 = safeReadF32(wp.add(0x54));
                w_58 = safeReadF32(wp.add(0x58));
                w_5c = safeReadF32(wp.add(0x5c));
                w_60 = safeReadF32(wp.add(0x60));
            }
            if (voice_ptr_u32 && voice_ptr_u32 >= 0x100000) {
                var voice = ptr(voice_ptr_u32);
                // pair_table = *(int*)(*(int*)(voice+0x98) + group_idx*4)
                // (voice+0x98 holds a pointer to the per-group-idx table
                //  of pair-entry arrays, NOT the table inline)
                var pair_arr_ptr = safeReadU32(voice.add(0x98));
                if (pair_arr_ptr && pair_arr_ptr >= 0x100000 &&
                    group_idx !== null) {
                    pair_table_u32 = safeReadU32(
                        ptr(pair_arr_ptr).add(group_idx * 4));
                }
                var grp_arr = safeReadU32(voice.add(0x9c));
                if (grp_arr && grp_arr >= 0x100000 && group_idx !== null) {
                    var grp_entry = safeReadU32(
                        ptr(grp_arr).add(group_idx * 0x1c + 0x18));
                    if (grp_entry && grp_entry >= 0x100000 &&
                        cklx_match_idx !== null) {
                        n_input = safeReadS32(
                            ptr(grp_entry).add(cklx_match_idx * 0xc + 4));
                        match_list_u32 = safeReadU32(
                            ptr(grp_entry).add(cklx_match_idx * 0xc + 8));
                    }
                }
            }
        }

        // Stash for onLeave
        this.state = {
            n_call:         stats.calls,
            type_arg:       type_arg,
            first_hp:       first_hp,
            last_hp:        last_hp,
            group_idx:      group_idx,
            cklx_match_idx: cklx_match_idx,
            n_input:        n_input,
            cost_w_44:      w_44,
            anchor_max_syl: w_54,
            anchor_slope_syl:  w_58,
            anchor_max_word:   w_5c,
            anchor_slope_word: w_60,
            cost_array_out: cost_array_out,
            idx_array_out:  idx_array_out,
            pair_table:     pair_table_u32,
            match_list:     match_list_u32
        };
    },

    onLeave: function (retval) {
        stats.leaves++;
        var s = this.state;
        if (!s) return;
        // EAX = local_114 = n_survivors after histogram prune.
        var n_kept = retval ? retval.toInt32() : 0;
        if (n_kept < 0) n_kept = 0;
        if (n_kept > 9999) n_kept = 9999;  // engine cap

        var cands = [];
        if (n_kept > 0 &&
            s.cost_array_out && s.cost_array_out >= 0x100000 &&
            s.idx_array_out  && s.idx_array_out  >= 0x100000) {
            var ca = ptr(s.cost_array_out);
            var ia = ptr(s.idx_array_out);
            var have_pair = (s.pair_table  && s.pair_table  >= 0x100000);
            var have_list = (s.match_list  && s.match_list  >= 0x100000);
            var pt = have_pair ? ptr(s.pair_table)  : null;
            var ml = have_list ? ptr(s.match_list)  : null;
            for (var i = 0; i < n_kept; ++i) {
                var c = safeReadF32(ca.add(i * 4));
                var ix = safeReadS32(ia.add(i * 4));
                if (c === null || ix === null) {
                    cands.push({ idx: -1, cost: NaN,
                                 s_first_uid: -1, s_last_uid: -1 });
                    continue;
                }
                // Resolve idx -> (s_first_uid, s_last_uid) via:
                //   pair_idx   = match_list[idx]
                //   pair_entry = pair_table + pair_idx * 0xc
                //   s_first    = *(int*)(pair_entry + 4)
                //   s_last     = *(int*)(pair_entry + 8)
                var s_first = -1, s_last = -1;
                if (ml && pt && ix >= 0) {
                    var pair_idx = safeReadS32(ml.add(ix * 4));
                    if (pair_idx !== null && pair_idx >= 0) {
                        var pe = pt.add(pair_idx * 0xc);
                        var sf = safeReadS32(pe.add(4));
                        var sl = safeReadS32(pe.add(8));
                        if (sf !== null) s_first = sf;
                        if (sl !== null) s_last  = sl;
                    }
                }
                cands.push({ idx: ix, cost: c,
                             s_first_uid: s_first, s_last_uid: s_last });
            }
            stats.kept_total += n_kept;
        }

        send({
            type:           'ccos_apply',
            n_call:         s.n_call,
            type_arg:       (s.type_arg === null ? -1 : s.type_arg),
            first_hp:       (s.first_hp === null ? -1 : s.first_hp),
            last_hp:        (s.last_hp === null ? -1 : s.last_hp),
            group_idx:      (s.group_idx === null ? -1 : s.group_idx),
            cklx_match_idx: (s.cklx_match_idx === null ? -1 : s.cklx_match_idx),
            n_input:        (s.n_input === null ? -1 : s.n_input),
            n_kept:         n_kept,
            cost_w_44:      (s.cost_w_44 === null ? 0.0 : s.cost_w_44),
            anchor_max_syl:    (s.anchor_max_syl    === null ? 0.0 : s.anchor_max_syl),
            anchor_slope_syl:  (s.anchor_slope_syl  === null ? 0.0 : s.anchor_slope_syl),
            anchor_max_word:   (s.anchor_max_word   === null ? 0.0 : s.anchor_max_word),
            anchor_slope_word: (s.anchor_slope_word === null ? 0.0 : s.anchor_slope_word),
            cands:          cands
        });
    }
});

if (!sentReady) {
    send({ type: 'ready', stats: stats });
    sentReady = true;
}

rpc.exports = {
    flush: function () { return stats; },
    drain: function () { return stats; },
    reset: function () { stats = { calls: 0, leaves: 0, kept_total: 0 }; }
};
