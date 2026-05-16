"""Anchor pre_dp v7 -- uses Frida-captured first_hp/last_hp directly.

Key change vs v6: instead of computing anchor_first_hp_is from
unit-span_len (which broke for boundary anchors with units != HPs),
we use Frida-captured first_hp/last_hp (from anchor_score hook's
onLeave with first_cand_uid) and match by (cand_uid, cand_jk).
"""
import json, glob, os, struct, sys

def _f32(x):
    """Round a Python float to nearest float32 — engine uses 32-bit floats
    throughout the cost path; this matches its accumulator precision."""
    return struct.unpack("<f", struct.pack("<f", float(x)))[0]
sys.path.insert(0, "c:/tmp")
import cklx_ckls_parse as parser  # noqa: E402
import ccos_parse  # noqa: E402
import vcf_parse  # noqa: E402

CORPUS = os.path.expanduser("~/Documents/Speechify/spfy/test/oracle/traces")
VIN = os.path.expanduser("~/Documents/Speechify/en-US/tom/tom.vin")
VCF = os.path.expanduser("~/Documents/Speechify/en-US/tom/tom.vcf")

TOM_FORWARD_SWAP = {9: 10, 10: 11, 11: 9, 14: 15, 15: 14}
TOM_INVERSE_SWAP = {v: k for k, v in TOM_FORWARD_SWAP.items()}
ENGINE_MATRIX_ORDER = ("sylInPhraseCosts", "sylTypeCosts",
                       "sylInWordCosts", "wordInPhraseCosts",
                       "phoneInSylCosts")

SP_W = [0.05, 0.05, 0.05, 0.05, 0.0]
W_FLAG_3C = 0.25
W_FLAG_SCALE = 0.01
W_CCOS = 1.0
W_DUR = 0.30
W_F0 = 0.20
W_F0_MISS = 5.0
DAT_971D8 = 0.1
DAT_98A24 = 50.0
DAT_98528 = 10000.0
DAT_8E9857C = 1.0
ANCHOR_NORM = 0.7
ANCHOR_NORM2 = 0.005


def s_ctx_remap(c, n):
    if c is None: return None
    label = c >> 1
    if label >= n: return None
    return TOM_FORWARD_SWAP.get(label, label)


def hp_class_remap(c, n):
    if c is None: return None
    side = c & 1
    label = c >> 1
    if label >= n: return None
    return side * n + TOM_FORWARD_SWAP.get(label, label)


def load_unit_data(raw):
    chunk = parser.find_chunk(raw, b"unit")
    p = 0
    while p + 8 <= len(chunk):
        cc = chunk[p:p + 4]
        sz = struct.unpack_from("<I", chunk, p + 4)[0]
        if cc == b"data":
            return chunk[p + 8:p + 8 + sz]
        p += 8 + sz + (sz & 1)
    return b""


_HPCLASS_TABLE = None
def _load_hpclass_table():
    global _HPCLASS_TABLE
    if _HPCLASS_TABLE is None:
        # Engine-truth mem+0x13 for all 169579 Tom units, dumped via
        # viz/frida_hooks/unit_hpclass_dump_hook.js. NOT disk-derived.
        import os
        here = os.path.dirname(os.path.abspath(__file__))
        path = os.path.normpath(os.path.join(here, "..", "..", "data", "tom_hpclass.bin"))
        with open(path, "rb") as f:
            _HPCLASS_TABLE = f.read()
    return _HPCLASS_TABLE


def unit_hp_class(unit_data, uid):
    """Engine-truth hp_class from voice+0x20+uid*0x18+0x13.
    Frida-dumped, NOT disk-derived. mem+0x13 is stored directly in the
    in-memory unit table; cannot be computed from disk[0x14]+disk[0x15]
    alone (units with same disk fields can have different mem+0x13).
    """
    tbl = _load_hpclass_table()
    return tbl[uid] if uid < len(tbl) else 0xff


def signed8(b):
    return b - 256 if b >= 128 else b


