#include "vb_corpus.h"

#include "vb_lang.h"
#include "../common/log.h"
#include "../../include/spfy/spfy.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
/* Template                                                                */

/* The template's unit records, as a flat view under the layout its OWN `vers`
 * subchunk declares.
 *
 * ⛔⛔ THE STRIDE IS NOT ALWAYS 29. jill ships v100008 at stride 30 -- it
 * inserts `phone_in_syl` at 0x10, so f0_mid, f0_context, phone_center and
 * is_first_half all sit ONE BYTE LATER than in tom's v100006. This reader
 * assumed 29 for every template, and the misread was silent in exactly the way
 * that costs the most: it produced a FULL, PLAUSIBLE per-phone duration table
 * that disagreed with vb_build1.py's on 42 of 43 phones, and an f0_context fit
 * whose only symptom was the "weak fit" line nobody had a control for.
 *
 * Refusing an unknown version is deliberate. There is no safe default here --
 * v100004/5/7 move the same fields again, and guessing 29 is what produced the
 * table above. */
typedef struct {
    const uint8_t *data;
    size_t         n;          /* records */
    uint32_t       ver;
    size_t         stride;
    size_t         o_dur, o_f0_mid, o_f0_ctx, o_pc;
} tmpl_unit_view;

static int tmpl_units(const spfy_vb_riff *vin, tmpl_unit_view *v)
{
    memset(v, 0, sizeof *v);
    const spfy_vb_chunk *u = spfy_vb_riff_get(vin, "unit");
    if (!u) return SPFY_E_FORMAT;
    size_t pos = 0, raw = 0;
    char id[5];
    const uint8_t *d;
    size_t dn;
    while (spfy_vb_subchunk(u->data, u->n, &pos, id, &d, &dn)) {
        if (!memcmp(id, "vers", 4) && dn >= 4)
            v->ver = (uint32_t)d[0] | ((uint32_t)d[1] << 8) |
                     ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
        else if (!memcmp(id, "data", 4)) { v->data = d; raw = dn; }
    }
    if (!v->data) return SPFY_E_FORMAT;
    v->o_dur = 0x0A;                        /* the one field both agree on */
    switch (v->ver) {
    case 100006u:
        v->stride = 29; v->o_f0_mid = 0x12; v->o_f0_ctx = 0x13; v->o_pc = 0x14;
        break;
    case 100008u:
        v->stride = 30; v->o_f0_mid = 0x13; v->o_f0_ctx = 0x14; v->o_pc = 0x15;
        break;
    default:
        return SPFY_E_FORMAT;
    }
    v->n = raw / v->stride;
    return SPFY_OK;
}

/* f0_context = a*log(dur+1) + b, least squares over the template's own units.
 *
 * ⚠ Reports R². The relation is asserted by the existing pipeline; if it is
 * weak, a fitted value is no better than a constant and should be called out
 * rather than shipped as if it were derived. */
static void fit_f0_context(const tmpl_unit_view *v,
                           double *a_out, double *b_out, double *r2_out)
{
    const uint8_t *u = v->data;
    size_t n = v->n;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    size_t m = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t *r = u + i * v->stride;
        long dl = (long)r[v->o_dur] | ((long)r[v->o_dur + 1u] << 8);
        double fc = r[v->o_f0_ctx];
        if (dl <= 0 || dl >= 20000 || fc <= 0) continue;
        double x = log((double)dl + 1.0);
        sx += x; sy += fc; sxx += x * x; sxy += x * fc;
        ++m;
    }
    if (m < 2) { *a_out = 0; *b_out = 0; *r2_out = 0; return; }
    double den = (double)m * sxx - sx * sx;
    double a = den != 0.0 ? ((double)m * sxy - sx * sy) / den : 0.0;
    double b = ((double)m ? (sy - a * sx) / (double)m : 0.0);
    double ybar = sy / (double)m, ss_res = 0, ss_tot = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t *r = u + i * v->stride;
        long dl = (long)r[v->o_dur] | ((long)r[v->o_dur + 1u] << 8);
        double fc = r[v->o_f0_ctx];
        if (dl <= 0 || dl >= 20000 || fc <= 0) continue;
        double p = a * log((double)dl + 1.0) + b;
        ss_res += (fc - p) * (fc - p);
        ss_tot += (fc - ybar) * (fc - ybar);
    }
    *a_out = a;
    *b_out = b;
    *r2_out = ss_tot > 0 ? 1.0 - ss_res / ss_tot : 0.0;
}

/* ⭐ NO DONOR AT ALL.
 *
 * Builds the two containers from nothing but the embedded en-US language
 * tables (vb_lang.h -- every byte of which is measured IDENTICAL across jill
 * and tom) and writes an all-zero `ccos` carrying our own label list. The
 * builder then replaces every other chunk as usual, and the trees are grown
 * rather than patched, so no vendor byte survives into the output.
 *
 * ⚠ THE ONE JUDGEMENT CALL IS THE DURATION ENCODING. `f0_context` is
 * a·log(dur+1)+b -- the byte `durt` predicts and the engine compares against
 * -- and with no donor there is nothing to fit it to. It is not a per-speaker
 * quantity: fitted independently on each vendor's own units it comes out
 *
 *     jill  a=47.2220  b=-51.7121  R²=0.9147
 *     tom   a=48.6851  b=-57.3633  R²=0.9374
 *
 * and the two curves agree to about one byte across 20-150 ms (both give
 * 129.1 at 45 ms). Two voices landing on the same transfer is what a FORMAT
 * constant looks like, so the midpoint is used and named here rather than
 * being quietly inherited. --f0-slope / --f0-offset still override it. */
#define SPFY_VB_F0CTX_A   47.95
#define SPFY_VB_F0CTX_B  (-54.54)

static int ccos_zero_body(const char *const *labels, size_t n_labels,
                          uint8_t **out, size_t *out_n)
{
    const size_t tri = n_labels * (n_labels - 1u) / 2u;
    spfy_vb_buf lab = {0}, data = {0}, body = {0};
    int rc = spfy_vb_buf_u32(&lab, (uint32_t)n_labels);
    for (size_t i = 0; i < n_labels && rc == SPFY_OK; ++i)
        rc = spfy_vb_buf_pstr(&lab, labels[i]);
    for (uint32_t hp = 0; hp < 2u * (uint32_t)n_labels && rc == SPFY_OK; ++hp)
        for (uint32_t s = 0; s < 4u && rc == SPFY_OK; ++s) {
            rc = spfy_vb_buf_u32(&data, hp);
            if (rc == SPFY_OK) rc = spfy_vb_buf_u32(&data, s);
            for (size_t k = 0; k < tri && rc == SPFY_OK; ++k)
                rc = spfy_vb_buf_f32(&data, 0.0f);
        }
    if (rc == SPFY_OK) rc = spfy_vb_buf_put(&body, "labl", 4);
    if (rc == SPFY_OK) rc = spfy_vb_buf_u32(&body, (uint32_t)lab.n);
    if (rc == SPFY_OK) rc = spfy_vb_buf_put(&body, lab.p, lab.n);
    if (rc == SPFY_OK && (lab.n & 1u)) rc = spfy_vb_buf_u8(&body, 0);
    if (rc == SPFY_OK) rc = spfy_vb_buf_put(&body, "data", 4);
    if (rc == SPFY_OK) rc = spfy_vb_buf_u32(&body, (uint32_t)data.n);
    if (rc == SPFY_OK) rc = spfy_vb_buf_put(&body, data.p, data.n);
    if (rc == SPFY_OK && (data.n & 1u)) rc = spfy_vb_buf_u8(&body, 0);
    spfy_vb_buf_free(&lab);
    spfy_vb_buf_free(&data);
    if (rc != SPFY_OK) { spfy_vb_buf_free(&body); return rc; }
    *out = body.p;
    *out_n = body.n;
    return SPFY_OK;
}

int spfy_vb_template_new(spfy_vb_template *t, uint32_t unit_ver)
{
    memset(t, 0, sizeof *t);
    int rc = spfy_vb_riff_new(&t->vin, "svin");
    if (rc != SPFY_OK) return rc;
    rc = spfy_vb_riff_new(&t->vdb, "WAVE");
    if (rc != SPFY_OK) { spfy_vb_riff_free(&t->vin); return rc; }

    size_t n_lab = 0;
    const char *const *labels = spfy_vb_lang_labl(&n_lab);
    uint8_t *ccos = NULL;
    size_t ccos_n = 0;
    rc = ccos_zero_body(labels, n_lab, &ccos, &ccos_n);
    if (rc != SPFY_OK) goto fail;

    /* ⛔ AN EMPTY `hash` IS NOT A VALID VIN. `spfy_vin_load` refuses a hash
     * chunk with no `head` sub-chunk, so the builder's own S4 pass -- which
     * re-opens the VIN it just wrote -- fails with `vin_load -3` and the arm
     * dies after everything else succeeded. Ship a structurally valid EMPTY
     * one: head{n_rows=0, n_cells=0}. S4 replaces it. */
    spfy_vb_buf hb = {0};
    rc = spfy_vb_buf_put(&hb, "head", 4);
    if (rc == SPFY_OK) rc = spfy_vb_buf_u32(&hb, 8u);
    if (rc == SPFY_OK) rc = spfy_vb_buf_u32(&hb, 0u);   /* n_rows  */
    if (rc == SPFY_OK) rc = spfy_vb_buf_u32(&hb, 0u);   /* n_cells */
    if (rc != SPFY_OK) { spfy_vb_buf_free(&hb); goto fail; }

    /* The vendors' own chunk order, kept because a container that differs
     * from theirs only in ordering is a variable nobody wants to debug. */
    static const char *const VIN_ORDER[] = {
        "LIST", "vers", "cnts", "feat", "mean", "hash", "ckls", "cklx",
        "unit", "f0tr", "durt", "ccos", "prsl", "hist"
    };
    for (size_t i = 0; i < sizeof VIN_ORDER / sizeof VIN_ORDER[0]; ++i) {
        if (!strcmp(VIN_ORDER[i], "ccos")) {
            rc = spfy_vb_riff_put(&t->vin, "ccos", ccos, ccos_n);
            if (rc == SPFY_OK) ccos = NULL;
        } else if (!strcmp(VIN_ORDER[i], "hash")) {
            rc = spfy_vb_riff_put(&t->vin, "hash", hb.p, hb.n);
            if (rc == SPFY_OK) memset(&hb, 0, sizeof hb);
        } else {
            rc = spfy_vb_riff_put(&t->vin, VIN_ORDER[i], NULL, 0);
        }
        if (rc != SPFY_OK) { spfy_vb_buf_free(&hb); goto fail; }
    }
    static const char *const VDB_ORDER[] = { "LIST", "fmt ", "indx", "data" };
    for (size_t i = 0; i < sizeof VDB_ORDER / sizeof VDB_ORDER[0]; ++i) {
        rc = spfy_vb_riff_put(&t->vdb, VDB_ORDER[i], NULL, 0);
        if (rc != SPFY_OK) goto fail;
    }

    size_t feat_n = 0;
    const uint8_t *feat = spfy_vb_lang_feat(&feat_n);
    rc = spfy_vb_phone_index_build(feat, feat_n, &t->pidx);
    if (rc != SPFY_OK) goto fail;
    const spfy_vb_chunk *cc = spfy_vb_riff_get(&t->vin, "ccos");
    rc = spfy_vb_labl_map_build(cc->data, cc->n, feat, feat_n, &t->labl);
    if (rc != SPFY_OK) goto fail;

    t->pau_feat = spfy_vb_phone_id(&t->pidx, "pau");
    if (t->pau_feat < 0) t->pau_feat = 32;
    t->f0ctx_a  = SPFY_VB_F0CTX_A;
    t->f0ctx_b  = SPFY_VB_F0CTX_B;
    /* ⚠ NOT a fitted R². Reported as 0 so the builder's own "weak fit"
     * warning cannot claim a quality it did not measure. */
    t->f0ctx_r2 = 0.0;
    t->unit_ver = unit_ver ? unit_ver : SPFY_VB_UNIT_V8_VERSION;
    printf("  ⭐ NO TEMPLATE: %zu phones, %zu labels, all-zero ccos, "
           "f0_context = %.4f*log(dur+1) %+.4f\n",
           t->pidx.n, n_lab, t->f0ctx_a, t->f0ctx_b);
    return SPFY_OK;

fail:
    free(ccos);
    spfy_vb_template_free(t);
    return rc;
}

int spfy_vb_template_load(const char *vin_path, const char *vdb_path,
                          spfy_vb_template *t)
{
    memset(t, 0, sizeof *t);
    int rc = spfy_vb_riff_load(vin_path, &t->vin);
    if (rc != SPFY_OK) return rc;
    rc = spfy_vb_riff_load(vdb_path, &t->vdb);
    if (rc != SPFY_OK) { spfy_vb_riff_free(&t->vin); return rc; }

    const spfy_vb_chunk *feat = spfy_vb_riff_get(&t->vin, "feat");
    const spfy_vb_chunk *ccos = spfy_vb_riff_get(&t->vin, "ccos");
    if (!feat || !ccos) { rc = SPFY_E_FORMAT; goto fail; }

    rc = spfy_vb_phone_index_build(feat->data, feat->n, &t->pidx);
    if (rc != SPFY_OK) goto fail;
    rc = spfy_vb_labl_map_build(ccos->data, ccos->n, feat->data, feat->n, &t->labl);
    if (rc != SPFY_OK) goto fail;

    t->pau_feat = spfy_vb_phone_id(&t->pidx, "pau");
    if (t->pau_feat < 0) t->pau_feat = 32;

    tmpl_unit_view uv;
    rc = tmpl_units(&t->vin, &uv);
    if (rc != SPFY_OK) {
        spfy_log_err("template unit chunk v%u is unmapped -- only v100006 "
                     "(stride 29) and v100008 (stride 30) are; reading one "
                     "under the other's map is SILENT", uv.ver);
        goto fail;
    }
    t->unit_ver = uv.ver;
    printf("  template units: v%u, stride %zu, %zu records\n",
           uv.ver, uv.stride, uv.n);
    fit_f0_context(&uv, &t->f0ctx_a, &t->f0ctx_b, &t->f0ctx_r2);
    return SPFY_OK;

fail:
    spfy_vb_template_free(t);
    return rc;
}

