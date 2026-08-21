/* spfy_vb_build -- build a voice from a corpus, in one program.
 *
 * The seven stages of src/vb/README.md, in the order build_voice.ps1 spent a
 * session establishing:
 *
 *     S1 CORPUS  -> S2 FEATURES -> S7 container
 *                -> S3 NORM  -> S6 TREES  -> S5 PRESEL -> S4 JOIN
 *
 * ⚠ THE ORDER IS NOT COSMETIC. Running `mean` and `durt` on top of an
 * already-applied join table produced a voice whose ASR word error was 56.4%
 * where the correct order gave 18.9%, with every intermediate check -- ctx
 * cross-check, unit count, join miss rate -- looking identical. Nothing
 * downstream reports it, so the order is enforced by this program rather than
 * remembered.
 *
 * What this REPLACES, and why it is not a transliteration:
 *
 *   * Steps 3, 6 and 7 of build_voice.ps1 are gone. Those bootstrapped a
 *     throwaway triphone join table, rendered 1,200 lines through the voice
 *     to harvest preselection pools, and rebuilt the table from them. S4 here
 *     derives its domain from S5's own candidate structure, so there is no
 *     bootstrap, no render pass, and no pools file to go stale against a
 *     rebuilt inventory.
 *   * The audio statistics `mean` needs are captured in S1 while each wav and
 *     f0 track is already open, instead of being re-derived from the written
 *     VIN. That removes the chunk-name round trip that once cost 25.2% of a
 *     voice's units their statistics with no symptom but a coverage line.
 *
 * What it does NOT yet generate, and this is the honest list:
 *   `ccos`  entirely the template's -- there is no writer for it at all, and
 *           it is measured BYTE-IDENTICAL to jill's in shipped builds.
 *   `f0tr`  the template's TOPOLOGY, QUESTIONS and leaf VARIANCES; only the
 *           leaf means are ours, and only when --f0 is not `absent`.
 *   `durt`  same -- topology, questions and variances are the template's.
 * `hist` IS written (see S6), but under the default --f0 absent there is no
 * F0 in the inventory so the curve it writes is flat.
 * That is 1.56 MB of a ~110 MB container, and it is the largest term in the
 * target cost describing a different speaker.
 *
 *   spfy_vb_build --voice donnac --wav-dir <dir> --tg-dir <dir> --out-dir <dir>
 */

#include "../common/log.h"
#include "../vb/vb_chunk.h"
#include "../vb/vb_corpus.h"
#include "../vb/vb_lang.h"
#include "../vb/vb_vcf.h"
#include "../vb/vb_io.h"
#include "../vb/vb_stages.h"
#include "../../include/spfy/spfy.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- stage clock ------------------------------------------------------
 *
 * Added because a 15-minute build was diagnosed by guessing. The two regimes
 * are not the same shape: with backoff off, S1 CORPUS dominates and S4 is
 * ~20 s; at --prsl-backoff 24 the S4 K-best pass costs 2.4 BILLION candidate
 * joins and swamps everything. Optimising the wrong one is free to do and
 * worth nothing, so the build now says where its time went. */
static double g_t0 = 0.0, g_t_stage = 0.0;

/* ⚠ WALL time, not CPU time. POSIX clock() counts every thread's CPU, so once
 * S4 runs on 24 of them it would report 24x the elapsed seconds and the whole
 * point of the clock would be lost. omp_get_wtime() is wall by definition;
 * without OpenMP there is no threading and clock() is equivalent. */
#ifdef _OPENMP
#   include <omp.h>
#endif

static double now_s(void)
{
#ifdef _OPENMP
    return omp_get_wtime();
#else
    return (double)clock() / (double)CLOCKS_PER_SEC;
#endif
}

static void stage(const char *name)
{
    double t = now_s();
    if (g_t_stage > 0.0)
        printf("  [%.1fs]\n", t - g_t_stage);
    g_t_stage = t;
    if (!g_t0) g_t0 = t;
    if (name) printf("\n=== %s ===\n", name);
}

static uint32_t rd_le_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int copy_file(const char *src, const char *dst)
{
    uint8_t *b = NULL;
    size_t n = 0;
    if (spfy_vb_read_bytes(src, &b, &n) != SPFY_OK) return SPFY_E_IO;
    FILE *f = fopen(dst, "wb");
    if (!f) { free(b); return SPFY_E_IO; }
    size_t w = n ? fwrite(b, 1, n, f) : 0;
    fclose(f);
    free(b);
    return (w == n) ? SPFY_OK : SPFY_E_IO;
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* One lowercased token per line, `#` comments and blanks skipped. Separate
 * from vb_corpus.c's stem loader because that one is static there and this
 * list is words, not recordings -- and words are matched case-folded, since
 * the corpus text is FE-normalised lowercase while a human writes "Alki". */
static int word_list_load(const char *path, char ***out, size_t *out_n)
{
    uint8_t *b = NULL;
    size_t n = 0;
    if (spfy_vb_read_bytes(path, &b, &n) != SPFY_OK) return SPFY_E_IO;
    char **v = NULL;
    size_t cnt = 0, cap = 0, i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && b[j] != '\n' && b[j] != '\r') ++j;
        size_t a = i, e = j;
        while (a < e && (b[a] == ' ' || b[a] == '\t')) ++a;
        while (e > a && (b[e - 1] == ' ' || b[e - 1] == '\t')) --e;
        if (e > a && b[a] != '#') {
            if (cnt == cap) {
                size_t nc = cap ? cap * 2 : 32;
                char **nv = (char **)realloc(v, nc * sizeof *nv);
                if (!nv) { free(b); for (size_t k = 0; k < cnt; ++k) free(v[k]);
                           free(v); return SPFY_E_NOMEM; }
                v = nv; cap = nc;
            }
            char *s = (char *)malloc(e - a + 1);
            if (!s) { free(b); for (size_t k = 0; k < cnt; ++k) free(v[k]);
                      free(v); return SPFY_E_NOMEM; }
            for (size_t k = a; k < e; ++k) {
                int c = b[k];
                s[k - a] = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
            }
            s[e - a] = '\0';
            v[cnt++] = s;
        }
        i = (j < n && b[j] == '\r' && j + 1 < n && b[j + 1] == '\n') ? j + 2
                                                                    : j + 1;
    }
    free(b);
    *out = v;
    *out_n = cnt;
    return SPFY_OK;
}

/* (preselection key, unit index) so the sole-source rescue can group by key
 * without touching vb_chunk.c's own sort. */
typedef struct { uint32_t key; size_t idx; } key_uid_g;

static int cmp_key_idx(const void *a, const void *b)
{
    const key_uid_g *x = (const key_uid_g *)a, *y = (const key_uid_g *)b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return x->idx < y->idx ? -1 : (x->idx > y->idx);
}

static int word_list_has(char **v, size_t n, const char *w, uint8_t *hit)
{
    if (!w) return 0;
    int found = 0;
    for (size_t k = 0; k < n; ++k) {
        const char *a = v[k];
        const char *bq = w;
        while (*a && *bq) {
            int cb = *bq;
            if (cb >= 'A' && cb <= 'Z') cb = cb - 'A' + 'a';
            if (*a != cb) break;
            ++a; ++bq;
        }
        if (!*a && !*bq) { hit[k] = 1; found = 1; }
    }
    return found;
}

/* S4 against a vin/vdb already on disk, then splice. Reloading is deliberate:
 * the chunk has to land in the container the engine will actually read, not in
 * one still in memory from before the write. */
static int run_s4(const char *vinp, const char *vdbp,
                  const spfy_vb_join_cfg *jc)
{
    uint8_t *hash = NULL;
    size_t hash_n = 0;
    spfy_vb_join_stats st;
    int rc = spfy_vb_s4_join(vinp, vdbp, jc, &hash, &hash_n, &st);
    if (rc != SPFY_OK) { fprintf(stderr, "join %d\n", rc); return rc; }
    printf("  domain %zu pairs (%zu natural continuations, %.2f%%), "
           "%.2f per unit\n", st.n_pairs, st.n_cont,
           100.0 * (double)st.n_cont / (double)(st.n_pairs ? st.n_pairs : 1),
           (double)st.n_pairs / (double)(st.n_rows ? st.n_rows : 1));
    printf("  %zu rows, %zu cells, fill %.1f%%, %zu bytes\n",
           st.n_rows, st.n_cells,
           100.0 * (double)st.n_pairs / (double)(st.n_cells ? st.n_cells : 1),
           hash_n);
    printf("  %zu candidate joins costed\n", st.n_scored);
    if (st.n_no_frames)
        printf("  %zu units had no resolvable audio for their edge frames\n",
               st.n_no_frames);

    spfy_vb_riff v;
    rc = spfy_vb_riff_load(vinp, &v);
    if (rc != SPFY_OK) { free(hash); return rc; }
    rc = spfy_vb_riff_set(&v, "hash", hash, hash_n);
    if (rc != SPFY_OK) { free(hash); spfy_vb_riff_free(&v); return rc; }
    rc = spfy_vb_riff_save(&v, vinp);
    spfy_vb_riff_free(&v);
    if (rc == SPFY_OK) printf("  hash spliced into %s\n", vinp);
    return rc;
}

static int write_raw(const char *path, const uint8_t *b, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return SPFY_E_IO;
    size_t w = n ? fwrite(b, 1, n, f) : 0;
    fclose(f);
    return (w == n) ? SPFY_OK : SPFY_E_IO;
}

/* A comma list of phone NAMES -> a 256-entry membership table over feat phone
 * ids. A name the template does not know is reported rather than ignored: a
 * typo in an allow-list silently widens or narrows the policy it names. */
static int phone_set(const spfy_vb_phone_index *pi, const char *csv,
                     uint8_t set[256], const char *what)
{
    memset(set, 0, 256);
    int n = 0;
    const char *p = csv;
    while (*p) {
        while (*p == ',' || *p == ' ') ++p;
        const char *s = p;
        while (*p && *p != ',') ++p;
        size_t ln = (size_t)(p - s);
        while (ln && s[ln - 1u] == ' ') --ln;
        if (!ln) continue;
        char nm[16];
        if (ln >= sizeof nm) ln = sizeof nm - 1u;
        memcpy(nm, s, ln);
        nm[ln] = '\0';
        int id = spfy_vb_phone_id(pi, nm);
        if (id < 0 || id > 255) {
            fprintf(stderr, "  ⚠ %s: no phone named \"%s\" in the template's "
                            "feat[name]; ignored\n", what, nm);
            continue;
        }
        set[id] = 1;
        ++n;
    }
    return n;
}

/* The per-phone bound, with the two extremes NAMED and the phones that got no
 * bound counted. A lookup that silently collapsed to one value for every phone
 * would print an identical-looking summary, so the spread is the evidence that
 * the gate is per phone at all -- and `dx` sitting far below `ae` is the whole
 * reason a flat millisecond cutoff was the wrong design. */
static void dump_pct_range(const spfy_vb_phone_index *pi, const double *v,
                           double pct, size_t n_set, size_t n_phones,
                           const char *what)
{
    size_t lo = (size_t)-1, hi = (size_t)-1;
    for (size_t i = 0; i < pi->n && i < 256u; ++i) {
        if (v[i] <= 0.0) continue;
        if (lo == (size_t)-1 || v[i] < v[lo]) lo = i;
        if (hi == (size_t)-1 || v[i] > v[hi]) hi = i;
    }
    if (lo == (size_t)-1) {
        printf("  ⚠ duration %s p%.4g: NO phone in the reference had %d+ "
               "examples -- nothing is gated\n",
               what, pct, SPFY_VB_DUR_PCT_MIN_N);
        return;
    }
    printf("  duration %s p%.4g: %zu of %zu phones, %.1f ms (%s) .. %.1f ms "
           "(%s)\n", what, pct, n_set, n_phones,
           v[lo], pi->name[lo], v[hi], pi->name[hi]);
    if (n_set < n_phones)
        printf("    %zu phone(s) ungated -- fewer than %d examples in the "
               "reference\n", n_phones - n_set, SPFY_VB_DUR_PCT_MIN_N);
    /* The whole table, for diffing against vb_build1.py's phone_dur_floors().
     * Two percentile conventions that disagree only on interpolation differ by
     * under a millisecond, which the summary line above cannot show. */
    if (getenv("SPFY_VB_DUR_DUMP"))
        for (size_t i = 0; i < pi->n && i < 256u; ++i)
            printf("    dur_%s\t%s\t%.6f\n", what, pi->name[i], v[i]);
}