def proscost_to_arrays(p):
    out = {}
    for mname, rows in p.items():
        first = next(iter(rows.values()))
        cols = list(first.keys())
        n = len(cols)
        idx = {c: i for i, c in enumerate(cols)}
        m = [[0.0] * n for _ in range(n)]
        for rn, cells in rows.items():
            r = idx.get(rn)
            if r is None: continue
            for c, v in cells.items():
                ci = idx.get(c)
                if ci is not None: m[r][ci] = v
        out[mname] = {"data": m, "n": n}
    return out


def compute_4cell_ccos(unit_data, ss, se, first_ctx, last_ctx,
                        ccos_tables, n_labels):
    hp_first_remap = hp_class_remap(first_ctx[2], n_labels)
    hp_last_remap = hp_class_remap(last_ctx[2], n_labels)
    if hp_first_remap is None or hp_last_remap is None:
        return None
    sl0 = s_ctx_remap(first_ctx[0], n_labels)
    sl1 = s_ctx_remap(first_ctx[1], n_labels)
    sl2 = s_ctx_remap(last_ctx[3], n_labels)
    sl3 = s_ctx_remap(last_ctx[4], n_labels)
    if any(x is None for x in (sl0, sl1, sl2, sl3)):
        return None
    matrix_floats = n_labels * n_labels
    ss_rec = unit_data[ss * 29:(ss + 1) * 29]
    se_rec = unit_data[se * 29:(se + 1) * 29]
    # voice+0xc0[uid*4+k] = direct copy of disk[0x17+k] (already labels,
    # 0xFF as sentinel). Verified via Frida cand_ctx_probe vs disk dump.
    pc_ss = [signed8(ss_rec[0x17 + k]) for k in range(4)]
    pc_se = [signed8(se_rec[0x17 + k]) for k in range(4)]

    def cell(hp, slot, row, col_signed):
        base = (hp * 4 + slot) * matrix_floats
        off = row * n_labels + col_signed
        if off < 0 or off >= matrix_floats:
            return 0.0
        return ccos_tables[base + off]

    c0 = _f32(cell(hp_first_remap, 0, sl0, pc_ss[0]))
    c1 = _f32(cell(hp_first_remap, 1, sl1, pc_ss[1]))
    c2 = _f32(cell(hp_last_remap, 2, sl2, pc_se[2]))
    c3 = _f32(cell(hp_last_remap, 3, sl3, pc_se[3]))
    s = _f32(_f32(_f32(c0 + c1) + c2) + c3)
    return _f32(s * W_CCOS)


def histogram_prune(cands_4cell, anchor_type):
    """Float32-throughout to match engine accumulator precision."""
    norm  = _f32(ANCHOR_NORM)
    norm2 = _f32(ANCHOR_NORM2)
    bin_w = _f32(DAT_971D8)
    norm_scale = _f32(DAT_98A24)
    best = _f32(10000.0)
    threshold_running = _f32(norm + 10000.0)
    accepted = []
    for cost, pid, ss, se in cands_4cell:
        cost32 = _f32(cost)
        if cost32 < threshold_running:
            if cost32 < best:
                best = cost32
                threshold_running = _f32(norm + cost32)
            accepted.append((cost32, pid, ss, se))

    if not accepted:
        return float("inf"), best, accepted
    bins = [0] * 50
    scale = _f32(norm_scale / threshold_running) if threshold_running > 0 else _f32(1.0)
    for cost, _, _, _ in accepted:
        bin_idx = int(round(_f32(_f32(cost - best) * scale)))
        if 0 <= bin_idx < 50:
            bins[bin_idx] += 1

    # Engine FUN_08e8adc0 histogram walk (asm 0x08e8b240..b46e): unrolled
    # 50-step loop. Step k uses cum = sum(bins[0..k-1]) and bin_dist = k*0.1.
    # Break when slack DROPS below bin_dist; threshold = best + bin_dist_at_break
    # (NOT best + slack). See project_b44_anchor_gap.md.
    cum = 0
    final_bin_dist = _f32(50 * bin_w)        # default if loop never breaks
    for k in range(1, 51):
        cum += bins[k - 1]
        slack = _f32(norm - _f32(cum * norm2))
        bin_dist = _f32(k * bin_w)
        if slack < bin_dist:
            final_bin_dist = bin_dist
            break
    return _f32(best + final_bin_dist), best, accepted