void spfy_vb_template_free(spfy_vb_template *t)
{
    spfy_vb_phone_index_free(&t->pidx);
    spfy_vb_riff_free(&t->vin);
    spfy_vb_riff_free(&t->vdb);
    memset(t, 0, sizeof *t);
}

static int cmp_dbl(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x < y) ? -1 : (x > y);
}

int spfy_vb_template_fit_f0(spfy_vb_template *t,
                            const uint8_t *const *tracks,
                            const size_t *track_n, size_t n_tracks)
{
    tmpl_unit_view uv;
    if (tmpl_units(&t->vin, &uv) != SPFY_OK) return SPFY_E_FORMAT;

    /* Template side: stored f0_mid, non-zero only. */
    double *tv = (double *)malloc((uv.n ? uv.n : 1u) * sizeof *tv);
    if (!tv) return SPFY_E_NOMEM;
    size_t tn = 0;
    for (size_t i = 0; i < uv.n; ++i) {
        uint8_t v = uv.data[i * uv.stride + uv.o_f0_mid];
        if (v > 0) tv[tn++] = v;
    }

    size_t on = 0;
    for (size_t i = 0; i < n_tracks; ++i) on += track_n[i];
    double *ov = (double *)malloc((on ? on : 1u) * sizeof *ov);
    if (!ov) { free(tv); return SPFY_E_NOMEM; }
    size_t om = 0;
    for (size_t i = 0; i < n_tracks; ++i)
        for (size_t k = 0; k < track_n[i]; ++k)
            if (tracks[i][k] > 0) ov[om++] = tracks[i][k];

    if (om < 100 || tn < 100) {
        free(tv); free(ov);
        t->f0q_fitted = 0;
        return SPFY_OK;
    }
    qsort(tv, tn, sizeof *tv, cmp_dbl);
    qsort(ov, om, sizeof *ov, cmp_dbl);
    double t_med = tv[tn / 2], o_med = ov[om / 2];
    double tm = 0, om_ = 0;
    for (size_t i = 0; i < tn; ++i) tm += tv[i];
    tm /= (double)tn;
    for (size_t i = 0; i < om; ++i) om_ += ov[i];
    om_ /= (double)om;
    double tsd = 0, osd = 0;
    for (size_t i = 0; i < tn; ++i) tsd += (tv[i] - tm) * (tv[i] - tm);
    for (size_t i = 0; i < om; ++i) osd += (ov[i] - om_) * (ov[i] - om_);
    tsd = sqrt(tsd / (double)tn);
    osd = sqrt(osd / (double)om);
    free(tv); free(ov);

    /* ⛔⛔ MATCHING THE TEMPLATE'S STORED sd IS NOT THE SAME AS USING ITS
     * ENCODING, and the difference is a real defect.
     *
     * `slope = tsd / osd` forces OUR stored bytes to have the template's
     * spread whatever our speaker's pitch range actually is. Measured (see
     * durwork/f0meaning.py): jill's byte is genuinely scaled F0 --
     * r(stored, measured Hz) = +0.797 over 1,517 reliably-voiced units, and
     * her transfer is stored = 0.124*Hz + 106 (OLS) with a sd-ratio estimate
     * of 0.155. Our fit emits 0.2074, i.e. 1.3x to 1.7x STEEPER, because our
     * speaker varies less in Hz (sd 25.6) than jill does (sd 35.5) and the
     * sd match stretches us to fill her range.
     *
     * The engine's f0 terms are QUADRATIC in the deviation
     * ((f0_start - f0tr_mean) * f0tr_var, squared), so a 1.3-1.7x steep
     * encoding over-weights pitch matching by roughly 2-3x against every
     * other cost -- the DP then buys pitch agreement with bad units. Same
     * bug class as shipping a ccos 2200x the vendor's magnitude.
     *
     * --f0-slope/--f0-offset override the fit with a measured transfer. */
    t->f0q_slope = osd > 1e-6 ? tsd / osd : 1.0;
    t->f0q_off   = t_med - t->f0q_slope * o_med;
    printf("  f0 quantisation: template median %.1f sd %.1f; ours %.1f sd %.1f\n",
           t_med, tsd, o_med, osd);
    if (t->f0q_user) {
        printf("    fitted stored = %.4f * Hz + %.2f  (sd-match)\n",
               t->f0q_slope, t->f0q_off);
        t->f0q_slope = t->f0q_user_slope;
        t->f0q_off   = t->f0q_user_off;
        printf("    ⭐ OVERRIDDEN stored = %.4f * Hz + %.2f  -- the vendor's "
               "own measured transfer, so a Hz deviation costs what it costs "
               "for jill\n", t->f0q_slope, t->f0q_off);
    } else {
        printf("    stored = %.4f * Hz + %.2f\n", t->f0q_slope, t->f0q_off);
    }
    t->f0q_fitted = 1;
    return SPFY_OK;
}

/* numpy.percentile's default interpolation: linear between the two ranks the
 * position falls between. Ported rather than approximated because a
 * nearest-rank percentile lands a whole millisecond away on a thin pool, and
 * the whole point of reading the floor off a shipped voice is that the number
 * is not one someone picked. */
static double pct_linear(const double *v, size_t n, double pct)
{
    if (!n) return 0.0;
    if (pct <= 0.0) return v[0];
    if (pct >= 100.0) return v[n - 1u];
    double pos = (pct / 100.0) * (double)(n - 1u);
    size_t lo = (size_t)pos;
    if (lo + 1u >= n) return v[n - 1u];
    return v[lo] + (v[lo + 1u] - v[lo]) * (pos - (double)lo);
}

/* ⛔ ALWAYS RE-READ THE REFERENCE FROM DISK, NEVER `t->vin`.
 *
 * The build MUTATES the template riff in place -- S7 does riff_set on unit,
 * feat, prsl, ckls, durt, hist -- so by S5 `t->vin`'s unit chunk holds OUR
 * units, not the template's. Computed off it, this returned a per-phone table
 * derived from the corpus being gated: 9 of 46 phones instead of 43, and a
 * floor that rises with whatever the corpus already contains, which is the
 * opposite of reading it off a working inventory. */
/* ⭐ THE SAME PERCENTILE, READ OFF OUR OWN CORPUS.
 *
 * `spfy_vb_dur_percentiles` reads the bound off a SHIPPED VOICE, which makes
 * `--dur-floor-pct` a donor dependency nobody had listed: it decides which of
 * OUR units are too short using jill's per-phone durations. This is the same
 * statistic taken from the units we just cut.
 *
 * ⚠ It is self-referential BY DESIGN and that is what the flag already meant
 * in practice -- jill's 10th percentile withheld 9.77% of our units, i.e.
 * almost exactly our own bottom 10%. The reason the flag is per-phone rather
 * than a flat millisecond cutoff is unchanged: a flap or a stop closure really
 * is that short, so whatever rejects a 5 ms `ae` has to pass a 5 ms `dx`.
 *
 * Indexed by the FEAT phone id, matching the other function, because that is
 * what `spfy_vb_unit.phone` carries. */
int spfy_vb_dur_percentiles_corpus(const spfy_vb_corpus *c, double pct,
                                   double *out, size_t out_n,
                                   size_t *n_set, size_t *n_phones)
{
    for (size_t i = 0; i < out_n; ++i) out[i] = 0.0;
    if (n_set) *n_set = 0;
    if (n_phones) *n_phones = 0;
    if (!c || !out || out_n == 0) return SPFY_E_INVAL;

    size_t *cnt = (size_t *)calloc(out_n, sizeof *cnt);
    size_t *fill = (size_t *)calloc(out_n, sizeof *fill);
    size_t *base = (size_t *)calloc(out_n, sizeof *base);
    if (!cnt || !fill || !base) { free(cnt); free(fill); free(base); return SPFY_E_NOMEM; }

    for (size_t i = 0; i < c->n_units; ++i) {
        uint8_t ph = c->units[i].phone;
        if ((size_t)ph < out_n && c->units[i].dur_like > 0) ++cnt[ph];
    }
    size_t total = 0, np = 0;
    for (size_t p = 0; p < out_n; ++p) {
        base[p] = total;
        total += cnt[p];
        if (cnt[p]) ++np;
    }
    double *vals = (double *)malloc((total ? total : 1u) * sizeof *vals);
    if (!vals) { free(cnt); free(fill); free(base); return SPFY_E_NOMEM; }
    for (size_t i = 0; i < c->n_units; ++i) {
        uint8_t ph = c->units[i].phone;
        if ((size_t)ph >= out_n || c->units[i].dur_like <= 0) continue;
        vals[base[ph] + fill[ph]++] = (double)c->units[i].dur_like;
    }

    size_t set = 0;
    for (size_t p = 0; p < out_n; ++p) {
        /* Too few examples is no basis for a floor; leave that phone alone. */
        if (cnt[p] < SPFY_VB_DUR_PCT_MIN_N) continue;
        qsort(vals + base[p], cnt[p], sizeof *vals, cmp_dbl);
        size_t k = (size_t)(pct / 100.0 * (double)(cnt[p] - 1u) + 0.5);
        if (k >= cnt[p]) k = cnt[p] - 1u;
        out[p] = vals[base[p] + k];
        ++set;
    }
    free(vals); free(cnt); free(fill); free(base);
    if (n_set) *n_set = set;
    if (n_phones) *n_phones = np;
    return SPFY_OK;
}

int spfy_vb_dur_percentiles(const spfy_vb_template *t, const char *ref_vin,
                            double pct, double *out, size_t out_n,
                            size_t *n_set, size_t *n_phones)
{
    for (size_t i = 0; i < out_n; ++i) out[i] = 0.0;
    if (n_set) *n_set = 0;
    if (n_phones) *n_phones = 0;
    if (!ref_vin) return SPFY_E_INVAL;

    spfy_vb_riff own;
    spfy_vb_phone_index rpi;
    spfy_vb_labl_map rlm;
    memset(&own, 0, sizeof own);
    memset(&rpi, 0, sizeof rpi);
    size_t *cnt = NULL, *fill = NULL;
    double *vals = NULL;
    int have_pi = 0;

    int rc = spfy_vb_riff_load(ref_vin, &own);
    if (rc != SPFY_OK) return rc;

    const spfy_vb_chunk *feat = spfy_vb_riff_get(&own, "feat");
    const spfy_vb_chunk *ccos = spfy_vb_riff_get(&own, "ccos");
    if (!feat || !ccos) { rc = SPFY_E_FORMAT; goto done; }
    rc = spfy_vb_phone_index_build(feat->data, feat->n, &rpi);
    if (rc != SPFY_OK) goto done;
    have_pi = 1;
    rc = spfy_vb_labl_map_build(ccos->data, ccos->n, feat->data, feat->n, &rlm);
    if (rc != SPFY_OK) goto done;

    tmpl_unit_view uv;
    rc = tmpl_units(&own, &uv);
    if (rc != SPFY_OK) goto done;

    size_t np = rpi.n;
    if (!np) { rc = SPFY_E_FORMAT; goto done; }
    cnt  = (size_t *)calloc(np + 1u, sizeof *cnt);
    fill = (size_t *)calloc(np + 1u, sizeof *fill);
    if (!cnt || !fill) { rc = SPFY_E_NOMEM; goto done; }

    /* Counting sort by phone: one pass to size the buckets, one to fill.
     * `dl > 0` matches the Python -- a zero-length record is a hole in the
     * reference's own table and says nothing about how short the phone runs. */
    size_t total = 0;
    for (size_t i = 0; i < uv.n; ++i) {
        const uint8_t *r = uv.data + i * uv.stride;
        int fid = rlm.l2f[r[uv.o_pc]];
        long dl = (long)r[uv.o_dur] | ((long)r[uv.o_dur + 1u] << 8);
        if (fid < 0 || (size_t)fid >= np || dl <= 0) continue;
        ++cnt[fid];
        ++total;
    }
    for (size_t p = 1; p <= np; ++p) fill[p] = fill[p - 1u] + cnt[p - 1u];
    vals = (double *)malloc((total ? total : 1u) * sizeof *vals);
    if (!vals) { rc = SPFY_E_NOMEM; goto done; }
    for (size_t i = 0; i < uv.n; ++i) {
        const uint8_t *r = uv.data + i * uv.stride;
        int fid = rlm.l2f[r[uv.o_pc]];
        long dl = (long)r[uv.o_dur] | ((long)r[uv.o_dur + 1u] << 8);
        if (fid < 0 || (size_t)fid >= np || dl <= 0) continue;
        vals[fill[fid]++] = (double)dl;
    }

    size_t base = 0, set = 0;
    for (size_t p = 0; p < np; ++p) {
        size_t n = cnt[p];
        if (n >= SPFY_VB_DUR_PCT_MIN_N) {
            qsort(vals + base, n, sizeof *vals, cmp_dbl);
            /* BY NAME, because phone_center is in LABL space and the
             * reference need not order it as the template does. */
            int tid = spfy_vb_phone_id(&t->pidx, rpi.name[p]);
            if (tid >= 0 && (size_t)tid < out_n) {
                out[tid] = pct_linear(vals + base, n, pct);
                ++set;
            }
        }
        base += n;
    }
    if (n_set) *n_set = set;
    if (n_phones) *n_phones = t->pidx.n;

done:
    free(vals);
    free(fill);
    free(cnt);
    if (have_pi) spfy_vb_phone_index_free(&rpi);
    spfy_vb_riff_free(&own);
    return rc;
}

static uint8_t f0_quant(const spfy_vb_template *t, double hz);

/* ⭐⭐ A ZERO f0 BYTE IS THE DP's VOICING BIT, NOT A MISSING VALUE.
 *
 * usel/viterbi.c walks a state machine over these bytes: a unit with
 * f0_mid >= 21 RESETS "milliseconds since the last voiced pitch mark", and the
 * F0-probability join gate only fires inside a voiced stretch
 * (curr.c6c > 20 && prev_c80 < 15 && prev_c7c > 20). The threshold 21 is far
 * below any real stored value -- the vendors' non-zero range is ~100-155 -- so
 * it is a pure zero/non-zero test.
 *
 * Sampling ONE frame therefore hands the DP a voicing map built out of pitch
 * tracker dropouts. Measured, the share of units-marked-unvoiced that actually
 * sit on a voiceless phone:
 *
 *     jill 75.7%   tom 75.6%   felix 79.7%   javier 64.0%   OURS 48.3%
 *
 * -- barely better than chance, and javier marks 100% of his voiceless phones.
 * A single dropped frame at the midpoint was condemning a whole voiced unit.
 *
 * So use the whole extent: take the frame if it is voiced, else the median of
 * every voiced frame in the unit, and report unvoiced only when NOTHING in the
 * unit is voiced. Median by 256-bin histogram -- no allocation, and the values
 * are bytes already. */
