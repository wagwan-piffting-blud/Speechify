/* spfy_hp_score_test -- quick validation that spfy_hp_innerscorer
 * reproduces the engine's per-HP cand_totals on a single utterance.
 *
 * Compares C-computed per-HP cost to captured cand_totals from
 * inner_scorer/%s.jsonl utt 0. Expected: 99% bit-exact match
 * (same as the Python verify_per_hp_cost.py reference).
 *
 *   spfy_hp_score_test <voice.vin> <voice.vcf> <hpclass.bin> <traces_dir>
 */

#include <spfy/spfy.h>
#include "../usel/anchor_score.h"
#include "../voice/voice.h"
#include "../voice/unit_table.h"
#include "../voice/ccos.h"
#include "../voice/voice_runtime.h"
#include "../voice/vcf_matrix.h"
#include "../voice/chunk_table.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *find_lit(const char *p, const char *end, const char *lit)
{
    size_t lp = strlen(lit);
    if ((size_t)(end - p) < lp) return NULL;
    for (const char *q = p; q + lp <= end; ++q) {
        if (memcmp(q, lit, lp) == 0) return q;
    }
    return NULL;
}

static int read_key_i64(const char *s, const char *e, const char *key,
                        int64_t *out)
{
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\":", key);
    const char *p = find_lit(s, e, needle);
    if (!p) return -1;
    p += strlen(needle);
    char *ep = NULL;
    long long v = strtoll(p, &ep, 10);
    if (ep == p) return -1;
    *out = (int64_t)v;
    return 0;
}

static int read_key_f64(const char *s, const char *e, const char *key,
                        double *out)
{
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\":", key);
    const char *p = find_lit(s, e, needle);
    if (!p) return -1;
    p += strlen(needle);
    char *ep = NULL;
    double v = strtod(p, &ep);
    if (ep == p) return -1;
    *out = v;
    return 0;
}

static int read_key_int_array(const char *s, const char *e, const char *key,
                              int64_t *out, int out_cap)
{
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\":[", key);
    const char *p = find_lit(s, e, needle);
    if (!p) return -1;
    p += strlen(needle);
    int n = 0;
    while (p < e && n < out_cap) {
        while (p < e && (*p == ' ' || *p == ',')) ++p;
        if (p >= e || *p == ']') break;
        char *ep = NULL;
        long long v = strtoll(p, &ep, 10);
        if (ep == p) break;
        out[n++] = v;
        p = ep;
    }
    return n;
}

static int read_file(const char *path, char **buf, size_t *n)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)sz + 1);
    if (!b) { fclose(fp); return -1; }
    if (fread(b, 1, (size_t)sz, fp) != (size_t)sz) { free(b); fclose(fp); return -1; }
    b[sz] = 0;
    fclose(fp);
    *buf = b; *n = (size_t)sz;
    return 0;
}

