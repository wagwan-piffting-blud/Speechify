'use strict';
/*
 * fe_tree_hook.js -- dump the FE-emitted utterance tree at the entry
 * to SWIttsUSelUnitSelection. Closes the M3.4r Phase B input gap:
 * with the utterance topology captured we can implement BuildGraph
 * (FUN_08e89a70 + FUN_08e8a130) and LinkGraph (FUN_08e8c700) in C
 * and reproduce the engine's slot DAG bit-for-bit from a captured
 * trace -- a stepping stone toward synthesizing from text alone
 * once a C frontend exists (FE F0-F4 milestones).
 *
 * --- Reverse engineering basis ---
 *
 * Function:    FUN_08e819e0 = SWIttsUSelUnitSelection (DLL export).
 *              4 args on stack (__cdecl); the third arg (param_3)
 *              is the FE-built utterance pointer that USelGraph::
 *              Initialize / FUN_08e89a70 (BuildGraph) walks via the
 *              FUN_08e93xxx helpers.
 *
 * Object layout (Festival/FreeTTS-style "utterance with relations"):
 *
 *   Utterance (param_3):
 *     +0  ?
 *     +4  utterance-level features dict
 *     +8  relations dict (string name -> Relation*)
 *
 *   Relation:
 *     +0  name (string)? (we don't need it -- walks happen by-name
 *                          via the utterance.relations dict lookup)
 *     +0xc  head ItemRelation*
 *
 *   ItemRelation (the per-relation handle):
 *     +0    -> shared Item* (cross-relation identity)
 *     +4    -> Relation* (this IR's home relation)
 *     +8    -> next IR in this relation (linked list)
 *     +0xc  -> prev IR in this relation (NULL on first daughter)
 *     +0x10 -> parent IR (valid only when +0xc == NULL)
 *     +0x14 -> first daughter IR (in this relation)
 *
 *   Shared Item:
 *     +0  ?
 *     +4  features dict (cross-relation links + per-item features)
 *
 *   Type-tagged val (val_t):
 *     +0   short type_tag
 *     +4   payload (u32 / f32 / ptr)
 *
 * --- Safety ---
 *
 * Function-entry hook (onEnter only on a public DLL export, not inside
 * any hot loop). All pointer reads are guarded with rangeOK + try/catch.
 * Output volume: one snapshot per call to SWIttsUSelUnitSelection
 * (= one per utterance), each holding the full Word/Syllable/Segment/
 * SylStructure relation tree -- typically a few hundred IRs at most.
 */

var ADDR_USEL = ptr('0x08E819E0');

var TOTAL_CAP = 100;     /* utterances per session */
var stats = { calls: 0, sent: 0, ptr_invalid: 0, read_errors: 0 };

function rangeOK(addr) {
    try {
        var r = Process.findRangeByAddress(addr);
        return r !== null && r.protection.indexOf('r') !== -1;
    } catch (e) { return false; }
}
function safeReadU32(addr) {
    if (!rangeOK(addr)) { stats.ptr_invalid++; return null; }
    try { return addr.readU32(); }
    catch (e) { stats.read_errors++; return null; }
}
/* Read a NUL-terminated 7-bit-ASCII string up to maxlen bytes. */
function safeReadCString(addr, maxlen) {
    if (!rangeOK(addr)) return null;
    try { return addr.readUtf8String(maxlen || 256); }
    catch (e) { return null; }
}

/* Look up a relation in utterance.relations by name. Returns the
 * Relation* (which is the val_t.payload) or NULL.
 *
 * Walking the engine's hash dictionary in JS is awkward; instead we
 * use the engine's own lookup helper. The helpers are at:
 *
 *   FUN_08e93440 = FUN_08e94590 + type-tagged unwrap (relation type)
 *   FUN_08e93520 = type-tagged unwrap helper
 *
 * Calling FUN_08e94590 directly (the raw lookup) gives us the val_t*
 * back; we then read its payload at +4. Avoids the diagnostic logging
 * inside FUN_08e93440 on miss. */
var ADDR_LOOKUP = ptr('0x08E94590');     /* FUN_08e94590(dict, name) */
var lookup_fn = new NativeFunction(ADDR_LOOKUP, 'pointer',
                                   ['pointer', 'pointer'],
                                   'mscdecl');

function lookup_relation(utt_ptr, name_str) {
    var dict = safeReadU32(ptr(utt_ptr).add(0x8));
    if (dict === null || dict < 0x100000) return 0;
    var name_buf = Memory.allocUtf8String(name_str);
    var val_t = lookup_fn(ptr(dict), name_buf);
    if (val_t.isNull()) return 0;
    /* val_t->payload at +4 (relation pointer). */
    var payload = safeReadU32(val_t.add(4));
    return (payload && payload >= 0x100000) ? (payload >>> 0) : 0;
}

