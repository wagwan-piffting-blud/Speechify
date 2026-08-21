/* Rebuild a voice's `hash` chunk with SUBSTITUTED costs, holding the domain
 * fixed at the input table's own pair set.
 *
 * The point is to price the cost function in the only currency that matters:
 * how many units the Viterbi actually picks differently. Holding the domain
 * fixed is what isolates the cost -- which pairs get a cell is a separate,
 * already-solved question (SPEC_S4_hash.md, "(a) Domain").
 *
 * Modes, and why each one is here:
 *   vendor   identity. The NO-OP CONTROL: repack the input's own costs. Any
 *            pick difference at all means the harness is lying.
 *   const    every non-continuation pair gets one value. This is the FLOOR:
 *            what selection does with NO cost information whatsoever. It bounds
 *            the whole question -- our error cannot cost more than deleting the
 *            signal entirely. Measured on jill it also BEATS every metric we
 *            can currently compute, which is why it is what a new voice ships.
 *   shuffle  the input's own cost multiset, permuted. Actively wrong rather
 *            than merely uninformative.
 *   mfcc     our computed cost, calibrated so its RAW median matches the
 *            input's, because selection is known to be scale-sensitive and
 *            this experiment is about the metric, not the gauge.
 *   file     per-pair costs supplied externally, in this tool's own
 *            enumeration order (see --dump-pairs).
 *
 * Natural continuations keep their hard zero in every mode: that is structure,
 * not a cost value, and changing it would confound the comparison.
 *
 * ---------------------------------------------------------------------------
 * --fix-domain, and why a voice of ours needs it
 *
 * Both vendors satisfy `cost == 0 <=> uid_right == uid_left + 1` EXACTLY --
 * jill 169,151/169,151/169,151, tom 150,927 likewise. Our own tables do not,
 * and the two ways they fail are both invisible to every existing gate:
 *
 *   1. GHOST CELLS. `vb_hash_pack.c` leaves its cell array uninitialised, so
 *      every unwritten cell reads back as key 0 -- and 0 is a valid uid_right.
 *      The cells that happen to land inside row 0's readable window RESOLVE:
 *      the engine returns their cost, 0.0, for a join into unit 0. Measured on
 *      donnart: 196,850 of them, i.e. 196,850 left units handed a free join
 *      into one specific unit. The rest are dead weight -- 5.10 M cells,
 *      ~41 MB of a 78.7 MB chunk, that no lookup can ever reach.
 *   2. SILENT-BOUNDARY ZEROS. A mean-removed band-energy distance scores two
 *      digitally silent windows at exactly 0, which vb_hash_cost's own header
 *      flags. Those are free joins too, and they are not continuations.
 *
 * The vendor invariant separates both from real pairs with no heuristic:
 * uid_right == 0 can never be a continuation (it would need uid_left == -1),
 * and neither can any other zero-cost non-adjacent pair. So --fix-domain
 * drops every non-continuation zero and adds back every genuine continuation
 * at 0, which is the vendor's structure exactly.
 *
 * The continuation set is the engine's own same-rec test (dag_join_cb):
 * (l, l+1) admitted iff unit l+1 carries flag_b.
 *
 *   spfy_vb_hashgen <voice.vin> [voice.vdb] [voice.vcf] --mode M --out FILE
 */

#include "../vb/edge_frames.h"
#include "../vb/join_cost.h"
#include "../usel/hash.h"
#include "../usel/hash_build.h"
#include "../voice/voice.h"
#include "../voice/unit_table.h"
#include "../../include/spfy/spfy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmp_f(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x < y) ? -1 : (x > y);
}

