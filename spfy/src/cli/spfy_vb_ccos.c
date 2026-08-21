/* spfy_vb_ccos -- generate (or check) the `ccos` context-cost tables.
 *
 * ccos[hp_class][slot][target_label][cand_label] is what the S cost charges a
 * candidate for carrying the wrong phone at one of four context positions
 * (spfy_cost_s, usel/cost_s.c). Festival computes the same quantity as
 * ac_unit_distance between units and hands the resulting matrix to `wagon`;
 * Speechify kept the quantity and stored a table instead of growing a tree.
 * So a cell here is the mean ac_unit_distance between the units of this class
 * whose slot-s neighbour is `target_label` and those whose slot-s neighbour is
 * `cand_label`.
 *
 * ⚠ --compare FIRST, ALWAYS. The gate on generating this chunk for a voice of
 * ours is reproducing a VENDOR's from that vendor's own audio. 1.5 MB of cost
 * table that nothing can check is exactly the sort of thing that scores well
 * and sounds worse.
 *
 *   spfy_vb_ccos --vin V --vdb D --compare
 *   spfy_vb_ccos --vin V --vdb D --out ccos.bin
 */

#include "../vb/vb_track.h"
#include "../vb/vb_chunk.h"
#include "../vb/vb_io.h"
#include "../voice/ccos.h"
#include "../voice/unit_table.h"
#include "../voice/voice.h"
#include "../usel/prsl.h"
#include "../../include/spfy/spfy.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LAB 64u

typedef struct {
    uint32_t *uid;
    uint32_t  n, cap;
} ulist;

static int ulist_push(ulist *l, uint32_t u)
{
    if (l->n == l->cap) {
        uint32_t c = l->cap ? l->cap * 2u : 8u;
        uint32_t *p = (uint32_t *)realloc(l->uid, (size_t)c * sizeof *p);
        if (!p) return SPFY_E_NOMEM;
        l->uid = p; l->cap = c;
    }
    l->uid[l->n++] = u;
    return SPFY_OK;
}

/* Deterministic sub-sample: a hash of the uid, so the same corpus always
 * yields the same table and two runs are comparable. */