/* Look up a feature on a shared item. Per FUN_08e93680 / FUN_08e94700
 * the features dict is at *(shared+0) (NOT shared+4 -- shared+4 holds
 * the cross-relation links dict used by `R:Relation` paths). The
 * engine's helper FUN_08e94590 accepts a dict ptr as the first arg
 * and a name as the second. Returns {tag, payload, str}. Returns null
 * if the feature is missing. */
function lookup_feature(shared_ptr, name_str) {
    if (!shared_ptr || shared_ptr < 0x100000) return null;
    var dict = safeReadU32(ptr(shared_ptr).add(0x0));
    if (dict === null || dict < 0x100000) return null;
    var name_buf = Memory.allocUtf8String(name_str);
    var val_t = lookup_fn(ptr(dict), name_buf);
    if (val_t.isNull()) return null;
    var tag = null;
    try { tag = val_t.readShort(); } catch (e) {}
    var payload = safeReadU32(val_t.add(4));
    /* Engine string vals (type tag 5) -- payload is a NUL-terminated
     * C string in engine-allocated memory. (Confirmed via raw byte
     * probe; frida's readUtf8String/readCString are flaky on these
     * cross-DLL pointers, so we read byte-by-byte manually.) */
    var str = null;
    if (payload !== null && payload >= 0x100000) {
        try {
            var chars = [];
            var p = ptr(payload);
            for (var b = 0; b < 64; ++b) {
                var c = p.add(b).readU8();
                if (c === 0) break;
                chars.push(c);
            }
            str = String.fromCharCode.apply(null, chars);
        } catch (e) {}
    }
    return { tag: tag, payload: payload, str: str };
}

/* Walk all IRs in a relation's daughter chain. The relation's head IR
 * is at relation+0xc. We walk via IR+8 (next sibling) at the top
 * level and recurse via IR+0x14 (daughter) for nested relations.
 *
 * For Word/Syllable/Segment, the relations are flat (no nesting), so
 * we just walk the +8 chain.
 *
 * For SylStructure, the relation is a tree (Phrase -> Word -> Syllable
 * -> Segment) so we walk recursively, capturing parent/daughter links
 * via the IR fields. */

/* Per-relation feature names we want to dump on each IR. The dump
 * deliberately keeps the set narrow -- enough for BuildGraph + Post-
 * ScoringAdj to identify segments/syllables/words by phone class +
 * stress + accent context. The Festival/SWIfts feature set has many
 * more (lisp_*, syl_break, contentp, etc.) that we can add later. */
var FEATURES_FOR_REL = {
    /* Word features. Eloquence POS-related strings appear in the DLL
     * (category, subcat, noun_verb_s, auxil, quantif, subord, etc.)
     * but are STORED on the Token/word internal struct, not as
     * dict-lookup features. Try common Festival/SWI naming
     * conventions too. */
    "Word":         ["name", "category", "subcat", "noun_verb_s",
                     "stress", "pl_type", "number", "origin",
                     "contrac", "phonesAssigned",
                     "pos", "gpos", "type", "subtype", "wordtype",
                     "POS", "Category"],
    /* Syllable features for accent/intonation: stress_level + acc_valu
     * (the accent value the FE computes), plus break-index. */
    "Syllable":     ["stress", "stress_level", "acc_valu",
                     "bk_index", "annot", "str_acc"],
    "Segment":      ["name", "stress", "duration"],
    "Phrase":       ["name", "phr_tone", "bound_tone", "nuc_tone",
                     "brk_priority"],
    /* IntEvent.name is the accent string ("H*", "L*", "L+H*", "H+L*",
     * "L*+H", "H*+L"). FUN_08e8a250 reads this and maps it to the
     * accent_type stored in workspace+0x20 (used by FUN_08e8c7d0 +
     * FUN_08e8a670 for SP_target derivation). */
    "IntEvent":     ["name"],
    "SylStructure": [],
};

function add_features(entry) {
    var feats = FEATURES_FOR_REL[entry.rel];
    if (!feats || feats.length === 0) return;
    var fobj = {};
    for (var k = 0; k < feats.length; ++k) {
        var fn = feats[k];
        var v = lookup_feature(entry.shared, fn);
        if (v !== null) fobj[fn] = v;
    }
    entry.feat = fobj;
}

function dump_irs_flat(rel_ptr, rel_name, out_list, cap) {
    if (!rel_ptr) return;
    var head = safeReadU32(ptr(rel_ptr).add(0xc));
    if (head === null || head < 0x100000) return;
    var ir = head;
    var n = 0;
    while (ir && n < cap) {
        var ip = ptr(ir);
        var entry = {
            rel:      rel_name,
            ir:       ir >>> 0,
            shared:   safeReadU32(ip.add(0x0)),
            relhdr:   safeReadU32(ip.add(0x4)),
            next:     safeReadU32(ip.add(0x8)),
            prev:     safeReadU32(ip.add(0xc)),
            parent:   safeReadU32(ip.add(0x10)),
            daughter: safeReadU32(ip.add(0x14)),
        };
        add_features(entry);
        out_list.push(entry);
        n++;
        ir = entry.next;
        if (!ir || ir < 0x100000) break;
    }
}