int main(int argc, char **argv)
{
    const char *voice = NULL, *wav_dir = NULL, *tg_dir = NULL, *seg_dir = NULL;
    const char *out_dir = NULL, *tmpl_vin = NULL, *tmpl_vdb = NULL;
    const char *vcf_src = NULL, *drop_path = NULL, *drop_chunks = NULL;
    const char *compress_path = NULL;
    const char *gate_words = NULL;
    int gate_sole_keep = 1;
    const char *copyright = NULL, *version = "3.0.0.0";
    const char *rvc_policy = "prefer-real";
    const char *rvc_phones = NULL, *rvc_equal_phones = NULL;
    int rvc_anchors_drop = 1;
    int f0_calibrated = 0, f0_join_only = 0, trim_silence = 0, limit = 0;
    int f0_render_only = 0;
    double f0q_slope = 0.0, f0q_off = 0.0;
    double level_target = 0.0, level_max_gain = 4.0, level_peak = -0.5;
    int k_best = 12, join_const = 1;
    /* 0 = the old single-heap-over-all-keys behaviour. */
    int k_per_key = 0;
    int dur_floor_ms = 0, dur_ceil_ms = 0;
    double dur_floor_pct = 0.0, dur_ceil_pct = 0.0;
    const char *dur_ref_vin = NULL;
    int unit_version = 0;          /* 0 = follow the template */
    /* ⭐ OWN TREES, DEFAULT ON since 2026-08-21. `durt`/`f0tr` are grown from
     * our corpus over our own question inventory instead of inheriting the
     * donor's topology, questions and leaf variances. Gated on the vendors
     * first: our tree beats their own shipped tree on held-out RMSE for jill
     * and tom, durt and f0tr. --donor-trees restores the old patch-the-leaves
     * behaviour for a comparison run. */
    int own_trees = 1;
    int tree_min_cluster = 50;
    /* ⭐ --no-template: no donor voice at all. The containers are built from
     * the embedded en-US language tables (vb_lang.h -- every byte measured
     * IDENTICAL across jill and tom) plus an all-zero `ccos` carrying our own
     * label list, and every other chunk is written as usual. Nothing of a
     * vendor's survives into the output.
     * ⚠ The `ccos` a --no-template build ships is ZERO, i.e. no context cost
     * at all. That is honest -- we have not reproduced the quantity, r=0.146
     * against a split-half ceiling of 0.82 -- but it is a real change to
     * selection and has to be judged by ear against a donor-ccos arm. */
    int no_template = 0;
    /* --vcf-set KEY=VALUE, applied to the plaintext before it is enciphered.
     * A key the template does not hold is an ERROR, not a no-op: a typo that
     * changed nothing would look exactly like a weight that is inert, and
     * several of these weights genuinely are. */
    spfy_vb_vcf_set vcf_sets[32];
    size_t n_vcf_sets = 0;
    /* ⭐ DEFAULT `word`. Synthetic WHOLE WORDS are the point of the render
     * corpus; synthetic SYLLABLES get spliced into words the render never
     * spoke, which is audible as one word changing accent. */
    const char *syn_anchors = "word";
    double const_cost = 1.0;
    /* 0 = off (the historic partition). See spfy_vb_groups_backoff(). */
    int prsl_backoff = 0;
    /* ⭐ DEFAULT 2026-08-18, after the arm was heard and preferred.
     * Filling every enumerable key put contextually unrelated units under
     * 97.8% of the space; the vendors populate ~39.5%. The bigram gate lands
     * at 70,111 groups against jill's 77,412, raises the share of fills that
     * match the key's context from 42.7% to 76.9%, holds the accent (+7.73 vs
     * +7.88) and costs 632 M candidate joins instead of 2.43 B.
     * `--prsl-gate all` restores the old behaviour. */
    int prsl_gate = SPFY_VB_BG_BIGRAM;
    /* Off by default until measured: it changes which partners S4 caches, so
     * it is a selection change and gets its own arm. */
    int join_f0 = 0;
    int do_s4 = 1, s4_only = 0;
    uint8_t *gated = NULL;

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        int has = (i + 1 < argc);
#define OPT(name, var) if (!strcmp(a, name) && has) { var = argv[++i]; continue; }
        OPT("--voice", voice)
        OPT("--wav-dir", wav_dir)
        OPT("--tg-dir", tg_dir)
        OPT("--seg-dir", seg_dir)
        OPT("--out-dir", out_dir)
        OPT("--template-vin", tmpl_vin)
        OPT("--template-vdb", tmpl_vdb)
        if (!strcmp(a, "--no-template")) { no_template = 1; continue; }
        if (!strcmp(a, "--vcf-set") && has) {
            char *kv = argv[++i];
            char *eq = strchr(kv, '=');
            if (!eq) {
                fprintf(stderr, "--vcf-set wants KEY=VALUE, got %s\n", kv);
                return 2;
            }
            if (n_vcf_sets >= sizeof vcf_sets / sizeof vcf_sets[0]) {
                fprintf(stderr, "too many --vcf-set\n");
                return 2;
            }
            *eq = 0;
            vcf_sets[n_vcf_sets].key = kv;
            vcf_sets[n_vcf_sets].value = eq + 1;
            ++n_vcf_sets;
            continue;
        }
        if (!strcmp(a, "--donor-trees")) { own_trees = 0; continue; }
        if (!strcmp(a, "--own-trees")) { own_trees = 1; continue; }
        if (!strcmp(a, "--tree-min-cluster") && has) {
            tree_min_cluster = atoi(argv[++i]); continue;
        }
        OPT("--vcf", vcf_src)
        OPT("--drop", drop_path)
        OPT("--drop-chunks", drop_chunks)
        OPT("--compress", compress_path)
        OPT("--gate-words", gate_words)
        OPT("--copyright", copyright)
        OPT("--version-string", version)
        OPT("--rvc-policy", rvc_policy)
        OPT("--rvc-phones", rvc_phones)
        OPT("--rvc-equal-phones", rvc_equal_phones)
        OPT("--dur-ref-vin", dur_ref_vin)
        OPT("--syn-anchors", syn_anchors)
#undef OPT
        if (!strcmp(a, "--gate-sole") && has) {
            const char *v = argv[++i];
            if (!strcmp(v, "keep")) gate_sole_keep = 1;
            else if (!strcmp(v, "drop")) gate_sole_keep = 0;
            else {
                spfy_log_err("vb: --gate-sole takes keep|drop, got %s", v);
                return 2;
            }
            continue;
        }
        if (!strcmp(a, "--rvc-anchors") && has) {
            ++i;
            rvc_anchors_drop = strcmp(argv[i], "keep") != 0;
            continue;
        }
        if (!strcmp(a, "--f0") && has) {
            ++i;
            /* `joinonly` = calibrated, then the f0tr target cost neutralised.
             * See spfy_vb_f0tr_zero_var() for why the two are separable and
             * what each is worth. */
            /* `render` writes f0_start/f0_end for WSOLA's PSOLA crossfade but
             * forces f0_mid to 0 and zeroes the f0tr leaf variances, so every
             * f0 consumer in SELECTION stays constant. The picks must come out
             * identical to `absent`; see spfy_vb_corpus_cfg.f0_render_only. */
            f0_render_only = !strcmp(argv[i], "render");
            f0_join_only  = !strcmp(argv[i], "joinonly");
            f0_calibrated = f0_join_only || !strcmp(argv[i], "calibrated");
            continue;
        }
        if (!strcmp(a, "--f0-slope") && has) { f0q_slope = atof(argv[++i]); continue; }
        if (!strcmp(a, "--f0-offset") && has) { f0q_off = atof(argv[++i]); continue; }
        if (!strcmp(a, "--trim-silence")) { trim_silence = 1; continue; }
        if (!strcmp(a, "--level-target") && has) { level_target = atof(argv[++i]); continue; }
        if (!strcmp(a, "--level-max-gain") && has) { level_max_gain = atof(argv[++i]); continue; }
        if (!strcmp(a, "--level-peak") && has) { level_peak = atof(argv[++i]); continue; }
        if (!strcmp(a, "--limit") && has) { limit = atoi(argv[++i]); continue; }
        if (!strcmp(a, "--k-best") && has) { k_best = atoi(argv[++i]); continue; }
        if (!strcmp(a, "--prsl-backoff") && has) { prsl_backoff = atoi(argv[++i]); continue; }
        if (!strcmp(a, "--join-f0")) { join_f0 = 1; continue; }
        if (!strcmp(a, "--prsl-gate") && has) {
            const char *v = argv[++i];
            if (!strcmp(v, "all"))         prsl_gate = SPFY_VB_BG_ALL;
            else if (!strcmp(v, "bigram")) prsl_gate = SPFY_VB_BG_BIGRAM;
            else { fprintf(stderr, "unknown --prsl-gate %s (all|bigram)\n", v); return 2; }
            continue;
        }
        if (!strcmp(a, "--k-per-key") && has) { k_per_key = atoi(argv[++i]); continue; }
        if (!strcmp(a, "--dur-floor-ms") && has) { dur_floor_ms = atoi(argv[++i]); continue; }
        if (!strcmp(a, "--dur-ceil-ms") && has) { dur_ceil_ms = atoi(argv[++i]); continue; }
        if (!strcmp(a, "--unit-version") && has) { unit_version = atoi(argv[++i]); continue; }
        if (!strcmp(a, "--dur-floor-pct") && has) { dur_floor_pct = atof(argv[++i]); continue; }
        if (!strcmp(a, "--dur-ceil-pct") && has) { dur_ceil_pct = atof(argv[++i]); continue; }
        if (!strcmp(a, "--const-cost") && has) { const_cost = atof(argv[++i]); continue; }
        if (!strcmp(a, "--join-mode") && has) {
            ++i;
            join_const = strcmp(argv[i], "cost") != 0;
            continue;
        }
        if (!strcmp(a, "--no-join")) { do_s4 = 0; continue; }
        if (!strcmp(a, "--s4-only")) { s4_only = 1; continue; }
        fprintf(stderr, "unknown option %s\n", a);
        return 2;
    }
    /* ⛔ There is nothing to inherit FROM. --donor-trees reads the template's
     * durt/f0tr bodies, which under --no-template are empty placeholders; the
     * combination would segfault rather than say so. */
    if (no_template && !own_trees) {
        fprintf(stderr, "--no-template and --donor-trees are exclusive: there "
                        "is no donor to take a tree from\n");
        return 2;
    }
    if (s4_only ? (!voice || !out_dir)
                : (!voice || !wav_dir || !tg_dir || !out_dir ||
                   (!no_template && (!tmpl_vin || !tmpl_vdb)))) {
        fprintf(stderr,
            "usage: %s --voice NAME --wav-dir D --tg-dir D --out-dir D\n"
            "          --template-vin F --template-vdb F [--vcf F]\n"
            "  --no-template    build the containers from the embedded en-US\n"
            "                   language tables instead of a donor voice. No\n"
            "                   vendor byte survives; ccos ships ZERO.\n"
            "  --donor-trees    inherit durt/f0tr topology, questions and leaf\n"
            "                   precisions instead of growing our own\n"
            "  --tree-min-cluster N   wagon's min cluster size (default 50)\n"
            "  --seg-dir D      engine segmentation; used INSTEAD of a TextGrid\n"
            "  --f0 absent|calibrated|joinonly   default absent, which\n"
            "                   measured far better here (18.9%% vs 40.3%%\n"
            "                   ASR word error) because f0tr is the template's\n"
            "                   ⭐ `calibrated` switches on TWO mechanisms at\n"
            "                   once and they fight. The `hist` JOIN cost\n"
            "                   halves audible seams (>2 st: 1.246 -> 0.904\n"
            "                   per second, vb_seamf0.py); the f0tr TARGET\n"
            "                   cost pulls selection to the tree's near-mean\n"
            "                   prediction and flattens the accent (\"the\n"
            "                   NAtional weather service\": +7.4 st -> +1.2)\n"
            "                   ⭐ `joinonly` keeps the join half and zeroes\n"
            "                   the f0tr leaf VARIANCES, which makes the\n"
            "                   target term identically 0. The chunk must stay\n"
            "                   present -- the voice will not load without it\n"
            , argv[0]);
        /* Split here purely to stay under the 4095-char string-literal limit
         * ISO C99 guarantees; -Woverlength-strings flags a single literal
         * past it and this help text is well over. */
        fprintf(stderr,
            "  --trim-silence   MEASURED HARMFUL on this corpus; off by default\n"
            "  --level-target D normalise each recording's SPEECH level to D\n"
            "                   dBFS before the u-law encode. jill sits near\n"
            "                   -13.4, tom -11.4, ours -16.9. Off by default,\n"
            "                   and donnart did NOT use it -- its VDB data is\n"
            "                   byte-identical to an unlevelled build\n"
            "  --level-max-gain X   LINEAR ceiling on that gain (default 4.0)\n"
            "  --level-peak D   dBFS ceiling each recording's own PEAK may\n"
            "                   reach (default -0.5); a linear ceiling alone\n"
            "                   does not stop clipping and 49%% of ours peak\n"
            "                   too high to take the full lift. 0 disables\n"
            "  --drop FILE      JSON {\"exclude\":[stem,...]} (or one stem per\n"
            "                   line) to leave out, for recordings whose\n"
            "                   transcript describes different audio than the\n"
            "                   aligner put it on. ⚠ `rvc_hold/` is EMPTY, so\n"
            "                   converted renders sit in the audio dir; a build\n"
            "                   meant to exclude them MUST pass this and check\n"
            "                   the pair count actually moved\n"
            "  --gate-words F   One word per line (`#` comments, case-folded).\n"
            "                   Withholds every unit cut from those words from\n"
            "                   PRESELECTION and drops any anchor touching\n"
            "                   them, without renumbering uids. For words the\n"
            "                   FE pronounces wrongly: the aligner shares its\n"
            "                   dictionary, so table and TextGrid AGREE on a\n"
            "                   sound the audio does not have, and the units\n"
            "                   leak into ordinary words -- `alki` read as\n"
            "                   `ae l k iy` is what makes \"keep\" say \"kipe\".\n"
            "                   Costs ~0.65%% of units for the whole rare\n"
            "                   non-English class (vb_wordrisk.py)\n"
            "  --gate-sole M    keep (default) or drop. `keep` refuses to gate\n"
            "                   a unit that is the SOLE surviving member of\n"
            "                   its preselection key, so the gate cannot cost\n"
            "                   a context outright -- rare words are the leak\n"
            "                   but also where the rare contexts live (0.65%%\n"
            "                   of units, sole source of 1.75%% of contexts).\n"
            "                   A unit only leaks when it WINS against\n"
            "                   something else; where nothing competes,\n"
            "                   withholding it just moves the defect\n"
            );
        fprintf(stderr,
            "  --drop-chunks F  COMPACTION. One indx name per line (`stem`, or\n"
            "                   `stem~N` for a split); a listed chunk keeps\n"
            "                   NEITHER its audio NOR its units. Safe at build\n"
            "                   time because uids are assigned once, after S1,\n"
            "                   to whatever survived -- there is no renumbering\n"
            "                   for the anchor scorer's uid arithmetic to trip\n"
            "                   over, and anchors spanning a dropped chunk are\n"
            "                   removed rather than left pointing elsewhere\n"
            "  --rvc-policy P   how SYNTHETIC units may compete -- both\n"
            "                   `rvc_*` (Applio conversions) and `st2_*`\n"
            "                   (StyleTTS2 renders). Neither is her own audio.\n"
            "                   none        withheld from preselection entirely\n"
            "                   prefer-real her own audio wins any group it is\n"
            "                               in; converted fills only the gaps\n"
            "                   equal       they compete on level terms\n"
            "                   Measured at n=60 ASR word error: none 14.3%%,\n"
            "                   equal 19.0%%, prefer-real 22.2%% -- and NONE of\n"
            "                   that has been judged by ear. Default prefer-real\n"
            "  --rvc-phones L   comma list of phones converted units may serve\n"
            "                   at all. Applied BEFORE --rvc-policy, and the\n"
            "                   order is not cosmetic: run after, it withholds\n"
            "                   exactly ZERO units and the log says so while\n"
            "                   every converted unit stays live\n"
            "  --rvc-equal-phones L   phones for which prefer-real does NOT\n"
            "                   strip converted units. For a phone she barely\n"
            "                   has, prefer-real hands the DP one token to\n"
            "                   splice into every context, which is what\n"
            "                   \"disjointed\" sounds like\n"
            "  --rvc-anchors keep|drop   default drop. Converted _WORD_ records\n"
            "                   match the FE 54.53%% of the time against her own\n"
            "                   77.00%%, and 92 of 792 recordings are wrong\n"
            "                   WHOLESALE -- all converted, none of hers. The\n"
            "                   PHONE labels are fine, so the units stay\n");
        /* ⚠ SPLIT DELIBERATELY. ISO C99 only requires 4095-char string
         * literals and this help had already reached ~4070; one more option
         * tripped -Woverlength-strings. Add new options to the SECOND half. */
        fprintf(stderr,
            "  --dur-floor-ms N withhold units shorter than N ms from\n"
            "                   preselection; they stay in the table. Measured:\n"
            "                   \"you\" came out INAUDIBLE from 5/5/15/15 ms\n"
            "                   half-phones, and short units are what leaves\n"
            "                   WSOLA stretching a slot it cannot fill\n"
            "  --dur-ceil-ms N  withhold units LONGER than N ms; WSOLA has to\n"
            "                   compress those into the slot. ⚠ must exceed\n"
            "                   --dur-floor-ms or the build refuses\n"
            "  --dur-floor-pct P  ⭐ THE BETTER FLOOR. Same withholding, but\n"
            "                   the bound is that phone's Pth percentile in a\n"
            "                   SHIPPED voice, so it is what a working\n"
            "                   inventory contains rather than a number\n"
            "                   someone picked. A flat cutoff is wrong in both\n"
            "                   directions: jill keeps 3.91%% of her units at\n"
            "                   <=10 ms to our 0.20%%, because a flap or a stop\n"
            "                   closure really is that short. vb_build1.py\n"
            "                   used 2.0. A phone with fewer than 200 examples\n"
            "                   in the reference is left ungated\n"
            "  --dur-ceil-pct P   the same at the top. 99 catches the 160+160\n"
            "                   ms /ae/ cut from the county name \"Mahoning\"\n"
            "                   that made \"management\" -> \"mahonagement\";\n"
            "                   jill's own ae p99 is 142 ms. ⚠ p95 WAS TESTED\n"
            "                   AND REJECTED -- ours 4.64%% above jill's p95,\n"
            "                   jill herself 4.87%%. The tail is where the\n"
            "                   audible ones are\n"
            "  --dur-ref-vin F  read those percentiles from F instead of the\n"
            "                   template. Phones are matched BY NAME, since\n"
            "                   phone_center is in LABL space and two voices\n"
            "                   need not agree on that order\n"
            "                   ⚠ -ms and -pct are two different bounds for\n"
            "                   the same side; passing both is refused\n"
            "  --k-best N       S4 left partners per right unit (default 12;\n"
            "                   vendor mean row is 10.1 tom / 10.5 jill)\n"
            "  --join-mode const|cost   default const, which is what the\n"
            "                   measurements in SPEC_S4_hash.md say to ship\n"
            "  --const-cost X   the constant (default 1.0)\n"
            "  --no-join        stop after S7; leaves the template's hash\n"
            "  --s4-only        rebuild ONLY the join table of an existing\n"
            "                   --out-dir/--voice pair; nothing else moves, so\n"
            "                   a K or cost change is a single-variable test\n"
            "  --copyright S    LIST/ICOP. Defaults to a neutral line; the\n"
            "                   template's claims SpeechWorks 2003 and must\n"
            "                   NOT be carried into a voice of your own\n"
            "  --version-string S   LIST/vers (default 3.0.0.0, as jill)\n"
            "  --unit-version V  100006 or 100008; default FOLLOWS THE\n"
            "                   TEMPLATE. v100008 is the same record plus a\n"
            "                   phoneInSyl byte at 0x10. It matters because the\n"
            "                   VCF ships with the template: jill sets\n"
            "                   PHONE_IN_SYL_MISMATCH_COST=.3 and stores the\n"
            "                   column, tom sets 0 and has none. Writing\n"
            "                   v100006 under jill's VCF leaves that weight\n"
            "                   live over a constant 6, which cannot\n"
            "                   discriminate. The `.sp` sidecars have carried\n"
            "                   the value all along\n"
            "  --syn-anchors S  which anchors SYNTHETIC recordings may supply.\n"
            "                   none  no anchors from them at all\n"
            "                   word  whole-word anchors only (DEFAULT)\n"
            "                   both  words and syllables\n"
            "                   A word anchor plays a whole word from one\n"
            "                   render. A SYLLABLE anchor plays a fragment the\n"
            "                   engine splices into a DIFFERENT word: \"body\"\n"
            "                   came out of a render saying \"...a sturdy\n"
            "                   building...\", and was heard as that one word\n"
            "                   changing accent. `rvc_*` word anchors are\n"
            "                   separately suppressed by --rvc-anchors, whose\n"
            "                   concern is the TEXT rather than the fragment\n"
            "  --limit N        first N recordings, for a smoke run\n");
        return 2;
    }
    /* A vendor tree is not a build target. */
    if (!strcmp(voice, "tom") || !strcmp(voice, "jill") ||
        !strcmp(voice, "mara") || !strcmp(voice, "craig") ||
        !strcmp(voice, "donna")) {
        fprintf(stderr, "refusing: build into a NEW name, never a vendor voice\n");
        return 1;
    }
    /* ⚠ CHECKED BEFORE ANY WORK. A floor at or above the ceiling withholds
     * EVERYTHING except units of exactly that length; refusing here costs
     * nothing, refusing after S1 wastes the whole corpus pass. */
    if (dur_floor_ms > 0 && dur_ceil_ms > 0 && dur_floor_ms >= dur_ceil_ms) {
        fprintf(stderr, "refusing: --dur-floor-ms %d >= --dur-ceil-ms %d leaves "
                "almost nothing admissible\n", dur_floor_ms, dur_ceil_ms);
        return 2;
    }
    if (dur_floor_pct > 0.0 && dur_ceil_pct > 0.0 &&
        dur_floor_pct >= dur_ceil_pct) {
        fprintf(stderr, "refusing: --dur-floor-pct %.4g >= --dur-ceil-pct %.4g "
                "leaves almost nothing admissible\n", dur_floor_pct, dur_ceil_pct);
        return 2;
    }
    if (dur_floor_pct < 0.0 || dur_floor_pct > 100.0 ||
        dur_ceil_pct < 0.0 || dur_ceil_pct > 100.0) {
        fprintf(stderr, "refusing: duration percentiles must be in [0,100]\n");
        return 2;
    }
    /* ⚠ ONE PROVENANCE PER SIDE. Given both, whichever bound is tighter wins
     * per phone and the log names the other -- so a build reports a floor it
     * did not apply, which is the shape of the "0 withheld while every
     * converted unit stayed live" failure. Refuse instead of picking. */
    if (dur_floor_ms > 0 && dur_floor_pct > 0.0) {
        fprintf(stderr, "refusing: --dur-floor-ms and --dur-floor-pct are two "
                "different floors; pass one\n");
        return 2;
    }
    if (dur_ceil_ms > 0 && dur_ceil_pct > 0.0) {
        fprintf(stderr, "refusing: --dur-ceil-ms and --dur-ceil-pct are two "
                "different ceilings; pass one\n");
        return 2;
    }
    /* A reference with nothing reading it is a flag that looks applied. */
    if (dur_ref_vin && dur_floor_pct <= 0.0 && dur_ceil_pct <= 0.0) {
        fprintf(stderr, "refusing: --dur-ref-vin has no effect without "
                "--dur-floor-pct or --dur-ceil-pct\n");
        return 2;
    }
    if (strcmp(syn_anchors, "none") && strcmp(syn_anchors, "word") &&
        strcmp(syn_anchors, "both")) {
        fprintf(stderr, "unknown --syn-anchors %s (none|word|both)\n",
                syn_anchors);
        return 2;
    }
    if (unit_version && !spfy_vb_unit_stride((uint32_t)unit_version)) {
        fprintf(stderr, "unknown --unit-version %d (100006|100008)\n",
                unit_version);
        return 2;
    }
    if (strcmp(rvc_policy, "none") && strcmp(rvc_policy, "prefer-real") &&
        strcmp(rvc_policy, "equal")) {
        fprintf(stderr, "unknown --rvc-policy %s (none|prefer-real|equal)\n",
                rvc_policy);
        return 2;
    }

    int rc;
    spfy_vb_join_cfg jc;
    memset(&jc, 0, sizeof jc);
    jc.k_best = (uint32_t)(k_best > 0 ? k_best : 12);
    jc.k_per_key = (uint32_t)(k_per_key > 0 ? k_per_key : 0);
    /* Render-only f0 must not re-cost the join table; see edge_frames.h. */
    jc.zero_f0_dim = f0_render_only;
    jc.mode = join_const ? SPFY_VB_JC_CONST : SPFY_VB_JC_COST;
    jc.const_cost = (float)const_cost;
    jc.join_w = 1.0f;
    jc.join_off = 0.0f;
    jc.sample_rate = 0u;             /* 0 = take it from the VDB's own fmt */

    if (s4_only) {
        char vinp[1024], vdbp[1024];
        snprintf(vinp, sizeof vinp, "%s/%s.vin", out_dir, voice);
        snprintf(vdbp, sizeof vdbp, "%s/%s8.vdb", out_dir, voice);
        printf("=== S4 JOIN (only) ===\n");
        printf("  %s\n  k_best %u, mode %s, const %.4f\n", vinp, jc.k_best,
               join_const ? "const" : "cost", const_cost);
        return run_s4(vinp, vdbp, &jc) == SPFY_OK ? 0 : 1;
    }

    spfy_vb_template tmpl;
    spfy_vb_corpus   corp;
    memset(&corp, 0, sizeof corp);

    printf("=== template ===\n");
    if (no_template) {
        rc = spfy_vb_template_new(&tmpl, (uint32_t)unit_version);
        if (rc != SPFY_OK) { fprintf(stderr, "template_new %d\n", rc); return 1; }
    } else {
        rc = spfy_vb_template_load(tmpl_vin, tmpl_vdb, &tmpl);
        if (rc != SPFY_OK) { fprintf(stderr, "template load %d\n", rc); return 1; }
    }
    if (f0q_slope != 0.0) {
        tmpl.f0q_user_slope = f0q_slope;
        tmpl.f0q_user_off   = f0q_off;
        tmpl.f0q_user       = 1;
    }
    printf("  %zu phones in feat[name], pau feat id %d\n",
           tmpl.pidx.n, tmpl.pau_feat);
    if (no_template) {
        /* ⚠ NOT a fit and must not print an R² for one. The constants are the
         * midpoint of the two vendors' own fits (jill 47.2220/-51.7121 at
         * R²=0.9147, tom 48.6851/-57.3633 at R²=0.9374), which agree to about
         * one byte across 20-150 ms -- that agreement is the evidence, not a
         * regression we ran here. */
        printf("  f0_context encoding: %.3f*log(dur+1) + %.3f  (the vendors' "
               "own fits agree to ~1 byte over 20-150 ms; not fitted here)\n",
               tmpl.f0ctx_a, tmpl.f0ctx_b);
    } else {
        printf("  f0_context fit: %.3f*log(dur+1) + %.3f   R^2 = %.4f\n",
               tmpl.f0ctx_a, tmpl.f0ctx_b, tmpl.f0ctx_r2);
        if (tmpl.f0ctx_r2 < 0.25)
            printf("  ⚠ weak fit -- f0_context is largely NOT explained by "
                   "duration; the written value is a placeholder\n");
    }

    /* ⚠ SAMPLE THE F0 TRACKS EVEN WHEN THEY WILL NOT BE WRITTEN. Feeding the
     * quantiser nothing makes it fall back to the identity map and store raw
     * Hz in a field whose template scale is median 118 +/- 6.3; the f0tr and
     * the difference curve then index out of range and clamp, and ASR word
     * error went 18.9% -> 56.4% with every summary check unchanged. */
    if (f0_calibrated || f0_render_only) {
        char **stems = NULL;
        size_t n_stems = 0;
        spfy_vb_list_stems(wav_dir, &stems, &n_stems);
        size_t want = n_stems < 400u ? n_stems : 400u;
        uint8_t **trk = (uint8_t **)calloc(want ? want : 1u, sizeof *trk);
        size_t *tn = (size_t *)calloc(want ? want : 1u, sizeof *tn);
        size_t got = 0;
        for (size_t i = 0; i < want && trk && tn; ++i) {
            char p[1024];
            snprintf(p, sizeof p, "%s/%s.f0", wav_dir, stems[i]);
            if (spfy_vb_read_bytes(p, &trk[got], &tn[got]) == SPFY_OK) ++got;
        }
        if (!got)
            fprintf(stderr, "  ⚠ NO .f0 TRACKS FOUND -- f0 fields will be "
                            "uncalibrated. Run vb_f0.py over the audio dir.\n");
        spfy_vb_template_fit_f0(&tmpl, (const uint8_t *const *)trk, tn, got);
        for (size_t i = 0; i < got; ++i) free(trk[i]);
        free(trk); free(tn);
        spfy_vb_free_stems(stems, n_stems);
    }

    stage("S1 CORPUS");
    spfy_vb_corpus_cfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.wav_dir = wav_dir;
    cfg.tg_dir = tg_dir;
    cfg.seg_dir = seg_dir;
    cfg.drop_path = drop_path;
    cfg.drop_chunks_path = drop_chunks;
    cfg.compress_path = compress_path;
    cfg.rvc_anchors_drop = rvc_anchors_drop;
    cfg.f0_calibrated = f0_calibrated;
    cfg.f0_render_only = f0_render_only;
    cfg.trim_silence = trim_silence;
    cfg.limit = limit;
    cfg.level_target = level_target;
    cfg.level_max_gain = level_max_gain > 0.0 ? level_max_gain : 4.0;
    cfg.level_peak_dbfs = level_peak;
    /* Echo it. A level pass that silently did not run reports nothing at all,
     * which is indistinguishable from a build that was never asked to level. */
    printf("  level pass: %s (target %.2f dBFS, peak ceiling %.2f, "
           "max gain %.2fx)\n",
           cfg.level_target != 0.0 ? "ON" : "off",
           cfg.level_target, cfg.level_peak_dbfs, cfg.level_max_gain);
    rc = spfy_vb_corpus_build(&tmpl, &cfg, &corp);
    if (rc != SPFY_OK) { fprintf(stderr, "corpus %d\n", rc); goto fail; }

    printf("  %zu indx entries (%zu recordings + %zu splits, of which %zu at "
           "gaps > %d ms), %zu bytes of u-law\n",
           corp.n_indx - 1u, corp.n_indx - 1u - corp.n_chunk_extra,
           corp.n_chunk_extra, corp.n_gap_split, SPFY_VB_GAP_SPLIT_MS,
           corp.n_data);
    printf("  %zu half-phone units   (skipped %zu unalignable, %zu "
           "unknown-phone, %zu over-long; %zu spans trimmed)\n",
           corp.n_units, corp.n_skip_align, corp.n_skip_phone,
           corp.n_skip_long, corp.n_skip_silent);
    if (corp.n_syllabic_merged)
        printf("  ⭐ %zu syllabic(s) given back the schwa the aligner left "
               "unpaired beside them (el/en; english_us_arpa has no such "
               "phone, so the unit was the consonant alone)\n",
               corp.n_syllabic_merged);
    if (corp.n_fe_mismatch)
        printf("⛔ %zu recording(s) had a `.fe` that does NOT describe the same "
               "utterance as their `.seg`. Units kept, ANCHORS refused.\n",
               corp.n_fe_mismatch);
    /* ⚠ SIDECAR COMPLETENESS. Each of these used to be read with its return
     * value discarded, so an incomplete ingest built silently: a stem with no
     * `.fe` contributed unlabelled units, one with no TextGrid vanished from
     * the corpus entirely, and the log looked exactly like a clean run. */
    if (corp.n_no_tg || corp.n_no_fe || corp.n_no_sp || corp.n_no_f0) {
        printf("  ⚠ INCOMPLETE SIDECARS -- a partial ingest, not a clean "
               "corpus:\n");
        if (corp.n_no_tg)
            printf("      %zu wav(s) DROPPED: no .TextGrid and no .seg\n",
                   corp.n_no_tg);
        if (corp.n_no_fe)
            printf("      %zu of %zu used had no .fe (phone truth; units from "
                   "these are unlabelled)\n", corp.n_no_fe, corp.n_used);
        if (corp.n_no_sp)
            printf("      %zu had no .sp (prosody slots collapse to a "
                   "constant)\n", corp.n_no_sp);
        if (corp.n_no_f0)
            printf("      %zu had no .f0\n", corp.n_no_f0);
        printf("      generate with vb_fecap.py / vb_spcap.py / vb_f0.py, "
               "then MFA\n");
    } else {
        printf("  ✅ every used recording has .fe .sp .f0 and an alignment\n");
    }
    if (corp.n_end_drop)
        printf("  ⛔ %zu HALF-PHONE(S) NOT EMITTED at the recording end -- a "
               "dropped half shifts every later ckls span in that recording "
               "by one unit\n", corp.n_end_drop);
    if (corp.n_end_clamp)
        printf("  ⭐ %zu unit extent(s) clamped to the recording end -- real "
               "Speechify REFUSES the whole DB for one sample over\n",
               corp.n_end_clamp);
    /* ⚠ SAY HOW MANY RECORDINGS ACTUALLY USED THE `.seg`. A --seg-dir that
     * points somewhere wrong does not error: every stem simply falls back to
     * its TextGrid and the build reports a normal unit count. This is the only
     * line that separates "engine boundaries" from "the aligner's". */
    if (seg_dir) {
        printf("  ⭐ %zu of %zu recordings used the ENGINE's own segmentation",
               corp.n_seg_rec, corp.n_used);
        if (corp.n_seg_bad)
            printf("; %zu phone(s) dropped on a half-pair mismatch",
                   corp.n_seg_bad);
        printf("\n");
        if (!corp.n_seg_rec)
            printf("  ⛔ NONE did -- check --seg-dir; every unit here carries "
                   "aligner boundaries, not the engine's\n");
    }
    /* ⚠ REPORT THE CONVERTED SHARE EVEN WHEN IT IS ZERO. "we folded the RVC
     * corpus in" and "the build contains no converted material" have looked
     * identical from a build log before. */
    printf("  converted (rvc_*): %zu of %zu recordings, %zu of %zu units "
           "(%.1f%%)\n", corp.n_rvc_recs, corp.n_used, corp.n_rvc_units,
           corp.n_units,
           100.0 * (double)corp.n_rvc_units / (double)corp.n_units);
    if (corp.n_rvc_anchor_sup)
        printf("  ⛔ %zu anchor(s) suppressed for `rvc_*` recordings "
               "(--rvc-anchors drop); their UNITS are kept\n",
               corp.n_rvc_anchor_sup);
    else if (corp.n_rvc_recs && !rvc_anchors_drop)
        printf("  ⚠ --rvc-anchors keep: converted _WORD_ records match the FE "
               "54.53%% of the time and 92 of 792 recordings are wrong "
               "wholesale\n");
    /* ⭐ THE COUNT THAT DID NOT EXIST. Only the SUPPRESSED total was ever
     * printed, so a rule that silently swallowed 35,976 `st2_*` anchors --
     * in a corpus with zero `rvc_*` files -- looked exactly like a rule doing
     * its job. */
    if (corp.n_syn_anchor_kept)
        printf("  ⭐ %zu anchor(s) KEPT from synthetic recordings whose text "
               "is their own render input (`st2_*`)\n",
               corp.n_syn_anchor_kept);
    if (corp.n_f0_rescued)
        printf("  ⭐ %zu f0 byte(s) taken from the unit's own voiced frames "
               "instead of reading unvoiced off one empty frame (the zero is "
               "the DP's voicing bit, not a missing value)\n",
               corp.n_f0_rescued);
    if (corp.n_leveled)
        printf("  level: %zu recordings scaled, mean %+.2f dB, %zu clipped "
               "samples of %zu (%.4f%%)\n",
               corp.n_leveled, corp.level_gain_db / (double)corp.n_leveled,
               corp.n_clip, corp.n_samp_level,
               100.0 * (double)corp.n_clip
                     / (double)(corp.n_samp_level ? corp.n_samp_level : 1));
    if (corp.n_level_peaklim)
        printf("  level: %zu of %zu held back by their own peak, mean "
               "shortfall %.2f dB (this is why the median lands under the "
               "target)\n",
               corp.n_level_peaklim, corp.n_leveled,
               corp.level_short_db / (double)corp.n_level_peaklim);
    if (corp.n_lp_over) {
        fprintf(stderr, "refusing: %zu units exceed the u16 local_pos limit "
                        "after chunking at %d ms\n",
                corp.n_lp_over, SPFY_VB_CHUNK_MS);
        rc = SPFY_E_INVAL;
        goto fail;
    }
    if (corp.n_indx - 1u > 65535u) {
        fprintf(stderr, "refusing: %zu indx entries exceeds the u16 file_idx "
                        "limit\n", corp.n_indx - 1u);
        rc = SPFY_E_INVAL;
        goto fail;
    }
    if (!corp.n_units) { fprintf(stderr, "nothing built\n"); rc = SPFY_E_INVAL; goto fail; }
    printf("  sp targets from the engine: %zu/%zu units (%.1f%%)\n",
           corp.n_sp, corp.n_units,
           100.0 * (double)corp.n_sp / (double)corp.n_units);
    /* ⚠ COMPARE TO THE SHIPPED VOICES, NOT TO A THRESHOLD. 50% means the
     * field reverted to marking phone starts; 0% means the FE gave us no
     * syllable spans and the engine's D-target can never advance. */
    printf("  syllable starts (unit +0x15): %zu (%.2f%%)   [tom 19.65, jill 19.93]\n",
           corp.n_first_half,
           100.0 * (double)corp.n_first_half / (double)corp.n_units);
    if (corp.n_ctx_seen)
        printf("  ctx cross-check: %zu/%zu agree with our prsl key (%.2f%%)\n",
               corp.n_ctx_seen - corp.n_ctx_bad, corp.n_ctx_seen,
               100.0 * (double)(corp.n_ctx_seen - corp.n_ctx_bad)
                     / (double)corp.n_ctx_seen);

    /* Names are the engine's key into the VDB (feat.filename -> indx binary
     * search), so a duplicate would silently point two entries at one offset. */
    {
        char **nm = (char **)malloc((corp.n_indx - 1u) * sizeof *nm);
        if (!nm) { rc = SPFY_E_NOMEM; goto fail; }
        size_t nn = corp.n_indx - 1u;
        for (size_t i = 0; i < nn; ++i) nm[i] = corp.indx[i].name;
        qsort(nm, nn, sizeof *nm, cmp_name);
        size_t dups = 0;
        for (size_t i = 1; i < nn; ++i) if (!strcmp(nm[i], nm[i - 1u])) ++dups;
        free(nm);
        if (dups) {
            fprintf(stderr, "refusing: %zu duplicate recording names in indx\n", dups);
            rc = SPFY_E_INVAL;
            goto fail;
        }
    }

    char path[1024];

    /* The container's own identity. ⚠ The template's LIST reads "Copyright
     * 2003 SpeechWorks International, Inc."; carrying that into a voice built
     * from someone else's recordings is a false claim that nothing in the
     * pipeline would ever flag. */
    char today[16];
    {
        time_t t = time(NULL);
        struct tm *g = gmtime(&t);
        if (!g || !strftime(today, sizeof today, "%Y-%m-%d", g))
            snprintf(today, sizeof today, "1970-01-01");
    }
    if (!copyright) copyright = "Built with spfy_vb_build. Recordings remain "
                                "the property of their owner.";

    /* ---- S7 VDB ---- */
    stage("S7 PACK (vdb)");
    {
        uint8_t *indx = NULL;
        size_t indx_n = 0;
        rc = spfy_vb_encode_indx(corp.indx, corp.n_indx, &indx, &indx_n);
        if (rc != SPFY_OK) goto fail;
        rc = spfy_vb_riff_set(&tmpl.vdb, "indx", indx, indx_n);
        if (rc != SPFY_OK) { free(indx); goto fail; }
        uint8_t *data = corp.data;
        size_t data_n = corp.n_data;
        corp.data = NULL;                 /* the riff owns it now */
        corp.n_data = 0;
        rc = spfy_vb_riff_set(&tmpl.vdb, "data", data, data_n);
        if (rc != SPFY_OK) { free(data); goto fail; }

        uint8_t *fmtc = NULL;
        size_t fmt_n = 0;
        rc = spfy_vb_encode_fmt(corp.sample_rate, 1u, &fmtc, &fmt_n);
        if (rc != SPFY_OK) goto fail;
        rc = spfy_vb_riff_set(&tmpl.vdb, "fmt ", fmtc, fmt_n);
        if (rc != SPFY_OK) { free(fmtc); goto fail; }
        uint8_t *lst = NULL;
        size_t lst_n = 0;
        rc = spfy_vb_encode_list(copyright, today, &lst, &lst_n);
        if (rc != SPFY_OK) goto fail;
        rc = spfy_vb_riff_set(&tmpl.vdb, "LIST", lst, lst_n);
        if (rc != SPFY_OK) { free(lst); goto fail; }
        snprintf(path, sizeof path, "%s/%s8.vdb", out_dir, voice);
        rc = spfy_vb_riff_save(&tmpl.vdb, path);
        if (rc != SPFY_OK) { fprintf(stderr, "vdb write %d\n", rc); goto fail; }
        printf("  wrote %s\n", path);
    }

    /* ---- S2 FEATURES ---- */
    stage("S2 FEATURES");
    {
        /* Under --no-template the container's `feat` is still empty, so the
         * sections come from the embedded language table instead. Both paths
         * then get the same treatment: keep every section but `filename`,
         * write ours. */
        const uint8_t *src_feat;
        size_t src_feat_n;
        if (no_template) {
            src_feat = spfy_vb_lang_feat(&src_feat_n);
        } else {
            const spfy_vb_chunk *tf = spfy_vb_riff_get(&tmpl.vin, "feat");
            if (!tf) { rc = SPFY_E_FORMAT; goto fail; }
            src_feat = tf->data;
            src_feat_n = tf->n;
        }
        char **names = (char **)malloc((corp.n_indx - 1u) * sizeof *names);
        if (!names) { rc = SPFY_E_NOMEM; goto fail; }
        for (size_t i = 0; i + 1u < corp.n_indx; ++i) names[i] = corp.indx[i].name;
        uint8_t *feat = NULL;
        size_t feat_n = 0;
        rc = spfy_vb_build_feat(src_feat, src_feat_n, names, corp.n_indx - 1u,
                                &feat, &feat_n);
        free(names);
        if (rc != SPFY_OK) goto fail;
        rc = spfy_vb_riff_set(&tmpl.vin, "feat", feat, feat_n);
        if (rc != SPFY_OK) { free(feat); goto fail; }
        printf("  feat %zu B, %zu filenames\n", feat_n, corp.n_indx - 1u);

        /* ⭐ THE RECORD VERSION FOLLOWS THE TEMPLATE UNLESS TOLD OTHERWISE.
         * The VCF ships with the template, and the two have to agree: jill
         * prices phoneInSyl at .3 and stores it (v100008); tom prices it at 0
         * and has no column (v100006). Writing tom's record under jill's VCF
         * leaves that weight live over a constant 6 -- a cost that cannot
         * discriminate. */
        uint32_t uver = unit_version ? (uint32_t)unit_version : tmpl.unit_ver;
        size_t ustride = spfy_vb_unit_stride(uver);
        if (!ustride) {
            fprintf(stderr, "refusing: cannot WRITE unit version %u "
                    "(mapped: 100006, 100008)\n", uver);
            rc = SPFY_E_FORMAT;
            goto fail;
        }
        spfy_vb_buf ub = {0};
        rc = spfy_vb_buf_put(&ub, "vers", 4);
        if (rc == SPFY_OK) rc = spfy_vb_buf_u32(&ub, 4);
        if (rc == SPFY_OK) rc = spfy_vb_buf_u32(&ub, uver);
        if (rc == SPFY_OK) rc = spfy_vb_buf_put(&ub, "data", 4);
        if (rc == SPFY_OK)
            rc = spfy_vb_buf_u32(&ub, (uint32_t)(corp.n_units * ustride));
        if (rc != SPFY_OK) { spfy_vb_buf_free(&ub); goto fail; }
        rc = spfy_vb_buf_reserve(&ub, corp.n_units * ustride);
        if (rc != SPFY_OK) { spfy_vb_buf_free(&ub); goto fail; }
        for (size_t i = 0; i < corp.n_units; ++i)
            spfy_vb_pack_unit_ver(&corp.units[i], uver, ub.p + ub.n + i * ustride);
        ub.n += corp.n_units * ustride;
        rc = spfy_vb_riff_set(&tmpl.vin, "unit", ub.p, ub.n);
        if (rc != SPFY_OK) { spfy_vb_buf_free(&ub); goto fail; }
        printf("  unit %zu B, %zu records at v%u stride %zu%s\n",
               ub.n, corp.n_units, uver, ustride,
               uver == tmpl.unit_ver ? " (template's)" : " (OVERRIDDEN)");
        /* ⚠ Reported either way. Silence here is how a discarded feature stays
         * discarded: the sidecars carried it all along. */
        if (uver == SPFY_VB_UNIT_V8_VERSION)
            printf("  phoneInSyl: %zu of %zu units carry a real class; the "
                   "rest default to %u (SyllUnknown)\n",
                   corp.n_sp_phone_in_syl, corp.n_units,
                   SPFY_VB_PHONE_IN_SYL_UNKNOWN);
        else
            printf("  ⚠ phoneInSyl NOT WRITTEN (v%u has no column) -- %zu units "
                   "had a real class in their `.sp` and every candidate will "
                   "read as %u\n", uver, corp.n_sp_phone_in_syl,
                   SPFY_VB_PHONE_IN_SYL_UNKNOWN);

        const spfy_vb_chunk *tc = spfy_vb_riff_get(&tmpl.vin, "cnts");
        const spfy_vb_chunk *fc = spfy_vb_riff_get(&tmpl.vin, "feat");
        if (tc) {
            uint8_t *cn = (uint8_t *)malloc(12);
            if (!cn) { rc = SPFY_E_NOMEM; goto fail; }
            /* All three words are derived below; the donor's first eight
             * bytes are only a starting point when there IS a donor. */
            memset(cn, 0, 12);
            if (tc->n >= 12) memcpy(cn, tc->data, 8);
            /* ⛔ DERIVE cnts[0..1], DO NOT INHERIT THEM. They are feat["name"]'s
             * entry count and the feat key count; carrying the donor's bytes
             * makes them silently wrong the moment the schema moves, and real
             * Speechify validates cnts against what it loaded. */
            uint32_t n_name = 0, n_keys = 0;
            if (fc && spfy_vb_feat_counts(fc->data, fc->n, &n_name, &n_keys)
                      == SPFY_OK && n_name && n_keys) {
                uint32_t w[2] = { n_name, n_keys };
                for (int i = 0; i < 2; ++i) {
                    cn[i * 4 + 0] = (uint8_t)(w[i] & 0xFFu);
                    cn[i * 4 + 1] = (uint8_t)((w[i] >> 8) & 0xFFu);
                    cn[i * 4 + 2] = (uint8_t)((w[i] >> 16) & 0xFFu);
                    cn[i * 4 + 3] = (uint8_t)((w[i] >> 24) & 0xFFu);
                }
                /* ⛔ GUARD ON tc->n, NOT ON tc. Under --no-template the chunk
                 * exists as a placeholder with data == NULL, and memcmp
                 * against it segfaults inside msvcrt with the stdout buffer
                 * unflushed -- so the log simply stops mid-line and says
                 * nothing about where. */
                if (tc->n >= 8 && memcmp(cn, tc->data, 8))
                    printf("  cnts: %u classes / %u features derived from our "
                           "feat (donor said %u / %u)\n", n_name, n_keys,
                           rd_le_u32(tc->data), rd_le_u32(tc->data + 4));
                else if (tc->n < 8)
                    printf("  cnts: %u classes / %u features derived from our "
                           "feat (no donor)\n", n_name, n_keys);
            }
            uint32_t v = (uint32_t)corp.n_units;
            cn[8] = (uint8_t)(v & 0xFFu);
            cn[9] = (uint8_t)((v >> 8) & 0xFFu);
            cn[10] = (uint8_t)((v >> 16) & 0xFFu);
            cn[11] = (uint8_t)((v >> 24) & 0xFFu);
            rc = spfy_vb_riff_set(&tmpl.vin, "cnts", cn, 12);
            if (rc != SPFY_OK) { free(cn); goto fail; }
        } else {
            fprintf(stderr, "cnts: the container has no such chunk -- the "
                            "engine rejects a voice without a unit count\n");
            rc = SPFY_E_FORMAT;
            goto fail;
        }
    }

    /* ---- S3 NORM ---- */
    stage("S3 NORM");
    {
        uint8_t *mean = NULL;
        size_t mean_n = 0, live = 0, have = 0;
        rc = spfy_vb_s3_norm(&corp, &tmpl.labl, &mean, &mean_n, &live, &have);
        if (rc != SPFY_OK) goto fail;
        rc = spfy_vb_riff_set(&tmpl.vin, "mean", mean, mean_n);
        if (rc != SPFY_OK) { free(mean); goto fail; }
        printf("  audio stats for %zu units (%.1f%%); %zu populated classes\n",
               have, 100.0 * (double)have / (double)corp.n_units, live);
    }

    /* ---- S6 TREES ---- */
    stage("S6 TREES");
    {
        /* LABL index -> phone name, for the trees' own `labl` and for the
         * LEFTlabel/RIGHTlabel question values. phone_center is in LABL
         * space; pidx is in FEAT order, and the two are permutations. */
        static char tree_labels[256][8];
        size_t n_tree_labels = 0;
        for (size_t i = 0; i < 256u; ++i) {
            int f = tmpl.labl.l2f[i];
            if (f < 0 || (size_t)f >= tmpl.pidx.n) continue;
            size_t ln = strlen(tmpl.pidx.name[f]);
            if (ln > 7) ln = 7;
            memcpy(tree_labels[i], tmpl.pidx.name[f], ln);
            tree_labels[i][ln] = 0;
            if (i + 1u > n_tree_labels) n_tree_labels = i + 1u;
        }

        if (own_trees) {
            uint8_t *durt = NULL;
            size_t durt_n = 0;
            spfy_vb_treestat ts;
            rc = spfy_vb_s6_durt_grow(&corp, tree_labels, n_tree_labels,
                                      (uint32_t)tree_min_cluster,
                                      &durt, &durt_n, &ts);
            if (rc != SPFY_OK) goto fail;
            rc = spfy_vb_riff_set(&tmpl.vin, "durt", durt, durt_n);
            if (rc != SPFY_OK) { free(durt); goto fail; }
            printf("  ⭐ durt GROWN: %zu labels, %zu questions, %zu nodes, "
                   "%zu leaves from %zu units (%zu label(s) with none), "
                   "%zu B\n",
                   n_tree_labels, ts.n_questions, ts.n_nodes, ts.n_leaves,
                   ts.n_samples, ts.n_empty, durt_n);
        } else {
        const spfy_vb_chunk *td = spfy_vb_riff_get(&tmpl.vin, "durt");
        if (!td) { rc = SPFY_E_FORMAT; goto fail; }
        uint8_t *durt = NULL;
        size_t durt_n = 0, rec = 0, kept = 0;
        rc = spfy_vb_s6_durt(&corp, td->data, td->n, &durt, &durt_n, &rec, &kept);
        if (rc != SPFY_OK) goto fail;
        rc = spfy_vb_riff_set(&tmpl.vin, "durt", durt, durt_n);
        if (rc != SPFY_OK) { free(durt); goto fail; }
        printf("  durt leaves: %zu recomputed from our f0_context, %zu kept "
               "(fewer than %d samples)\n", rec, kept, SPFY_VB_TREE_MIN_SAMPLES);
        }

        /* ⛔ f0tr ONLY WITH REAL F0, AND IT IS NOT OPTIONAL THEN.
         * With f0_start == 0 the engine charges every candidate the same flat
         * w_f0_miss and never consults the tree, so regenerating it would be
         * a no-op. With real F0 and the TEMPLATE's leaves the absolute cost
         * scores our units against the TEMPLATE SPEAKER's pitch contours --
         * measured at 82.3% of picks moved on one sentence, heard as worse.
         * So the two settings travel together. */
        /* ⭐ Under --own-trees f0tr is generated in EVERY f0 mode, not just
         * the calibrated ones. With `--f0 absent` the engine takes the flat
         * w_f0_miss branch and never walks it, so the tree is inert either
         * way -- but an inherited one leaves the donor's pitch contours in
         * the file, and --no-template has no donor to inherit from. */
        if (f0_calibrated || f0_render_only || own_trees) {
            uint8_t *f0tr = NULL;
            size_t f0tr_n = 0, frec = 0, fkept = 0, fused = 0;
            if (own_trees) {
                spfy_vb_treestat ts;
                rc = spfy_vb_s6_f0tr_grow(&corp, tree_labels, n_tree_labels,
                                          (uint32_t)tree_min_cluster,
                                          &f0tr, &f0tr_n, &ts);
                if (rc != SPFY_OK) goto fail;
                frec = ts.n_leaves;
                fused = ts.n_samples;
                printf("  ⭐ f0tr GROWN: %zu questions, %zu nodes, %zu leaves "
                       "from %zu voiced units, %zu B\n",
                       ts.n_questions, ts.n_nodes, ts.n_leaves,
                       ts.n_samples, f0tr_n);
            } else {
            const spfy_vb_chunk *tf = spfy_vb_riff_get(&tmpl.vin, "f0tr");
            if (!tf) { rc = SPFY_E_FORMAT; goto fail; }
            rc = spfy_vb_s6_f0tr(&corp, tf->data, tf->n, &f0tr, &f0tr_n,
                                 &frec, &fkept, &fused);
            if (rc != SPFY_OK) goto fail;
            }
            size_t fzero = 0;
            if (f0_join_only || f0_render_only) {
                rc = spfy_vb_f0tr_zero_var(f0tr, f0tr_n, &fzero);
                if (rc != SPFY_OK) { free(f0tr); goto fail; }
            }
            rc = spfy_vb_riff_set(&tmpl.vin, "f0tr", f0tr, f0tr_n);
            if (rc != SPFY_OK) { free(f0tr); goto fail; }
            if (!own_trees)
                printf("  f0tr leaves: %zu recomputed from our f0_start, %zu "
                       "kept (fewer than %d samples); %zu units contributed\n",
                       frec, fkept, SPFY_VB_TREE_MIN_SAMPLES, fused);
            if (f0_join_only)
                printf("  ⭐ --f0 joinonly: %zu f0tr leaf variance(s) zeroed. "
                       "The f0 TARGET cost is now identically 0 for every "
                       "candidate; f0_start and the `hist` JOIN cost are "
                       "untouched\n", fzero);
            if (!fused)
                printf("  ⚠ NO unit had a non-zero f0_start -- the pitch tree "
                       "%s\n", own_trees ? "is one inert leaf at mean 0"
                                         : "is still the template's");
            if (!own_trees)
                printf("  ⚠ durt/f0tr keep their TOPOLOGY\n");
        } else {
            printf("  ⚠ f0tr is still the template's and durt keeps its "
                   "TOPOLOGY. f0tr is not regenerated because --f0 absent "
                   "leaves f0_start 0, which makes the engine skip the tree\n");
        }

        uint8_t *hist = NULL;
        size_t hist_n = 0, obs = 0;
        uint32_t peak = 0;
        int mode = -1;
        rc = spfy_vb_s6_hist(&corp, &hist, &hist_n, &obs, &peak, &mode);
        if (rc != SPFY_OK) goto fail;
        rc = spfy_vb_riff_set(&tmpl.vin, "hist", hist, hist_n);
        if (rc != SPFY_OK) { free(hist); goto fail; }
        if (peak)
            printf("  hist: %zu natural joins past the gate, mode bin %d, "
                   "peak %u, floor %.4f\n", obs, mode, peak, log((double)peak));
        else
            printf("  hist: no F0 in the inventory -- flat curve written "
                   "(the engine's gate cannot fire, so it is never read)\n");
    }

    /* ---- S5 PRESEL ---- */
    stage("S5 PRESEL");
    {
        spfy_vb_group *exact = NULL, *all = NULL;
        size_t n_exact = 0, n_all = 0;

        /* ⭐ ADMIT CONVERTED UNITS ONLY FOR THE PHONES SHE DOES NOT HAVE, AND
         * DO IT BEFORE prefer-real.
         *
         * ⚠⚠ THE ORDER IS NOT COSMETIC. Run after prefer-real this withheld
         * exactly ZERO units: prefer-real had already stripped converted units
         * out of every group holding real audio, so the only groups still
         * containing a blocked unit were groups where it was the ONLY unit,
         * and the never-empty guard kept all 1,794 of them. The log read
         * "0 withheld" while ~16,354 converted units stayed live across every
         * phone -- the configuration that measured 22.2%.
         *
         * prefer-real is per CONTEXT GROUP and that is too generous alone: a
         * converted /t/ still enters wherever some rare triphone of /t/ is
         * missing, which is thousands of slots she already covered through the
         * fallback chain. The gap that is REAL is per PHONE. */
        /* ⛔ WITHHOLD UNITS TOO SHORT TO BE HEARD.
         * Measured on crsmara: "you" was reported MISSING by ear and it was
         * not missing -- its four half-phones were 5, 5, 15, 15 ms, a 40 ms
         * word. A 5 ms half-phone is nothing to concatenate and it is what
         * leaves WSOLA stretching a slot it cannot fill.
         * The units stay in the TABLE; only their candidacy is removed, so a
         * slot falls through to the fallback chain instead of emitting silence.
         *
         * ⭐ TWO PROVENANCES, AND THE PERCENTILE ONE IS THE BETTER DESIGN. A
         * flat millisecond bound is wrong in both directions -- jill keeps
         * 3.91% of her units at <=10 ms against our 0.20%, because a flap or a
         * stop closure really is that short -- so whatever rejects a 5 ms `ae`
         * has to pass a 5 ms `dx`. --dur-floor-pct reads the bound off the
         * template's own units per phone, which makes it what a working
         * inventory contains rather than a number someone picked. */
        size_t n_short = 0, n_long = 0;
        double *pfloor = NULL, *pceil = NULL;
        if (dur_floor_pct > 0.0 || dur_ceil_pct > 0.0) {
            /* ⛔ NOT `dur_ref_vin ? dur_ref_vin : tmpl_vin`. Under
             * --no-template `tmpl_vin` is NULL, the percentile call fails, and
             * the error path fed NULL to a %s -- which mingw's printf does not
             * survive. It also hid the real point: reading the bound off a
             * SHIPPED VOICE made --dur-floor-pct a donor dependency. With no
             * reference the same statistic now comes from our own units. */
            const char *ref = dur_ref_vin ? dur_ref_vin : tmpl_vin;
            size_t np = 0, nf = 0, nc = 0;
            if (dur_floor_pct > 0.0) {
                pfloor = (double *)calloc(256, sizeof *pfloor);
                if (!pfloor) { rc = SPFY_E_NOMEM; goto fail; }
                rc = ref ? spfy_vb_dur_percentiles(&tmpl, ref, dur_floor_pct,
                                                   pfloor, 256, &nf, &np)
                         : spfy_vb_dur_percentiles_corpus(&corp, dur_floor_pct,
                                                          pfloor, 256, &nf, &np);
                if (rc != SPFY_OK) {
                    fprintf(stderr, "duration reference %s: rc=%d\n",
                            ref ? ref : "(our own corpus)", rc);
                    free(pfloor);
                    goto fail;
                }
            }
            if (dur_ceil_pct > 0.0) {
                pceil = (double *)calloc(256, sizeof *pceil);
                if (!pceil) { rc = SPFY_E_NOMEM; free(pfloor); goto fail; }
                rc = ref ? spfy_vb_dur_percentiles(&tmpl, ref, dur_ceil_pct,
                                                   pceil, 256, &nc, &np)
                         : spfy_vb_dur_percentiles_corpus(&corp, dur_ceil_pct,
                                                          pceil, 256, &nc, &np);
                if (rc != SPFY_OK) {
                    fprintf(stderr, "duration reference %s: rc=%d\n",
                            ref ? ref : "(our own corpus)", rc);
                    free(pfloor); free(pceil);
                    goto fail;
                }
            }
            printf("  duration reference: %s\n",
                   ref ? ref : "OUR OWN CORPUS (no donor)");
            /* ⚠ PRINTED PER PHONE, WITH THE EXTREMES NAMED. The entire claim
             * of this gate is that the bound VARIES by phone; a single summary
             * number would read exactly the same if the lookup silently
             * returned one value for everything. */
            if (pfloor) dump_pct_range(&tmpl.pidx, pfloor, dur_floor_pct,
                                       nf, np, "floor");
            if (pceil)  dump_pct_range(&tmpl.pidx, pceil, dur_ceil_pct,
                                       nc, np, "ceiling");
        }
        if (dur_floor_ms > 0 || dur_ceil_ms > 0 || pfloor || pceil) {
            if (!gated) {
                gated = (uint8_t *)calloc(corp.n_units, 1);
                if (!gated) { rc = SPFY_E_NOMEM; free(pfloor); free(pceil); goto fail; }
            }
            for (size_t i = 0; i < corp.n_units; ++i) {
                if (gated[i]) continue;
                double d = (double)corp.units[i].dur_like;
                uint8_t ph = corp.units[i].phone;
                /* 0 means "this phone has no bound", from either provenance:
                 * an unset flag, or a reference with fewer than
                 * SPFY_VB_DUR_PCT_MIN_N examples of that phone. */
                double lo = pfloor ? pfloor[ph] : (double)dur_floor_ms;
                double hi = pceil  ? pceil[ph]  : (double)dur_ceil_ms;
                if (lo > 0.0 && d < lo) {
                    gated[i] = 1; ++n_short;
                } else if (hi > 0.0 && d > hi) {
                    /* ⛔ A very long half-phone is the other half of the same
                     * defect: WSOLA has to COMPRESS it into the slot, and the
                     * table-level `n_skip_long` drop only catches the extreme
                     * tail. Withheld from candidacy, kept in the table. */
                    gated[i] = 1; ++n_long;
                }
            }
            if (dur_floor_ms > 0 || pfloor)
                printf("  duration floor: %zu of %zu units withheld (%.2f%%)\n",
                       n_short, corp.n_units,
                       100.0 * (double)n_short / (double)(corp.n_units ? corp.n_units : 1));
            if (dur_ceil_ms > 0 || pceil)
                printf("  duration ceiling: %zu of %zu units withheld (%.2f%%)\n",
                       n_long, corp.n_units,
                       100.0 * (double)n_long / (double)(corp.n_units ? corp.n_units : 1));
        }
        free(pfloor);
        free(pceil);

        size_t n_block = 0;
        if (corp.n_rvc_units) {
            uint8_t allow[256];
            int n_named = 0;
            if (!strcmp(rvc_policy, "none")) {
                memset(allow, 0, sizeof allow);
            } else if (rvc_phones) {
                n_named = phone_set(&tmpl.pidx, rvc_phones, allow, "--rvc-phones");
            } else {
                memset(allow, 1, sizeof allow);
            }
            if (!strcmp(rvc_policy, "none") || rvc_phones) {
                /* ⚠ Do NOT reallocate: --dur-floor-ms may already have gated
                 * units here, and a fresh calloc would silently discard them. */
                if (!gated) gated = (uint8_t *)calloc(corp.n_units, 1);
                if (!gated) { rc = SPFY_E_NOMEM; goto fail; }
                for (size_t i = 0; i < corp.n_units; ++i)
                    if (corp.units[i].is_rvc && !allow[corp.units[i].phone]) {
                        gated[i] = 1;
                        ++n_block;
                    }
            }
            if (!strcmp(rvc_policy, "none"))
                printf("  rvc-policy=none: all %zu converted units withheld "
                       "from preselection (the 14.3%% baseline)\n", n_block);
            else if (rvc_phones)
                printf("  rvc phone allow-list (%s, %d known): %zu converted "
                       "units are for phones she already has and are withheld\n",
                       rvc_phones, n_named, n_block);
        }

        /* ⭐⭐ WITHHOLD UNITS BY THE WORD THEY WERE CUT FROM.
         *
         * The build takes each unit's LABEL from the FE and its SPAN from the
         * aligner, and MFA's dictionary is BUILT FROM THE FE -- so where the
         * FE mispronounces a word, the aligner force-fits those phones onto
         * the audio and the two AGREE on a sound the recording does not have.
         * No label audit can see it. Proven: the FE reads `alki` as
         * `ae l k iy`, Alki Point is "al-KYE", and "keep" -- built from
         * k(cape) + k,iy,iy,p(ALKI) + p(pampa) -- comes out "kipe" with all
         * six units reading OK. Measured directly: those /iy/ units carry
         * F1 978/609/882 Hz where a real /iy/ sits at 360.
         *
         * ⚠ THE POINT IS THAT DETECTION IS NOT ENOUGH. `alki` occurs ONCE in
         * 54,512 word tokens, is absent from the gazetteer (Alki is a point,
         * not an incorporated city) and is spelled the FE's wrong way in
         * english_us_arpa.dict, so no lexicon or place-name filter finds it.
         * With 190 distinct recordings a unit does not need to be common to
         * be selected -- only to EXIST, because nothing competes for its
         * context. This gate is the standing defence for the ones never
         * found: withhold units cut from words the FE had no corroboration
         * for, and an undetected bad word cannot reach an ordinary one.
         *
         * Cost measured by tools/voicebuild/vb_wordrisk.py: the rare
         * non-English class is 0.65% of units and 1.75% of contexts.
         *
         * Uses the same `gated[]` path as --dur-floor-pct, so units keep
         * their uids -- renumbering would move picks all by itself through
         * the anchor scorer's uid arithmetic -- and are merely absent from
         * the preselection groups. corp.words carries (text, uid span), so
         * the mapping needs nothing from vb_corpus.c. */
        if (gate_words) {
            char **gw = NULL;
            size_t n_gw = 0;
            rc = word_list_load(gate_words, &gw, &n_gw);
            if (rc != SPFY_OK) {
                spfy_log_err("vb: cannot read --gate-words %s", gate_words);
                goto fail;
            }
            if (!gated) gated = (uint8_t *)calloc(corp.n_units, 1);
            if (!gated) { rc = SPFY_E_NOMEM; goto fail; }
            uint8_t *hit = (uint8_t *)calloc(n_gw ? n_gw : 1, 1);
            uint8_t *wg = (uint8_t *)calloc(corp.n_units ? corp.n_units : 1, 1);
            if (!hit || !wg) { rc = SPFY_E_NOMEM; free(hit); free(wg);
                               goto fail; }
            size_t n_gu = 0, n_gspan = 0;
            for (size_t k = 0; k < corp.words.n; ++k) {
                const spfy_vb_anchor *w = &corp.words.v[k];
                if (!w->text || !word_list_has(gw, n_gw, w->text, hit))
                    continue;
                ++n_gspan;
                for (uint32_t u = w->span_start;
                     u <= w->span_end && u < corp.n_units; ++u)
                    if (!gated[u]) { gated[u] = 1; wg[u] = 1; ++n_gu; }
            }
            /* ⭐⭐ NEVER GATE A UNIT THAT HAS NO REPLACEMENT.
             *
             * Rare words are the leak, but they are also where the RARE
             * CONTEXTS live -- measured, the rare non-English class is 0.65%
             * of units yet the sole source of 1.75% of all contexts. Gating
             * the class blind therefore buys safety with coverage, and this
             * corpus cannot afford coverage: 190 distinct recordings against
             * jill's 7,870.
             *
             * It does not have to. A unit only leaks when it WINS against
             * something else, so gating is worth doing exactly where an
             * alternative exists. Where none does, the unit is the only way
             * to say that context at all and withholding it just moves the
             * defect. So: after the word gate, any preselection key left with
             * zero survivors gets its lowest-uid word-gated unit back.
             *
             * Scoped to the word gate on purpose. --dur-floor-pct is allowed
             * to empty a key -- the wide groups are unions of the surviving
             * exact ones, so the slot falls through to real units of the
             * right phone -- and rescuing there would change arms already
             * measured. */
            size_t n_rescued = 0;
            if (gate_sole_keep && n_gu) {
                key_uid_g *kk = (key_uid_g *)malloc(
                    (corp.n_units ? corp.n_units : 1u) * sizeof *kk);
                if (!kk) { rc = SPFY_E_NOMEM; free(hit); goto fail; }
                for (size_t i = 0; i < corp.n_units; ++i) {
                    kk[i].key = corp.units[i].key;
                    kk[i].idx = i;
                }
                qsort(kk, corp.n_units, sizeof *kk, cmp_key_idx);
                size_t i = 0;
                while (i < corp.n_units) {
                    size_t j = i;
                    while (j < corp.n_units && kk[j].key == kk[i].key) ++j;
                    size_t alive = 0, cand = (size_t)-1;
                    for (size_t k = i; k < j; ++k) {
                        size_t u = kk[k].idx;
                        if (!gated[u]) { ++alive; break; }
                        if (wg[u] && cand == (size_t)-1) cand = u;
                    }
                    if (!alive && cand != (size_t)-1) {
                        gated[cand] = 0;
                        ++n_rescued;
                    }
                    i = j;
                }
                free(kk);
            }
            size_t n_miss = 0;
            for (size_t k = 0; k < n_gw; ++k) if (!hit[k]) ++n_miss;
            printf("  --gate-words %s: %zu of %zu listed word(s) present, "
                   "%zu span(s), %zu unit(s) withheld (%.2f%%)\n",
                   gate_words, n_gw - n_miss, n_gw, n_gspan, n_gu - n_rescued,
                   100.0 * (double)(n_gu - n_rescued)
                         / (double)(corp.n_units ? corp.n_units : 1));
            if (gate_sole_keep)
                printf("       %zu unit(s) kept back: sole source of their "
                       "preselection key, so gating them would cost the "
                       "context outright\n", n_rescued);
            /* ⚠ NAME THE MISSES. A gate list that matches nothing looks
             * exactly like a gate that worked, and this one is spelling-
             * sensitive: the corpus writes FE-normalised lowercase, so
             * "Alki" never matches and "alki" does. */
            if (n_miss) {
                printf("       ⚠ %zu not found in the corpus:", n_miss);
                size_t shown = 0;
                for (size_t k = 0; k < n_gw && shown < 12; ++k)
                    if (!hit[k]) { printf(" %s", gw[k]); ++shown; }
                printf("%s\n", n_miss > 12 ? " ..." : "");
            }
            for (size_t k = 0; k < n_gw; ++k) free(gw[k]);
            free(gw);
            free(hit);
            free(wg);
        }

        /* The group count WITHOUT the gate, so "N groups dropped to the
         * fallback chain" is a measured difference and not an assertion. A key
         * whose every unit is gated produces no group at all -- deliberately,
         * because the wide groups are unions of the surviving exact ones, so
         * the slot falls through to real units of the right phone. */
        if (gated) {
            spfy_vb_group *ungated = NULL;
            size_t n_ungated = 0;
            rc = spfy_vb_group_units(corp.units, corp.n_units, NULL,
                                     &ungated, &n_ungated);
            if (rc != SPFY_OK) goto fail;
            spfy_vb_groups_free(ungated, n_ungated);
            rc = spfy_vb_group_units(corp.units, corp.n_units, gated,
                                     &exact, &n_exact);
            if (rc != SPFY_OK) goto fail;
            printf("  %zu converted-only group(s) dropped to the fallback "
                   "chain\n", n_ungated - n_exact);
        } else {
            rc = spfy_vb_group_units(corp.units, corp.n_units, NULL,
                                     &exact, &n_exact);
            if (rc != SPFY_OK) goto fail;
        }

        if (corp.n_rvc_units && !strcmp(rvc_policy, "equal"))
            printf("  ⚠ rvc-policy=equal: %zu converted units compete with her "
                   "real audio on level terms\n", corp.n_rvc_units);
        if (corp.n_rvc_units && !strcmp(rvc_policy, "prefer-real")) {
            uint8_t eq[256];
            int n_eq = rvc_equal_phones
                     ? phone_set(&tmpl.pidx, rvc_equal_phones, eq,
                                 "--rvc-equal-phones")
                     : (memset(eq, 0, sizeof eq), 0);
            uint8_t *is_real = (uint8_t *)malloc(corp.n_units);
            if (!is_real) { rc = SPFY_E_NOMEM; goto fail; }
            size_t n_exempt = 0;
            for (size_t i = 0; i < corp.n_units; ++i) {
                int ex = corp.units[i].is_rvc && eq[corp.units[i].phone];
                /* An allow-list gate already removed this unit from every
                 * group, so counting it as "exempted" would report an
                 * exemption that cannot have had any effect. */
                if (ex && !(gated && gated[i])) ++n_exempt;
                is_real[i] = (uint8_t)(!corp.units[i].is_rvc || ex);
            }
            size_t n_dropped = spfy_vb_groups_prefer_real(exact, n_exact,
                                                          is_real, corp.n_units);
            free(is_real);
            printf("  real-audio-wins: %zu converted units, %zu removed from "
                   "groups her own audio already covers\n",
                   corp.n_rvc_units, n_dropped);
            if (n_eq)
                printf("  ⭐ equal-terms phones (%s): %zu converted units "
                       "exempted -- her own audio is too thin to carry these\n",
                       rvc_equal_phones, n_exempt);
        }

        uint32_t hp_bound = (uint32_t)(2u * tmpl.pidx.n);
        if (!hp_bound) hp_bound = 92u;

        /* ⚠ ORDER IS LOAD-BEARING. The wide fallbacks are built from the
         * EXACT groups only, so they come out byte-identical to a no-backoff
         * build; the backoff groups are merged in AFTER. wide_add() does not
         * dedupe, and routing 194k backoff groups through it would append
         * every unit to (92,c,92) once per listing. */
        rc = spfy_vb_with_fallbacks(exact, n_exact, hp_bound, &all, &n_all);
        if (rc != SPFY_OK) { spfy_vb_groups_free(exact, n_exact); goto fail; }

        /* ⭐ BACKOFF: give the keys our corpus never recorded a real group.
         * Purely additive -- exact and wide groups both survive the merge
         * untouched, so a slot that already reached an exact group keeps the
         * identical pool. See spfy_vb_groups_backoff() for the measurement. */
        if (prsl_backoff > 0) {
            uint8_t *is_real = NULL;
            if (!strcmp(rvc_policy, "prefer-real") && corp.n_rvc_units) {
                is_real = (uint8_t *)malloc(corp.n_units);
                if (!is_real) { spfy_vb_groups_free(exact, n_exact);
                                rc = SPFY_E_NOMEM; goto fail; }
                for (size_t i = 0; i < corp.n_units; ++i)
                    is_real[i] = (uint8_t)(!corp.units[i].is_rvc);
            }
            spfy_vb_group *bo = NULL, *merged = NULL;
            size_t n_bo = 0, n_merged = 0;
            spfy_vb_backoff_stats bs;
            rc = spfy_vb_groups_backoff(corp.units, corp.n_units, gated,
                                        is_real, &tmpl.pidx, exact, n_exact,
                                        hp_bound, (uint32_t)prsl_backoff,
                                        (uint32_t)prsl_gate,
                                        &bo, &n_bo, &bs);
            free(is_real);
            if (rc != SPFY_OK) { spfy_vb_groups_free(exact, n_exact); goto fail; }
            rc = spfy_vb_groups_merge(all, n_all, bo, n_bo, &merged, &n_merged);
            spfy_vb_groups_free(bo, n_bo);
            if (rc != SPFY_OK) { spfy_vb_groups_free(exact, n_exact); goto fail; }
            spfy_vb_groups_free(all, n_all);
            all = merged;
            n_all = n_merged;
            printf("  backoff: %zu keys added of %zu enumerable "
                   "(%zu unfillable, %zu gated), %zu listings, "
                   "%zu exact-ctx / %zu same-class / %zu any\n",
                   bs.n_keys_added, bs.n_keys_possible, bs.n_keys_unfillable,
                   bs.n_keys_gated, bs.n_listings_added, bs.n_rank_ctx,
                   bs.n_rank_class, bs.n_rank_any);
        }
        spfy_vb_groups_free(exact, n_exact);
        size_t slots = 0;
        for (size_t i = 0; i < n_all; ++i) slots += all[i].n;
        uint8_t *prsl = NULL;
        size_t prsl_n = 0;
        rc = spfy_vb_encode_prsl(all, n_all, &prsl, &prsl_n);
        printf("  prsl: %zu context groups (%zu exact + %zu wide fallbacks, "
               "hp_bound %u), %zu candidate slots\n",
               n_all, n_exact, n_all - n_exact, hp_bound, slots);
        spfy_vb_groups_free(all, n_all);
        if (rc != SPFY_OK) goto fail;
        rc = spfy_vb_riff_set(&tmpl.vin, "prsl", prsl, prsl_n);
        if (rc != SPFY_OK) { free(prsl); goto fail; }
    }

    /* ---- anchors ---- */
    stage("S1 ANCHORS");
    {
        uint8_t *ckls = NULL, *cklx = NULL;
        size_t ckls_n = 0, cklx_n = 0;
        /* ⭐ AND THE ANCHORS OBEY THE SAME GATE. Without this the gate is only
         * half applied and the log reports a clean build while the defect
         * survives: an anchor expands over its whole span and OVERWRITES the
         * DP's per-half-phone picks, so a unit withheld from every
         * preselection group can still be played. */
        if (gated) {
            size_t dw = spfy_vb_anchors_filter(&corp.words, gated, corp.n_units);
            size_t ds = spfy_vb_anchors_filter(&corp.syls, gated, corp.n_units);
            printf("  anchor gate: %zu word and %zu syllable anchors dropped "
                   "for containing a withheld unit (%zu/%zu kept)\n",
                   dw, ds, corp.words.n, corp.syls.n);
        }
        /* ⭐ AND prefer-real APPLIES HERE TOO, for the same reason the gate
         * does: an anchor overwrites the DP's picks, so a policy enforced only
         * on the prsl pools is enforced only half the time. Without this, the
         * synthetic corpus's 1,888 "the" compete with her 1,282. */
        if (!strcmp(rvc_policy, "prefer-real") && corp.n_syn_anchor_kept) {
            size_t dw = spfy_vb_anchors_prefer_real(&corp.words);
            size_t ds = spfy_vb_anchors_prefer_real(&corp.syls);
            printf("  anchor prefer-real: %zu word and %zu syllable synthetic "
                   "anchors dropped for tokens she already says (%zu/%zu "
                   "kept)\n", dw, ds, corp.words.n, corp.syls.n);
        }
        /* ⛔ SYNTHETIC SYLLABLES ARE NOT SYNTHETIC WORDS.
         * A word anchor plays a whole word from one render. A syllable anchor
         * plays a FRAGMENT that gets spliced into a DIFFERENT word: "body"
         * was served by a contiguous run from `st2_wxa_0173`, which says
         * "...a sturdy building..." and never says "body", and the result was
         * heard as that word acquiring an accent. Word-level gains (tornado)
         * survive; cross-source fragment splicing does not. */
        if (!strcmp(syn_anchors, "word") || !strcmp(syn_anchors, "none")) {
            size_t ds = spfy_vb_anchors_drop_synth(&corp.syls);
            size_t dw = 0;
            if (!strcmp(syn_anchors, "none"))
                dw = spfy_vb_anchors_drop_synth(&corp.words);
            printf("  --syn-anchors %s: %zu synthetic syllable and %zu word "
                   "anchors dropped (%zu word / %zu syl kept)\n",
                   syn_anchors, ds, dw, corp.words.n, corp.syls.n);
        }
        rc = spfy_vb_encode_ckls(&corp.words, &corp.syls, &ckls, &ckls_n);
        if (rc != SPFY_OK) goto fail;
        rc = spfy_vb_encode_cklx(&corp.words, &corp.syls, &cklx, &cklx_n);
        if (rc != SPFY_OK) { free(ckls); goto fail; }
        /* An anchor pointing at the wrong span is a whole wrong word in the
         * output, not a subtle join artefact, so the invariant is checked
         * before it ships: a span starts on a syllable-start L unit and ends
         * on an R unit. */
        size_t bad = 0;
        for (size_t g = 0; g < 2; ++g) {
            const spfy_vb_anchors *a = g ? &corp.syls : &corp.words;
            for (size_t i = 0; i < a->n; ++i) {
                uint32_t ss = a->v[i].span_start, se = a->v[i].span_end;
                if (ss > se || se >= corp.n_units ||
                    corp.units[ss].is_first_half != 1 ||
                    corp.units[se].is_first_half != 0) ++bad;
            }
        }
        printf("  _WORD_ %zu occurrences, _SYL_ %zu; span invariant "
               "violations %zu\n", corp.words.n, corp.syls.n, bad);
        rc = spfy_vb_riff_set(&tmpl.vin, "ckls", ckls, ckls_n);
        if (rc == SPFY_OK) rc = spfy_vb_riff_set(&tmpl.vin, "cklx", cklx, cklx_n);
        if (rc != SPFY_OK) goto fail;
    }

    /* ---- container identity: vers + LIST, ours, not the template's ---- */
    {
        uint8_t *b = NULL;
        size_t bn = 0;
        rc = spfy_vb_encode_vers(version, &b, &bn);
        if (rc != SPFY_OK) goto fail;
        rc = spfy_vb_riff_set(&tmpl.vin, "vers", b, bn);
        if (rc != SPFY_OK) { free(b); goto fail; }
        b = NULL; bn = 0;
        rc = spfy_vb_encode_list(copyright, today, &b, &bn);
        if (rc != SPFY_OK) goto fail;
        rc = spfy_vb_riff_set(&tmpl.vin, "LIST", b, bn);
        if (rc != SPFY_OK) { free(b); goto fail; }
        printf("\n=== identity ===\n  vers \"%s\"  ICRD %s\n  ICOP %s\n",
               version, today, copyright);
    }

    /* ---- write the VIN, plus the sidecars ---- */
    snprintf(path, sizeof path, "%s/%s.vin", out_dir, voice);
    rc = spfy_vb_riff_save(&tmpl.vin, path);
    if (rc != SPFY_OK) { fprintf(stderr, "vin write %d\n", rc); goto fail; }
    printf("\nwrote %s\n", path);

    {
        /* Per-unit prsl key, for anything downstream that wants to know which
         * joins are phonetically plausible without re-deriving them. */
        uint32_t *k = (uint32_t *)malloc(corp.n_units * sizeof *k);
        if (!k) { rc = SPFY_E_NOMEM; goto fail; }
        for (size_t i = 0; i < corp.n_units; ++i) k[i] = corp.units[i].key;
        snprintf(path, sizeof path, "%s/%s.keys", out_dir, voice);
        rc = write_raw(path, (const uint8_t *)k, corp.n_units * 4u);
        free(k);
        if (rc != SPFY_OK) goto fail;
    }
    /* ---- config ----
     * ⭐ WRITTEN, NOT COPIED. Every arm used to ship a byte-for-byte copy of
     * jill.vcf. It never showed in a donor audit because it is not part of
     * the VIN or VDB, yet it weights every cost term the selector uses.
     * `--vcf` still copies, for reproducing an older arm exactly. */
    if (vcf_src) {
        snprintf(path, sizeof path, "%s/%s.vcf", out_dir, voice);
        if (copy_file(vcf_src, path) != SPFY_OK)
            fprintf(stderr, "  ⚠ could not copy %s\n", vcf_src);
        printf("  vcf COPIED from %s\n", vcf_src);
    } else {
        size_t n_set = 0;
        rc = spfy_vb_vcf_write(out_dir, voice, vcf_sets, n_vcf_sets, &n_set);
        if (rc != SPFY_OK) {
            fprintf(stderr, "vcf write %d\n", rc);
            goto fail;
        }
        printf("  vcf WRITTEN (%zu override(s) applied)\n", n_set);
    }
    {
        /* The sidecar the engine reads first; it is where the identity lives.
         * 8 kHz is the only format this builder emits. */
        rc = spfy_vb_sidecar_write(out_dir, voice, 8, "en-US", 5575);
        if (rc != SPFY_OK) {
            fprintf(stderr, "sidecar write %d\n", rc);
            goto fail;
        }
        printf("  sidecar %s8.xml written\n", voice);
    }

    /* ---- S4 JOIN ---- */
    if (do_s4) {
        stage("S4 JOIN");
        char vinp[1024], vdbp[1024];
        snprintf(vinp, sizeof vinp, "%s/%s.vin", out_dir, voice);
        snprintf(vdbp, sizeof vdbp, "%s/%s8.vdb", out_dir, voice);
        /* ⭐ Hand S4 the MEASURED edge F0. The written bytes are 0 under
         * --f0 absent, so without this dim 0 of the join cost is identically
         * zero and the K-best partner ranking never sees pitch. */
        uint8_t *jf0 = NULL;
        if (join_f0 && corp.n_units) {
            jf0 = (uint8_t *)calloc(corp.n_units * 2u, 1);
            if (!jf0) { rc = SPFY_E_NOMEM; goto fail; }
            size_t nz = 0;
            for (size_t i = 0; i < corp.n_units; ++i) {
                uint32_t u = corp.units[i].uid;
                if (u >= corp.n_units) continue;
                jf0[(size_t)u * 2u]      = corp.units[i].jf0_start;
                jf0[(size_t)u * 2u + 1u] = corp.units[i].jf0_end;
                if (corp.units[i].jf0_start || corp.units[i].jf0_end) ++nz;
            }
            jc.f0_edge = jf0;
            jc.n_f0_edge = (uint32_t)corp.n_units;
            printf("  join f0: measured edge pitch on %zu/%zu units "
                   "(%.1f%%); the stored bytes stay 0\n",
                   nz, corp.n_units, 100.0 * (double)nz / (double)corp.n_units);
        }
        rc = run_s4(vinp, vdbp, &jc);
        free(jf0);
        jc.f0_edge = NULL;
        jc.n_f0_edge = 0u;
        if (rc != SPFY_OK) goto fail;
    } else {
        printf("\n=== S4 JOIN: SKIPPED ===\n");
        printf("  ⚠ the template's hash indexes the TEMPLATE's units. This "
               "voice will render badly.\n");
    }

    stage(NULL);          /* closes the last stage's timer */
    free(gated);
    spfy_vb_corpus_free(&corp);
    spfy_vb_template_free(&tmpl);
    printf("\ndone in %.1fs", now_s() - g_t0);
#ifdef _OPENMP
    printf(" (S4 on %d threads)", omp_get_max_threads());
#else
    printf(" (single-threaded: built without OpenMP)");
#endif
    printf("\n");
    return 0;

fail:
    free(gated);
    spfy_vb_corpus_free(&corp);
    spfy_vb_template_free(&tmpl);
    return 1;
}