/* ⚠ THE RESCUE MUST BE GATED ON THE PHONE. Rescuing every empty frame also
 * rescues voiceless phones that caught a little voicing bleed from a
 * neighbour, and those SHOULD read unvoiced: doing it ungated took the share
 * of voiceless phones marked unvoiced from 39.5% down to 20.3% (jill 48.3%,
 * javier 100%) while precision fell too. Only a VOICED phone may be rescued;
 * a voiceless one keeps whatever the tracker said, exactly as before. */
static int phone_is_voiceless(const char *nm)
{
    static const char *V[] = { "p", "t", "k", "f", "th", "s", "sh", "ch",
                               "hh", "pau" };
    if (!nm) return 0;
    for (size_t i = 0; i < sizeof V / sizeof *V; ++i)
        if (!strcmp(nm, V[i])) return 1;
    return 0;
}

/* ⛔⛔ THE ZERO IS THE DP'S VOICING BIT, SO THE PHONE DECIDES IT -- NOT THE
 * TRACKER. `viterbi.c` resets "ms since voiced" on a run of >= 21 zeros and
 * that feeds the join-miss gate, so a zero on a voiced phone is not a missing
 * measurement, it is a false statement that steers the DP.
 *
 * The old order asked the tracker FIRST and only fell back to the phone, which
 * is backwards in both directions and measured so against the vendors:
 *
 *                       of f0==0, voiceless   of voiceless phones, marked 0
 *     jill                        76.7%                   70.2%
 *     tom                         73.4%                   79.2%
 *     ours (tracker-first)        56.8%                   33.8%
 *
 * A tracker leaks pitch onto /s/ and /f/, so a voiceless phone came back
 * nonzero; and a voiced phone whose span the tracker missed entirely came back
 * 0. Both are now impossible: voiceless is answered before the track is read,
 * and a voiced phone falls back to the recording's own median rather than
 * claiming to be unvoiced. `rec_med` is that median, quantised by the caller's
 * own transfer; 0 means the caller has none and the old behaviour stands. */
static uint8_t f0_span(const spfy_vb_template *t, const uint8_t *trk,
                       size_t n, int lo, int hi, int at, int voiceless,
                       double rec_med, size_t *n_rescued)
{
    if (!trk || !n) return 0;
    if (voiceless) return 0;
    if (at >= 0 && (size_t)at < n && trk[at]) return f0_quant(t, trk[at]);
    if (lo < 0) lo = 0;
    if (hi < lo) hi = lo;
    uint32_t hist[256];
    memset(hist, 0, sizeof hist);
    uint32_t tot = 0;
    for (int i = lo; i <= hi && (size_t)i < n; ++i)
        if (trk[i]) { ++hist[trk[i]]; ++tot; }
    if (tot) {
        uint32_t half = tot / 2u, run = 0;
        for (uint32_t v = 1; v < 256u; ++v) {
            run += hist[v];
            if (run > half) {
                if (n_rescued) ++*n_rescued;
                return f0_quant(t, (double)v);
            }
        }
    }
    /* Voiced, but the tracker found nothing anywhere in the span. */
    if (rec_med > 0.0) {
        if (n_rescued) ++*n_rescued;
        return f0_quant(t, rec_med);
    }
    return 0;
}

/* Median of the non-zero frames of one recording's track, for the fallback
 * above. Zero if the recording has no voiced frame at all. */
static double f0_track_median(const uint8_t *trk, size_t n)
{
    if (!trk || !n) return 0.0;
    uint32_t hist[256];
    memset(hist, 0, sizeof hist);
    uint32_t tot = 0;
    for (size_t i = 0; i < n; ++i)
        if (trk[i]) { ++hist[trk[i]]; ++tot; }
    if (!tot) return 0.0;
    uint32_t half = tot / 2u, run = 0;
    for (uint32_t v = 1; v < 256u; ++v) {
        run += hist[v];
        if (run > half) return (double)v;
    }
    return 0.0;
}

static uint8_t f0_quant(const spfy_vb_template *t, double hz)
{
    double v = t->f0q_fitted ? t->f0q_slope * hz + t->f0q_off : hz;
    long r = lround(v);
    if (r < SPFY_VB_F0_ABSENT_MAX + 1) r = SPFY_VB_F0_ABSENT_MAX + 1;
    if (r > 255) r = 255;
    return (uint8_t)r;
}

/* ====================================================================== */
/* difflib.SequenceMatcher.ratio()                                         */

/* 2*M/T over the matching blocks, with autojunk: for len(b) >= 200, an
 * element occupying more than 1% of b is "popular" and excluded from the
 * index, which is what stops one very common element from dominating. The
 * guard this feeds decided whether 92 recordings' anchors were believed, so
 * it is ported rather than approximated. */

#define B2J_BUCKETS 4096u

typedef struct b2j_node {
    int32_t          v;
    uint32_t        *idx;
    uint32_t         n, cap;
    struct b2j_node *next;
} b2j_node;

static b2j_node *b2j_get(b2j_node **tab, int32_t v, int create)
{
    uint32_t h = ((uint32_t)v * 2654435761u) % B2J_BUCKETS;
    for (b2j_node *e = tab[h]; e; e = e->next)
        if (e->v == v) return e;
    if (!create) return NULL;
    b2j_node *e = (b2j_node *)calloc(1, sizeof *e);
    if (!e) return NULL;
    e->v = v;
    e->next = tab[h];
    tab[h] = e;
    return e;
}

typedef struct { size_t i, j, n; } match_t;

static match_t find_longest(const int32_t *a, const int32_t *b,
                            b2j_node **tab,
                            size_t alo, size_t ahi, size_t blo, size_t bhi,
                            int *j2len, int *newj2len, size_t nb)
{
    size_t besti = alo, bestj = blo, bestsize = 0;
    memset(j2len, 0, (nb + 1u) * sizeof *j2len);
    for (size_t i = alo; i < ahi; ++i) {
        memset(newj2len, 0, (nb + 1u) * sizeof *newj2len);
        b2j_node *e = b2j_get(tab, a[i], 0);
        if (e) {
            for (uint32_t t = 0; t < e->n; ++t) {
                size_t j = e->idx[t];
                if (j < blo) continue;
                if (j >= bhi) break;
                int k = (j ? j2len[j - 1] : 0) + 1;
                newj2len[j] = k;
                if ((size_t)k > bestsize) {
                    besti = i - (size_t)k + 1u;
                    bestj = j - (size_t)k + 1u;
                    bestsize = (size_t)k;
                }
            }
        }
        memcpy(j2len, newj2len, (nb + 1u) * sizeof *j2len);
    }
    /* difflib then extends the match over junk/non-junk; with no junk
     * classes the two extension loops below are the whole of it. */
    while (besti > alo && bestj > blo && a[besti - 1] == b[bestj - 1]) {
        --besti; --bestj; ++bestsize;
    }
    while (besti + bestsize < ahi && bestj + bestsize < bhi &&
           a[besti + bestsize] == b[bestj + bestsize]) {
        ++bestsize;
    }
    match_t m = { besti, bestj, bestsize };
    return m;
}

double spfy_vb_seq_ratio(const int32_t *a, size_t na,
                         const int32_t *b, size_t nb)
{
    if (na + nb == 0) return 1.0;
    b2j_node **tab = (b2j_node **)calloc(B2J_BUCKETS, sizeof *tab);
    if (!tab) return 0.0;
    for (size_t j = 0; j < nb; ++j) {
        b2j_node *e = b2j_get(tab, b[j], 1);
        if (!e) goto done;
        if (e->n == e->cap) {
            uint32_t nc = e->cap ? e->cap * 2u : 4u;
            uint32_t *nv = (uint32_t *)realloc(e->idx, (size_t)nc * sizeof *nv);
            if (!nv) goto done;
            e->idx = nv;
            e->cap = nc;
        }
        e->idx[e->n++] = (uint32_t)j;
    }
    /* autojunk */
    if (nb >= 200) {
        uint32_t ntest = (uint32_t)(nb / 100u) + 1u;
        for (uint32_t h = 0; h < B2J_BUCKETS; ++h)
            for (b2j_node *e = tab[h]; e; e = e->next)
                if (e->n > ntest) e->n = 0;
    }

    {
        int *j2len    = (int *)malloc((nb + 1u) * sizeof *j2len);
        int *newj2len = (int *)malloc((nb + 1u) * sizeof *newj2len);
        if (!j2len || !newj2len) { free(j2len); free(newj2len); goto done; }

        /* Iterative queue, as difflib's get_matching_blocks does it. */
        size_t qcap = 64, qn = 1;
        size_t (*q)[4] = malloc(qcap * sizeof *q);
        if (!q) { free(j2len); free(newj2len); goto done; }
        q[0][0] = 0; q[0][1] = na; q[0][2] = 0; q[0][3] = nb;
        size_t total = 0;
        while (qn) {
            size_t alo = q[qn - 1][0], ahi = q[qn - 1][1];
            size_t blo = q[qn - 1][2], bhi = q[qn - 1][3];
            --qn;
            match_t m = find_longest(a, b, tab, alo, ahi, blo, bhi,
                                     j2len, newj2len, nb);
            if (!m.n) continue;
            total += m.n;
            if (qn + 2 > qcap) {
                size_t nc = qcap * 2u;
                size_t (*nq)[4] = realloc(q, nc * sizeof *nq);
                if (!nq) break;
                q = nq;
                qcap = nc;
            }
            if (alo < m.i && blo < m.j) {
                q[qn][0] = alo; q[qn][1] = m.i; q[qn][2] = blo; q[qn][3] = m.j;
                ++qn;
            }
            if (m.i + m.n < ahi && m.j + m.n < bhi) {
                q[qn][0] = m.i + m.n; q[qn][1] = ahi;
                q[qn][2] = m.j + m.n; q[qn][3] = bhi;
                ++qn;
            }
        }
        free(q);
        free(j2len);
        free(newj2len);

        for (uint32_t h = 0; h < B2J_BUCKETS; ++h) {
            b2j_node *e = tab[h];
            while (e) { b2j_node *nx = e->next; free(e->idx); free(e); e = nx; }
        }
        free(tab);
        return 2.0 * (double)total / (double)(na + nb);
    }

done:
    for (uint32_t h = 0; h < B2J_BUCKETS; ++h) {
        b2j_node *e = tab[h];
        while (e) { b2j_node *nx = e->next; free(e->idx); free(e); e = nx; }
    }
    free(tab);
    return 0.0;
}

/* ====================================================================== */
/* FE <-> MFA alignment                                                    */

static const char *PHONE_NORM_FROM[] = { "ax", "ix", "dx", "el", "en" };
static const char *PHONE_NORM_TO[]   = { "ah", "ih", "t",  "l",  "n"  };

static void norm_phone(const char *in, char *out, size_t out_n)
{
    for (int i = 0; i < 5; ++i) {
        if (strcmp(in, PHONE_NORM_FROM[i]) == 0) {
            snprintf(out, out_n, "%s", PHONE_NORM_TO[i]);
            return;
        }
    }
    snprintf(out, out_n, "%s", in);
}

/* ⭐ THE FE's SYLLABICS ARE SYLLABLES; english_us_arpa HAS NO SUCH PHONE.
 * It writes "vehicle" as ... K AH0 L, so norm_phone (el->l) makes NW pair the
 * FE's single `el` with MFA's `L` and leave the `AH0` unpaired. The unit then
 * carries the consonant alone. Measured: our el/l duration ratio was 0.94
 * where jill is 1.57 and tom 1.48 -- two independent vendor controls agreeing
 * that a syllabic runs about half again as long as a plain one. That missing
 * nucleus is what made "vehicle" sound cut off.
 *
 * ⛔ `el` ONLY, and the vendor controls are why. Merging `en` too took our
 * en/n ratio from 1.45 to 2.12 where jill is 1.15 and tom 1.03 -- our `en`
 * already carried its nucleus by some other route, so the merge double-counts
 * it. el/l lands at 1.65 against jill 1.57 / tom 1.48, which is the band.
 * (Our en/n of 1.45 is still above both vendors, but that predates this and
 * is not something to "fix" without its own measurement.) */
static int is_syllabic(const char *p)
{
    return !strcmp(p, "el");
}

/* The raw MFA label, stress digit intact -- mfa_ph has already had it
 * stripped, and AH0 must not be confused with a stressed AH. */
static int is_schwa_label(const char *lab)
{
    return lab && (!strncmp(lab, "AH0", 3) || !strncmp(lab, "ah0", 3)
                   || !strncmp(lab, "IH0", 3) || !strncmp(lab, "ih0", 3));
}

static int is_vowel(const char *p)
{
    static const char *V[] = { "aa","ae","ah","ao","aw","ay","eh","er","ey",
                               "ih","iy","ow","oy","uh","uw" };
    for (int i = 0; i < 15; ++i) if (!strcmp(p, V[i])) return 1;
    return 0;
}

static int sim_score(const char *x, const char *y)
{
    if (!strcmp(x, y)) return 2;
    int vx = is_vowel(x), vy = is_vowel(y);
    if (vx && vy) return 1;
    if (*x && *y && !vx && !vy) return 0;
    return -1;
}

/* Needleman-Wunsch over PHONE_NORM-collapsed labels, gap -0.5. Returns the
 * number of pairs, or 0 when fewer than min_frac*min(n,m) pair up -- which
 * is how a recording whose transcript describes different audio is rejected
 * rather than force-fitted. */