static uint32_t mix(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/* ⭐ THE EDGE NEAREST THE NEIGHBOUR, NOT THE WHOLE UNIT.
 *
 * A ccos cell asks what it costs to carry the wrong phone at ONE context
 * position, so whatever the table measures has to be the part of the unit that
 * position actually changes -- coarticulation, which lives at the boundary.
 * Slots 0 and 1 are the LEFT neighbour, so they read the unit's own start;
 * slots 2 and 3 are the RIGHT neighbour and read its end. The whole-unit
 * kernel averages that signal against the unit's steady state, which would
 * explain an estimator that reproduces ITSELF at r=0.82 and the vendor at
 * 0.146.
 *
 * ⚠ dur_ms is scaled with the slice so ac_unit_distance's warp still lines the
 * two up; with equal frame counts its duration term becomes a constant and
 * drops out of the correlation, which is what we want here -- ccos is about
 * context, not length. */
static spfy_vb_track edge_view(const spfy_vb_track *t, uint32_t slot,
                               int edge, uint32_t n_cep)
{
    spfy_vb_track v = *t;
    if (edge <= 0 || t->n_frames <= (uint32_t)edge) return v;
    uint32_t k = (uint32_t)edge;
    if (slot <= 1u) {
        v.n_frames = k;
    } else {
        v.f = t->f + (size_t)(t->n_frames - k) * n_cep;
        v.n_frames = k;
    }
    v.dur_ms = t->dur_ms * (float)k / (float)t->n_frames;
    return v;
}

static double pearson(const double *a, const double *b, size_t n)
{
    if (n < 10) return 0.0;
    double ma = 0, mb = 0;
    for (size_t i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
    ma /= (double)n; mb /= (double)n;
    double num = 0, da = 0, db = 0;
    for (size_t i = 0; i < n; ++i) {
        double x = a[i] - ma, y = b[i] - mb;
        num += x * y; da += x * x; db += y * y;
    }
    return (da > 0 && db > 0) ? num / sqrt(da * db) : 0.0;
}

int main(int argc, char **argv)
{
    const char *vin_path = NULL, *vdb_path = NULL, *out_path = NULL;
    const char *out_vin = NULL;
    /* Our raw distances are ~3000x the vendors', because ac_unit_distance is
     * a sum of squared cepstral differences in whatever units the front end
     * produced and nothing normalises it. The COST SCALE is not a speaker
     * property -- it is what the VCF's w_ccos weight was balanced against --
     * so the table is rescaled to a stated median rather than shipped raw.
     * 2.0 is both vendors' own raw median. */
    double target_median = 2.0;
    int do_compare = 0, per_group = 8;
    float dur_pen_w = 1.0f, shift_ms = 5.0f;
    int n_cep = 12;
    /* 0 = mean pairwise (Festival's own aggregation), 1 = energy distance.
     *
     * ⚠ The mean pairwise distance between two noisy groups is
     * ||mA-mB||^2 + tr(SA) + tr(SB): with 8 units of one phone per group the
     * WITHIN term dominates, and a table made of within-group variance would
     * correlate with a plain per-phone matrix -- which is exactly the shape
     * the first attempt had (r 0.117 against a rival of 0.108). `energy`
     * subtracts the two within terms and leaves the between-group part. */
    int agg = 0;
    /* Frames of the unit nearest that slot's neighbour; 0 = the whole unit. */
    int edge = 0;

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        int has = (i + 1 < argc);
        if (!strcmp(a, "--vin") && has) { vin_path = argv[++i]; continue; }
        if (!strcmp(a, "--vdb") && has) { vdb_path = argv[++i]; continue; }
        if (!strcmp(a, "--out") && has) { out_path = argv[++i]; continue; }
        if (!strcmp(a, "--compare")) { do_compare = 1; continue; }
        if (!strcmp(a, "--per-group") && has) { per_group = atoi(argv[++i]); continue; }
        if (!strcmp(a, "--dur-pen") && has) { dur_pen_w = (float)atof(argv[++i]); continue; }
        if (!strcmp(a, "--shift-ms") && has) { shift_ms = (float)atof(argv[++i]); continue; }
        if (!strcmp(a, "--n-cep") && has) { n_cep = atoi(argv[++i]); continue; }
        if (!strcmp(a, "--edge") && has) { edge = atoi(argv[++i]); continue; }
        if (!strcmp(a, "--out-vin") && has) { out_vin = argv[++i]; continue; }
        if (!strcmp(a, "--median") && has) {
            target_median = atof(argv[++i]); continue;
        }
        if (!strcmp(a, "--agg") && has) {
            const char *m = argv[++i];
            if (!strcmp(m, "pair"))        agg = 0;
            else if (!strcmp(m, "energy")) agg = 1;
            else { fprintf(stderr, "--agg wants pair|energy\n"); return 2; }
            continue;
        }
        fprintf(stderr, "unknown option %s\n", a);
        return 2;
    }
    if (!vin_path || !vdb_path) {
        fprintf(stderr,
            "usage: %s --vin V --vdb D [--out F] [--compare]\n"
            "  --compare        rebuild the table and correlate against the\n"
            "                   one already in the VIN, with a PERMUTED\n"
            "                   control beside it. Run this on a vendor\n"
            "                   before trusting a generated table.\n"
            "  --per-group N    units sampled per context group (default 8)\n"
            "  --dur-pen W      Festival dur_pen_weight (default 1.0)\n"
            "  --shift-ms X     frame shift (default 5)\n"
            "  --n-cep N        cepstral coefficients (default 12)\n"
            "  --agg MODE       pair (default, Festival's own) or energy,\n"
            "                   which subtracts the two within-group means so\n"
            "                   the cell is between-group distance alone\n"
            "  --out-vin F      write the whole VIN with OUR ccos spliced in,\n"
            "                   rescaled to --median (default 2.0, both\n"
            "                   vendors' own raw median)\n"
            "  --median X       the raw median to rescale to\n",
            argv[0]);
        return 2;
    }

    int rc;
    spfy_vin_t vin = {0};
    spfy_vdb_t vdb = {0};
    spfy_ccos_t cc = {0};
    spfy_unit_table_t ut = {0};

    if ((rc = spfy_vin_load(vin_path, &vin)) != SPFY_OK) {
        fprintf(stderr, "vin_load %d\n", rc); return 1;
    }
    if ((rc = spfy_vdb_load(vdb_path, &vdb)) != SPFY_OK) {
        fprintf(stderr, "vdb_load %d\n", rc); return 1;
    }
    if ((rc = spfy_ccos_load(&vin, &cc)) != SPFY_OK) {
        fprintf(stderr, "ccos_load %d\n", rc); return 1;
    }
    if ((rc = spfy_unit_table_load(&vin, &ut)) != SPFY_OK) {
        fprintf(stderr, "unit_table_load %d\n", rc); return 1;
    }
    const uint32_t NL = cc.n_labels;
    const uint32_t NC = cc.n_hp_classes;
    const uint32_t NU = ut.n_units;
    printf("%s: %u labels, %u hp_classes, %u units\n", vin_path, NL, NC, NU);
    if (NL > MAX_LAB) { fprintf(stderr, "too many labels\n"); return 1; }

    /* Per-unit half, from the prsl key's parity. ⚠ NOT uid & 1: on tom, 3,390
     * of 6,849 recordings start at an odd uid, so uid parity is not the side. */
    uint8_t *half = (uint8_t *)calloc(NU ? NU : 1u, 1);
    uint8_t *hasf = (uint8_t *)calloc(NU ? NU : 1u, 1);
    if (!half || !hasf) return 1;
    {
        spfy_prsl_t prsl;
        if (spfy_prsl_load(&vin, &prsl) == SPFY_OK) {
            for (uint32_t g = 0; g < prsl.n_groups; ++g) {
                uint32_t k = prsl.groups[g].context_key;
                uint32_t c = (k / 100u) % 100u;
                if (k / 10000u >= 92u || k % 100u >= 92u) continue;
                for (uint32_t q = 0; q < prsl.groups[g].n_candidates; ++q) {
                    uint32_t u = spfy_prsl_cand(prsl.groups[g].candidates, q);
                    if (u < NU) { half[u] = (uint8_t)(c & 1u); hasf[u] = 1; }
                }
            }
            spfy_prsl_free(&prsl);
        }
    }
    size_t n_half = 0;
    for (uint32_t u = 0; u < NU; ++u) n_half += hasf[u];
    printf("  half known for %zu/%u units from prsl\n", n_half, NU);

    /* Context phones, walked in PHONE steps within each recording. */
    uint8_t *ctx = (uint8_t *)malloc((size_t)NU * 4u);
    if (!ctx) return 1;
    memset(ctx, 0xFF, (size_t)NU * 4u);
    {
        uint32_t s = 0;
        while (s < NU) {
            spfy_unit_record_t r0;
            if (spfy_unit_record_get(&ut, s, &r0) != SPFY_OK) { ++s; break; }
            uint32_t e = s;
            while (e + 1u < NU) {
                spfy_unit_record_t rn;
                if (spfy_unit_record_get(&ut, e + 1u, &rn) != SPFY_OK) break;
                if (rn.file_idx != r0.file_idx) break;
                ++e;
            }
            /* Within a recording the units alternate L,R; the phone at index p
             * is the unit at 2p relative to the run's first LEFT half. */
            uint32_t base = s;
            if (hasf[s] && half[s]) base = s + 1u;    /* run starts on an R */
            uint32_t np = (e >= base) ? (e - base + 1u) / 2u : 0u;
            for (uint32_t p = 0; p < np; ++p) {
                for (uint32_t hh = 0; hh < 2u; ++hh) {
                    uint32_t u = base + p * 2u + hh;
                    if (u > e) break;
                    int32_t idx[4] = { (int32_t)p - 2, (int32_t)p - 1,
                                       (int32_t)p + 1, (int32_t)p + 2 };
                    for (int t = 0; t < 4; ++t) {
                        if (idx[t] < 0 || (uint32_t)idx[t] >= np) continue;
                        spfy_unit_record_t rc2;
                        uint32_t v = base + (uint32_t)idx[t] * 2u;
                        if (v > e) continue;
                        if (spfy_unit_record_get(&ut, v, &rc2) != SPFY_OK) continue;
                        if (rc2.phone_center < NL)
                            ctx[(size_t)u * 4u + (uint32_t)t] = rc2.phone_center;
                    }
                }
            }
            s = e + 1u;
        }
    }

    /* Group units by (hp_class, slot, context label), keeping `per_group`. */
    const size_t NG = (size_t)NC * SPFY_CCOS_N_SLOTS * NL;
    ulist *g = (ulist *)calloc(NG, sizeof *g);
    if (!g) return 1;
    size_t n_placed = 0;
    for (uint32_t u = 0; u < NU; ++u) {
        if (!hasf[u]) continue;
        spfy_unit_record_t r;
        if (spfy_unit_record_get(&ut, u, &r) != SPFY_OK) continue;
        if (r.phone_center >= NL) continue;
        /* hp_class = label + (half ? n_labels : 0) -- voice_runtime.c:63 */
        uint32_t C = (uint32_t)r.phone_center + (half[u] ? NL : 0u);
        if (C >= NC) continue;
        for (uint32_t s = 0; s < SPFY_CCOS_N_SLOTS; ++s) {
            uint8_t cl = ctx[(size_t)u * 4u + s];
            if (cl >= NL) continue;
            size_t gi = ((size_t)C * SPFY_CCOS_N_SLOTS + s) * NL + cl;
            if (g[gi].n >= (uint32_t)per_group) {
                /* Reservoir-ish: keep the lowest-hash `per_group` uids so the
                 * sample does not favour the start of the corpus. */
                uint32_t worst = 0, wv = 0;
                for (uint32_t q = 0; q < g[gi].n; ++q) {
                    uint32_t h = mix(g[gi].uid[q]);
                    if (h > wv) { wv = h; worst = q; }
                }
                if (mix(u) < wv) g[gi].uid[worst] = u;
                continue;
            }
            if (ulist_push(&g[gi], u) != SPFY_OK) return 1;
            ++n_placed;
        }
    }
    size_t nonempty = 0;
    for (size_t i = 0; i < NG; ++i) if (g[i].n) ++nonempty;
    printf("  %zu of %zu context groups populated, %zu unit slots kept\n",
           nonempty, NG, n_placed);

    /* Build tracks only for the units actually sampled. */
    uint8_t *want = (uint8_t *)calloc(NU ? NU : 1u, 1);
    if (!want) return 1;
    for (size_t i = 0; i < NG; ++i)
        for (uint32_t q = 0; q < g[i].n; ++q) want[g[i].uid[q]] = 1;
    uint32_t n_want = 0;
    for (uint32_t u = 0; u < NU; ++u) n_want += want[u];
    uint32_t *uids = (uint32_t *)malloc((size_t)(n_want ? n_want : 1u) * 4u);
    int32_t  *slot = (int32_t *)malloc((size_t)NU * sizeof *slot);
    if (!uids || !slot) return 1;
    uint32_t w = 0;
    for (uint32_t u = 0; u < NU; ++u) {
        slot[u] = -1;
        if (want[u]) { slot[u] = (int32_t)w; uids[w++] = u; }
    }
    printf("  building tracks for %u units (shift %.1f ms, %d cep)...\n",
           n_want, shift_ms, n_cep);

    spfy_vb_cfg_t fcfg;
    spfy_vb_cfg_default(&fcfg);
    fcfg.n_cep = (uint32_t)n_cep;
    fcfg.keep_c0 = 1;
    spfy_vb_tracks tr;
    rc = spfy_vb_tracks_build(&vin, &vdb, vdb.sample_rate ? vdb.sample_rate : 8000u,
                              &fcfg, shift_ms, uids, n_want, &tr);
    if (rc != SPFY_OK) { fprintf(stderr, "tracks_build %d\n", rc); return 1; }
    printf("  %zu tracks, %zu without audio\n", tr.n - tr.n_missing, tr.n_missing);

    float *wts = (float *)malloc((size_t)n_cep * sizeof *wts);
    if (!wts) return 1;
    for (int k = 0; k < n_cep; ++k) wts[k] = 1.0f;

    /* One table per (hp_class, slot): the mean distance between the two
     * context groups, over the stored triangle i = 1..NL-1, j = 0..i-1. */
    const size_t TRI = (size_t)NL * (NL - 1u) / 2u;
    float *tab  = (float *)calloc(TRI ? TRI : 1u, sizeof *tab);
    float *tabA = (float *)calloc(TRI ? TRI : 1u, sizeof *tabA);
    float *tabB = (float *)calloc(TRI ? TRI : 1u, sizeof *tabB);
    /* ⚠ "populated" cannot be inferred from the value once --agg energy is
     * available: a corrected cell is legitimately 0 or negative, and the old
     * `!= 0.0f` test silently dropped exactly the cells the correction was
     * meant to move. */
    uint8_t *pop  = (uint8_t *)calloc(TRI ? TRI : 1u, 1);
    uint8_t *popA = (uint8_t *)calloc(TRI ? TRI : 1u, 1);
    uint8_t *popB = (uint8_t *)calloc(TRI ? TRI : 1u, 1);
    double *mine = (double *)malloc(TRI * sizeof *mine);
    double *theirs = (double *)malloc(TRI * sizeof *theirs);
    double *permd = (double *)malloc(TRI * sizeof *permd);
    double *halfA = (double *)malloc(TRI * sizeof *halfA);
    double *halfB = (double *)malloc(TRI * sizeof *halfB);
    if (!tab || !tabA || !tabB || !pop || !popA || !popB || !mine || !theirs
        || !permd || !halfA || !halfB) return 1;

    FILE *fo = NULL;
    if (out_path && !(fo = fopen(out_path, "wb"))) {
        fprintf(stderr, "cannot write %s\n", out_path); return 1;
    }
    float *alltab = NULL;
    if (out_vin) {
        alltab = (float *)calloc((size_t)NC * SPFY_CCOS_N_SLOTS * TRI,
                                 sizeof *alltab);
        if (!alltab) { fprintf(stderr, "out of memory for the table\n"); return 1; }
    }

    /* RIVAL MODEL. The vowel/consonant control says ccos carries phone
     * identity: VV < CC < VC < silence, on both vendors. That is what a plain
     * phone-to-phone acoustic distance looks like, with no conditioning on
     * class or slot at all -- the per-table variation would then be a
     * modulation of one shared matrix rather than 368 independent estimates.
     * Built from the SAME tracks, so the two models are compared through
     * identical processing. */
    double *gsum = (double *)calloc(TRI ? TRI : 1u, sizeof *gsum);
    uint32_t *gcnt = (uint32_t *)calloc(TRI ? TRI : 1u, sizeof *gcnt);
    double *glob = (double *)malloc(TRI * sizeof *glob);
    if (!gsum || !gcnt || !glob) return 1;
    {
        /* Units per label, capped, drawn from the tracks already built. */
        ulist *byl = (ulist *)calloc(NL, sizeof *byl);
        if (!byl) return 1;
        for (uint32_t k = 0; k < n_want; ++k) {
            spfy_unit_record_t r;
            if (spfy_unit_record_get(&ut, uids[k], &r) != SPFY_OK) continue;
            if (r.phone_center >= NL) continue;
            if (byl[r.phone_center].n >= 24u) continue;
            if (ulist_push(&byl[r.phone_center], uids[k]) != SPFY_OK) return 1;
        }
        size_t t = 0;
        for (uint32_t i = 1; i < NL; ++i) {
            for (uint32_t j = 0; j < i; ++j, ++t) {
                for (uint32_t x = 0; x < byl[i].n; ++x) {
                    int32_t sa = slot[byl[i].uid[x]];
                    if (sa < 0 || !tr.t[sa].n_frames) continue;
                    for (uint32_t y = 0; y < byl[j].n; ++y) {
                        int32_t sb = slot[byl[j].uid[y]];
                        if (sb < 0 || !tr.t[sb].n_frames) continue;
                        gsum[t] += spfy_vb_ac_unit_distance(&tr.t[sa], &tr.t[sb],
                                                            wts, (uint32_t)n_cep,
                                                            dur_pen_w);
                        ++gcnt[t];
                    }
                }
            }
        }
        for (size_t q = 0; q < TRI; ++q)
            glob[q] = gcnt[q] ? gsum[q] / (double)gcnt[q] : 0.0;
        for (uint32_t i = 0; i < NL; ++i) free(byl[i].uid);
        free(byl);
    }

    /* Within-group mean distance for each label group of the current
     * (hp_class, slot), split by the same parity the between-group pass uses
     * so the split-half ceiling stays a ceiling on the SAME statistic. */
    double *win0 = (double *)malloc((size_t)NL * sizeof *win0);
    double *win1 = (double *)malloc((size_t)NL * sizeof *win1);
    if (!win0 || !win1) return 1;

    double sum_r = 0.0, sum_p = 0.0, sum_h = 0.0, sum_g = 0.0;
    size_t n_tab = 0, n_half_t = 0, n_g = 0;
    for (uint32_t C = 0; C < NC; ++C) {
        for (uint32_t s = 0; s < SPFY_CCOS_N_SLOTS; ++s) {
            if (agg) {
                for (uint32_t i = 0; i < NL; ++i) {
                    ulist *A = &g[((size_t)C * SPFY_CCOS_N_SLOTS + s) * NL + i];
                    double w0 = 0.0, w1 = 0.0;
                    uint32_t c0 = 0, c1 = 0;
                    for (uint32_t x = 0; x < A->n; ++x) {
                        int32_t sa = slot[A->uid[x]];
                        if (sa < 0 || !tr.t[sa].n_frames) continue;
                        spfy_vb_track va = edge_view(&tr.t[sa], s, edge,
                                                     (uint32_t)n_cep);
                        for (uint32_t y = x + 1u; y < A->n; ++y) {
                            int32_t sb = slot[A->uid[y]];
                            if (sb < 0 || !tr.t[sb].n_frames) continue;
                            spfy_vb_track vb = edge_view(&tr.t[sb], s, edge,
                                                         (uint32_t)n_cep);
                            float d = spfy_vb_ac_unit_distance(&va, &vb,
                                                               wts, (uint32_t)n_cep,
                                                               dur_pen_w);
                            if (((x ^ y) & 1u) == 0u) { w0 += d; ++c0; }
                            else                      { w1 += d; ++c1; }
                        }
                    }
                    /* A one-unit group has no within estimate; 0 leaves the
                     * cell as a plain pairwise distance rather than
                     * over-subtracting from the other side. */
                    win0[i] = c0 ? w0 / (double)c0 : 0.0;
                    win1[i] = c1 ? w1 / (double)c1 : 0.0;
                }
            }
            size_t nm = 0;
            size_t t = 0;
            for (uint32_t i = 1; i < NL; ++i) {
                for (uint32_t j = 0; j < i; ++j, ++t) {
                    ulist *A = &g[((size_t)C * SPFY_CCOS_N_SLOTS + s) * NL + i];
                    ulist *B = &g[((size_t)C * SPFY_CCOS_N_SLOTS + s) * NL + j];
                    double acc = 0.0, acc2 = 0.0;
                    uint32_t cnt = 0, cnt2 = 0;
                    for (uint32_t x = 0; x < A->n; ++x) {
                        int32_t sa = slot[A->uid[x]];
                        if (sa < 0 || !tr.t[sa].n_frames) continue;
                        spfy_vb_track va = edge_view(&tr.t[sa], s, edge,
                                                     (uint32_t)n_cep);
                        for (uint32_t y = 0; y < B->n; ++y) {
                            int32_t sb = slot[B->uid[y]];
                            if (sb < 0 || !tr.t[sb].n_frames) continue;
                            spfy_vb_track vb = edge_view(&tr.t[sb], s, edge,
                                                         (uint32_t)n_cep);
                            float d = spfy_vb_ac_unit_distance(&va, &vb,
                                                               wts, (uint32_t)n_cep,
                                                               dur_pen_w);
                            /* SPLIT HALF: two disjoint halves of the same
                             * sample. Correlating them measures how much of
                             * this estimator is signal at all, i.e. the
                             * CEILING on any agreement with the vendor. A
                             * comparison read without that ceiling is a
                             * number with no scale. */
                            if (((x ^ y) & 1u) == 0u) { acc += d; ++cnt; }
                            else                      { acc2 += d; ++cnt2; }
                        }
                    }
                    double corr0 = 0.0, corr1 = 0.0;
                    if (agg) {
                        corr0 = 0.5 * (win0[i] + win0[j]);
                        corr1 = 0.5 * (win1[i] + win1[j]);
                    }
                    tab[t]  = (cnt || cnt2)
                            ? (float)((acc + acc2) / (double)(cnt + cnt2)
                                      - 0.5 * (corr0 + corr1)) : 0.0f;
                    tabA[t] = cnt  ? (float)(acc  / (double)cnt  - corr0) : 0.0f;
                    tabB[t] = cnt2 ? (float)(acc2 / (double)cnt2 - corr1) : 0.0f;
                    pop[t]  = (uint8_t)((cnt + cnt2) ? 1 : 0);
                    popA[t] = (uint8_t)(cnt  ? 1 : 0);
                    popB[t] = (uint8_t)(cnt2 ? 1 : 0);
                    if (cnt + cnt2) ++nm;
                }
            }
            if (fo) {
                uint32_t hdr[2] = { C, s };
                fwrite(hdr, 4, 2, fo);
                fwrite(tab, 4, TRI, fo);
            }
            if (alltab)
                memcpy(alltab + ((size_t)C * SPFY_CCOS_N_SLOTS + s) * TRI,
                       tab, TRI * sizeof *tab);
            if (do_compare && nm > TRI / 4u) {
                /* The VIN's table, undoing the runtime (raw+0.1)*scale so both
                 * sides are in raw units. */
                const float *ref = spfy_ccos_table(&cc, C, s);
                const float *prm = spfy_ccos_table(&cc, (C + 7u) % NC,
                                                   (s + 1u) % SPFY_CCOS_N_SLOTS);
                if (!ref || !prm) continue;
                size_t m = 0, tt = 0, mh = 0;
                for (uint32_t i = 1; i < NL; ++i)
                    for (uint32_t j = 0; j < i; ++j, ++tt) {
                        if (popA[tt] && popB[tt]) {
                            halfA[mh] = tabA[tt];
                            halfB[mh] = tabB[tt];
                            ++mh;
                        }
                        if (!pop[tt]) continue;
                        mine[m]   = tab[tt];
                        theirs[m] = ref[(size_t)i * NL + j];
                        permd[m]  = prm[(size_t)i * NL + j];
                        ++m;
                    }
                if (m >= 100) {
                    sum_r += pearson(mine, theirs, m);
                    sum_p += pearson(mine, permd, m);
                    if (mh >= 100) { sum_h += pearson(halfA, halfB, mh); ++n_half_t; }
                    /* The rival: ONE global phone matrix against this table. */
                    size_t mg = 0, u2 = 0;
                    for (uint32_t i = 1; i < NL; ++i)
                        for (uint32_t j = 0; j < i; ++j, ++u2) {
                            if (glob[u2] == 0.0) continue;
                            halfA[mg] = glob[u2];
                            halfB[mg] = ref[(size_t)i * NL + j];
                            ++mg;
                        }
                    if (mg >= 100) { sum_g += pearson(halfA, halfB, mg); ++n_g; }
                    ++n_tab;
                }
            }
        }
    }
    if (fo) { fclose(fo); printf("  wrote %s\n", out_path); }

    if (alltab) {
        /* ---- rescale ----
         * The median over POPULATED cells only. Including the empty ones --
         * 11.6% of jill's are exactly 0 and far more of a small corpus's --
         * would drag the median toward zero and inflate every real cell to
         * compensate. */
        size_t total = (size_t)NC * SPFY_CCOS_N_SLOTS * TRI;
        float *pos = (float *)malloc(total * sizeof *pos);
        if (!pos) { fprintf(stderr, "out of memory\n"); return 1; }
        size_t np = 0;
        for (size_t i = 0; i < total; ++i)
            if (alltab[i] > 0.0f) pos[np++] = alltab[i];
        double med = 0.0;
        if (np) {
            /* Partial selection: a full sort of 380k floats is not needed and
             * this runs once. */
            for (size_t i = 0; i + 1u < np; ++i) {
                size_t m = i;
                for (size_t j = i + 1u; j < np; ++j)
                    if (pos[j] < pos[m]) m = j;
                float t = pos[i]; pos[i] = pos[m]; pos[m] = t;
                if (i > np / 2u) break;
            }
            med = pos[np / 2u];
        }
        free(pos);
        double scale = (med > 0.0) ? target_median / med : 1.0;
        printf("  rescale: %zu of %zu cells populated, raw median %.6g "
               "-> x%.6g for a median of %.3f\n",
               np, total, med, scale, target_median);
        for (size_t i = 0; i < total; ++i) {
            double v = (double)alltab[i] * scale;
            /* The loader adds 0.1 before scaling, so a raw below -0.1 turns
             * a cost negative -- a REWARD for the wrong context. `energy`
             * can produce those legitimately; clamp rather than ship one. */
            if (v < 0.0) v = 0.0;
            alltab[i] = (float)v;
        }

        /* ---- the chunk ---- */
        spfy_vb_riff riff;
        int rc2 = spfy_vb_riff_load(vin_path, &riff);
        if (rc2 != SPFY_OK) { fprintf(stderr, "riff_load %d\n", rc2); return 1; }
        const spfy_vb_chunk *old = spfy_vb_riff_get(&riff, "ccos");
        if (!old) { fprintf(stderr, "no ccos to replace\n"); return 1; }
        /* Keep the existing `labl` verbatim: it is the LABEL ORDERING the
         * unit records' phone_center and phone_ctx are already written in, so
         * regenerating it here would renumber the whole voice. Owning the
         * ordering belongs with the unit writer, not here. */
        spfy_vb_buf lab = {0};
        {
            size_t p2 = 0;
            char id2[5];
            const uint8_t *d2;
            size_t d2n;
            while (spfy_vb_subchunk(old->data, old->n, &p2, id2, &d2, &d2n))
                if (!memcmp(id2, "labl", 4))
                    { spfy_vb_buf_put(&lab, d2, d2n); break; }
        }
        if (!lab.n) { fprintf(stderr, "ccos has no labl\n"); return 1; }

        spfy_vb_buf data = {0}, body = {0};
        rc2 = SPFY_OK;
        for (uint32_t C = 0; C < NC && rc2 == SPFY_OK; ++C)
            for (uint32_t s = 0; s < SPFY_CCOS_N_SLOTS && rc2 == SPFY_OK; ++s) {
                rc2 = spfy_vb_buf_u32(&data, C);
                if (rc2 == SPFY_OK) rc2 = spfy_vb_buf_u32(&data, s);
                const float *t = alltab +
                    ((size_t)C * SPFY_CCOS_N_SLOTS + s) * TRI;
                for (size_t k = 0; k < TRI && rc2 == SPFY_OK; ++k)
                    rc2 = spfy_vb_buf_f32(&data, t[k]);
            }
        if (rc2 == SPFY_OK) rc2 = spfy_vb_buf_put(&body, "labl", 4);
        if (rc2 == SPFY_OK) rc2 = spfy_vb_buf_u32(&body, (uint32_t)lab.n);
        if (rc2 == SPFY_OK) rc2 = spfy_vb_buf_put(&body, lab.p, lab.n);
        if (rc2 == SPFY_OK && (lab.n & 1u)) rc2 = spfy_vb_buf_u8(&body, 0);
        if (rc2 == SPFY_OK) rc2 = spfy_vb_buf_put(&body, "data", 4);
        if (rc2 == SPFY_OK) rc2 = spfy_vb_buf_u32(&body, (uint32_t)data.n);
        if (rc2 == SPFY_OK) rc2 = spfy_vb_buf_put(&body, data.p, data.n);
        if (rc2 == SPFY_OK && (data.n & 1u)) rc2 = spfy_vb_buf_u8(&body, 0);
        if (rc2 != SPFY_OK) { fprintf(stderr, "chunk build %d\n", rc2); return 1; }

        size_t was = old->n;
        rc2 = spfy_vb_riff_set(&riff, "ccos", body.p, body.n);
        if (rc2 == SPFY_OK) rc2 = spfy_vb_riff_save(&riff, out_vin);
        if (rc2 != SPFY_OK) { fprintf(stderr, "write %d\n", rc2); return 1; }
        printf("  wrote %s with OUR ccos (%zu B, was %zu)\n",
               out_vin, body.n, was);
        spfy_vb_buf_free(&lab);
        spfy_vb_buf_free(&data);
    }
    if (do_compare) {
        if (!n_tab) {
            printf("  COMPARE: no table had enough populated cells\n");
        } else {
            double r = sum_r / (double)n_tab;
            double p = sum_p / (double)n_tab;
            double h = n_half_t ? sum_h / (double)n_half_t : 0.0;
            printf("\n  COMPARE over %zu tables  (--agg %s, --per-group %d, "
                   "--edge %d):\n",
                   n_tab, agg ? "energy" : "pair", per_group, edge);
            printf("    SPLIT-HALF reliability       : %+.4f   <- the ceiling\n", h);
            printf("    mean r vs the VIN's own ccos : %+.4f\n", r);
            printf("    mean r vs PERMUTED control   : %+.4f\n", p);
            if (n_g)
                printf("    RIVAL: one global phone matrix vs ccos : %+.4f\n",
                       sum_g / (double)n_g);
            if (h > 0.02)
                printf("    attenuation-corrected        : %+.4f\n", r / sqrt(h));
            /* Judge against the CEILING, not against 1.0: an estimator that
             * cannot reproduce ITSELF cannot be asked to reproduce a vendor. */
            if (h < 0.20)
                printf("    ⚠ estimator is noise-dominated (split-half %.3f); "
                       "raise --per-group before reading anything into r\n", h);
            printf("    %s\n",
                   (r > 2.0 * fabs(p) && h > 0.20 && r / sqrt(h) > 0.5)
                   ? "REPRODUCES -- the kernel is right"
                   : "DOES NOT REPRODUCE -- do not generate this chunk yet");
        }
    }
    return 0;
}
