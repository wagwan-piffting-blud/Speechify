/* Precompute a voice's join edge frames for spfy_synth's SPFY_JOIN_EDGE mode.
 *
 * SWIttsUSel.dll carries two Viterbi paths: ViterbiWithJoinCache (the `hash`
 * table, where a pair outside the table costs MISSING_JOIN_COST) and the
 * edge-frames Viterbi, which prices ANY pair live from per-unit boundary
 * frames. No 3.0.5 voice ships edge frames. This builds them from the VDB with
 * the recovered 3-point formula, calibrated so the raw median over the voice's
 * own hash pairs matches the table's (same gauge as spfy_vb_hashgen --mode mfcc).
 *
 *   spfy_edge_cache <voice.vin> <voice.vdb> [voice.vcf] --out FILE
 *
 * File: "EDGE" u32 n_units u32 dim f32 f0_gate f32 scale f32 join_w f32 join_off
 *       f32 weights[dim]  f32 frames[n_units * 2 * dim]
 */

#include "../vb/edge_frames.h"
#include "../vb/join_cost.h"
#include "../usel/hash.h"
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
    const char *vinp = NULL, *vdbp = NULL, *vcfp = NULL, *outp = NULL;
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) outp = argv[++i];
        else if (positional == 0) { vinp = argv[i]; ++positional; }
        else if (positional == 1) { vdbp = argv[i]; ++positional; }
        else if (positional == 2) { vcfp = argv[i]; ++positional; }
    }
    if (!vinp || !vdbp || !outp) {
        fprintf(stderr, "usage: %s <voice.vin> <voice.vdb> [voice.vcf] --out FILE\n", argv[0]);
        return 2;
    }

    spfy_vin_t vin = {0};
    spfy_vdb_t vdb = {0};
    spfy_vb_frames_t fr = {0};
    spfy_hash_t h;
    int rc = spfy_vin_load(vinp, &vin);
    if (rc != SPFY_OK) { fprintf(stderr, "vin_load %d\n", rc); return 1; }
    rc = spfy_vdb_load(vdbp, &vdb);
    if (rc != SPFY_OK) { fprintf(stderr, "vdb_load %d\n", rc); return 1; }
    rc = spfy_hash_load(&vin, &h);
    if (rc != SPFY_OK) { fprintf(stderr, "hash_load %d\n", rc); return 1; }

    float jw = 1.75f, jo = 0.15f;
    if (vcfp) {
        spfy_vcf_t vcf = {0};
        if (spfy_vcf_load(vcfp, &vcf) == SPFY_OK) {
            jw = spfy_vcf_f32(&vcf, "JOIN_COST_WEIGHT", jw);
            jo = spfy_vcf_f32(&vcf, "JOIN_COST_OFFSET", jo);
            spfy_vcf_free(&vcf);
        }
    }

    spfy_vb_cfg_t cfg;
    spfy_vb_cfg_default(&cfg);
    cfg.anchor = SPFY_VB_ANCHOR_CENTER;
    rc = spfy_vb_frames_build_ex(&vin, &vdb, 8000u, &cfg, &fr);
    if (rc != SPFY_OK) { fprintf(stderr, "frames_build %d\n", rc); return 1; }
    spfy_jc_t jc;
    spfy_vb_frames_bind(&fr, &jc);
    rc = spfy_jc_derive_weights(&jc, 1.0f);
    if (rc != SPFY_OK) { fprintf(stderr, "derive_weights %d\n", rc); return 1; }

    /* Calibrate on the voice's own non-continuation hash pairs. */
    size_t cap = 0, nv = 0;
    for (uint32_t i = 0; i < h.n_cells; ++i)
        if (spfy_hash_cell_a(&h, i) != 0xFFFFFFFFu) ++cap;
    float *vend = (float *)malloc((cap ? cap : 1) * sizeof *vend);
    float *ours = (float *)malloc((cap ? cap : 1) * sizeof *ours);
    if (!vend || !ours) { fprintf(stderr, "nomem\n"); return 1; }
    for (uint32_t i = 0; i < h.n_cells; ++i) {
        uint32_t r = spfy_hash_cell_a(&h, i);
        if (r == 0xFFFFFFFFu || r >= h.n_rows || r >= fr.n_units) continue;
        uint32_t base = spfy_hash_row(&h, r);
        if (i < base) continue;
        uint32_t l = i - base;
        if (l >= fr.n_units || r == l + 1u) continue;
        vend[nv] = spfy_hash_cell_b(&h, i);
        ours[nv] = spfy_jc_raw(&jc, l, r);
        ++nv;
    }
    qsort(vend, nv, sizeof *vend, cmp_f);
    qsort(ours, nv, sizeof *ours, cmp_f);
    float v_med = nv ? vend[nv / 2] : 1.0f, o_med = nv ? ours[nv / 2] : 1.0f;
    double v_raw_med = ((double)v_med - jo) / (jw != 0.0f ? jw : 1.0);
    float scale = (o_med > 0.0f) ? (float)(v_raw_med / (double)o_med) : 1.0f;
    printf("units %u dim %u; hash pairs %zu; vendor median %.4f, our raw median %.4f -> scale %.5f "
           "(join_w %.3f off %.3f)\n", fr.n_units, fr.dim, nv, (double)v_med, (double)o_med,
           (double)scale, (double)jw, (double)jo);

    FILE *f = fopen(outp, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", outp); return 1; }
    uint32_t hdr[3] = {0x45474445u, fr.n_units, fr.dim};
    float hf[4] = {jc.f0_gate, scale, jw, jo};
    fwrite(hdr, sizeof hdr, 1, f);
    fwrite(hf, sizeof hf, 1, f);
    fwrite(jc.weights, sizeof(float), fr.dim, f);
    fwrite(fr.frames, sizeof(float), (size_t)fr.n_units * 2u * fr.dim, f);
    fclose(f);
    free(vend);
    free(ours);
    printf("wrote %s\n", outp);
    return 0;
}
