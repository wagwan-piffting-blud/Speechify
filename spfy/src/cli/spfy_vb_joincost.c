/* S4 calibration: build edge frames from a voice's own audio, compute join
 * costs with the recovered formula, and compare the distribution against the
 * costs the vendor cached in `hash`.
 *
 * What is being checked is SCALE, not agreement. The vendor's spectral
 * representation is not recoverable from a reader, so our dims >= 2 are 12
 * MFCC and the per-pair values will differ. What must line up is the
 * distribution the VCF weights are tuned against: zero for continuations,
 * a floor above JOIN_COST_OFFSET, and a comparable median. See
 * spfy/src/vb/SPEC_S4_hash.md.
 *
 *   spfy_vb_joincost <voice.vin> <voice.vdb> [voice.vcf]
 */

#include "../vb/edge_frames.h"
#include "../vb/join_cost.h"
#include "../usel/hash.h"
#include "../voice/voice.h"
#include "../../include/spfy/spfy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmp_f(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x < y) ? -1 : (x > y);
}

static void report(const char *tag, float *v, size_t n)
{
    if (!n) { printf("  %-8s (empty)\n", tag); return; }
    qsort(v, n, sizeof *v, cmp_f);
    printf("  %-8s n=%-9zu p0 %8.4f  p25 %8.4f  p50 %8.4f  p75 %8.4f  "
           "p99 %8.4f  max %8.4f\n",
           tag, n, v[0], v[n / 4], v[n / 2], v[3 * n / 4], v[(n * 99) / 100], v[n - 1]);
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: %s <voice.vin> <voice.vdb> [voice.vcf]\n", argv[0]);
        return 2;
    }

    spfy_vin_t vin = {0};
    spfy_vdb_t vdb = {0};
    int rc = spfy_vin_load(argv[1], &vin);
    if (rc != SPFY_OK) { fprintf(stderr, "vin_load: %d\n", rc); return 1; }
    rc = spfy_vdb_load(argv[2], &vdb);
    if (rc != SPFY_OK) { fprintf(stderr, "vdb_load: %d\n", rc); spfy_vin_free(&vin); return 1; }

    float jw = 1.75f, jo = 0.15f;
    if (argc == 4) {
        spfy_vcf_t vcf = {0};
        if (spfy_vcf_load(argv[3], &vcf) == SPFY_OK) {
            jw = spfy_vcf_f32(&vcf, "JOIN_COST_WEIGHT", jw);
            jo = spfy_vcf_f32(&vcf, "JOIN_COST_OFFSET", jo);
            spfy_vcf_free(&vcf);
        }
    }
    printf("JOIN_COST_WEIGHT %.4f  JOIN_COST_OFFSET %.4f\n", jw, jo);

    spfy_vb_frames_t fr = {0};
    printf("building edge frames ...\n");
    rc = spfy_vb_frames_build(&vin, &vdb, 8000u, &fr);
    if (rc != SPFY_OK) { fprintf(stderr, "frames_build: %d\n", rc); goto done; }
    printf("  units %u, unresolved %u (%.2f%%)\n", fr.n_units, fr.n_missing,
           100.0 * (double)fr.n_missing / (double)(fr.n_units ? fr.n_units : 1));

    spfy_jc_t jc;
    spfy_vb_frames_bind(&fr, &jc);
    rc = spfy_jc_derive_weights(&jc, 1.0f);
    if (rc != SPFY_OK) { fprintf(stderr, "derive_weights: %d\n", rc); goto done; }

    printf("joinweights (dim %u):\n", jc.dim);
    for (uint32_t k = 0; k < jc.dim; ++k) {
        const char *note = (k == SPFY_JC_DIM_F0) ? "  F0"
                         : (k == SPFY_JC_DIM_DEAD) ? "  *DISABLED*" : "";
        printf("   [%2u] %12.6f%s\n", k, (double)jc.weights[k], note);
    }

    /* Score exactly the pairs the vendor cached, so the two distributions are
     * over the same population. */
    spfy_hash_t h;
    rc = spfy_hash_load(&vin, &h);
    if (rc != SPFY_OK) { fprintf(stderr, "hash_load: %d\n", rc); goto done; }

    size_t live = 0;
    for (uint32_t i = 0; i < h.n_cells; ++i)
        if (spfy_hash_cell_a(&h, i) != 0xFFFFFFFFu) ++live;

    float *ours = (float *)malloc(live * sizeof *ours);
    float *theirs = (float *)malloc(live * sizeof *theirs);
    if (!ours || !theirs) { free(ours); free(theirs); rc = SPFY_E_NOMEM; goto done; }

    size_t n = 0, n_cont = 0, n_cont_zero = 0;
    for (uint32_t i = 0; i < h.n_cells; ++i) {
        uint32_t r = spfy_hash_cell_a(&h, i);
        if (r == 0xFFFFFFFFu || r >= h.n_rows) continue;
        uint32_t base = spfy_hash_row(&h, r);
        if (i < base) continue;
        uint32_t l = i - base;
        if (l >= fr.n_units || r >= fr.n_units) continue;

        float v = spfy_hash_cell_b(&h, i);
        if (r == l + 1u) {
            ++n_cont;
            if (v == 0.0f) ++n_cont_zero;
            continue;                       /* both sides define these as 0 */
        }
        theirs[n] = v;
        ours[n]   = spfy_jc_cached_value(&jc, l, r, jw, jo);
        ++n;
    }
    printf("\ncontinuations: %zu, of which vendor stores exactly 0: %zu (%.1f%%)\n",
           n_cont, n_cont_zero, 100.0 * (double)n_cont_zero / (double)(n_cont ? n_cont : 1));

    printf("\nnon-continuation cost distribution over the same %zu pairs:\n", n);
    report("vendor", theirs, n);
    report("ours",   ours,   n);

    /* Scale is a free gauge (the vendor's spectral representation is
     * unrecoverable), so it is SHAPE that tests the formula. p99/p50 is
     * scale-invariant; if it agrees, the cost is built the same way and only
     * the units differ. */
    if (n > 100) {
        double vm = theirs[n / 2], om = ours[n / 2];
        double vs = (double)theirs[(n * 99) / 100] / (vm ? vm : 1.0);
        double os = (double)ours  [(n * 99) / 100] / (om ? om : 1.0);
        double raw_v = ((double)vm - jo) / (jw ? jw : 1.0);
        double raw_o = ((double)om - jo) / (jw ? jw : 1.0);
        printf("\n  median ratio ours/vendor : %.3f\n", om / (vm ? vm : 1.0));
        printf("  implied raw median       : vendor %.4f   ours %.4f   (x%.2f)\n",
               raw_v, raw_o, raw_o / (raw_v ? raw_v : 1.0));
        printf("  p99/p50 (scale-invariant): vendor %.3f   ours %.3f\n", vs, os);
        printf("  suggested raw scale to align medians: %.4f\n",
               raw_v / (raw_o ? raw_o : 1.0));
    }

    free(ours); free(theirs);
    rc = SPFY_OK;

done:
    spfy_vb_frames_free(&fr);
    spfy_vdb_free(&vdb);
    spfy_vin_free(&vin);
    return rc == SPFY_OK ? 0 : 1;
}
