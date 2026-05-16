"""Anchor pre_dp v6 -- adds histogram pruning per FUN_08e8adc0.

Histogram algorithm (decoded from disassembly at 0x08e8adc0..0x08e8b46e):

Phase 1: Build cand list with 4-cell ccos costs.
   For each posting (ss, se):
     if boundary matches:
       cost = 4-cell ccos sum * w_44
       if cost < threshold:                  # threshold = norm + best
         if cost < best:
           best = cost
           threshold = norm + best
         emit (cost, posting_idx)
     else:
       emit (10000.0, posting_idx)

Phase 2: 50-bin histogram by (cost - best) * 50 / threshold:
   bins = [0]*50
   scale = 50 / (norm + best)
   for (cost, _) in cands:
     bin = round((cost - best) * scale)
     if 0 <= bin < 50: bins[bin] += 1

Phase 3: Scan bins for early-exit threshold:
   cum = 0
   final_slack = norm     # default if loop exhausts
   for k in 1..49:
     cum += bins[k-1]
     slack = norm - cum * norm2
     bin_dist = (k-1) * DAT_971d8   # 0.1
     # NOTE: at sub-test for k, the formula uses (k-1)*0.1, NOT k*0.1
     # because the value pushed via LEA EAX,[EDX-1] is c-1, where the
     # tested k = c-1 for first sub-test, c for second, etc.
     # Asm shows ST0 = (k_displayed) * 0.1 where k_displayed runs 1..49.
     # See disassembly at 0x08e8b243..0x08e8b25c.
     if slack > bin_dist:
       final_slack = slack
       break
   # If loop exhausted, final_slack = slack at last iter (= norm - max_cum * norm2).

Phase 4: Filter cands where cost <= best + final_slack.

Then the cost stack from FUN_08e8ce60:
  total = ccos_4cell                           # for surviving cands
        + FLAG_sum * w_3c * 0.01
        + SP_span (FUN_08e897b0)
        + D_span POOLED (FUN_08e89530, if voice+0xd0 != 0)
        + F0_span Mahalanobis (FUN_08e893b0, if voice+0xcc != 0)
"""
import os
import json, glob, os, struct, sys
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

# Frida-confirmed values (from anchor_score_hook capture).
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
ANCHOR_NORM_TYPE2 = 0.7      # w_54 (syl)
ANCHOR_NORM2_TYPE2 = 0.005   # w_58
ANCHOR_NORM_TYPE4 = 0.7      # w_5c (word)
ANCHOR_NORM2_TYPE4 = 0.005   # w_60


def s_ctx_remap(c, n):
    if c is None:
        return None
    label = c >> 1
    if label >= n:
        return None
    return TOM_FORWARD_SWAP.get(label, label)


def hp_class_remap(c, n):
    if c is None:
        return None
    side = c & 1
    label = c >> 1
    if label >= n:
        return None
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


def unit_hp_class(unit_data, uid):
    rec = unit_data[uid * 29:(uid + 1) * 29]
    label = TOM_INVERSE_SWAP.get(rec[0x14], rec[0x14])
    isfh = rec[0x15]
    return label * 2 + (0 if isfh else 1)


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
            if r is None:
                continue
            for c, v in cells.items():
                ci = idx.get(c)
                if ci is not None:
                    m[r][ci] = v
        out[mname] = {"data": m, "n": n}
    return out