int main(int argc, char **argv)
{
    const char *vinp = NULL, *vdbp = NULL, *vcfp = NULL;
    const char *mode = "vendor", *outp = NULL, *costp = NULL, *pairsp = NULL;
    int positional = 0, fix_domain = 0, rows_units = 0;
    double const_cost = -1.0;      /* <0 = use the measured median */

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (!strcmp(a, "--mode") && i + 1 < argc)      mode = argv[++i];
        else if (!strcmp(a, "--out") && i + 1 < argc)  outp = argv[++i];
        else if (!strcmp(a, "--costs") && i + 1 < argc) costp = argv[++i];
        else if (!strcmp(a, "--dump-pairs") && i + 1 < argc) pairsp = argv[++i];
        else if (!strcmp(a, "--const-cost") && i + 1 < argc)
            const_cost = atof(argv[++i]);
        else if (!strcmp(a, "--fix-domain")) fix_domain = 1;
        else if (!strcmp(a, "--rows-units")) rows_units = 1;
        else if (a[0] == '-') { fprintf(stderr, "unknown option %s\n", a); return 2; }
        else if (positional == 0) { vinp = a; ++positional; }
        else if (positional == 1) { vdbp = a; ++positional; }
        else if (positional == 2) { vcfp = a; ++positional; }
        else { fprintf(stderr, "unexpected argument %s\n", a); return 2; }
    }
    if (!vinp || !outp) {
        fprintf(stderr, "usage: %s <voice.vin> [voice.vdb] [voice.vcf] "
                        "--mode vendor|const|shuffle|mfcc|file --out FILE\n"
                        "  --fix-domain       enforce the vendor invariant\n"
                        "                     cost==0 <=> r==l+1: drop every\n"
                        "                     non-continuation zero (ghost cells\n"
                        "                     and silent-boundary zeros) and add\n"
                        "                     back every genuine continuation\n"
                        "  --rows-units       n_rows = unit count, not the\n"
                        "                     input's padded row count\n"
                        "  --const-cost X     the constant for --mode const\n"
                        "                     (default: measured median)\n"
                        "  --costs FILE       per-pair costs for --mode file\n"
                        "  --dump-pairs FILE  write (l, r, cost) in the\n"
                        "                     enumeration order --costs expects\n",
                        argv[0]);
        return 2;
    }
    if (!strcmp(mode, "mfcc") && !vdbp) {
        fprintf(stderr, "--mode mfcc needs the .vdb\n");
        return 2;
    }

    spfy_vin_t vin = {0};
    spfy_vdb_t vdb = {0};
    spfy_vb_frames_t fr = {0};
    spfy_hash_pair *pairs = NULL;
    float *ours = NULL, *sorted = NULL;
    uint8_t *blob = NULL;
    spfy_hash_table tbl = {0};
    spfy_unit_table_t ut = {0};
    spfy_hash_t h;
    int have_vdb = 0;
    uint32_t n_units = 0, out_rows = 0;

    int rc = spfy_vin_load(vinp, &vin);
    if (rc != SPFY_OK) { fprintf(stderr, "vin_load %d\n", rc); return 1; }

    rc = spfy_unit_table_load(&vin, &ut);
    if (rc != SPFY_OK) { fprintf(stderr, "unit_table_load %d\n", rc); goto done; }
    n_units = ut.n_units;

    if (!strcmp(mode, "mfcc")) {
        rc = spfy_vdb_load(vdbp, &vdb);
        if (rc != SPFY_OK) { fprintf(stderr, "vdb_load %d\n", rc); goto done; }
        have_vdb = 1;
    }

    float jw = 1.75f, jo = 0.15f;
    if (vcfp) {
        spfy_vcf_t vcf = {0};
        if (spfy_vcf_load(vcfp, &vcf) == SPFY_OK) {
            jw = spfy_vcf_f32(&vcf, "JOIN_COST_WEIGHT", jw);
            jo = spfy_vcf_f32(&vcf, "JOIN_COST_OFFSET", jo);
            spfy_vcf_free(&vcf);
        }
    }

    rc = spfy_hash_load(&vin, &h);
    if (rc != SPFY_OK) { fprintf(stderr, "hash_load %d\n", rc); goto done; }

    size_t live = 0;
    for (uint32_t i = 0; i < h.n_cells; ++i)
        if (spfy_hash_cell_a(&h, i) != 0xFFFFFFFFu) ++live;

    /* Worst case the recovered set survives intact AND every unit contributes
     * a continuation, so size for both. */
    pairs = (spfy_hash_pair *)malloc((live + n_units + 1u) * sizeof *pairs);
    if (!pairs) { rc = SPFY_E_NOMEM; goto done; }

    /* --- recover the domain by inverting the addressing rule ---------------
     * A cell is REACHABLE only if the engine's own lookup can land on it:
     * idx = rows[r] + l with 0 <= l < n_units. Anything else is a cell that
     * costs 8 bytes and can never be read -- and counting it as a pair is how
     * a repack carries the waste forward. */
    size_t n = 0, n_cont = 0, dead_oob = 0, dead_neg = 0, dead_left = 0;
    for (uint32_t i = 0; i < h.n_cells; ++i) {
        uint32_t r = spfy_hash_cell_a(&h, i);
        if (r == 0xFFFFFFFFu) continue;
        if (r >= h.n_rows || r >= n_units) { ++dead_oob; continue; }
        uint32_t base = spfy_hash_row(&h, r);
        if (i < base) { ++dead_neg; continue; }
        uint32_t l = i - base;
        if (l >= n_units) { ++dead_left; continue; }
        pairs[n].uid_right = r;
        pairs[n].uid_left  = l;
        pairs[n].cost      = spfy_hash_cell_b(&h, i);
        if (r == l + 1u) ++n_cont;
        ++n;
    }
    printf("units %u   input n_rows %u  n_cells %u  live %zu\n",
           n_units, h.n_rows, h.n_cells, live);
    printf("domain: %zu reachable (%zu natural continuations)\n", n, n_cont);
    if (dead_oob || dead_neg || dead_left)
        printf("        DEAD cells dropped: %zu (key out of range %zu, "
               "left<0 %zu, left>=units %zu)\n",
               dead_oob + dead_neg + dead_left, dead_oob, dead_neg, dead_left);

    if (fix_domain) {
        /* (1) Drop every non-continuation zero. A zero cost that is not an
         *     adjacency is a free join the vendor never grants, and it is
         *     exactly the signature of both failure modes: an uninitialised
         *     cell claiming row 0, and a silent-window distance of 0. */
        size_t dropped = 0, k = 0;
        for (size_t i = 0; i < n; ++i) {
            int is_cont = (pairs[i].uid_right == pairs[i].uid_left + 1u);
            if (!is_cont && pairs[i].cost == 0.0f) { ++dropped; continue; }
            pairs[k++] = pairs[i];
        }
        n = k;

        /* (2) Add back every genuine continuation, using the engine's own
         *     same-rec test so the table agrees with dag_join_cb by
         *     construction rather than by discipline. Mark the ones already
         *     present so the pass cannot double-insert. */
        uint8_t *have = (uint8_t *)calloc(n_units, 1);
        if (!have) { rc = SPFY_E_NOMEM; goto done; }
        for (size_t i = 0; i < n; ++i)
            if (pairs[i].uid_right == pairs[i].uid_left + 1u)
                have[pairs[i].uid_right] = 1;

        size_t added = 0, no_flag = 0;
        for (uint32_t u = 1; u < n_units; ++u) {
            spfy_unit_record_t r;
            if (spfy_unit_record_get(&ut, u, &r) != SPFY_OK) continue;
            if (!r.flag_b) { ++no_flag; continue; }
            if (have[u]) continue;
            pairs[n].uid_right = u;
            pairs[n].uid_left  = u - 1u;
            pairs[n].cost      = 0.0f;
            ++n; ++added;
        }
        free(have);

        n_cont = 0;
        for (size_t i = 0; i < n; ++i)
            if (pairs[i].uid_right == pairs[i].uid_left + 1u) ++n_cont;
        printf("fix-domain: dropped %zu non-continuation zeros, "
               "added %zu continuations (%zu units lack flag_b)\n",
               dropped, added, no_flag);
        printf("            -> %zu pairs, %zu continuations (%.2f%%)\n",
               n, n_cont, 100.0 * (double)n_cont / (double)(n ? n : 1));
    }

    if (!n) { fprintf(stderr, "empty domain\n"); rc = SPFY_E_FORMAT; goto done; }

    /* Reference statistics over the NON-continuation pairs only. */
    sorted = (float *)malloc(n * sizeof *sorted);
    if (!sorted) { rc = SPFY_E_NOMEM; goto done; }
    size_t nn = 0;
    for (size_t i = 0; i < n; ++i)
        if (pairs[i].uid_right != pairs[i].uid_left + 1u) sorted[nn++] = pairs[i].cost;
    qsort(sorted, nn, sizeof *sorted, cmp_f);
    float v_med = nn ? sorted[nn / 2] : 1.0f;
    printf("input non-continuation median %.4f  (p25 %.4f  p99 %.4f)\n",
           v_med, nn ? sorted[nn / 4] : 0.0f, nn ? sorted[(nn * 99) / 100] : 0.0f);

    if (!strcmp(mode, "vendor")) {
        /* identity */
    } else if (!strcmp(mode, "const")) {
        float c = (const_cost >= 0.0) ? (float)const_cost : v_med;
        printf("const: %.4f on every non-continuation pair%s\n",
               c, (const_cost >= 0.0) ? " (--const-cost)" : " (measured median)");
        for (size_t i = 0; i < n; ++i)
            if (pairs[i].uid_right != pairs[i].uid_left + 1u) pairs[i].cost = c;
    } else if (!strcmp(mode, "shuffle")) {
        /* Fisher-Yates over the non-continuation costs, fixed seed. */
        size_t *idx = (size_t *)malloc(nn * sizeof *idx);
        if (!idx) { rc = SPFY_E_NOMEM; goto done; }
        size_t k = 0;
        for (size_t i = 0; i < n; ++i)
            if (pairs[i].uid_right != pairs[i].uid_left + 1u) idx[k++] = i;
        uint32_t s = 12345u;
        for (size_t i = nn; i > 1; --i) {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            size_t j = s % i;
            float t = pairs[idx[i - 1]].cost;
            pairs[idx[i - 1]].cost = pairs[idx[j]].cost;
            pairs[idx[j]].cost = t;
        }
        free(idx);
    } else if (!strcmp(mode, "mfcc")) {
        spfy_vb_cfg_t cfg;
        spfy_vb_cfg_default(&cfg);
        cfg.anchor = SPFY_VB_ANCHOR_CENTER;
        rc = spfy_vb_frames_build_ex(&vin, &vdb, 8000u, &cfg, &fr);
        if (rc != SPFY_OK) { fprintf(stderr, "frames_build %d\n", rc); goto done; }
        spfy_jc_t jc;
        spfy_vb_frames_bind(&fr, &jc);
        rc = spfy_jc_derive_weights(&jc, 1.0f);
        if (rc != SPFY_OK) goto done;

        ours = (float *)malloc(n * sizeof *ours);
        if (!ours) { rc = SPFY_E_NOMEM; goto done; }
        float *raws = (float *)malloc((nn ? nn : 1) * sizeof *raws);
        if (!raws) { rc = SPFY_E_NOMEM; goto done; }
        size_t k = 0;
        for (size_t i = 0; i < n; ++i) {
            ours[i] = spfy_jc_raw(&jc, pairs[i].uid_left, pairs[i].uid_right);
            if (pairs[i].uid_right != pairs[i].uid_left + 1u) raws[k++] = ours[i];
        }
        qsort(raws, k, sizeof *raws, cmp_f);
        float o_med = k ? raws[k / 2] : 1.0f;
        free(raws);

        /* Calibrate the gauge, so the comparison is about the METRIC. */
        double v_raw_med = ((double)v_med - jo) / (jw ? jw : 1.0);
        double scale = (o_med > 0.0f) ? v_raw_med / (double)o_med : 1.0;
        printf("mfcc: raw median ours %.4f -> vendor %.4f, scale %.4f\n",
               (double)o_med, v_raw_med, scale);
        for (size_t i = 0; i < n; ++i) {
            if (pairs[i].uid_right == pairs[i].uid_left + 1u) { pairs[i].cost = 0.0f; continue; }
            double c = (double)ours[i] * scale * jw + jo;
            pairs[i].cost = (float)(c > 0.0 ? c : 0.0);
        }
    } else if (!strcmp(mode, "file")) {
        /* Costs supplied per pair, in this tool's own enumeration order. Lets
         * an arm be built from anything computable outside C -- a fitted
         * metric, or the vendor's costs degraded to a chosen correlation --
         * without porting each one. */
        if (!costp) { fprintf(stderr, "--mode file needs --costs FILE\n");
                      rc = SPFY_E_INVAL; goto done; }
        FILE *cf = fopen(costp, "rb");
        if (!cf) { fprintf(stderr, "cannot read %s\n", costp); rc = SPFY_E_IO; goto done; }
        float *cin = (float *)malloc(n * sizeof *cin);
        if (!cin) { fclose(cf); rc = SPFY_E_NOMEM; goto done; }
        size_t got = fread(cin, sizeof *cin, n, cf);
        fclose(cf);
        if (got != n) {
            fprintf(stderr, "%s holds %zu costs, expected %zu\n", costp, got, n);
            free(cin); rc = SPFY_E_INVAL; goto done;
        }
        for (size_t i = 0; i < n; ++i)
            pairs[i].cost = (pairs[i].uid_right == pairs[i].uid_left + 1u)
                          ? 0.0f : cin[i];
        free(cin);
        printf("costs read from %s\n", costp);
    } else {
        fprintf(stderr, "unknown mode %s\n", mode);
        rc = SPFY_E_INVAL;
        goto done;
    }

    if (pairsp) {
        FILE *pf = fopen(pairsp, "wb");
        if (!pf) { fprintf(stderr, "cannot write %s\n", pairsp); rc = SPFY_E_IO; goto done; }
        uint32_t nn32 = (uint32_t)n;
        fwrite("HGP1", 1, 4, pf);
        fwrite(&nn32, sizeof nn32, 1, pf);
        for (size_t i = 0; i < n; ++i) fwrite(&pairs[i].uid_left, 4, 1, pf);
        for (size_t i = 0; i < n; ++i) fwrite(&pairs[i].uid_right, 4, 1, pf);
        for (size_t i = 0; i < n; ++i) fwrite(&pairs[i].cost, 4, 1, pf);
        fclose(pf);
        printf("pair list written to %s (%zu pairs, enumeration order)\n", pairsp, n);
    }

    /* uid_right is a unit index, so a row per unit is all a reader can ask
     * for. jill ships 560,534 rows for 185,475 units -- 1.5 MB of pure
     * padding -- and there is no reason to inherit that. */
    out_rows = rows_units ? n_units : h.n_rows;
    if (out_rows < n_units) out_rows = n_units;

    rc = spfy_hash_build(pairs, n, out_rows, SPFY_HASH_ORDER_FFD, &tbl);
    if (rc != SPFY_OK) { fprintf(stderr, "hash_build %d\n", rc); goto done; }

    size_t blob_n = 0;
    rc = spfy_hash_serialise(&tbl, &blob, &blob_n);
    if (rc != SPFY_OK) { fprintf(stderr, "serialise %d\n", rc); goto done; }

    FILE *f = fopen(outp, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", outp); rc = SPFY_E_IO; goto done; }
    fwrite(blob, 1, blob_n, f);
    fclose(f);
    printf("mode %-8s -> %s\n", mode, outp);
    printf("  n_rows %u  n_cells %u  fill %.1f%%  %zu bytes"
           "  (%.1f B per pair)\n",
           tbl.n_rows, tbl.n_cells,
           100.0 * (double)n / (double)(tbl.n_cells ? tbl.n_cells : 1),
           blob_n, (double)blob_n / (double)n);
    rc = SPFY_OK;

done:
    free(pairs); free(ours); free(sorted); free(blob);
    spfy_hash_table_free(&tbl);
    spfy_vb_frames_free(&fr);
    if (have_vdb) spfy_vdb_free(&vdb);
    spfy_vin_free(&vin);
    return rc == SPFY_OK ? 0 : 1;
}
