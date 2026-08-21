/* S4: does a candidate spectral representation reproduce the costs the vendor
 * cached in `hash`, PAIR BY PAIR?
 *
 * spfy_vb_joincost compares two sorted distributions. That is an aggregate: it
 * cannot tell "the vendor's representation" from "any representation with a
 * similar spread", so its 4%-on-p99/p50 agreement is not evidence about the
 * feature set. This tool scores the same pairs and correlates them one to one,
 * which is the measurement that discriminates.
 *
 * Controls printed on every run, because a correlation without them is
 * unreadable:
 *   FLOOR   ours vs the vendor's costs for DIFFERENT pairs (rotated) -- what
 *           zero agreement looks like through this exact arithmetic.
 *   F0-only the cost with the spectral weights zeroed -- what the part we did
 *           NOT have to guess already buys.
 *   SPEC    the cost with the F0 weight zeroed -- so the spectral claim is
 *           separated from the F0 term that came free from the unit record.
 *
 * A candidate is compared against another candidate with --dump: write the
 * per-pair costs of two configurations and correlate the files. That measures
 * the metric's DISCRIMINATING POWER -- how far apart two near-miss
 * representations sit -- which is what says whether a given r against the
 * vendor is close or not.
 *
 *   spfy_vb_jcfit <voice.vin> <voice.vdb> [voice.vcf] [options]
 */

#include "../vb/edge_frames.h"
#include "../vb/join_cost.h"
#include "../usel/hash.h"
#include "../voice/voice.h"
#include "../../include/spfy/spfy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct { float v; uint32_t i; } rank_t;

static int cmp_rank(const void *a, const void *b)
{
    float x = ((const rank_t *)a)->v, y = ((const rank_t *)b)->v;
    return (x < y) ? -1 : (x > y);
}

static double pearson(const float *a, const float *b, size_t n, size_t rot)
{
    long double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
    for (size_t i = 0; i < n; ++i) {
        long double x = a[i], y = b[(i + rot) % n];
        sa += x; sb += y; saa += x * x; sbb += y * y; sab += x * y;
    }
    long double nn = (long double)n;
    long double cov = sab - sa * sb / nn;
    long double va  = saa - sa * sa / nn;
    long double vb  = sbb - sb * sb / nn;
    if (va <= 0 || vb <= 0) return 0.0;
    return (double)(cov / sqrtl(va * vb));
}

/* Fractional ranks with ties averaged, written back over `dst`. */
static void to_ranks(const float *v, size_t n, rank_t *scratch, float *dst)
{
    for (size_t i = 0; i < n; ++i) { scratch[i].v = v[i]; scratch[i].i = (uint32_t)i; }
    qsort(scratch, n, sizeof *scratch, cmp_rank);
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j + 1 < n && scratch[j + 1].v == scratch[i].v) ++j;
        double r = 0.5 * (double)(i + j) + 1.0;
        for (size_t k = i; k <= j; ++k) dst[scratch[k].i] = (float)r;
        i = j + 1;
    }
}