def compute_4cell_ccos(unit_data, ss, se, first_ctx, last_ctx,
                        ccos_tables, n_labels):
    """Phase 1 cost: 4-cell ccos boundary check (for histogram input)."""
    hp_first_remap = hp_class_remap(first_ctx[2], n_labels)
    hp_last_remap = hp_class_remap(last_ctx[2], n_labels)
    if hp_first_remap is None or hp_last_remap is None:
        return None
    # Per FUN_08e8adc0 disasm. Workspace per-hp block at USelNet+0x14
    # mirrors slice ctx layout: +0x04=ctx[0], +0x08=ctx[1], +0x10=ctx[3],
    # +0x14=ctx[4]. So:
    #   slot 0 row = s_ctx_remap[ctx[0] of first_hp]
    #   slot 1 row = s_ctx_remap[ctx[1] of first_hp]
    #   slot 2 row = s_ctx_remap[ctx[3] of last_hp]
    #   slot 3 row = s_ctx_remap[ctx[4] of last_hp]
    sl_slot0 = s_ctx_remap(first_ctx[0], n_labels)
    sl_slot1 = s_ctx_remap(first_ctx[1], n_labels)
    sl_slot2 = s_ctx_remap(last_ctx[3], n_labels)
    sl_slot3 = s_ctx_remap(last_ctx[4], n_labels)
    if any(x is None for x in (sl_slot0, sl_slot1, sl_slot2, sl_slot3)):
        return None

    matrix_floats = n_labels * n_labels
    ss_rec = unit_data[ss * 29:(ss + 1) * 29]
    se_rec = unit_data[se * 29:(se + 1) * 29]
    pc_ss = [signed8(ss_rec[0x17 + k]) for k in range(4)]
    pc_se = [signed8(se_rec[0x17 + k]) for k in range(4)]

    def cell(hp, slot, row, col_signed):
        base = (hp * 4 + slot) * matrix_floats
        off = row * n_labels + col_signed
        if off < 0 or off >= matrix_floats:
            return 0.0
        return ccos_tables[base + off]

    # Per asm 0x08e8b03b/b040/b053/b05c: cand col uses SS phone_ctx[0..1]
    # for slots 0/1, SE phone_ctx[2..3] for slots 2/3.
    return (cell(hp_first_remap, 0, sl_slot0, pc_ss[0]) +
            cell(hp_first_remap, 1, sl_slot1, pc_ss[1]) +
            cell(hp_last_remap, 2, sl_slot2, pc_se[2]) +
            cell(hp_last_remap, 3, sl_slot3, pc_se[3])) * W_CCOS


def histogram_prune(cands_4cell, anchor_type, n_total):
    """Run FUN_08e8adc0's histogram pruning. Returns (final_threshold, best).

    cands_4cell: list of (cost_4cell, posting_idx, ss, se)
    Includes both boundary-match (real cost) and boundary-mismatch (10000).
    """
    if anchor_type == 2:
        norm, norm2 = ANCHOR_NORM_TYPE2, ANCHOR_NORM2_TYPE2
    else:
        norm, norm2 = ANCHOR_NORM_TYPE4, ANCHOR_NORM2_TYPE4

    # Phase 1 (TRY: no dynamic threshold tightening, just collect all):
    best = min((c for c, _, _, _ in cands_4cell if c < 10000), default=10000.0)
    accepted = list(cands_4cell)

    # Phase 2: build histogram
    bins = [0] * 50
    if not accepted:
        return float("inf"), best, accepted
    scale = DAT_98A24 / (norm + best) if (norm + best) > 0 else 1.0
    for cost, _, _, _ in accepted:
        bin_idx = int(round((cost - best) * scale))
        if 0 <= bin_idx < 50:
            bins[bin_idx] += 1

    # Phase 3: scan bins -- 49 sub-tests, k = 1..49
    # At sub-test k: cum = sum(bin[0..k-1]), slack = norm - cum * norm2.
    # Exit if slack > (k-1)*0.1.   <-- per asm at 0x08e8b243 (LEA [EDX-1])
    # Hmm, actually re-read: first sub-test in iter 1 uses (c-1)*0.1 with c=2,
    # so k=1 sub-test compares slack vs 0.1. So bin_dist = k * 0.1 where k=1..49.
    # Wait let me recount: c starts at 2; sub-test 0 in iter 1 uses (c-1)*0.1
    # = 1*0.1 = 0.1. So that's for k_display = c-1 = 1.
    # Sub-test 1 uses (c)*0.1 = 2*0.1 = 0.2. k_display = c = 2.
    # So bin_dist for k_display = k_display * 0.1.

    cum = 0
    final_slack = norm   # default if loop exhausts (no early exit)
    for k in range(1, 50):
        cum += bins[k - 1]
        slack = norm - cum * norm2
        bin_dist = k * DAT_971D8
        if slack > bin_dist:
            final_slack = slack
            break

    final_threshold = best + final_slack
    return final_threshold, best, accepted