def compute_anchor_full_cost(unit_data, ss, se, first_ctx, last_ctx,
                              first_hp, last_hp, workspace, voicing,
                              cart_walks, ccos_tables, n_labels, eng_mat,
                              syl_idx_array=None):
    ccos_4cell = compute_4cell_ccos(unit_data, ss, se, first_ctx, last_ctx,
                                      ccos_tables, n_labels)
    if ccos_4cell is None:
        return None

    flag_sum = 0
    sp_cost = 0.0
    d_delta_sum = 0.0
    d_var_sum = 0.0
    f0_cost = 0.0

    hp_seq = list(range(first_hp, last_hp + 1))
    target_idx = 0
    target_hp = hp_seq[0]
    cur_syl = (syl_idx_array[hp_seq[0]] if syl_idx_array and
               hp_seq[0] < len(syl_idx_array) else None)

    for u_idx, u in enumerate(range(ss, se + 1)):
        u_rec = unit_data[u * 29:(u + 1) * 29]

        if (syl_idx_array is not None and u_idx > 0 and
                u_rec[0x15] != 0 and target_idx < len(hp_seq) - 1):
            while target_idx < len(hp_seq) - 1:
                target_idx += 1
                target_hp = hp_seq[target_idx]
                ns = (syl_idx_array[target_hp]
                      if target_hp < len(syl_idx_array) else cur_syl)
                if ns != cur_syl:
                    cur_syl = ns
                    break

        ws = workspace.get(target_hp)
        flag_sum += u_rec[0x1C]

        if ws is not None:
            for k, (mat_idx, ws_off, byte_off) in enumerate([
                (0, 0x2c, 0x0c), (1, 0x28, 0x0d),
                (2, 0x34, 0x0e), (3, 0x38, 0x0f),
                (4, 0x3c, None),
            ]):
                if SP_W[k] == 0 or byte_off is None:
                    continue
                mat = eng_mat[mat_idx]
                if mat is None: continue
                ri = ws[ws_off]
                ci = u_rec[byte_off]
                if ri >= mat["n"] or ci >= mat["n"]: continue
                sp_cost += mat["data"][ri][ci] * SP_W[k]

        cart_t = cart_walks.get(target_hp, {})
        if "durt" in cart_t:
            d_mean, d_var = cart_t["durt"]
            d_delta_sum += u_rec[0x13] - d_mean
            inv_var = DAT_8E9857C / d_var if d_var != 0 else 0
            d_var_sum += inv_var * inv_var

        unit_hpc = u_rec[0x14] * 2 + (0 if u_rec[0x15] else 1)
        v_active = (voicing[unit_hpc] if voicing and 0 <= unit_hpc < len(voicing) else 1)
        if v_active != 0 and "f0tr" in cart_t:
            f0_b = u_rec[0x10]
            if f0_b == 0:
                f0_cost += W_F0_MISS
            else:
                f_mean, f_var = cart_t["f0tr"]
                f_delta = abs((f0_b - f_mean) * f_var)
                f0_cost += f_delta * W_F0 * f_delta

    flag_cost = flag_sum * W_FLAG_3C * W_FLAG_SCALE
    d_cost = 0.0
    if d_var_sum > 0:
        d_cost = (d_delta_sum / d_var_sum) * d_delta_sum * W_DUR

    # Test: disable D and F0 to check if they're the discrepancy.
    return ccos_4cell + flag_cost + sp_cost + d_cost + f0_cost