static int u32_arg(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (!end || *end) return 0;
    *out = (uint32_t)v;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
            "usage: %s <voice.vin> <voice.vdb> [voice.vcf] [options]\n"
            "  --win N       window samples, power of two   (default 256)\n"
            "  --cep N       cepstral coefficients kept     (default 12)\n"
            "  --mel N       mel filterbank channels        (default 26)\n"
            "  --preemph F   pre-emphasis, 0 disables       (default 0.97)\n"
            "  --center      centre the window on the boundary (default: edge)\n"
            "  --c0          keep DCT bin 0 (energy) as the first coefficient\n"
            "  --lifter N    sinusoidal liftering           (default off)\n"
            "  --magnitude   |X| into the filterbank instead of |X|^2\n"
            "  --logmel      emit the log filterbank itself (no DCT); sets cep=mel\n"
            "  --lpc N       LPC order N -> cepstrum, instead of the filterbank\n"
            "  --nonorm      leave the spectral block unnormalised (cross-voice)\n"
            "  --pitch F     Festival pitch-synchronous frames, F*period long\n"
            "                (F=2.0 is DEFAULT_FRAME_FACTOR); --win is the FFT size\n"
            "  --pitch-align N  0 raw boundary, 1 nearest epoch inside the unit,\n"
            "                2 anti-phase (control for the epoch detector)\n"
            "  --pitch-f0    dim 0 = rate/period, Festival's channel 0\n"
            "  --pitch-fixlen  keep analysis length at --win in pitch mode\n"
            "  --pitch-offset F  shift the window centre by F local periods\n"
            "  --shift N     shift the window centre by N samples (fixed path)\n"
            "  --dump FILE   write our per-pair costs for cross-config compare\n"
            "  --export FILE write frames + the vendor's pair list, so the\n"
            "                spectral metric can be FITTED rather than guessed\n",
            argv[0]);
        return 2;
    }

    const char *vinp = argv[1], *vdbp = argv[2], *vcfp = NULL;
    const char *dump = NULL, *expo = NULL;
    uint32_t *pair_l = NULL, *pair_r = NULL;
    spfy_vb_cfg_t cfg;
    spfy_vb_cfg_default(&cfg);

    for (int i = 3; i < argc; ++i) {
        const char *a = argv[i];
        if (a[0] != '-') {
            if (vcfp) { fprintf(stderr, "unexpected argument: %s\n", a); return 2; }
            vcfp = a;
            continue;
        }
        if (!strcmp(a, "--center"))         { cfg.anchor = SPFY_VB_ANCHOR_CENTER; continue; }
        if (!strcmp(a, "--c0"))             { cfg.keep_c0 = 1; continue; }
        if (!strcmp(a, "--magnitude"))      { cfg.power = 0; continue; }
        if (!strcmp(a, "--logmel"))         { cfg.logmel = 1; continue; }
        /* Per-dim variance normalisation is a diagonal rescale, so a metric
         * fitted under one voice's sd does not mean the same thing under
         * another's. Turn it off to compare or transfer M across voices. */
        if (!strcmp(a, "--nonorm"))         { cfg.norm = 0; continue; }
        if (!strcmp(a, "--pitch-f0"))       { cfg.pitch_f0 = 1; continue; }
        if (!strcmp(a, "--pitch-fixlen"))   { cfg.pitch_fixlen = 1; continue; }

        if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", a); return 2; }
        const char *v = argv[++i];
        if      (!strcmp(a, "--win"))     { if (!u32_arg(v, &cfg.win))   goto badval; }
        else if (!strcmp(a, "--cep"))     { if (!u32_arg(v, &cfg.n_cep)) goto badval; }
        else if (!strcmp(a, "--mel"))     { if (!u32_arg(v, &cfg.n_mel)) goto badval; }
        else if (!strcmp(a, "--preemph")) cfg.preemph = (float)atof(v);
        else if (!strcmp(a, "--lifter"))  cfg.lifter  = atoi(v);
        else if (!strcmp(a, "--lpc"))     cfg.lpc     = atoi(v);
        else if (!strcmp(a, "--noise"))   cfg.noise_snr = (float)atof(v);
        else if (!strcmp(a, "--pitch"))   cfg.pitch_factor = (float)atof(v);
        else if (!strcmp(a, "--pitch-align")) cfg.pitch_align = atoi(v);
        else if (!strcmp(a, "--pitch-offset")) cfg.pitch_offset = (float)atof(v);
        else if (!strcmp(a, "--shift"))   cfg.shift_abs = atoi(v);
        else if (!strcmp(a, "--shift-start")) cfg.shift_start = atoi(v);
        else if (!strcmp(a, "--shift-end"))   cfg.shift_end = atoi(v);
        else if (!strcmp(a, "--dump"))    dump = v;
        else if (!strcmp(a, "--export"))  expo = v;
        else { fprintf(stderr, "unknown option: %s\n", a); return 2; }
        continue;
    badval:
        fprintf(stderr, "bad value for %s: %s\n", a, v);
        return 2;
    }

    if (cfg.logmel) cfg.n_cep = cfg.n_mel;   /* the block IS the filterbank */

    spfy_vin_t vin = {0};
    spfy_vdb_t vdb = {0};
    float *ours = NULL, *theirs = NULL, *f0only = NULL, *spec = NULL;
    float *ra = NULL, *rb = NULL;
    rank_t *scratch = NULL;
    spfy_vb_frames_t fr = {0};

    int rc = spfy_vin_load(vinp, &vin);
    if (rc != SPFY_OK) { fprintf(stderr, "vin_load: %d\n", rc); return 1; }
    rc = spfy_vdb_load(vdbp, &vdb);
    if (rc != SPFY_OK) { fprintf(stderr, "vdb_load: %d\n", rc); spfy_vin_free(&vin); return 1; }

    float jw = 1.75f, jo = 0.15f;
    if (vcfp) {
        spfy_vcf_t vcf = {0};
        if (spfy_vcf_load(vcfp, &vcf) == SPFY_OK) {
            jw = spfy_vcf_f32(&vcf, "JOIN_COST_WEIGHT", jw);
            jo = spfy_vcf_f32(&vcf, "JOIN_COST_OFFSET", jo);
            spfy_vcf_free(&vcf);
        }
    }

    printf("cfg: win %u  cep %u  mel %u  preemph %.2f  anchor %s  c0 %s  "
           "lifter %d  %s\n",
           cfg.win, cfg.n_cep, cfg.n_mel, (double)cfg.preemph,
           cfg.anchor == SPFY_VB_ANCHOR_CENTER ? "center" : "edge",
           cfg.keep_c0 ? "kept" : "dropped", cfg.lifter,
           cfg.power ? "power" : "magnitude");
    printf("JOIN_COST_WEIGHT %.4f  JOIN_COST_OFFSET %.4f\n", jw, jo);

    rc = spfy_vb_frames_build_ex(&vin, &vdb, 8000u, &cfg, &fr);
    if (rc != SPFY_OK) { fprintf(stderr, "frames_build: %d\n", rc); goto done; }
    printf("units %u, unresolved %u (%.2f%%), dim %u\n", fr.n_units, fr.n_missing,
           100.0 * (double)fr.n_missing / (double)(fr.n_units ? fr.n_units : 1), fr.dim);

    spfy_jc_t jc;
    spfy_vb_frames_bind(&fr, &jc);
    rc = spfy_jc_derive_weights(&jc, 1.0f);
    if (rc != SPFY_OK) { fprintf(stderr, "derive_weights: %d\n", rc); goto done; }

    spfy_hash_t h;
    rc = spfy_hash_load(&vin, &h);
    if (rc != SPFY_OK) { fprintf(stderr, "hash_load: %d\n", rc); goto done; }

    size_t live = 0;
    for (uint32_t i = 0; i < h.n_cells; ++i)
        if (spfy_hash_cell_a(&h, i) != 0xFFFFFFFFu) ++live;

    ours   = (float *)malloc(live * sizeof *ours);
    theirs = (float *)malloc(live * sizeof *theirs);
    f0only = (float *)malloc(live * sizeof *f0only);
    spec   = (float *)malloc(live * sizeof *spec);
    if (!ours || !theirs || !f0only || !spec) { rc = SPFY_E_NOMEM; goto done; }
    if (expo) {
        pair_l = (uint32_t *)malloc(live * sizeof *pair_l);
        pair_r = (uint32_t *)malloc(live * sizeof *pair_r);
        if (!pair_l || !pair_r) { rc = SPFY_E_NOMEM; goto done; }
    }

    /* Three scorings over the identical pair population: full, F0 term only,
     * spectral only. Same code path, one weight vector swapped, so the three
     * numbers are comparable to each other by construction. */
    float w_full[SPFY_VB_MAX_CEP + SPFY_JC_DIM_SPEC];
    memcpy(w_full, jc.weights, fr.dim * sizeof *w_full);

    size_t n = 0, n_cont = 0, n_cont_zero = 0;
    for (uint32_t i = 0; i < h.n_cells; ++i) {
        uint32_t r = spfy_hash_cell_a(&h, i);
        if (r == 0xFFFFFFFFu || r >= h.n_rows) continue;
        uint32_t base = spfy_hash_row(&h, r);
        if (i < base) continue;
        uint32_t l = i - base;
        if (l >= fr.n_units || r >= fr.n_units) continue;

        float v = spfy_hash_cell_b(&h, i);
        if (r == l + 1u) { ++n_cont; if (v == 0.0f) ++n_cont_zero; continue; }

        theirs[n] = v;
        if (expo) { pair_l[n] = l; pair_r[n] = r; }
        memcpy(jc.weights, w_full, fr.dim * sizeof *w_full);
        ours[n] = spfy_jc_cached_value(&jc, l, r, jw, jo);

        for (uint32_t k = SPFY_JC_DIM_SPEC; k < fr.dim; ++k) jc.weights[k] = 0.0f;
        f0only[n] = spfy_jc_cached_value(&jc, l, r, jw, jo);

        memcpy(jc.weights, w_full, fr.dim * sizeof *w_full);
        jc.weights[SPFY_JC_DIM_F0] = 0.0f;
        spec[n] = spfy_jc_cached_value(&jc, l, r, jw, jo);
        ++n;
    }
    memcpy(jc.weights, w_full, fr.dim * sizeof *w_full);

    printf("continuations %zu (vendor stores 0: %zu)   scored pairs %zu\n\n",
           n_cont, n_cont_zero, n);
    if (n < 1000) { fprintf(stderr, "too few pairs\n"); rc = SPFY_E_INVAL; goto done; }

    scratch = (rank_t *)malloc(n * sizeof *scratch);
    ra = (float *)malloc(n * sizeof *ra);
    rb = (float *)malloc(n * sizeof *rb);
    if (!scratch || !ra || !rb) { rc = SPFY_E_NOMEM; goto done; }

    /* Rotation for the floor control: coprime-ish with n and far from 0 so the
     * mismatched pairing shares no structure with the real one. */
    const size_t rot = n / 3u + 1u;

    to_ranks(theirs, n, scratch, rb);

    struct { const char *tag; const float *v; } arms[3] = {
        { "full   (F0 + spectral)", ours   },
        { "F0 only              ", f0only },
        { "spectral only        ", spec   },
    };

    printf("per-pair agreement with the vendor's cached cost, n = %zu\n", n);
    printf("  %-22s %10s %10s %10s\n", "arm", "pearson", "spearman", "FLOOR r");
    for (int k = 0; k < 3; ++k) {
        double pr = pearson(arms[k].v, theirs, n, 0);
        double fl = pearson(arms[k].v, theirs, n, rot);
        to_ranks(arms[k].v, n, scratch, ra);
        double sp = pearson(ra, rb, n, 0);
        printf("  %-22s %10.4f %10.4f %10.4f\n", arms[k].tag, pr, sp, fl);
    }

    if (dump) {
        FILE *f = fopen(dump, "wb");
        if (!f) { fprintf(stderr, "cannot write %s\n", dump); rc = SPFY_E_IO; goto done; }
        uint32_t nn = (uint32_t)n;
        fwrite("JCF1", 1, 4, f);
        fwrite(&nn, sizeof nn, 1, f);
        fwrite(ours, sizeof *ours, n, f);
        fwrite(theirs, sizeof *theirs, n, f);
        fclose(f);
        printf("\ndumped %zu pairs to %s\n", n, dump);
    }

    if (expo) {
        FILE *f = fopen(expo, "wb");
        if (!f) { fprintf(stderr, "cannot write %s\n", expo); rc = SPFY_E_IO; goto done; }
        uint32_t hdr[3] = { fr.n_units, fr.dim, (uint32_t)n };
        float    aff[2] = { jw, jo };
        fwrite("JCX1", 1, 4, f);
        fwrite(hdr, sizeof *hdr, 3, f);
        fwrite(aff, sizeof *aff, 2, f);
        fwrite(fr.frames, sizeof *fr.frames, (size_t)fr.n_units * 2u * fr.dim, f);
        fwrite(pair_l, sizeof *pair_l, n, f);
        fwrite(pair_r, sizeof *pair_r, n, f);
        fwrite(theirs, sizeof *theirs, n, f);
        fclose(f);
        printf("\nexported %u units x 2 frames x %u dims and %zu pairs to %s\n",
               fr.n_units, fr.dim, n, expo);
    }

    rc = SPFY_OK;
done:
    free(ours); free(theirs); free(f0only); free(spec);
    free(scratch); free(ra); free(rb);
    free(pair_l); free(pair_r);
    spfy_vb_frames_free(&fr);
    spfy_vdb_free(&vdb);
    spfy_vin_free(&vin);
    return rc == SPFY_OK ? 0 : 1;
}