static size_t nw_align(char (*a)[8], size_t n, char (*b)[8], size_t m,
                       uint32_t *pa, uint32_t *pb, double min_frac)
{
    if (!n || !m) return 0;
    double *prev = (double *)malloc((m + 1u) * sizeof *prev);
    double *cur  = (double *)malloc((m + 1u) * sizeof *cur);
    int8_t *bt   = (int8_t *)malloc((n + 1u) * (m + 1u));
    if (!prev || !cur || !bt) { free(prev); free(cur); free(bt); return 0; }

    const double gap = -0.5;
    for (size_t j = 0; j <= m; ++j) { prev[j] = gap * (double)j; bt[j] = 2; }
    bt[0] = 0;
    for (size_t i = 1; i <= n; ++i) {
        cur[0] = gap * (double)i;
        bt[i * (m + 1u)] = 1;
        for (size_t j = 1; j <= m; ++j) {
            double d = prev[j - 1] + sim_score(a[i - 1], b[j - 1]);
            double u = prev[j] + gap;
            double l = cur[j - 1] + gap;
            if (d >= u && d >= l)      { cur[j] = d; bt[i * (m + 1u) + j] = 0; }
            else if (u >= l)           { cur[j] = u; bt[i * (m + 1u) + j] = 1; }
            else                       { cur[j] = l; bt[i * (m + 1u) + j] = 2; }
        }
        double *tmp = prev; prev = cur; cur = tmp;
    }
    size_t np = 0, i = n, j = m;
    while (i > 0 || j > 0) {
        if (i > 0 && j > 0 && bt[i * (m + 1u) + j] == 0) {
            pa[np] = (uint32_t)(i - 1u);
            pb[np] = (uint32_t)(j - 1u);
            ++np;
            --i; --j;
        } else if (i > 0 && (j == 0 || bt[i * (m + 1u) + j] == 1)) {
            --i;
        } else {
            --j;
        }
    }
    free(prev); free(cur); free(bt);
    for (size_t k = 0; k < np / 2u; ++k) {
        uint32_t t = pa[k]; pa[k] = pa[np - 1u - k]; pa[np - 1u - k] = t;
        t = pb[k]; pb[k] = pb[np - 1u - k]; pb[np - 1u - k] = t;
    }
    size_t lo = n < m ? n : m;
    if ((double)np < min_frac * (double)lo) return 0;
    return np;
}

/* ====================================================================== */
/* S1                                                                      */

typedef struct {
    int32_t  f;          /* feat phone id */
    int32_t  lo, mid, hi;
    uint32_t fe_i;
} seq_ent;

static int is_silence_label(const char *s)
{
    char t[32];
    size_t n = 0;
    for (const char *p = s; *p && n < sizeof t - 1u; ++p) {
        if ((unsigned char)*p <= ' ') continue;
        t[n++] = (char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
    }
    t[n] = 0;
    return !n || !strcmp(t, "sil") || !strcmp(t, "sp") || !strcmp(t, "spn")
           || !strcmp(t, "silence") || !strcmp(t, "<sil>") || !strcmp(t, "<unk>");
}

/* ---------------------------------------------------------------------- */
/* Drop list                                                                */

static int cmp_cstr(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* vb_build1.py's --drop: JSON with an `exclude` array of stems. A plain text
 * file, one stem per line with '#' comments, is accepted too -- the decision
 * is usually made by hand and JSON quoting is the only part of it that has
 * ever gone wrong.
 *
 * Only the `exclude` array is read, so the `_note` and `voice` keys every
 * shipped list carries are ignored rather than tripping a parser. */
static int drop_list_load(const char *path, char ***out, size_t *out_n)
{
    *out = NULL;
    *out_n = 0;
    char *txt = NULL;
    size_t tn = 0;
    if (spfy_vb_read_text(path, &txt, &tn) != SPFY_OK) return SPFY_E_IO;

    char **v = NULL;
    size_t n = 0, cap = 0;
    const char *p = strstr(txt, "\"exclude\"");
    if (p) {
        while (*p && *p != '[') ++p;
        if (*p == '[') ++p;
        while (*p && *p != ']') {
            if (*p != '"') { ++p; continue; }
            ++p;
            const char *s = p;
            while (*p && *p != '"') p += (*p == '\\' && p[1]) ? 2 : 1;
            size_t ln = (size_t)(p - s);
            if (*p == '"') ++p;
            char *e = (char *)malloc(ln + 1u);
            if (!e) goto oom;
            memcpy(e, s, ln);
            e[ln] = '\0';
            if (n == cap) {
                size_t nc = cap ? cap * 2u : 256u;
                char **nv = (char **)realloc(v, nc * sizeof *nv);
                if (!nv) { free(e); goto oom; }
                v = nv; cap = nc;
            }
            v[n++] = e;
        }
    } else {
        for (char *line = txt; line && *line; ) {
            char *nl = strpbrk(line, "\r\n");
            if (nl) *nl = '\0';
            while (*line == ' ' || *line == '\t') ++line;
            size_t ln = strlen(line);
            while (ln && (line[ln - 1u] == ' ' || line[ln - 1u] == '\t')) --ln;
            if (ln && line[0] != '#') {
                char *e = (char *)malloc(ln + 1u);
                if (!e) goto oom;
                memcpy(e, line, ln);
                e[ln] = '\0';
                if (n == cap) {
                    size_t nc = cap ? cap * 2u : 256u;
                    char **nv = (char **)realloc(v, nc * sizeof *nv);
                    if (!nv) { free(e); goto oom; }
                    v = nv; cap = nc;
                }
                v[n++] = e;
            }
            line = nl ? nl + 1u : NULL;
        }
    }
    free(txt);
    if (n > 1) qsort(v, n, sizeof *v, cmp_cstr);
    *out = v;
    *out_n = n;
    return SPFY_OK;

oom:
    for (size_t i = 0; i < n; ++i) free(v[i]);
    free(v);
    free(txt);
    return SPFY_E_NOMEM;
}

/* ---- --compress: keep only the audio spans the inventory actually needs ----
 *
 * ⭐ CHUNK GRANULARITY IS THE WRONG UNIT FOR A SIZE CUT. Measured on
 * crsmara_pk1618: only 8.9% of units are ever picked over 1,588 held-out
 * sentences, but they are spread across 62.8% of chunks holding 78.3% of the
 * audio, so `--drop-chunks` drags four bytes of dead weight along for every
 * useful one. Cutting at the unit span instead keeps the SAME units in a
 * fraction of the bytes: 210,236 units (jill ships 185,475) in 90.8 MiB
 * against 243.0 MB for the whole corpus.
 *
 * The list is KEEP spans in recording-relative milliseconds, `stem<TAB>lo<TAB>hi`,
 * NOT unit ids. A uid is an artefact of the build that is about to change; a
 * time is the same in every build of the same corpus, which is what makes the
 * list reusable and what `--drop-chunks` already gets right by naming chunks.
 *
 * ⚠ THE SPANS MUST ALREADY CARRY THE WSOLA MARGIN. The overlap-add window
 * reads past a unit's last sample, and truncating exactly at the end changed
 * all three demo renders when vb_compact tried it with 8 ms instead of 20. */
typedef struct { uint32_t lo, hi; char *stem; } keep_in;
typedef struct { uint32_t lo, hi, off; size_t sid; } keep_eff;

static int cmp_keep_in(const void *a, const void *b)
{
    const keep_in *x = (const keep_in *)a, *y = (const keep_in *)b;
    int c = strcmp(x->stem, y->stem);
    if (c) return c;
    return (x->lo < y->lo) ? -1 : (x->lo > y->lo);
}

static int keep_load(const char *path, keep_in **out, size_t *out_n)
{
    char *txt = NULL;
    size_t n_txt = 0;
    if (spfy_vb_read_text(path, &txt, &n_txt) != SPFY_OK) return SPFY_E_IO;
    keep_in *v = NULL;
    size_t n = 0, cap = 0;
    char *line = txt;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        size_t ln = strlen(line);
        if (ln && line[ln - 1u] == '\r') line[--ln] = '\0';
        if (ln && line[0] != '#') {
            char *t1 = strchr(line, '\t');
            char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
            if (t1 && t2) {
                *t1 = *t2 = '\0';
                if (n == cap) {
                    size_t nc = cap ? cap * 2u : 1024u;
                    keep_in *nv = (keep_in *)realloc(v, nc * sizeof *nv);
                    if (!nv) goto oom;
                    v = nv; cap = nc;
                }
                size_t sl = strlen(line);
                char *s = (char *)malloc(sl + 1u);
                if (!s) goto oom;
                memcpy(s, line, sl + 1u);
                v[n].stem = s;
                v[n].lo = (uint32_t)strtoul(t1 + 1, NULL, 10);
                v[n].hi = (uint32_t)strtoul(t2 + 1, NULL, 10);
                if (v[n].hi > v[n].lo) ++n; else free(s);
            }
        }
        line = nl ? nl + 1u : NULL;
    }
    free(txt);
    if (n > 1) qsort(v, n, sizeof *v, cmp_keep_in);
    *out = v; *out_n = n;
    return SPFY_OK;

oom:
    for (size_t i = 0; i < n; ++i) free(v[i].stem);
    free(v);
    free(txt);
    return SPFY_E_NOMEM;
}

/* First index in the sorted list whose stem matches, or (size_t)-1. */
static size_t keep_find(const keep_in *v, size_t n, const char *stem)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        if (strcmp(v[mid].stem, stem) < 0) lo = mid + 1u; else hi = mid;
    }
    return (lo < n && !strcmp(v[lo].stem, stem)) ? lo : (size_t)-1;
}

/* The effective span covering ms `t`, or NULL. Linear over one recording's
 * spans, which is a handful; the caller walks units in time order anyway. */
static const keep_eff *keep_at(const keep_eff *e, size_t n, uint32_t t)
{
    for (size_t i = 0; i < n; ++i)
        if (t >= e[i].lo && t < e[i].hi) return &e[i];
    return NULL;
}

/* Membership test against the sorted chunk drop list, marking what was hit so
 * names that matched nothing can be reported by name afterwards. */
static int cdrop_has(char **v, size_t n, const char *nm, uint8_t *hit)
{
    char **f = (char **)bsearch(&nm, v, n, sizeof *v, cmp_cstr);
    if (!f) return 0;
    hit[f - v] = 1;
    return 1;
}

static int units_push(spfy_vb_corpus *c, size_t *cap, const spfy_vb_unit *u)
{
    if (c->n_units == *cap) {
        size_t nc = *cap ? *cap * 2u : 65536u;
        spfy_vb_unit *nv = (spfy_vb_unit *)realloc(c->units, nc * sizeof *nv);
        if (!nv) return SPFY_E_NOMEM;
        c->units = nv;
        *cap = nc;
    }
    c->units[c->n_units++] = *u;
    return SPFY_OK;
}

static int indx_push(spfy_vb_corpus *c, size_t *cap, uint32_t off, const char *nm)
{
    if (c->n_indx == *cap) {
        size_t nc = *cap ? *cap * 2u : 1024u;
        spfy_vb_indx_ent *nv =
            (spfy_vb_indx_ent *)realloc(c->indx, nc * sizeof *nv);
        if (!nv) return SPFY_E_NOMEM;
        c->indx = nv;
        *cap = nc;
    }
    size_t ln = strlen(nm);
    char *s = (char *)malloc(ln + 1u);
    if (!s) return SPFY_E_NOMEM;
    memcpy(s, nm, ln + 1u);
    c->indx[c->n_indx].off = off;
    c->indx[c->n_indx].name = s;
    ++c->n_indx;
    return SPFY_OK;
}