static int next_line(const char **p, const char *end,
                     const char **ls, const char **le)
{
    const char *s = *p;
    while (s < end && (*s == '\n' || *s == '\r')) ++s;
    if (s >= end) return 0;
    *ls = s;
    while (s < end && *s != '\n') ++s;
    *le = s;
    *p = s;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: %s <voice.vin> <voice.vcf> <hpclass.bin> <traces_dir>\n", argv[0]);
        return 2;
    }
    spfy_vin_t vin = {0}; spfy_vcf_t vcf = {0};
    if (spfy_vin_load(argv[1], &vin) != SPFY_OK) return 3;
    if (spfy_vcf_load(argv[2], &vcf) != SPFY_OK) return 3;
    spfy_unit_table_t units = {0};
    spfy_ccos_t ccos = {0};
    spfy_voice_maps_t maps = {0};
    spfy_proscost_matrix_t pros[SPFY_PROSCOST_N] = {0};
    spfy_chunk_tables_t ct = {0};
    spfy_unit_table_load(&vin, &units);
    spfy_ccos_load(&vin, &ccos);
    spfy_voice_maps_build(&ccos, &maps);
    spfy_proscost_load(&vcf, pros);
    spfy_chunk_tables_load(&vin, &ct);
    uint8_t *hpc = NULL; uint32_t hpc_n = 0;
    spfy_anchor_hpclass_load(argv[3], &hpc, &hpc_n);

    spfy_anchor_voice_t av = {0};
    av.units = &units;
    av.ccos = &ccos;
    av.maps = &maps;
    av.proscost = pros;
    av.hpclass_table = hpc;
    av.hpclass_n = hpc_n;
    spfy_anchor_voice_set_default_weights(&av);

    /* Load inner_scorer trace. Defaults to text_002, override via env. */
    const char *text_id = getenv("SPFY_TEXT");
    if (!text_id) text_id = "text_002";
    char path[1024];
    snprintf(path, sizeof path, "%s/inner_scorer/%s.jsonl", argv[4], text_id);
    char *buf = NULL; size_t buf_n = 0;
    if (read_file(path, &buf, &buf_n) != 0) {
        fprintf(stderr, "cannot read %s\n", path); return 3;
    }
    /* Also load prsl_slot for ctx, cart_walks for cart, voicing from
     * inner_scorer's join_consts. */
    snprintf(path, sizeof path, "%s/prsl_slot/%s.jsonl", argv[4], text_id);
    char *prsl_buf = NULL; size_t prsl_n = 0;
    read_file(path, &prsl_buf, &prsl_n);
    snprintf(path, sizeof path, "%s/cart_walks/%s.jsonl", argv[4], text_id);
    char *cw_buf = NULL; size_t cw_n = 0;
    read_file(path, &cw_buf, &cw_n);

    /* Build per-slot ctx, cart, voicing arrays (utt 0 only). */
    spfy_anchor_ctx_t ctxs[256] = {0};
    int ctx_have[256] = {0};
    {
        const char *p = prsl_buf, *end = prsl_buf + prsl_n;
        const char *ls, *le;
        int seen_slot0 = 0;
        while (next_line(&p, end, &ls, &le)) {
            if (!find_lit(ls, le, "\"type\":\"prsl_slot\"")) continue;
            int64_t slot;
            if (read_key_i64(ls, le, "slot", &slot) != 0) continue;
            if (slot == 0) {
                if (seen_slot0) break;   /* utt 1 starts */
                seen_slot0 = 1;
            }
            int64_t arr[8];
            int n = read_key_int_array(ls, le, "ctx", arr, 8);
            if (n != 5 || slot >= 256) continue;
            for (int i = 0; i < 5; ++i)
                ctxs[slot].ctx[i] = (int32_t)arr[i];
            ctx_have[slot] = 1;
        }
    }
    spfy_anchor_cart_t carts[256] = {0};
    {
        /* Detect utt boundary: slot drops back to 0/low after climbing.
         * Within utt 0, durt+f0tr each enumerate slots 0..N in order.
         * utt 1 starts with slot 0 durt after utt 0 has emitted slot N
         * f0tr (or last durt). Track running max; if slot is much lower
         * than the max we've seen, that's utt 1. */
        const char *p = cw_buf, *end = cw_buf + cw_n;
        const char *ls, *le;
        int64_t max_slot = -1;
        int phase = 0;   /* 0=durt phase, 1=f0tr phase, transition allowed */
        while (next_line(&p, end, &ls, &le)) {
            if (!find_lit(ls, le, "\"type\":\"cart_walk\"")) continue;
            int64_t slot;
            if (read_key_i64(ls, le, "slot", &slot) != 0) continue;
            int is_durt = find_lit(ls, le, "\"tree\":\"durt\"") != NULL;
            int is_f0tr = find_lit(ls, le, "\"tree\":\"f0tr\"") != NULL;
            /* If slot drops AND we're in f0tr phase entering durt, that's
             * utt 1 boundary. */
            if (is_durt && phase == 1 && slot == 0) break;
            /* Phase transition: first f0tr seen after durt phase. */
            if (is_f0tr) phase = 1;
            else if (is_durt && phase == 0 && slot < max_slot) {
                /* Within utt 0, durt is monotonically increasing.
                 * If slot drops while still in durt phase, that's utt 1. */
                break;
            }
            if (slot > max_slot) max_slot = slot;
            if (slot >= 256) continue;
            double m, v;
            if (read_key_f64(ls, le, "leaf_mean", &m) != 0) continue;
            if (read_key_f64(ls, le, "leaf_var", &v) != 0) continue;
            if (find_lit(ls, le, "\"tree\":\"durt\"")) {
                carts[slot].durt_mean = (float)m;
                carts[slot].durt_var = (float)v;
                carts[slot].durt_valid = 1;
            } else if (find_lit(ls, le, "\"tree\":\"f0tr\"")) {
                carts[slot].f0tr_mean = (float)m;
                carts[slot].f0tr_var = (float)v;
                carts[slot].f0tr_valid = 1;
            }
        }
    }

    /* Override durt_mean / durt_var with the engine's PRESELECT-TIME
     * durt CART output captured by inner_scorer_durt_hook.js (plan 02-05
     * D-17 Path R, sub-case DIFF-VALUES-SAME-TREE). cart_walks JSONL
     * captures the engine's SYNTH-TIME durt CART output, which differs
     * from the inner-scorer's preselect-time CART output for the same
     * slot. Use preselect-time values for the inner-scorer D-cost path
     * to close the silence-sentinel +2.4321 drift on UID 169578.
     *
     * For slots not present in inner_scorer_durt JSONL (e.g. older
     * captures), cart_walks values are preserved as a fallback so the
     * test still runs against historical traces. */
    {
        char isd_path[1024];
        snprintf(isd_path, sizeof isd_path, "%s/inner_scorer_durt/%s.jsonl",
                 argv[4], text_id);
        char *isd_buf = NULL; size_t isd_n = 0;
        if (read_file(isd_path, &isd_buf, &isd_n) == 0) {
            const char *p = isd_buf, *end = isd_buf + isd_n;
            const char *ls, *le;
            int n_override = 0;
            while (next_line(&p, end, &ls, &le)) {
                if (!find_lit(ls, le, "\"type\":\"inner_scorer_durt\"")) continue;
                int64_t utt = 0, slot = -1;
                if (read_key_i64(ls, le, "utt", &utt) != 0) continue;
                if (read_key_i64(ls, le, "slot", &slot) != 0) continue;
                if (utt != 0) continue;
                if (slot < 0 || slot >= 256) continue;
                double mean = 0.0, inv_std = 0.0;
                if (read_key_f64(ls, le, "mean", &mean) != 0) continue;
                if (read_key_f64(ls, le, "inv_std", &inv_std) != 0) continue;
                carts[slot].durt_mean  = (float)mean;
                carts[slot].durt_var   = (float)inv_std;
                carts[slot].durt_valid = 1;
                ++n_override;
            }
            free(isd_buf);
            if (getenv("SPFY_HP_DBG"))
                fprintf(stderr, "  inner_scorer_durt: overrode %d slots\n",
                        n_override);
        } else if (getenv("SPFY_HP_DBG")) {
            fprintf(stderr, "  inner_scorer_durt: %s not present, using cart_walks fallback\n",
                    isd_path);
        }
    }

    /* Read voicing from join_consts in inner_scorer. join_consts is only
     * captured once per session (typically in text_001's trace), so try
     * text_001 first, then fall back to current text. */
    char join_path[1024];
    snprintf(join_path, sizeof join_path, "%s/inner_scorer/text_001.jsonl",
             argv[4]);
    char *jc_buf = NULL; size_t jc_n = 0;
    if (read_file(join_path, &jc_buf, &jc_n) != 0) {
        jc_buf = buf; jc_n = buf_n;   /* fall back */
    }
    static uint32_t voicing[256];
    int voicing_n = 0;
    {
        const char *p = jc_buf, *end = jc_buf + jc_n;
        const char *ls, *le;
        while (next_line(&p, end, &ls, &le)) {
            if (find_lit(ls, le, "\"type\":\"join_consts\"")) {
                const char *bh = find_lit(ls, le, "\"by_hp_class\":[");
                if (bh) {
                    bh += strlen("\"by_hp_class\":[");
                    const char *q = bh;
                    while (q < le && voicing_n < 256) {
                        while (q < le && (*q == ' ' || *q == ',')) ++q;
                        if (q >= le || *q == ']') break;
                        char *ep = NULL;
                        long long v = strtoll(q, &ep, 10);
                        if (ep == q) break;
                        voicing[voicing_n++] = (uint32_t)v;
                        q = ep;
                    }
                }
                break;
            }
        }
        av.voicing = voicing;
        av.voicing_n = (uint32_t)voicing_n;
        if (getenv("SPFY_HP_DBG"))
            fprintf(stderr, "  voicing_n=%d, [0..5]=[%u %u %u %u %u %u], "
                    "[40..45]=[%u %u %u %u %u %u]\n",
                    voicing_n, voicing[0], voicing[1], voicing[2],
                    voicing[3], voicing[4], voicing[5],
                    voicing[40], voicing[41], voicing[42], voicing[43],
                    voicing[44], voicing[45]);
    }

    /* For each inner_scorer event in utt 0, compute pre_dp for each
     * cand_totals entry and compare. */
    int n_total = 0, n_match = 0;
    int n_close = 0;   /* within 1e-2 */
    {
        const char *p = buf, *end = buf + buf_n;
        const char *ls, *le;
        int seen_slot0 = 0;
        while (next_line(&p, end, &ls, &le)) {
            if (!find_lit(ls, le, "\"type\":\"inner_scorer\"")) continue;
            int64_t slot;
            if (read_key_i64(ls, le, "slot", &slot) != 0) continue;
            if (slot == 0) {
                if (seen_slot0) break;
                seen_slot0 = 1;
            }
            if (slot >= 256 || !ctx_have[slot]) continue;
            int64_t sp_arr[8];
            int n = read_key_int_array(ls, le, "sp_target", sp_arr, 8);
            if (n != 5) continue;
            spfy_anchor_sp_target_t sp;
            for (int i = 0; i < 5; ++i) sp.sp[i] = (uint32_t)sp_arr[i];

            /* Walk cand_totals: [[uid, cost], [uid, cost], ...] */
            const char *ct_p = find_lit(ls, le, "\"cand_totals\":[");
            if (!ct_p) continue;
            ct_p += strlen("\"cand_totals\":[");
            while (ct_p < le) {
                while (ct_p < le && (*ct_p == ' ' || *ct_p == ',')) ++ct_p;
                if (ct_p >= le || *ct_p == ']') break;
                if (*ct_p != '[') { ++ct_p; continue; }
                ++ct_p;
                /* parse uid */
                char *ep = NULL;
                long long uid_v = strtoll(ct_p, &ep, 10);
                if (ep == ct_p) break;
                ct_p = ep;
                while (ct_p < le && (*ct_p == ' ' || *ct_p == ',')) ++ct_p;
                /* parse cost */
                ep = NULL;
                double cost_v = strtod(ct_p, &ep);
                if (ep == ct_p) break;
                ct_p = ep;
                while (ct_p < le && *ct_p != ']') ++ct_p;
                if (ct_p < le) ++ct_p;

                /* Compute via spfy_hp_innerscorer. */
                float my_cost = NAN;
                if (spfy_hp_innerscorer(&av, &ctxs[slot], &sp,
                                          &carts[slot], (uint32_t)uid_v,
                                          &my_cost) == SPFY_OK
                    && !isnan(my_cost)) {
                    ++n_total;
                    float diff = fabsf(my_cost - (float)cost_v);
                    if (diff < 1e-3f) ++n_match;
                    if (diff < 1e-2f) ++n_close;
                    if (getenv("SPFY_HP_DBG") && diff >= 1e-3f
                        && (n_total - n_match) <= 3) {
                        printf("MISMATCH slot=%lld uid=%lld my=%.4f exp=%.4f diff=%.4f"
                               " ctx=[%d,%d,%d,%d,%d]"
                               " durt=(%g,%g,v=%d) f0tr=(%g,%g,v=%d)"
                               " voicing[%d]=%u\n",
                               (long long)slot, uid_v, my_cost, cost_v, diff,
                               ctxs[slot].ctx[0], ctxs[slot].ctx[1],
                               ctxs[slot].ctx[2], ctxs[slot].ctx[3],
                               ctxs[slot].ctx[4],
                               carts[slot].durt_mean, carts[slot].durt_var,
                               carts[slot].durt_valid,
                               carts[slot].f0tr_mean, carts[slot].f0tr_var,
                               carts[slot].f0tr_valid,
                               ctxs[slot].ctx[2],
                               (ctxs[slot].ctx[2] < voicing_n)
                                    ? voicing[ctxs[slot].ctx[2]] : 0xfffffff);
                    }
                }
            }
        }
    }

    printf("per-HP cost match (text_002 utt 0): %d/%d  (%.2f%% bit-exact, %.2f%% within 0.01)\n",
           n_match, n_total,
           n_total ? 100.0 * n_match / n_total : 0.0,
           n_total ? 100.0 * n_close / n_total : 0.0);

    free(buf); free(prsl_buf); free(cw_buf);
    spfy_anchor_hpclass_free(hpc);
    spfy_chunk_tables_free(&ct);
    spfy_proscost_free(pros);
    spfy_voice_maps_free(&maps);
    spfy_ccos_free(&ccos);
    spfy_vin_free(&vin);
    spfy_vcf_free(&vcf);
    return 0;
}