def main():
    with open(VIN, "rb") as f:
        raw = parser.deobfuscate(f.read())
    cklx = parser.parse_cklx(parser.find_chunk(raw, b"cklx"))
    ckls = parser.parse_ckls(parser.find_chunk(raw, b"ckls"))
    unit_data = load_unit_data(raw)
    ccos_tables, n_labels = ccos_parse.parse_ccos(parser.find_chunk(raw, b"ccos"))
    with open(VCF, "rb") as f:
        cipher = f.read()
    plain = vcf_parse.decrypt_vcf(cipher).decode("utf-8", errors="replace")
    matrices = proscost_to_arrays(vcf_parse.parse_proscost(plain))
    eng_mat = [matrices.get(m) for m in ENGINE_MATRIX_ORDER]

    n_anchors = 0; n_match = 0; sample = []

    for fe_path in sorted(glob.glob(
            os.path.join(CORPUS, "fe_tree", "text_*.jsonl"))):
        fname = os.path.basename(fe_path)
        vit_path = os.path.join(CORPUS, "viterbi_dp", fname)
        prsl_path = os.path.join(CORPUS, "prsl_slot", fname)
        is_path = os.path.join(CORPUS, "inner_scorer", fname)
        cw_path = os.path.join(CORPUS, "cart_walks", fname)
        as_path = os.path.join(CORPUS, "anchor_score", fname)
        if not all(os.path.exists(p) for p in
                    (vit_path, prsl_path, is_path, cw_path, as_path)):
            continue

        with open(vit_path, encoding="utf-8") as f:
            vit_utts = [json.loads(l) for l in f if l.strip() and
                        json.loads(l).get("stage") == "enter"]
        with open(prsl_path, encoding="utf-8") as f:
            prsl_evs = [json.loads(l) for l in f if l.strip() and
                        json.loads(l).get("type") == "prsl_slot"]
        with open(is_path, encoding="utf-8") as f:
            is_evs_all = [json.loads(l) for l in f if l.strip()]
        with open(cw_path, encoding="utf-8") as f:
            cw_evs = [json.loads(l) for l in f if l.strip() and
                      json.loads(l).get("type") == "cart_walk"]

        # Frida anchor_score: index by (uid, jk, type) -> first_hp/last_hp
        as_evs = []
        with open(as_path, encoding="utf-8") as f:
            for l in f:
                if not l.strip(): continue
                ev = json.loads(l)
                if ev.get("first_hp") is None: continue
                as_evs.append(ev)
        as_by_uid_jk = {}   # (uid, jk) -> [list of as_evs]
        for ev in as_evs:
            u = ev.get("first_cand_uid")
            j = ev.get("first_cand_jk")
            if u is not None and j is not None:
                as_by_uid_jk.setdefault((u, j), []).append(ev)

        voicing = None
        for ev in is_evs_all:
            if ev.get("type") == "join_consts":
                voicing = ev.get("voice_5fc_per_uid", {}).get("by_hp_class")
                break
        is_evs = [e for e in is_evs_all if e.get("type") == "inner_scorer"]

        def grp(evs):
            utts, cur = [], []
            for e in evs:
                if e.get("slot") == 0 and cur: utts.append(cur); cur = []
                cur.append(e)
            if cur: utts.append(cur)
            return utts
        prsl_utts = grp(prsl_evs)
        is_utts = grp(is_evs)
        cw_utts = grp(cw_evs)

        for utt_idx in range(min(len(vit_utts), len(prsl_utts),
                                  len(is_utts), len(cw_utts))):
            vit = vit_utts[utt_idx]
            prsl = prsl_utts[utt_idx]
            is_data = is_utts[utt_idx]
            cw = cw_utts[utt_idx]
            slots = vit.get("slots") or []

            workspace = {}
            for ev in is_data:
                hp = ev["slot"]
                sp = ev.get("sp_target") or [0] * 5
                workspace[hp] = {0x2c: sp[0], 0x28: sp[1],
                                  0x34: sp[2], 0x38: sp[3], 0x3c: sp[4]}
            hp_ctx = {ev["slot"]: ev.get("ctx") for ev in prsl}
            cart_per_hp = {}
            for ev in cw:
                hp = ev["slot"]
                cart_per_hp.setdefault(hp, {})[ev["tree"]] = (
                    ev["leaf_mean"], ev["leaf_var"])

            for slot_idx, slot in enumerate(slots):
                cands = slot.get("cands") or []
                if not cands or cands[0]["uid"] == cands[0]["join_key"]:
                    continue
                cand0 = cands[0]
                # Look up Frida-captured first_hp/last_hp by (uid, jk).
                key = (cand0["uid"], cand0["join_key"])
                as_list = as_by_uid_jk.get(key, [])
                if not as_list:
                    continue   # No Frida data for this anchor; skip.

                # Pick the matching slot_grp by Frida anchor_type. A cand0
                # like (15254, 15255) appears as a posting in BOTH _SYL_['ay']
                # and _WORD_['i'], so iterating _SYL_-first and stopping on
                # first hit (the old code) misclassifies WORD-anchor slots.
                # The as_ev.type (2=Syl, 4=Word) is authoritative — use it.
                # When the SAME (uid, jk) is first_cand for BOTH a SYL and a
                # WORD anchor (e.g. "Eight." text_023), disambiguate via
                # final_n_cands matching this slot's n_cands.
                n_cands_here = len(cands)
                chosen_ev = None
                for e in as_list:
                    if e.get("final_n_cands") == n_cands_here:
                        chosen_ev = e
                        break
                if chosen_ev is None:
                    chosen_ev = as_list[0]
                anchor_type = chosen_ev["type"]
                grp_for_type = {2: "_SYL_", 4: "_WORD_"}
                slot_grp = grp_for_type.get(anchor_type)
                slot_name = None
                if slot_grp:
                    for pid, txt in enumerate(ckls[slot_grp]):
                        ss_t, se_t, t = txt
                        if ss_t == cand0["uid"] and se_t == cand0["join_key"]:
                            slot_name = t
                            break
                if not slot_name:
                    continue
                # Use the disambiguated event chosen above (by type +
                # n_cands match against this Viterbi slot).
                as_ev = chosen_ev
                first_hp = as_ev["first_hp"]
                last_hp = as_ev["last_hp"]
                first_ctx = hp_ctx.get(first_hp)
                last_ctx = hp_ctx.get(last_hp)
                if first_ctx is None or last_ctx is None:
                    continue

                postings = cklx[slot_grp].get(slot_name, [])
                if not postings: continue

                cands_4cell = []
                for pid in postings:
                    ss, se, _ = ckls[slot_grp][pid]
                    if (unit_hp_class(unit_data, ss) != first_ctx[2] or
                            unit_hp_class(unit_data, se) != last_ctx[2]):
                        cands_4cell.append((10000.0, pid, ss, se))
                        continue
                    cost4 = compute_4cell_ccos(unit_data, ss, se, first_ctx,
                                                last_ctx, ccos_tables, n_labels)
                    if cost4 is None:
                        cands_4cell.append((10000.0, pid, ss, se))
                    else:
                        cands_4cell.append((_f32(cost4), pid, ss, se))

                final_threshold, _, accepted = histogram_prune(
                    cands_4cell, anchor_type)
                surviving = [(c, p, s, e) for (c, p, s, e) in accepted
                             if c <= final_threshold]

                syl_arr = as_ev.get("syl_idx_0_to_last")
                ranked = []
                for cost4, pid, ss, se in surviving:
                    cost = compute_anchor_full_cost(
                        unit_data, ss, se, first_ctx, last_ctx,
                        first_hp, last_hp, workspace, voicing,
                        cart_per_hp, ccos_tables, n_labels, eng_mat, syl_arr)
                    if cost is not None:
                        ranked.append((cost, ss, se, pid))

                ranked.sort()
                actual = set((c["uid"], c["join_key"]) for c in cands)
                n_actual = len(actual)
                pred = set((s, e) for (_, s, e, _) in ranked[:n_actual])

                n_anchors += 1
                if pred == actual:
                    n_match += 1
                elif len(sample) < 5:
                    cand_costs = {(s, e): round(c, 3)
                                  for c, s, e, _ in ranked
                                  if (s, e) in actual}
                    captured = {(c["uid"], c["join_key"]): round(c["pre_dp"], 4)
                                for c in cands}
                    sample.append((fname, utt_idx, slot_idx, slot_grp, slot_name,
                                   f"frida_first_hp={first_hp} last_hp={last_hp}",
                                   f"actual={sorted(actual)}",
                                   f"my_costs={cand_costs}",
                                   f"captured={captured}",
                                   f"surviving={len(surviving)}/{len(cands_4cell)}",
                                   f"top5={[(round(c, 3), s, e) for c, s, e, _ in ranked[:5]]}"))

    print(f"slots: {n_anchors}")
    print(f"top-N pre_dp == actual: {n_match}/{n_anchors}  "
          f"({n_match / max(n_anchors, 1) * 100:.2f}%)")
    print()
    for m in sample:
        for x in m: print(f"   {x}")
        print()


if __name__ == "__main__":
    main()