int spfy_vb_corpus_build(const spfy_vb_template *t,
                         const spfy_vb_corpus_cfg *cfg,
                         spfy_vb_corpus *out)
{
    memset(out, 0, sizeof *out);
    int rc = SPFY_OK;

    char **stems = NULL;
    size_t n_stems = 0;
    rc = spfy_vb_list_stems(cfg->wav_dir, &stems, &n_stems);
    if (rc != SPFY_OK) return rc;

    /* ⚠ A .seg IS AS GOOD AS A TEXTGRID -- better, in fact. Requiring a
     * TextGrid silently skipped every RVC-ingested recording, which has
     * engine segmentation and no aligner output at all. */
    size_t keep = 0;
    for (size_t i = 0; i < n_stems; ++i) {
        char p[1024];
        int have = 0;
        snprintf(p, sizeof p, "%s/%s.TextGrid", cfg->tg_dir, stems[i]);
        if (spfy_vb_file_exists(p)) have = 1;
        if (!have && cfg->seg_dir) {
            snprintf(p, sizeof p, "%s/%s.seg", cfg->seg_dir, stems[i]);
            if (spfy_vb_file_exists(p)) have = 1;
        }
        if (have) stems[keep++] = stems[i];
        else { ++out->n_no_tg; free(stems[i]); }
    }
    n_stems = keep;

    /* ⚠ NAME THE ONES THAT WERE NOT THERE. A drop list that matches nothing --
     * a renamed stem, a stale path -- builds the UNFILTERED voice and reports
     * success, and the A/B then measures two identical inventories and calls
     * the difference noise. */
    if (cfg->drop_path) {
        char **ex = NULL;
        size_t n_ex = 0;
        rc = drop_list_load(cfg->drop_path, &ex, &n_ex);
        if (rc != SPFY_OK) {
            spfy_log_err("vb: cannot read --drop %s", cfg->drop_path);
            spfy_vb_free_stems(stems, n_stems);
            return rc;
        }
        uint8_t *hit = (uint8_t *)calloc(n_ex ? n_ex : 1u, 1);
        if (!hit) {
            for (size_t i = 0; i < n_ex; ++i) free(ex[i]);
            free(ex);
            spfy_vb_free_stems(stems, n_stems);
            return SPFY_E_NOMEM;
        }
        size_t before = n_stems;
        keep = 0;
        for (size_t i = 0; i < n_stems; ++i) {
            char **f = (char **)bsearch(&stems[i], ex, n_ex, sizeof *ex, cmp_cstr);
            if (f) { hit[f - ex] = 1; free(stems[i]); continue; }
            stems[keep++] = stems[i];
        }
        n_stems = keep;
        out->n_drop_listed = n_ex;
        for (size_t i = 0; i < n_ex; ++i) if (hit[i]) ++out->n_drop_hit;
        out->n_drop_absent = n_ex - out->n_drop_hit;
        printf("drop list: %zu of %zu removed (%zu -> %zu recordings)\n",
               out->n_drop_hit, n_ex, before, n_stems);
        for (size_t i = 0; i < n_ex; ++i)
            if (!hit[i])
                printf("  ⚠ not in the corpus, so not dropped: %s\n", ex[i]);
        free(hit);
        for (size_t i = 0; i < n_ex; ++i) free(ex[i]);
        free(ex);
    }

    /* --compress: the loaded KEEP spans, and this stem's slice of them. `eff`
     * is the per-stem working set -- spans clipped to a chunk, each carrying
     * the ms offset it landed at in the emitted audio. */
    keep_in *kv_all = NULL;
    size_t   kv_all_n = 0, kv_first = 0, kv_n = 0;
    const keep_in *kv = NULL;
    keep_eff *eff = NULL;
    size_t    eff_n = 0, eff_cap = 0;
    char **cdrop = NULL;
    size_t n_cdrop = 0;
    uint8_t *cdrop_hit = NULL;
    if (cfg->drop_chunks_path) {
        rc = drop_list_load(cfg->drop_chunks_path, &cdrop, &n_cdrop);
        if (rc != SPFY_OK) {
            spfy_log_err("vb: cannot read --drop-chunks %s",
                         cfg->drop_chunks_path);
            spfy_vb_free_stems(stems, n_stems);
            return rc;
        }
        cdrop_hit = (uint8_t *)calloc(n_cdrop ? n_cdrop : 1u, 1);
        if (!cdrop_hit) {
            for (size_t i = 0; i < n_cdrop; ++i) free(cdrop[i]);
            free(cdrop);
            spfy_vb_free_stems(stems, n_stems);
            return SPFY_E_NOMEM;
        }
        out->n_cdrop_listed = n_cdrop;
        printf("chunk drop list: %zu names\n", n_cdrop);
    }

    if (cfg->compress_path) {
        rc = keep_load(cfg->compress_path, &kv_all, &kv_all_n);
        if (rc != SPFY_OK) {
            spfy_log_err("vb: cannot read --compress %s", cfg->compress_path);
            spfy_vb_free_stems(stems, n_stems);
            return rc;
        }
        size_t n_st = 0;
        for (size_t i = 0; i < kv_all_n; ++i)
            if (!i || strcmp(kv_all[i].stem, kv_all[i - 1u].stem)) ++n_st;
        out->n_keep_spans = kv_all_n;
        out->n_keep_stems = n_st;
        printf("compress: %zu keep span(s) over %zu recording(s)\n",
               kv_all_n, n_st);
    }

    if (cfg->limit > 0 && (size_t)cfg->limit < n_stems) {
        for (size_t i = (size_t)cfg->limit; i < n_stems; ++i) free(stems[i]);
        n_stems = (size_t)cfg->limit;
    }
    out->n_stems = n_stems;

    /* ⚠ file_idx is a u16. Exceeding it wraps SILENTLY: a build from 91,774
     * renders produced exactly 65,536 distinct values and ~26,000 recordings'
     * units addressed the wrong audio. Refuse rather than corrupt. */
    if (n_stems > 65535u) {
        spfy_log_err("vb: %zu recordings exceeds the u16 file_idx limit", n_stems);
        spfy_vb_free_stems(stems, n_stems);
        return SPFY_E_INVAL;
    }
    printf("%zu wav+boundary pairs\n", n_stems);

    spfy_vb_buf data = {0};
    size_t units_cap = 0, indx_cap = 0;

    /* Scratch that grows to the largest recording rather than per stem. */
    size_t sq_cap = 0;
    seq_ent *seq = NULL;
    uint32_t *pa = NULL, *pb = NULL;
    size_t pa_cap = 0;
    int32_t *fe_ids = NULL, *seg_ids = NULL;
    size_t id_cap = 0;
    uint32_t *bounds = NULL;
    size_t bounds_cap = 0;
    uint32_t *chunk_of = NULL;
    size_t chunk_cap = 0;
    /* chunk index -> indx position, or (size_t)-1 when the chunk was dropped */
    size_t *chunk_sid = NULL;
    size_t chunk_sid_cap = 0;
    /* seq index -> uid offset within this recording, counting only SURVIVING
     * entries. Without it every anchor span would still be computed as
     * base_uid + 2*k, which silently points at another word once anything in
     * front of it has been dropped. */
    uint32_t *seq_off = NULL;
    size_t seq_off_cap = 0;
    /* Per seq position: did it produce units, and which kept span holds it.
     * Under --compress "in a surviving chunk" is no longer the same question
     * as "has audio behind it". */
    uint8_t *pos_live = NULL;
    int32_t *pos_span = NULL;
    size_t pos_live_cap = 0;
    uint8_t *syl_start = NULL;
    size_t syl_cap = 0;
    int32_t *fe_feat = NULL;
    size_t ff_cap = 0;
    int32_t *fe_to_seq = NULL;
    size_t fts_cap = 0;

    for (size_t si = 0; si < n_stems; ++si) {
        const char *stem = stems[si];
        char path[1024];
        /* vb_build1.py reads this back off the indx name; the stem is the
         * same string and it is already in hand.
         *
         * ⚠ SYNTHETIC, not "RVC". Two sources are not her own audio: `rvc_*`
         * (Applio conversions) and `st2_*` (StyleTTS2 renders). Both must be
         * subject to --rvc-policy, or prefer-real silently fails to protect her
         * real recordings from them -- and it fails QUIETLY, because the build
         * log would simply report zero converted units. The prefix stays in the
         * indx name so provenance survives into the shipped voice. */
        /* ⚠ ONE PREDICATE, SHARED. This was a second copy of the prefix test
         * and drifted from the anchor-level one; both now ask
         * spfy_vb_stem_is_synth, which allow-lists the REAL <office>_<ts>_
         * shape instead of deny-listing prefixes it happens to know. */
        const int is_rvc = spfy_vb_stem_is_synth(stem);

        /* ⚠ A STEM WITH NO SPANS IS DROPPED ENTIRELY, not kept whole. The list
         * says what to KEEP; silence about a recording is not an exemption. */
        kv = NULL; kv_first = 0; kv_n = 0; eff_n = 0;
        if (kv_all) {
            size_t at = keep_find(kv_all, kv_all_n, stem);
            if (at == (size_t)-1) { ++out->n_comp_skipped; continue; }
            kv_first = at;
            while (at + kv_n < kv_all_n
                   && !strcmp(kv_all[at + kv_n].stem, stem)) ++kv_n;
            kv = kv_all;
        }

        /* ⛔⛔ TWO DIFFERENT CLAIMS THAT USED TO BE ONE FLAG.
         *
         * `is_rvc` means "not her real audio" and governs --rvc-policy. That
         * is right for both sources. Anchor suppression is a DIFFERENT claim:
         * that the recording's WORD TOKENS name text it does not speak. That
         * was measured on the Applio arm -- converted `_WORD_` records matched
         * the FE 54.53% against her 77.00%, and 92 of 792 recordings were
         * wrong WHOLESALE -- because a conversion was paired with a
         * transcript from elsewhere.
         *
         * ⭐ IT CANNOT BE TRUE OF `st2_*`. A StyleTTS2 render is GENERATED
         * FROM the text; the text is the input, so it cannot describe
         * different audio. Its `.fe` came from a fresh spfy_synth pass over
         * that same string. Sharing one flag silently suppressed 35,976
         * anchors -- roughly half the inventory, and the half whose text is
         * correct by construction -- in a corpus containing ZERO rvc_ files.
         *
         * What remains at risk for a render is ALIGNMENT, not text, and that
         * is gated separately (vb_alignaudit: st2 0.892 against a wrong-audio
         * control of 0.280). */
        const int anchor_text_suspect = !strncmp(stem, "rvc_", 4u);

        snprintf(path, sizeof path, "%s/%s.wav", cfg->wav_dir, stem);
        spfy_vb_wav w;
        if (spfy_vb_wav_read(path, &w) != SPFY_OK) continue;
        int bpms = w.rate / 1000;
        if (bpms < 1) bpms = 1;
        if (!out->sample_rate) out->sample_rate = (uint32_t)w.rate;
        else if (out->sample_rate != (uint32_t)w.rate) ++out->n_rate_mismatch;

        /* ⭐ LEVEL NORMALISATION, BEFORE THE u-LAW ENCODE so there is no
         * double quantisation. A recording's SPEECH level is the median of
         * the frames within 25 dB of its own loud frames, so leading silence
         * and a quietly-modulated office are treated alike, and it is scaled
         * to one global target.
         *
         * ⚠ NOT A QUALITY FIX, AND THE MEASUREMENT SAYS SO. Her per-recording
         * spread is 8.5 dB against jill's 8.5, so this closes no gap with the
         * vendor. What it buys is predictability: her median sits near
         * -16.9 dBFS against jill's -13.4 and tom's -11.4, so the whole voice
         * is quiet and every arm inherits it.
         *
         * ⚠ CLIPPING IS COUNTED AND REPORTED. Scaling up a recording that
         * already peaks near full scale is how a level pass silently makes
         * things worse, and a log that never mentions clipping cannot be
         * trusted to have looked. */
        if (cfg->level_target != 0.0 && w.n_samples >= 320u) {
            const size_t F = 160u;                 /* 20 ms at 8 kHz */
            size_t nf = w.n_samples / F;
            double *db = (double *)malloc(nf * sizeof *db);
            if (db) {
                for (size_t f = 0; f < nf; ++f) {
                    double acc = 0.0;
                    const int16_t *q = w.pcm + f * F;
                    for (size_t i = 0; i < F; ++i) acc += (double)q[i] * q[i];
                    db[f] = 20.0 * log10(sqrt(acc / (double)F + 1e-9)
                                         / 32768.0 + 1e-12);
                }
                double *cp = (double *)malloc(nf * sizeof *cp);
                if (cp) {
                    memcpy(cp, db, nf * sizeof *cp);
                    qsort(cp, nf, sizeof *cp, cmp_dbl);
                    double x = 0.95 * (double)(nf - 1u);
                    size_t lo = (size_t)x, hi = lo + 1u < nf ? lo + 1u : lo;
                    double p95 = cp[lo] + (x - (double)lo) * (cp[hi] - cp[lo]);
                    /* Median of the speech frames only. */
                    size_t ns = 0;
                    for (size_t f = 0; f < nf; ++f)
                        if (db[f] > p95 - 25.0) cp[ns++] = db[f];
                    if (ns >= 4u) {
                        qsort(cp, ns, sizeof *cp, cmp_dbl);
                        double cur = (ns & 1u) ? cp[ns / 2u]
                                   : 0.5 * (cp[ns / 2u - 1u] + cp[ns / 2u]);
                        double g = pow(10.0, (cfg->level_target - cur) / 20.0);
                        if (g > cfg->level_max_gain) g = cfg->level_max_gain;
                        /* Cap by this recording's own peak. u-law clips at
                         * 8159 on the >>2 input, i.e. 32636 here, so the
                         * ceiling is expressed against that and not 32768. */
                        if (cfg->level_peak_dbfs != 0.0) {
                            int32_t pk = 0;
                            for (size_t i = 0; i < w.n_samples; ++i) {
                                int32_t v = w.pcm[i];
                                if (v < 0) v = -v;
                                if (v > pk) pk = v;
                            }
                            if (pk > 0) {
                                double lim = 32636.0
                                    * pow(10.0, cfg->level_peak_dbfs / 20.0)
                                    / (double)pk;
                                if (lim < g) {
                                    out->level_short_db +=
                                        20.0 * log10(g / lim);
                                    ++out->n_level_peaklim;
                                    g = lim;
                                }
                            }
                        }
                        for (size_t i = 0; i < w.n_samples; ++i) {
                            double v = (double)w.pcm[i] * g;
                            if (v > 32767.0)  { v = 32767.0;  ++out->n_clip; }
                            if (v < -32768.0) { v = -32768.0; ++out->n_clip; }
                            w.pcm[i] = (int16_t)(v < 0 ? v - 0.5 : v + 0.5);
                        }
                        out->n_samp_level += w.n_samples;
                        out->level_gain_db += 20.0 * log10(g > 1e-9 ? g : 1e-9);
                        ++out->n_leveled;
                    }
                    free(cp);
                }
                free(db);
            }
        }

        /* ⚠ file_idx must be the INDX POSITION, not the loop index: the
         * format check above can skip a wav, and one rejected file would
         * shift every later unit onto a different recording's audio.
         *
         * ⚠ THE AUDIO IS NOT WRITTEN HERE ANY MORE. Chunk boundaries are not
         * known until the segmentation has been read, and a compacting build
         * must emit only the chunks that survive -- so both the indx entries
         * and the u-law now go out together, after chunking. */
        size_t ulaw_n = w.n_samples;

        /* ⛔ EVERY RECORDING MUST BE A WHOLE NUMBER OF MILLISECONDS.
         * The engine addresses audio as indx[file_idx] + local_pos*bpms, so a
         * recording whose stored length is not a multiple of bpms pushes every
         * LATER recording off the millisecond grid and each of its units is
         * fetched at a sub-millisecond phase offset from where local_pos
         * points. All five vendor VDBs are 100% aligned; ours measured 90.4%.
         * The tail dropped here is below the `end_ms` floor further down, so
         * no unit could address it in the first place -- which is also why the
         * clamp there stops firing on this cause. */
        ulaw_n -= ulaw_n % (size_t)bpms;

        /* Per-recording envelope, for the silent-unit trim. Relative to THIS
         * recording, so a quietly-recorded office is not read as silence. */
        double *env = NULL, p95 = 0.0;
        if (cfg->trim_silence && w.n_samples >= 800) {
            env = (double *)malloc(w.n_samples * sizeof *env);
            if (env) {
                /* np.convolve(x*x, ones(40)/40, mode="same") is CENTRED: for
                 * kernel length 40 the window is [i-20, i+19]. A trailing
                 * window shifts every boundary by 20 samples. */
                const long W = 40, LEFT = 20;
                double acc = 0.0;
                for (long i = 0; i < (long)w.n_samples; ++i) {
                    long add = i + W - 1L - LEFT;
                    long sub = i - LEFT - 1L;
                    if (i == 0) {
                        for (long k = -LEFT; k <= W - 1L - LEFT; ++k)
                            if (k >= 0 && k < (long)w.n_samples) {
                                double v = (double)w.pcm[k];
                                acc += v * v;
                            }
                    } else {
                        if (add >= 0 && add < (long)w.n_samples) {
                            double v = (double)w.pcm[add];
                            acc += v * v;
                        }
                        if (sub >= 0 && sub < (long)w.n_samples) {
                            double v = (double)w.pcm[sub];
                            acc -= v * v;
                        }
                    }
                    env[i] = sqrt(acc / (double)W);
                }
                double *cp = (double *)malloc(w.n_samples * sizeof *cp);
                if (cp) {
                    memcpy(cp, env, w.n_samples * sizeof *cp);
                    qsort(cp, w.n_samples, sizeof *cp, cmp_dbl);
                    /* np.percentile is linearly interpolated, not nearest. */
                    double x = 0.95 * (double)(w.n_samples - 1u);
                    size_t lo = (size_t)x;
                    size_t hi = lo + 1u < w.n_samples ? lo + 1u : lo;
                    p95 = cp[lo] + (x - (double)lo) * (cp[hi] - cp[lo]);
                    free(cp);
                }
            }
        }

        /* ---- boundaries: .seg wins where it exists ---- */
        spfy_vb_segent *seg = NULL;
        size_t n_seg = 0, seg_bad = 0;
        if (cfg->seg_dir) {
            snprintf(path, sizeof path, "%s/%s.seg", cfg->seg_dir, stem);
            if (spfy_vb_file_exists(path)) {
                if (spfy_vb_seg_read(path, &seg, &n_seg, &seg_bad) == SPFY_OK && n_seg)
                    out->n_seg_bad += seg_bad;
                else { free(seg); seg = NULL; n_seg = 0; }
            }
        }

        /* ---- the FE ---- */
        snprintf(path, sizeof path, "%s/%s.fe", cfg->wav_dir, stem);
        char *fe_txt = NULL;
        size_t fe_n = 0;
        spfy_vb_phones fe = {0};
        if (spfy_vb_read_text(path, &fe_txt, &fe_n) == SPFY_OK)
            spfy_vb_fe_phones(fe_txt, &fe);
        else
            ++out->n_no_fe;

        /* ⛔⛔ THE `.seg` PATH HAD NO CROSS-CHECK, AND THAT COST A CORPUS.
         * Units come from the engine's segmentation, word anchors from the FE
         * tagging; nothing tied them together, so a `.fe` written from a
         * DIFFERENT phrase named that other sentence's words and every index
         * still resolved. Keep the units, refuse the anchors. */
        int fe_ok = 1;
        if (seg && fe.n && n_seg) {
            if (id_cap < fe.n + n_seg) {
                size_t nc = fe.n + n_seg + 64u;
                int32_t *n1 = (int32_t *)realloc(fe_ids, nc * sizeof *n1);
                int32_t *n2 = (int32_t *)realloc(seg_ids, nc * sizeof *n2);
                if (!n1 || !n2) { free(n1 ? n1 : fe_ids); rc = SPFY_E_NOMEM; goto stem_fail; }
                fe_ids = n1; seg_ids = n2; id_cap = nc;
            }
            size_t na = 0, nb = 0;
            for (size_t k = 0; k < fe.n; ++k) {
                int id = spfy_vb_phone_id(&t->pidx, fe.ph[k]);
                if (id >= 0) fe_ids[na++] = id;
            }
            for (size_t k = 0; k < n_seg; ++k) seg_ids[nb++] = seg[k].phone;
            double ratio = spfy_vb_seq_ratio(fe_ids, na, seg_ids, nb);
            if (ratio < SPFY_VB_FE_SEG_MIN_RATIO) {
                ++out->n_fe_mismatch;
                fe_ok = 0;
                if (out->n_fe_mismatch <= 12)
                    printf("     %-28s %.3f\n", stem, ratio);
            }
        }

        /* ---- the alignment, when there is no .seg ---- */
        spfy_vb_interval *mfa = NULL;
        size_t n_mfa = 0, n_pairs = 0;
        char (*mfa_ph)[8] = NULL;
        if (!seg) {
            snprintf(path, sizeof path, "%s/%s.TextGrid", cfg->tg_dir, stem);
            spfy_vb_textgrid_phones(path, &mfa, &n_mfa);
            if (n_mfa) {
                mfa_ph = (char (*)[8])malloc(n_mfa * sizeof *mfa_ph);
                if (!mfa_ph) { rc = SPFY_E_NOMEM; goto stem_fail; }
                for (size_t k = 0; k < n_mfa; ++k) {
                    /* SILENCE contains "", so MFA's untitled gaps become pau. */
                    char lab[32];
                    if (is_silence_label(mfa[k].label)) snprintf(lab, sizeof lab, "pau");
                    else {
                        size_t o = 0;
                        for (const char *p = mfa[k].label; *p && o < sizeof lab - 1u; ++p) {
                            if (*p >= '0' && *p <= '9') continue;
                            lab[o++] = (char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
                        }
                        lab[o] = 0;
                    }
                    norm_phone(lab, mfa_ph[k], sizeof mfa_ph[k]);
                }
            }
            if (pa_cap < fe.n + n_mfa + 4u) {
                size_t nc = fe.n + n_mfa + 64u;
                uint32_t *n1 = (uint32_t *)realloc(pa, nc * sizeof *n1);
                uint32_t *n2 = (uint32_t *)realloc(pb, nc * sizeof *n2);
                if (!n1 || !n2) { rc = SPFY_E_NOMEM; goto stem_fail; }
                pa = n1; pb = n2; pa_cap = nc;
            }
            char (*fe_norm)[8] = NULL;
            if (fe.n) {
                fe_norm = (char (*)[8])malloc(fe.n * sizeof *fe_norm);
                if (!fe_norm) { rc = SPFY_E_NOMEM; goto stem_fail; }
                for (size_t k = 0; k < fe.n; ++k)
                    norm_phone(fe.ph[k], fe_norm[k], sizeof fe_norm[k]);
            }
            n_pairs = (fe.n && n_mfa)
                    ? nw_align(fe_norm, fe.n, mfa_ph, n_mfa, pa, pb, 0.5) : 0;
            free(fe_norm);
            if (!n_pairs) {
                ++out->n_skip_align;
                free(mfa_ph); free(mfa);
                spfy_vb_phones_free(&fe);
                free(fe_txt);
                free(env);
                spfy_vb_wav_free(&w);
                continue;
            }
        }

        /* ---- sidecars ---- */
        snprintf(path, sizeof path, "%s/%s.f0", cfg->wav_dir, stem);
        uint8_t *f0trk = NULL;
        size_t f0_n = 0;
        if (spfy_vb_read_bytes(path, &f0trk, &f0_n) != SPFY_OK || !f0_n)
            ++out->n_no_f0;
        /* Once per recording, for f0_span()'s voiced fallback. */
        const double f0_med = f0_track_median(f0trk, f0_n);

        snprintf(path, sizeof path, "%s/%s.sp", cfg->wav_dir, stem);
        spfy_vb_slot *slots = NULL;
        size_t n_slots = 0;
        if (spfy_vb_sp_read(path, &slots, &n_slots) != SPFY_OK || !n_slots)
            ++out->n_no_sp;

        /* ⭐ q5, halfphones-in-syllable -- a DIRECT PORT of spfy_synth.c's
         * compute_q5_per_slot, because the durt tree is walked with the
         * engine's value at synthesis and has to be POPULATED with the same
         * one at build time. jill asks q5 at 15 of her 161 questions, more
         * than she asks phoneInSyl; leaving it 0 sent every unit down one
         * branch of each, so those leaves averaged the wrong population.
         *
         * A syllable is a maximal run of slots agreeing on sp[2] and sp[3];
         * silence (centre ctx 64/65) is forced to 1 rather than taking the
         * run. Everything it needs is already in the `.sp` sidecar. */
        uint8_t *q5v = NULL;
        if (n_slots) {
            q5v = (uint8_t *)calloc(n_slots, 1);
            if (!q5v) { rc = SPFY_E_NOMEM; goto stem_fail2; }
            for (size_t i = 0; i < n_slots; ) {
                size_t j = i;
                int32_t s2 = slots[i].n_sp > 2 ? slots[i].sp[2] : 0;
                int32_t s3 = slots[i].n_sp > 3 ? slots[i].sp[3] : 0;
                while (j + 1u < n_slots
                       && (slots[j + 1u].n_sp > 2 ? slots[j + 1u].sp[2] : 0) == s2
                       && (slots[j + 1u].n_sp > 3 ? slots[j + 1u].sp[3] : 0) == s3)
                    ++j;
                size_t run = j - i + 1u;
                for (size_t k = i; k <= j; ++k) {
                    int32_t c2 = slots[k].n_ctx > 2 ? slots[k].ctx[2] : 0;
                    int sil = (c2 == 64 || c2 == 65);
                    size_t v = sil ? 1u : run;
                    q5v[k] = (uint8_t)(v < 255u ? v : 255u);
                }
                i = j + 1u;
            }
        }

        /* ---- the sequence the units come from ---- */
        size_t need = seg ? n_seg : n_pairs;
        if (sq_cap < need + 4u) {
            size_t nc = need + 64u;
            seq_ent *nv = (seq_ent *)realloc(seq, nc * sizeof *nv);
            if (!nv) { rc = SPFY_E_NOMEM; goto stem_fail2; }
            seq = nv; sq_cap = nc;
        }
        size_t n_sq = 0;
        if (seg) {
            /* ⭐ ENGINE-DERIVED. The half boundary is where the engine split
             * and the phone is the one it placed -- both exact, so none of
             * the rejection below can trigger and no context can shift. */
            for (size_t k = 0; k < n_seg; ++k) {
                if (!seg[k].ok || seg[k].hi <= seg[k].lo) continue;
                seq[n_sq].f = seg[k].phone;
                seq[n_sq].lo = seg[k].lo;
                seq[n_sq].mid = seg[k].mid;
                seq[n_sq].hi = seg[k].hi;
                seq[n_sq].fe_i = (uint32_t)k;
                ++n_sq;
            }
            ++out->n_seg_rec;
        } else {
            /* Which MFA intervals the alignment actually consumed, so a
             * schwa can only be given away if nothing else claimed it. */
            uint8_t *mfa_used = (uint8_t *)calloc(n_mfa ? n_mfa : 1u, 1);
            if (!mfa_used) { rc = SPFY_E_NOMEM; goto stem_fail2; }
            for (size_t k = 0; k < n_pairs; ++k)
                if (pb[k] < n_mfa) mfa_used[pb[k]] = 1;

            for (size_t k = 0; k < n_pairs; ++k) {
                int f = spfy_vb_phone_id(&t->pidx, fe.ph[pa[k]]);
                if (f < 0) { ++out->n_skip_phone; continue; }
                int lo = (int)lround(mfa[pb[k]].start * 1000.0);
                int hi = (int)lround(mfa[pb[k]].end * 1000.0);
                /* Give the swallowed schwa back to the syllabic. See
                 * is_syllabic() above for why this is not cosmetic. */
                if (pb[k] > 0 && !mfa_used[pb[k] - 1u]
                        && is_syllabic(fe.ph[pa[k]])
                        && is_schwa_label(mfa[pb[k] - 1u].label)) {
                    int slo = (int)lround(mfa[pb[k] - 1u].start * 1000.0);
                    if (slo < lo && (hi - slo) / 2 <= SPFY_VB_MAX_HALF_MS) {
                        lo = slo;
                        mfa_used[pb[k] - 1u] = 1;
                        ++out->n_syllabic_merged;
                    }
                }
                /* ⚠ REJECT IMPLAUSIBLE SPANS. Where the alignment pairs one
                 * phone against a huge MFA segment the "half-phone" holds
                 * whole words -- and those units are PREFERENTIALLY CHOSEN,
                 * 0.14% of the inventory supplying 1.83% of selections. */
                int half = (hi - lo) / 2;
                int ceil_ms = (f == t->pau_feat) ? SPFY_VB_MAX_PAU_HALF_MS
                                                 : SPFY_VB_MAX_HALF_MS;
                if (half > ceil_ms) { ++out->n_skip_long; continue; }
                if (f != t->pau_feat && env && p95 > 0 && cfg->trim_silence) {
                    int sps = w.rate / 1000;
                    if (sps < 1) sps = 1;
                    size_t s0 = (size_t)lo * (size_t)sps;
                    size_t s1 = (size_t)hi * (size_t)sps;
                    if (s1 > w.n_samples) s1 = w.n_samples;
                    if (s1 > s0) {
                        size_t first = s1, last = s0;
                        for (size_t x = s0; x < s1; ++x)
                            if (env[x] >= 0.05 * p95) { first = x; break; }
                        for (size_t x = s1; x > s0; --x)
                            if (env[x - 1u] >= 0.05 * p95) { last = x - 1u; break; }
                        if (first < s1) {
                            int a0 = lo + (int)(first - s0) / sps;
                            int a1 = lo + (int)(last - s0) / sps + 1;
                            a0 = a0 - 8 > lo ? a0 - 8 : lo;
                            a1 = a1 + 8 < hi ? a1 + 8 : hi;
                            if (a1 - a0 >= 12 && (a0 > lo || a1 < hi)) {
                                ++out->n_skip_silent;
                                lo = a0; hi = a1;
                            }
                        }
                    }
                }
                if (hi <= lo) continue;
                seq[n_sq].f = f;
                seq[n_sq].lo = lo;
                seq[n_sq].mid = (lo + hi) / 2;
                seq[n_sq].hi = hi;
                seq[n_sq].fe_i = pa[k];
                ++n_sq;
            }
            free(mfa_used);
        }

        /* ---- chunking ---- */
        if (bounds_cap < n_sq + 2u) {
            size_t nc = n_sq + 64u;
            uint32_t *nv = (uint32_t *)realloc(bounds, nc * sizeof *nv);
            if (!nv) { rc = SPFY_E_NOMEM; goto stem_fail2; }
            bounds = nv; bounds_cap = nc;
        }
        size_t n_bounds = 1;
        bounds[0] = 0;
        {
            int hi_max = 0, prev_hi = -1;
            for (size_t k = 0; k < n_sq; ++k) {
                int lo = seq[k].lo, hi = seq[k].hi;
                int over = hi - (int)bounds[n_bounds - 1u] > SPFY_VB_CHUNK_MS;
                int hole = prev_hi >= 0 && lo - prev_hi > SPFY_VB_GAP_SPLIT_MS;
                if ((over || hole) && lo > (int)bounds[n_bounds - 1u]
                    && lo >= hi_max && (size_t)lo * (size_t)bpms < ulaw_n) {
                    bounds[n_bounds++] = (uint32_t)lo;
                    if (hole && !over) ++out->n_gap_split;
                }
                if (hi > hi_max) hi_max = hi;
                prev_hi = hi;
            }
        }
        if (chunk_cap < n_sq + 1u) {
            size_t nc = n_sq + 64u;
            uint32_t *nv = (uint32_t *)realloc(chunk_of, nc * sizeof *nv);
            if (!nv) { rc = SPFY_E_NOMEM; goto stem_fail2; }
            chunk_of = nv; chunk_cap = nc;
        }
        {
            size_t ci = 0;
            for (size_t k = 0; k < n_sq; ++k) {
                while (ci + 1u < n_bounds && (uint32_t)seq[k].lo >= bounds[ci + 1u]) ++ci;
                chunk_of[k] = (uint32_t)ci;
            }
        }
        /* ---- emit the surviving chunks: indx entry + its own audio ----
         *
         * With no drop list this writes exactly the bytes the old
         * whole-recording path wrote, in the same order: chunk c covers
         * samples [bounds[c]*bpms, bounds[c+1]*bpms) and the last runs to the
         * end, so the concatenation is the recording. That equality is the
         * regression test for this restructure. */
        if (chunk_sid_cap < n_bounds + 1u) {
            size_t nc = n_bounds + 64u;
            size_t *nv = (size_t *)realloc(chunk_sid, nc * sizeof *nv);
            if (!nv) { rc = SPFY_E_NOMEM; goto stem_fail2; }
            chunk_sid = nv; chunk_sid_cap = nc;
        }
        for (size_t k = 0; k < n_bounds; ++k) {
            char nm[1024];
            if (k == 0) snprintf(nm, sizeof nm, "%s", stem);
            else snprintf(nm, sizeof nm, "%s%c%zu", stem, SPFY_VB_CHUNK_SEP, k);

            size_t s0 = (size_t)bounds[k] * (size_t)bpms;
            size_t s1 = (k + 1u < n_bounds)
                      ? (size_t)bounds[k + 1u] * (size_t)bpms : ulaw_n;
            if (s0 > ulaw_n) s0 = ulaw_n;
            if (s1 > ulaw_n) s1 = ulaw_n;

            if (cdrop && cdrop_has(cdrop, n_cdrop, nm, cdrop_hit)) {
                chunk_sid[k] = (size_t)-1;
                ++out->n_chunk_dropped;
                out->n_bytes_dropped += s1 > s0 ? s1 - s0 : 0u;
                continue;
            }
            chunk_sid[k] = out->n_indx;
            rc = indx_push(out, &indx_cap, (uint32_t)data.n, nm);
            if (rc != SPFY_OK) goto stem_fail2;

            /* ---- --compress: emit only the kept spans of this chunk ----
             * One indx entry per chunk still, with its kept spans concatenated;
             * each span records the ms offset it landed at so the unit loop can
             * rebase local_pos onto the emitted audio. */
            if (kv) {
                uint32_t c_lo = bounds[k];
                uint32_t c_hi = (k + 1u < n_bounds)
                              ? bounds[k + 1u]
                              : (uint32_t)(ulaw_n / (size_t)bpms);
                uint32_t emitted = 0;
                for (size_t q = kv_first; q < kv_first + kv_n; ++q) {
                    uint32_t lo = kv[q].lo > c_lo ? kv[q].lo : c_lo;
                    uint32_t hi = kv[q].hi < c_hi ? kv[q].hi : c_hi;
                    if (hi <= lo) continue;
                    size_t a0 = (size_t)lo * (size_t)bpms;
                    size_t a1 = (size_t)hi * (size_t)bpms;
                    if (a0 > ulaw_n) a0 = ulaw_n;
                    if (a1 > ulaw_n) a1 = ulaw_n;
                    if (a1 <= a0) continue;
                    if (eff_n == eff_cap) {
                        size_t nc = eff_cap ? eff_cap * 2u : 256u;
                        keep_eff *nv = (keep_eff *)realloc(eff, nc * sizeof *nv);
                        if (!nv) { rc = SPFY_E_NOMEM; goto stem_fail2; }
                        eff = nv; eff_cap = nc;
                    }
                    eff[eff_n].lo  = lo;
                    eff[eff_n].hi  = (uint32_t)(lo + (a1 - a0) / (size_t)bpms);
                    eff[eff_n].off = emitted;
                    eff[eff_n].sid = chunk_sid[k];
                    ++eff_n;
                    rc = spfy_vb_buf_reserve(&data, a1 - a0);
                    if (rc != SPFY_OK) goto stem_fail2;
                    spfy_vb_ulaw_encode_block(w.pcm + a0, a1 - a0,
                                              data.p + data.n);
                    data.n += a1 - a0;
                    emitted += (uint32_t)((a1 - a0) / (size_t)bpms);
                }
                out->n_comp_bytes += (s1 > s0 ? s1 - s0 : 0u)
                                   - (size_t)emitted * (size_t)bpms;
                if (k) ++out->n_chunk_extra;
                continue;
            }
            /* ⚠ COUNT ONLY SURVIVORS. Counting every split made
             * `n_indx - 1 - n_chunk_extra` underflow once chunks could be
             * dropped, and the build log reported "4294962514 recordings". */
            if (k) ++out->n_chunk_extra;
            if (s1 > s0) {
                rc = spfy_vb_buf_reserve(&data, s1 - s0);
                if (rc != SPFY_OK) goto stem_fail2;
                spfy_vb_ulaw_encode_block(w.pcm + s0, s1 - s0, data.p + data.n);
                data.n += s1 - s0;
            }
        }

        /* ---- context source ----
         * ⚠ From the FULL FE segment list, not the aligned subset:
         * slot_ctx.c indexes fe_segments_in_order[pos + i - 2], so deriving
         * neighbours from the surviving units shifts every later context. */
        size_t n_ff = seg ? n_seg : fe.n;
        if (ff_cap < n_ff + 1u) {
            size_t nc = n_ff + 64u;
            int32_t *nv = (int32_t *)realloc(fe_feat, nc * sizeof *nv);
            if (!nv) { rc = SPFY_E_NOMEM; goto stem_fail2; }
            fe_feat = nv; ff_cap = nc;
        }
        for (size_t k = 0; k < n_ff; ++k)
            fe_feat[k] = seg ? seg[k].phone : spfy_vb_phone_id(&t->pidx, fe.ph[k]);

        /* ---- anchors ---- */
        if (fts_cap < n_ff + 1u) {
            size_t nc = n_ff + 64u;
            int32_t *nv = (int32_t *)realloc(fe_to_seq, nc * sizeof *nv);
            if (!nv) { rc = SPFY_E_NOMEM; goto stem_fail2; }
            fe_to_seq = nv; fts_cap = nc;
        }
        for (size_t k = 0; k < n_ff; ++k) fe_to_seq[k] = -1;
        for (size_t k = 0; k < n_sq; ++k)
            if (seq[k].fe_i < n_ff) fe_to_seq[seq[k].fe_i] = (int32_t)k;

        size_t base_uid = out->n_units;

        /* ⚠ AN ANCHOR SPAN IS NOT base_uid + 2*k ONCE ANYTHING CAN BE DROPPED.
         * Every seq entry used to contribute exactly two units, so the
         * arithmetic was exact; with chunk compaction it is not, and an
         * uncorrected span points at a DIFFERENT word -- the same class of
         * defect as the mislabelled converted anchors, but self-inflicted and
         * silent. seq_off counts only surviving entries. */
        if (pos_live_cap < n_sq + 1u) {
            size_t nc = n_sq + 64u;
            uint8_t *nl = (uint8_t *)realloc(pos_live, nc);
            int32_t *ns = (int32_t *)realloc(pos_span, nc * sizeof *ns);
            if (!nl || !ns) {
                free(nl); free(ns);
                rc = SPFY_E_NOMEM; goto stem_fail2;
            }
            pos_live = nl; pos_span = ns; pos_live_cap = nc;
        }
        for (size_t k = 0; k < n_sq; ++k) {
            pos_span[k] = -1;
            if (chunk_sid[chunk_of[k]] == (size_t)-1) { pos_live[k] = 0; continue; }
            if (!kv) { pos_live[k] = 1; continue; }
            const keep_eff *ke = keep_at(eff, eff_n, (uint32_t)seq[k].lo);
            /* the WHOLE extent has to be inside ONE span or there is nothing
             * for the unit to point at */
            if (!ke || (uint32_t)seq[k].hi > ke->hi) { pos_live[k] = 0; continue; }
            pos_live[k] = 1;
            pos_span[k] = (int32_t)(ke - eff);
        }
        if (seq_off_cap < n_sq + 1u) {
            size_t nc = n_sq + 64u;
            uint32_t *nv = (uint32_t *)realloc(seq_off, nc * sizeof *nv);
            if (!nv) { rc = SPFY_E_NOMEM; goto stem_fail2; }
            seq_off = nv; seq_off_cap = nc;
        }
        {
            uint32_t live = 0;
            for (size_t k = 0; k < n_sq; ++k) {
                seq_off[k] = live;
                if (pos_live[k]) live += 2u;
            }
        }
#define SEQ_LIVE(k) (pos_live[(k)])

        spfy_vb_spans wsp = {0}, ssp = {0};
        if (fe_ok && fe_txt) spfy_vb_fe_spans(fe_txt, &wsp, &ssp);

        /* ⛔ A CONVERTED RECORDING'S WORD TOKENS COME FROM THE WRONG TEXT.
         * Its PHONE labels do not -- those are the engine's own segmentation
         * of the render -- so the units stay and only the anchors go. See the
         * cfg field's comment for the audit that measured it.
         * ⚠ `anchor_text_suspect`, NOT `is_rvc`: a StyleTTS2 render's text is
         * its own input and cannot be wrong. See the flag's definition. */
        const int sup_anchors = (anchor_text_suspect && cfg->rvc_anchors_drop);

        /* ⚠ COUNT THE SUPPRESSED ONES PAST THE SAME GUARDS THE PUSH USES.
         * Counting wsp.n + ssp.n instead reported 2,832 where the anchor lists
         * were only 2,828 shorter -- two numbers that are supposed to
         * correspond and quietly did not. */
        for (size_t k = 0; k < wsp.n; ++k) {
            if (wsp.v[k].first >= n_ff || wsp.v[k].last >= n_ff) continue;
            int32_t ka = fe_to_seq[wsp.v[k].first], kb = fe_to_seq[wsp.v[k].last];
            if (ka < 0 || kb < 0 || kb < ka) continue;
            if (sup_anchors) { ++out->n_rvc_anchor_sup; continue; }
            /* An anchor plays its whole span, so it cannot survive a hole. */
            int gone = 0;
            for (int32_t q = ka; q <= kb && !gone; ++q)
                if (!SEQ_LIVE((size_t)q)) gone = 1;
            if (gone) continue;
            rc = spfy_vb_anchors_push(&out->words, wsp.v[k].text,
                                      (uint32_t)(base_uid + seq_off[ka]),
                                      (uint32_t)(base_uid + seq_off[kb] + 1u),
                                      stem);
            if (rc != SPFY_OK) { spfy_vb_spans_free(&wsp); spfy_vb_spans_free(&ssp); goto stem_fail2; }
            if (is_rvc) ++out->n_syn_anchor_kept;
        }
        for (size_t k = 0; k < ssp.n; ++k) {
            if (ssp.v[k].first >= n_ff || ssp.v[k].last >= n_ff) continue;
            int32_t ka = fe_to_seq[ssp.v[k].first], kb = fe_to_seq[ssp.v[k].last];
            if (ka < 0 || kb < 0 || kb < ka) continue;
            if (sup_anchors) { ++out->n_rvc_anchor_sup; continue; }
            int gone = 0;
            for (int32_t q = ka; q <= kb && !gone; ++q)
                if (!SEQ_LIVE((size_t)q)) gone = 1;
            if (gone) continue;
            rc = spfy_vb_anchors_push(&out->syls, ssp.v[k].text,
                                      (uint32_t)(base_uid + seq_off[ka]),
                                      (uint32_t)(base_uid + seq_off[kb] + 1u),
                                      stem);
            if (rc != SPFY_OK) { spfy_vb_spans_free(&wsp); spfy_vb_spans_free(&ssp); goto stem_fail2; }
            if (is_rvc) ++out->n_syn_anchor_kept;
        }

        /* ⭐ +0x15 MARKS A SYLLABLE START, NOT THE FIRST HALF OF A PHONE.
         * Written as `side == 0` it is 50% of units; the shipped voices are
         * 19.65% (tom) and 19.93% (jill). FUN_08e89530 gates the syllable-
         * index advance on this byte and then indexes the DURATION tree with
         * the result, so firing it every phone runs that pointer 2.4x too
         * fast. anchor_score.c implements the same gate. */
        if (syl_cap < n_sq + 1u) {
            size_t nc = n_sq + 64u;
            uint8_t *nv = (uint8_t *)realloc(syl_start, nc);
            if (!nv) { spfy_vb_spans_free(&wsp); spfy_vb_spans_free(&ssp); rc = SPFY_E_NOMEM; goto stem_fail2; }
            syl_start = nv; syl_cap = nc;
        }
        memset(syl_start, 0, n_sq);
        for (size_t k = 0; k < ssp.n; ++k) {
            if (ssp.v[k].first >= n_ff) continue;
            int32_t p = fe_to_seq[ssp.v[k].first];
            if (p >= 0 && (size_t)p < n_sq) syl_start[p] = 1;
        }
        {
            int any = 0;
            for (size_t k = 0; k < n_sq; ++k) if (syl_start[k]) { any = 1; break; }
            if (n_sq && !any) ++out->n_no_syl;
        }
        spfy_vb_spans_free(&wsp);
        spfy_vb_spans_free(&ssp);

        /* ---- emit ---- */
        for (size_t pos = 0; pos < n_sq; ++pos) {
            uint32_t u_chunk = chunk_of[pos];
            size_t   u_sid   = chunk_sid[u_chunk];
            if (u_sid == (size_t)-1) { out->n_unit_dropped += 2u; continue; }
            uint32_t u_base  = bounds[u_chunk];
            /* ⭐ --compress rebases onto the emitted audio. */
            if (kv) {
                if (!pos_live[pos] || pos_span[pos] < 0) {
                    out->n_comp_units += 2u;
                    continue;
                }
                const keep_eff *ke = &eff[pos_span[pos]];
                u_sid  = ke->sid;
                u_base = ke->lo - ke->off;
            }
            /* ⚠ A SPAN BOUNDARY IS A RUN BOUNDARY. Under --compress the
             * audio inside one chunk is no longer continuous, so two units
             * either side of a cut are not adjacent and must not carry the
             * free-join flag. */
            int u_head = (pos == 0) || (chunk_of[pos - 1u] != u_chunk)
                       || !pos_live[pos - 1u]
                       || (kv && pos_span[pos - 1u] != pos_span[pos]);
            uint32_t fe_i = seq[pos].fe_i;
            int f = seq[pos].f;

            int edges[3] = { seq[pos].lo, seq[pos].mid, seq[pos].hi };
            for (int side = 0; side < 2; ++side) {
                int b0 = edges[side], b1 = edges[side + 1];
                int d = b1 - b0 > 1 ? b1 - b0 : 1;
                const spfy_vb_slot *sl = NULL;
                size_t sidx = (size_t)fe_i * 2u + (size_t)side;
                if (sidx < n_slots) sl = &slots[sidx];
                if (sl) ++out->n_sp;

                /* ⚠ ALL FIVE context slots take the CENTRE's side. There is
                 * no side flip for the neighbours; slot_ctx.c fills
                 * ctx5[i] = phone[pos+i-2]*2 + side. */
                int32_t lft = (fe_i >= 1 && (size_t)(fe_i - 1u) < n_ff
                               && fe_feat[fe_i - 1u] >= 0)
                            ? fe_feat[fe_i - 1u] : t->pau_feat;
                int32_t rgt = ((size_t)(fe_i + 1u) < n_ff && fe_feat[fe_i + 1u] >= 0)
                            ? fe_feat[fe_i + 1u] : t->pau_feat;
                uint32_t key = (uint32_t)((lft * 2 + side) * 10000
                                        + (f * 2 + side) * 100
                                        + (rgt * 2 + side));

                uint8_t ctx[4];
                static const int OFF[4] = { -2, -1, 1, 2 };
                for (int q = 0; q < 4; ++q) {
                    long j = (long)fe_i + OFF[q];
                    int32_t fv = (j >= 0 && (size_t)j < n_ff && fe_feat[j] >= 0)
                               ? fe_feat[j] : t->pau_feat;
                    int16_t l = (fv >= 0 && fv < 256) ? t->labl.f2l[fv] : -1;
                    ctx[q] = (uint8_t)(l >= 0 ? l : 255);
                }
                if (sl && sl->n_ctx >= 4) {
                    ++out->n_ctx_seen;
                    uint32_t got = (uint32_t)(sl->ctx[1] * 10000 + sl->ctx[2] * 100
                                              + sl->ctx[3]);
                    if (got != key) ++out->n_ctx_bad;
                }

                /* ⚠ NO CLAMP. A silent truncation here is 21% aliased units;
                 * the chunking above keeps every offset in range, and if it
                 * ever does not that is a build fault, not a rounding. */
                long lp = (long)b0 - (long)u_base;
                if (lp > 0xFFFF) { ++out->n_lp_over; lp = 0xFFFF; }
                if (lp < 0) lp = 0;

                /* ⛔⛔ THE RECORDING IS NOT A WHOLE NUMBER OF MILLISECONDS, AND
                 * REAL SPEECHIFY VALIDATES EXTENTS ON LOAD.
                 * `local_pos` and `dur_like` are both ms; the engine reads
                 * offset = local_pos*bpms and nsamples = dur_like*bpms. The
                 * final chunk ends at `ulaw_n` exactly, so a unit whose end
                 * time rounds up past ulaw_n/bpms claims a sample that is not
                 * there. Measured: st2_wxr_0062~4 asked for 17184+944 = 18128
                 * of 18127, one over, and the server refused the WHOLE
                 * database with "File end is beyond the speech DB end".
                 * ⚠ spfy_synth reads past it without complaint, so this is
                 * invisible here and fatal there -- our own engine being
                 * permissive is exactly why it shipped. */
                long end_ms = (long)(ulaw_n / (size_t)bpms);
                if (kv && pos_span[pos] >= 0) {
                    /* Under --compress the audio behind this unit ends where
                     * its span does, not where the recording does. */
                    end_ms = (long)eff[pos_span[pos]].hi;
                }
                long avail  = end_ms - (long)b0;
                if (avail < 1) { ++out->n_end_drop; continue; }
                if ((long)d > avail) { d = (int)avail; ++out->n_end_clamp; }

                spfy_vb_unit u;
                memset(&u, 0, sizeof u);
                u.file_idx  = (uint16_t)u_sid;
                u.local_pos = (uint16_t)lp;
                u.dur_like  = (uint16_t)(d < 0xFFFF ? d : 0xFFFF);
                /* ⭐ THE FIFTH SP TARGET IS IN THE SIDECAR AND WAS DISCARDED.
                 * `.sp` carries five values per slot; only four were read.
                 * sp[4] is phoneInSyl -- 1 WordInitial, 2 SyllInitial,
                 * 3 SyllMedial, 4 SyllFinal, 5 WordFinal (fe_parse.c's
                 * classify_phone_in_syl, which never returns 0 or 6).
                 *
                 * It only reaches the container in v100008, where it sits at
                 * disk 0x10. A v100006 record has no column and the engine's
                 * decoder yields 6 = SyllUnknown for every unit -- while
                 * jill's VCF weights this dimension at
                 * PHONE_IN_SYL_MISMATCH_COST = .3, her second largest. tom
                 * sets it to 0, which is why v100006 costs HIM nothing. */
                u.sp_phone_in_syl = SPFY_VB_PHONE_IN_SYL_UNKNOWN;
                if (q5v && sidx < n_slots) u.q5 = q5v[sidx];
                if (sl && sl->n_sp >= 4) {
                    u.sp_syl_in_phrase = (uint8_t)sl->sp[0];
                    u.sp_syl_type      = (uint8_t)sl->sp[1];
                    u.sp_word_in_phrase= (uint8_t)sl->sp[2];
                    u.sp_syl_in_word   = (uint8_t)sl->sp[3];
                    if (sl->n_sp >= 5) {
                        u.sp_phone_in_syl = (uint8_t)sl->sp[4];
                        ++out->n_sp_phone_in_syl;
                    }
                }
                if (f0trk) {
                    /* ⭐ ALWAYS measured, never packed. The S4 join cost reads
                     * these so its dim 0 is real pitch even when the stored
                     * bytes are 0; see spfy_vb_unit.jf0_start. */
                    int vlj = phone_is_voiceless(
                        (f >= 0 && (size_t)f < t->pidx.n)
                        ? t->pidx.name[f] : NULL);
                    u.jf0_start = f0_span(t, f0trk, f0_n, b0, b1, b0, vlj,
                                          f0_med, &out->n_f0_rescued);
                    u.jf0_end   = f0_span(t, f0trk, f0_n, b0, b1, b1, vlj,
                                          f0_med, &out->n_f0_rescued);
                }
                if ((cfg->f0_calibrated || cfg->f0_render_only) && f0trk) {
                    int m0 = b0, m1 = b1, mm = (b0 + b1) / 2;
                    /* See f0_span(): the zero is the DP's voicing bit, so it
                     * must describe the UNIT, not one frame of it. */
                    int vl = phone_is_voiceless(
                        (f >= 0 && (size_t)f < t->pidx.n)
                        ? t->pidx.name[f] : NULL);
                    u.f0_start = f0_span(t, f0trk, f0_n, b0, b1, m0, vl,
                                         f0_med, &out->n_f0_rescued);
                    u.f0_end   = f0_span(t, f0trk, f0_n, b0, b1, m1, vl,
                                         f0_med, &out->n_f0_rescued);
                    u.f0_mid   = f0_span(t, f0trk, f0_n, b0, b1, mm, vl,
                                         f0_med, &out->n_f0_rescued);
                    /* ⛔ f0_mid is the DP's VOICING MAP, not a pitch value.
                     * Leaving it 0 is what keeps render-only render-only:
                     * c80 stays pinned at the 100 sentinel, the join gate
                     * cannot fire, and the f0_end byte above stays invisible
                     * to selection. See spfy_vb_corpus_cfg.f0_render_only. */
                    if (cfg->f0_render_only) u.f0_mid = 0;
                }
                {
                    double v = t->f0ctx_a * log((double)d + 1.0) + t->f0ctx_b;
                    long r = lround(v);
                    if (r < 0) r = 0;
                    if (r > 255) r = 255;
                    u.f0_context = (uint8_t)r;
                }
                /* LABL space, not feat -- the engine maps it back. */
                u.phone_center = (uint8_t)((f >= 0 && f < 256 && t->labl.f2l[f] >= 0)
                                           ? t->labl.f2l[f] : f);
                u.is_first_half = (uint8_t)((side == 0 && syl_start[pos]) ? 1 : 0);
                if (u.is_first_half) ++out->n_first_half;
                u.voice_const = 3;
                memcpy(u.phone_ctx, ctx, 4);
                /* ⚠ flag_b gates the ZERO-COST join. The first unit of a
                 * recording follows the LAST unit of the previous one, so
                 * leaving it set makes every recording boundary a free join
                 * between unrelated audio. A CHUNK head counts too: the
                 * emitter breaks the run there anyway. */
                u.flag_b = (uint8_t)((side == 0 && u_head) ? 0 : 1);
                u.key = key;
                u.phone = (uint8_t)f;
                u.is_rvc = (uint8_t)is_rvc;
                if (is_rvc) ++out->n_rvc_units;

                /* S3's raw material, captured while the sidecars are open.
                 * ⚠ pitch here is REAL Hz, not the record's quantised byte:
                 * tom's stored f0_mid is 118 +/- 6.3 while his mean chunk
                 * reads 123.8 +/- 22.1, and computing the column from the
                 * bytes gave sd 4.77 where 22.1 was wanted. */
                if (f0trk && w.pcm) {
                    long a0 = b0, a1 = b0 + d;
                    if (a0 < 0) a0 = 0;
                    long t0 = a0, t1 = a1;
                    if (t1 > (long)f0_n) t1 = (long)f0_n;
                    long nv = 0, nvv = 0;
                    double sum = 0.0;
                    for (long x = t0; x < t1; ++x) {
                        ++nv;
                        if (f0trk[x] > 0) { ++nvv; sum += f0trk[x]; }
                    }
                    u.a_voice = nv ? (double)nvv / (double)nv : 0.0;
                    u.a_pitch = nvv ? sum / (double)nvv : 0.0;
                    long s0 = a0 * 8, s1 = a1 * 8;
                    if (s1 > (long)w.n_samples) s1 = (long)w.n_samples;
                    if (s1 > s0) {
                        double acc = 0.0;
                        for (long x = s0; x < s1; ++x) {
                            double v = (double)w.pcm[x];
                            acc += v * v;
                        }
                        u.a_power = log(sqrt(acc / (double)(s1 - s0)) + 1.0);
                    }
                    u.have_audio = 1;
                }
                rc = units_push(out, &units_cap, &u);
                if (rc != SPFY_OK) goto stem_fail2;
            }
        }
        ++out->n_used;
        if (is_rvc) ++out->n_rvc_recs;

        free(q5v);
        free(slots);
        free(f0trk);
        free(mfa_ph);
        free(mfa);
        free(seg);
        spfy_vb_phones_free(&fe);
        free(fe_txt);
        free(env);
        spfy_vb_wav_free(&w);
        continue;

stem_fail2:
        free(q5v);
        free(slots);
        free(f0trk);
stem_fail:
        free(mfa_ph);
        free(mfa);
        free(seg);
        spfy_vb_phones_free(&fe);
        free(fe_txt);
        free(env);
        spfy_vb_wav_free(&w);
        goto fail;
    }

    /* The indx ends with a sentinel whose offset is the total data size, so a
     * recording's length is always next_offset - offset. */
    rc = indx_push(out, &indx_cap, (uint32_t)data.n, "");
    if (rc != SPFY_OK) goto fail;

    /* ⭐ UIDS ARE ASSIGNED ONCE, HERE, TO WHATEVER SURVIVED. That is the whole
     * reason compaction is safe at build time and not as a post-process: there
     * is no renumbering step for the anchor scorer's uid arithmetic to trip
     * over, and flag_b was set from real audio contiguity inside each
     * surviving chunk. */
    for (size_t i = 0; i < out->n_units; ++i) out->units[i].uid = (uint32_t)i;

    if (cdrop) {
        for (size_t i = 0; i < n_cdrop; ++i) if (cdrop_hit[i]) ++out->n_cdrop_hit;
        out->n_cdrop_absent = n_cdrop - out->n_cdrop_hit;
        printf("chunk drop: %zu of %zu names matched; %zu chunks and %zu units "
               "removed, %zu bytes of audio\n",
               out->n_cdrop_hit, n_cdrop, out->n_chunk_dropped,
               out->n_unit_dropped, out->n_bytes_dropped);
        for (size_t i = 0; i < n_cdrop; ++i)
            if (!cdrop_hit[i])
                printf("  ⚠ no such chunk, so not dropped: %s\n", cdrop[i]);
    }

    out->data = data.p;
    out->n_data = data.n;
    spfy_vb_free_stems(stems, n_stems);
    free(seq); free(pa); free(pb); free(fe_ids); free(seg_ids);
    free(bounds); free(chunk_of); free(syl_start); free(fe_feat); free(fe_to_seq);
    free(chunk_sid); free(seq_off); free(pos_live); free(pos_span);
    if (kv_all) {
        printf("compress: %zu unit(s) withheld for falling outside a kept "
               "span, %.1f MB of audio not emitted (%zu recording(s) had no "
               "span at all)\n",
               out->n_comp_units, (double)out->n_comp_bytes / 1e6,
               out->n_comp_skipped);
        for (size_t i = 0; i < kv_all_n; ++i) free(kv_all[i].stem);
        free(kv_all);
    }
    free(eff);
    for (size_t i = 0; i < n_cdrop; ++i) free(cdrop[i]);
    free(cdrop); free(cdrop_hit);
    return SPFY_OK;

fail:
    spfy_vb_buf_free(&data);
    spfy_vb_free_stems(stems, n_stems);
    free(seq); free(pa); free(pb); free(fe_ids); free(seg_ids);
    free(bounds); free(chunk_of); free(syl_start); free(fe_feat); free(fe_to_seq);
    free(chunk_sid); free(seq_off); free(pos_live); free(pos_span);
    for (size_t i = 0; i < n_cdrop; ++i) free(cdrop[i]);
    free(cdrop); free(cdrop_hit);
    spfy_vb_corpus_free(out);
    return rc;
}
#undef SEQ_LIVE

void spfy_vb_corpus_free(spfy_vb_corpus *c)
{
    free(c->units);
    for (size_t i = 0; i < c->n_indx; ++i) free(c->indx[i].name);
    free(c->indx);
    free(c->data);
    spfy_vb_anchors_free(&c->words);
    spfy_vb_anchors_free(&c->syls);
    memset(c, 0, sizeof *c);
}
