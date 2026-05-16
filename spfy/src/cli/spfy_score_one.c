/* spfy_score_one -- M3.2 integration sanity check.
 *
 * Loads Tom voice (VIN + VCF + ccos), picks a candidate UID from the unit
 * table, computes the four target-cost components (S, D, SP, F0) for it
 * relative to a hard-coded synthetic target halfphone. This proves all
 * the loaders, maps, and cost functions wire together without crashing.
 *
 *   spfy_score_one <voice.vin> <voice.vcf> <unit_id>
 *
 * Component costs are printed plus their VCF-weighted sum. A real scorer
 * would compute these against an FE-generated target; here we use a
 * synthetic target so the values are illustrative, not engine-comparable. */

#include <spfy/spfy.h>

#include "../voice/voice.h"
#include "../voice/unit_table.h"
#include "../voice/ccos.h"
#include "../voice/voice_runtime.h"
#include "../voice/vcf_matrix.h"
#include "../usel/cost.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double get_vcf_f64(const spfy_vcf_t *vcf, const char *key, double dflt)
{
    for (const spfy_vcf_kv_t *kv = vcf->params; kv; kv = kv->next) {
        if (strcmp(kv->key, key) == 0) {
            char *e = NULL;
            double v = strtod(kv->value, &e);
            if (e != kv->value) return v;
        }
    }
    return dflt;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s <voice.vin> <voice.vcf> <unit_id>\n",
                argv[0]);
        return 2;
    }
    uint32_t uid = (uint32_t)strtoul(argv[3], NULL, 10);

    spfy_vin_t  vin = {0};
    spfy_vcf_t  vcf = {0};
    spfy_ccos_t ccos = {0};
    spfy_unit_table_t units = {0};
    spfy_voice_maps_t maps = {0};
    spfy_proscost_matrix_t pmats[SPFY_PROSCOST_N];
    memset(pmats, 0, sizeof pmats);

    int rc;
    if ((rc = spfy_vin_load(argv[1], &vin)) != SPFY_OK) goto fail;
    if ((rc = spfy_vcf_load(argv[2], &vcf)) != SPFY_OK) goto fail;
    if ((rc = spfy_ccos_load(&vin, &ccos)) != SPFY_OK) goto fail;
    if ((rc = spfy_unit_table_load(&vin, &units)) != SPFY_OK) goto fail;
    if ((rc = spfy_voice_maps_build(&ccos, &maps)) != SPFY_OK) goto fail;
    if ((rc = spfy_proscost_load(&vcf, pmats)) != SPFY_OK) goto fail;

    /* Read the candidate. */
    spfy_unit_record_t cand;
    if ((rc = spfy_unit_record_get(&units, uid, &cand)) != SPFY_OK) {
        fprintf(stderr, "unit %u: %s\n", uid, spfy_strerror(rc));
        goto fail;
    }

    /* Synthetic target halfphone: same phone center as the candidate,
     * predicted dur = 100 ms, predicted f0 = 118 Hz, scales = 0.1, all
     * SP target features = 1 (UNDEF), context phones = candidate's. */
    float durt_pred_mean  = 100.0f;
    float durt_pred_scale = 0.1f;
    float f0tr_pred_mean  = 118.0f;
    float f0tr_pred_scale = 0.1f;
    uint32_t target_sp[5] = {1, 1, 1, 1, 1};

    /* VCF weights. */
    double dur_w   = get_vcf_f64(&vcf, "tts.voiceCfg.DUR_WEIGHT",         0.3);
    double f0_w    = get_vcf_f64(&vcf, "tts.voiceCfg.ABS_F0_WEIGHT",      0.2);
    double ccos_w  = get_vcf_f64(&vcf, "tts.voiceCfg.CONTEXT_COST_WEIGHT",1.0);
    double miss_f0 = get_vcf_f64(&vcf, "tts.voiceCfg.MISSING_F0_COST", 1000.0);

    float D  = spfy_cost_d(cand.dur_like,  durt_pred_mean,
                           durt_pred_scale, (float)dur_w);
    float F0 = spfy_cost_f0(cand.f0_start, f0tr_pred_mean,
                            f0tr_pred_scale, (float)f0_w, (float)miss_f0);

    /* SP: 4 active matrices for Tom (5th is degenerate/missing). */
    spfy_sp_matrix_t sp_views[5];
    for (int k = 0; k < 5; ++k) {
        sp_views[k].data   = pmats[k].data;
        sp_views[k].n_rows = pmats[k].n_rows;
        sp_views[k].n_cols = pmats[k].n_cols;
    }
    uint32_t sp_cand[5] = {
        cand.sp_syl_in_phrase, cand.sp_syl_type,
        cand.sp_word_in_phrase, cand.sp_syl_in_word, 6
    };
    /* Default mismatch weights from VCF (Tom). */
    float w_sp[5] = {0.1f, 0.1f, 0.0f, 0.0f, 0.0f};
    float SP = spfy_cost_sp(sp_views, target_sp, sp_cand, w_sp);

    /* S: target context = candidate context (so S should be ~zero). */
    uint8_t target_ctx[4] = {
        cand.phone_ctx[0], cand.phone_ctx[1],
        cand.phone_ctx[2], cand.phone_ctx[3]
    };
    uint32_t hp_class_idx = (uint32_t)cand.phone_center * 2u +
                            (cand.is_first_half ? 0u : 1u);
    uint32_t hp_class = (hp_class_idx < maps.n_hp_entries)
                        ? maps.hp_class[hp_class_idx] : 0;
    float S = spfy_cost_s(ccos.tables, ccos.n_labels, hp_class,
                          maps.L, maps.n_labels,
                          target_ctx, cand.phone_ctx, (float)ccos_w);

    printf("uid %u (file_idx=%u, phone_center=%u, is_first_half=%u)\n",
           uid, cand.file_idx, cand.phone_center, cand.is_first_half);
    printf("  dur_like=%u, f0_start=%u\n", cand.dur_like, cand.f0_start);
    printf("  hp_class=%u (%s)\n", hp_class,
           hp_class < maps.n_labels ? "LEFT" : "RIGHT");
    printf("\nweighted components (VCF: D=%g, F0=%g, S=%g, missF0=%g)\n",
           dur_w, f0_w, ccos_w, miss_f0);
    printf("  D  = %g\n", D);
    printf("  F0 = %g\n", F0);
    printf("  SP = %g\n", SP);
    printf("  S  = %g\n", S);
    printf("  total target cost = %g\n", D + F0 + SP + S);

    rc = 0;

fail:
    spfy_proscost_free(pmats);
    spfy_voice_maps_free(&maps);
    spfy_ccos_free(&ccos);
    spfy_vcf_free(&vcf);
    spfy_vin_free(&vin);
    if (rc != 0) fprintf(stderr, "error: %s\n", spfy_strerror(rc));
    return rc == 0 ? 0 : 1;
}