function dump_irs_tree(rel_ptr, rel_name, out_list, cap) {
    /* SylStructure-style: walk daughter chains recursively. We dump
     * via DFS, carrying the parent IR explicitly so the parser can
     * reconstruct the tree without ambiguity. */
    if (!rel_ptr) return;
    var head = safeReadU32(ptr(rel_ptr).add(0xc));
    if (head === null || head < 0x100000) return;

    /* DFS iterative -- start with the head, then for each, push
     * daughter chain. */
    var stack = [head];
    var n = 0;
    while (stack.length > 0 && n < cap) {
        var cur = stack.pop();
        if (!cur || cur < 0x100000) continue;
        var ir = cur;
        /* Walk next-chain at this depth. */
        while (ir && n < cap) {
            var ip = ptr(ir);
            var entry = {
                rel:      rel_name,
                ir:       ir >>> 0,
                shared:   safeReadU32(ip.add(0x0)),
                relhdr:   safeReadU32(ip.add(0x4)),
                next:     safeReadU32(ip.add(0x8)),
                prev:     safeReadU32(ip.add(0xc)),
                parent:   safeReadU32(ip.add(0x10)),
                daughter: safeReadU32(ip.add(0x14)),
            };
            add_features(entry);
            out_list.push(entry);
            n++;
            if (entry.daughter && entry.daughter >= 0x100000) {
                stack.push(entry.daughter);
            }
            ir = entry.next;
            if (!ir || ir < 0x100000) break;
        }
    }
}

/* Relations we know are populated by the FE (per BuildGraph code +
 * SWIttsUSel.dll string table). */
var FLAT_RELATIONS = ['Word', 'Syllable', 'Segment', 'Phrase',
                      'IntEvent', 'Target',
                      'WordStructure'];
/* Intonation is a tree relation -- each accent-bearing Syllable has a
 * corresponding Intonation root IR whose daughter is the IntEvent
 * cross-link. FUN_08e8a250 uses Intonation.daughter -> daughter's shared
 * = IntEvent's shared -> name to assign accent_type. The flat walk only
 * sees the roots; we need the daughter IRs too. */
var TREE_RELATIONS = ['SylStructure', 'Intonation'];

Interceptor.attach(ADDR_USEL, {
    onEnter: function () {
        stats.calls++;
        if (stats.calls > TOTAL_CAP) return;
        var esp = this.context.esp;
        /* __cdecl -- esp+0 = return addr, esp+4 = arg0 (resource),
         *           esp+8 = arg1, esp+0xc = arg2 (utterance), esp+0x10 = arg3.
         * Actually looking at the SWIttsUSelUnitSelection signature:
         *   (int **resource, int *list, int **utterance, undefined4)
         * USelGraph::Initialize is called as FUN_08e8d4a0(local_e0, param_3),
         * which uses the third arg, so utterance = arg at [esp+0xc]. */
        var resource_ptr = safeReadU32(esp.add(0x4));
        var list_ptr     = safeReadU32(esp.add(0x8));
        var utt_ptr      = safeReadU32(esp.add(0xc));
        var arg4         = safeReadU32(esp.add(0x10));

        if (utt_ptr === null || utt_ptr < 0x100000) {
            send({ type: 'fe_tree_error',
                   reason: 'bad_utt_ptr',
                   esp: esp.toString(),
                   resource: resource_ptr, list: list_ptr,
                   utt: utt_ptr, arg4: arg4 });
            return;
        }

        var rels = {};
        for (var i = 0; i < FLAT_RELATIONS.length; ++i) {
            rels[FLAT_RELATIONS[i]] = lookup_relation(utt_ptr,
                                                     FLAT_RELATIONS[i]);
        }
        for (var i = 0; i < TREE_RELATIONS.length; ++i) {
            rels[TREE_RELATIONS[i]] = lookup_relation(utt_ptr,
                                                     TREE_RELATIONS[i]);
        }

        var irs = [];
        var CAP = 4096;
        for (var i = 0; i < FLAT_RELATIONS.length; ++i) {
            var rn = FLAT_RELATIONS[i];
            if (rels[rn]) dump_irs_flat(rels[rn], rn, irs, CAP);
        }
        for (var i = 0; i < TREE_RELATIONS.length; ++i) {
            var rn = TREE_RELATIONS[i];
            if (rels[rn]) dump_irs_tree(rels[rn], rn, irs, CAP);
        }

        send({
            type: 'fe_tree',
            n_call: stats.calls,
            utt_ptr: utt_ptr >>> 0,
            relations: rels,
            irs: irs,
            n_irs: irs.length,
        });
        stats.sent++;
    }
});

rpc.exports = {
    stats: function () { return stats; },
    flush: function () { return stats; },
    reset: function () {
        stats = { calls: 0, sent: 0, ptr_invalid: 0, read_errors: 0 };
    }
};

send({ type: 'ready', hook: 'fe_tree', addr: '0x08E819E0',
       cap: TOTAL_CAP });