def compute_anchor_full_cost(unit_data, ss, se, first_ctx, last_ctx, hp_seq,
                              workspace, voicing, cart_walks, ccos_tables,
                              n_labels, eng_mat, captured_syl_array=None):
    """Full FUN_08e8ce60 cost = 4cell + FLAG + SP + D + F0."""
    ccos_4cell = compute_4cell_ccos(unit_data, ss, se, first_ctx, last_ctx,
                                      ccos_tables, n_labels)
    if ccos_4cell is None:
        return None

    flag_sum = 0
    sp_cost = 0.0
    d_delta_sum = 0.0
    d_var_sum = 0.0
    f0_cost = 0.0

    target_idx = 0
    target_hp = hp_seq[0]
    cur_syl = (captured_syl_array[hp_seq[0]] if captured_syl_array else None)

    for u_idx, u in enumerate(range(ss, se + 1)):
        u_rec = unit_data[u * 29:(u + 1) * 29]

        # Advance-on-duplicate using captured syl_idx.
        if (captured_syl_array is not None and u_idx > 0 and
                u_rec[0x15] != 0 and target_idx < len(hp_seq) - 1):
            while target_idx < len(hp_seq) - 1:
                target_idx += 1
                target_hp = hp_seq[target_idx]
                ns = (captured_syl_array[target_hp]
                      if target_hp < len(captured_syl_array) else cur_syl)
                if ns != cur_syl:
                    cur_syl = ns
                    break
        elif captured_syl_array is None:
            target_hp = hp_seq[u_idx] if u_idx < len(hp_seq) else hp_seq[-1]

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
                if mat is None:
                    continue
                ri = ws[ws_off]
                ci = u_rec[byte_off]
                if ri >= mat["n"] or ci >= mat["n"]:
                    continue
                sp_cost += mat["data"][ri][ci] * SP_W[k]

        cart_t = cart_walks.get(target_hp, {})
        if "durt" in cart_t:
            d_mean, d_var = cart_t["durt"]
            d_delta_sum += u_rec[0x13] - d_mean
            inv_var = DAT_8E9857C / d_var if d_var != 0 else 0
            d_var_sum += inv_var * inv_var

        unit_hpc = u_rec[0x14] * 2 + (0 if u_rec[0x15] else 1)
        v_active = (voicing[unit_hpc] if voicing and 0 <= unit_hpc < len(voicing) else 1)
        if v_active != 0:
            if "f0tr" in cart_t:
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

    n_anchors = 0
    n_match = 0
    sample = []

    for fe_path in sorted(glob.glob(
            os.path.join(CORPUS, "fe_tree", "text_*.jsonl"))):
        fname = os.path.basename(fe_path)
        vit_path = os.path.join(CORPUS, "viterbi_dp", fname)
        prsl_path = os.path.join(CORPUS, "prsl_slot", fname)
        is_path = os.path.join(CORPUS, "inner_scorer", fname)
        cw_path = os.path.join(CORPUS, "cart_walks", fname)
        if not all(os.path.exists(p) for p in (vit_path, prsl_path, is_path, cw_path)):
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
        as_path = os.path.join(CORPUS, "anchor_score", fname)
        captured_syl = {}
        if os.path.exists(as_path):
            with open(as_path, encoding="utf-8") as f:
                for l in f:
                    if not l.strip():
                        continue
                    ev = json.loads(l)
                    if "syl_idx_0_to_last" in ev:
                        key = (ev.get("type"), ev.get("first_hp"),
                               ev.get("last_hp"))
                        captured_syl.setdefault(key, ev["syl_idx_0_to_last"])

        voicing = None
        for ev in is_evs_all:
            if ev.get("type") == "join_consts":
                voicing = ev.get("voice_5fc_per_uid", {}).get("by_hp_class")
                break
        is_evs = [e for e in is_evs_all if e.get("type") == "inner_scorer"]

        def grp(evs):
            utts, cur = [], []
            for e in evs:
                if e.get("slot") == 0 and cur:
                    utts.append(cur)
                    cur = []
                cur.append(e)
            if cur:
                utts.append(cur)
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

            anchor_first_hp_is = {}
            anchor_last_hp_is = {}
            is_seen = 0
            for vi, slot in enumerate(slots):
                cands = slot.get("cands") or []
                if not cands:
                    continue
                if cands[0]["uid"] == cands[0]["join_key"]:
                    is_seen += 1
                else:
                    span_len = cands[0]["join_key"] - cands[0]["uid"] + 1
                    anchor_first_hp_is[vi] = is_seen - span_len
                    anchor_last_hp_is[vi] = is_seen - 1

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
                first_hp = anchor_first_hp_is.get(slot_idx)
                last_hp = anchor_last_hp_is.get(slot_idx)
                if first_hp is None or last_hp is None or first_hp > last_hp:
                    continue
                first_ctx = hp_ctx.get(first_hp)
                last_ctx = hp_ctx.get(last_hp)
                if first_ctx is None or last_ctx is None:
                    continue

                hp_seq = list(range(first_hp, last_hp + 1))

                cand0 = cands[0]
                slot_name = None
                slot_grp = None
                anchor_type = None
                for grpn, atype in (("_SYL_", 2), ("_WORD_", 4)):
                    for pid, txt in enumerate(ckls[grpn]):
                        ss_t, se_t, t = txt
                        if ss_t == cand0["uid"] and se_t == cand0["join_key"]:
                            slot_name = t
                            slot_grp = grpn
                            anchor_type = atype
                            break
                    if slot_name:
                        break
                if not slot_name:
                    continue
                postings = cklx[slot_grp].get(slot_name, [])
                if not postings:
                    continue

                # Phase 1: 4-cell ccos for each posting + boundary check
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
                        cands_4cell.append((cost4, pid, ss, se))

                # Phase 2-3: histogram pruning -> filter by threshold
                final_threshold, best4, accepted = histogram_prune(
                    cands_4cell, anchor_type, len(cands_4cell))

                # Phase 4: filter accepted to those <= final_threshold
                surviving = [(c, p, s, e) for (c, p, s, e) in accepted
                             if c <= final_threshold]

                # Phase 5: compute FULL cost for surviving cands; rank
                captured_syl_arr = captured_syl.get(
                    (anchor_type, first_hp, last_hp))
                ranked = []
                for cost4, pid, ss, se in surviving:
                    cost = compute_anchor_full_cost(
                        unit_data, ss, se, first_ctx, last_ctx, hp_seq,
                        workspace, voicing, cart_per_hp, ccos_tables,
                        n_labels, eng_mat, captured_syl_arr)
                    if cost is None:
                        continue
                    ranked.append((cost, ss, se, pid))

                ranked.sort()
                actual = set((c["uid"], c["join_key"]) for c in cands)
                n_actual = len(actual)
                pred = set((s, e) for (_, s, e, _) in ranked[:n_actual])

                n_anchors += 1
                if pred == actual:
                    n_match += 1
                elif len(sample) < 6:
                    cand_costs = {(s, e): round(c, 3)
                                  for c, s, e, _ in ranked
                                  if (s, e) in actual}
                    captured = {(c["uid"], c["join_key"]): round(c["pre_dp"], 4)
                                for c in cands}
                    sample.append((fname, utt_idx, slot_idx, slot_grp, slot_name,
                                   f"actual={sorted(actual)}",
                                   f"my_costs={cand_costs}",
                                   f"captured={captured}",
                                   f"surviving={len(surviving)}/{len(cands_4cell)}",
                                   f"final_thr={round(final_threshold, 3)} best4={round(best4, 3)}",
                                   f"top5={[(round(c, 3), s, e) for c, s, e, _ in ranked[:5]]}"))

    print(f"slots: {n_anchors}")
    print(f"top-N pre_dp == actual: {n_match}/{n_anchors}  "
          f"({n_match / max(n_anchors, 1) * 100:.2f}%)")
    print()
    for m in sample:
        for x in m:
            print(f"   {x}")
        print()


if __name__ == "__main__":
    main()
