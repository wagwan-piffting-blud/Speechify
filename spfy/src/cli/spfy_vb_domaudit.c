/* Is `hash` a K-BEST CUT of the adjacency relation, or the WHOLE relation?
 *
 * The question this settles, and why it is worth a tool:
 *
 * Measured on the three demo texts, the hash HIT rate on the joins that
 * actually reach the audio is jill 97.3-98.2%, tom 92.2-96.9%, and ours
 * 41.7-70.4% -- while the hit rate over CONSIDERED edges is nearly identical
 * for all three (4-11%). A rate cannot explain that gap. What can is WHICH
 * pairs are in the table.
 *
 * Our S4 keeps each right unit's K best left partners ranked by the computed
 * cost (vb_stages.c, "WITH A CONSTANT COST, MEMBERSHIP *IS* THE METRIC"), with
 * K = 10 chosen to match the vendors' mean row of 10.1/10.5. But a mean row of
 * 10.5 is only evidence of truncation if the relation is WIDER than 10.5. Our
 * own build scored 158,004,028 candidate joins over 457,392 units -- a mean
 * bucket of 345 -- so we discard 97% of the relation. If jill's relation is
 * about 10 wide, she discards nothing, her mean row is a property of her
 * corpus rather than a policy, and K is not a knob we should be turning: it is
 * the whole rule.
 *
 * So, per right unit, this reports three numbers that separate the cases:
 *
 *   RELATION   how many left units the two families admit (no cost pass, so
 *              this is cheap and exact)
 *   ROW        how many partners the shipped table actually stores
 *   IN-REL     how many of those stored partners the relation contains
 *
 * ROW ~= RELATION       -> the vendor stores the relation whole
 * ROW << RELATION       -> the vendor truncates, and by what factor
 * IN-REL << ROW         -> our relation is wrong, not merely narrow
 *
 * The families and the parity rule are lifted from spfy_vb_s4_join() rather
 * than re-derived, because a second derivation that drifts is exactly how the
 * domain and the candidate pools stopped agreeing once before.
 *
 * --pairs answers the question the first mode raises. On jill only 2.25 of the
 * 10.99 partners she stores are pairs our relation admits at all, so 80% of
 * her table is outside our rule and no value of K can reach it. This mode
 * classifies every stored pair by the KEY RELATIONSHIP between its endpoints,
 * so the rule can be read off her table instead of guessed at.
 *
 *   spfy_vb_domaudit <voice.vin> [--pairs]
 */

#include "../usel/hash.h"
#include "../usel/prsl.h"
#include "../voice/voice.h"
#include "../voice/unit_table.h"
#include "../../include/spfy/spfy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NKA 1000000u                 /* max key is 91*10101 = 919,191 */
#define NB  (92u * 92u)

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x < y) ? -1 : (x > y);
}

/* Percentiles off a sorted array, printed as one line. */
static void report(const char *tag, uint32_t *v, size_t n, double denom)
{
    if (!n) { printf("  %-22s (none)\n", tag); return; }
    qsort(v, n, sizeof *v, cmp_u32);
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) sum += v[i];
    printf("  %-22s n %8zu  mean %10.2f  p50 %7u  p90 %8u  p99 %9u  "
           "max %9u  sum %12.0f  (%.2f%% of pairs)\n",
           tag, n, sum / (double)n, v[n / 2], v[(size_t)(n * 0.90)],
           v[(size_t)(n * 0.99)], v[n - 1], sum,
           denom > 0.0 ? 100.0 * sum / denom : 0.0);
}

/* One (dL,dC,dR) triple and how often a stored pair showed it. */
typedef struct { int dl, dc, dr; uint32_t n; } delta_t;

static int cmp_delta_n(const void *a, const void *b)
{
    uint32_t x = ((const delta_t *)a)->n, y = ((const delta_t *)b)->n;
    return (x > y) ? -1 : (x < y);
}

int main(int argc, char **argv)
{
    int want_pairs = 0, want_multi = 0;
    const char *keys_out = NULL;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--pairs")) want_pairs = 1;
        else if (!strcmp(argv[i], "--multi")) want_multi = 1;
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) keys_out = argv[++i];
        else {
            fprintf(stderr, "usage: %s <voice.vin> [--pairs] [--multi] "
                            "[--keys OUT]\n", argv[0]);
            return 2;
        }
    }
    if (argc < 2) {
        fprintf(stderr, "usage: %s <voice.vin> [--pairs] [--multi] "
                        "[--keys OUT]\n", argv[0]);
        return 2;
    }

    spfy_vin_t vin = {0};
    int rc = spfy_vin_load(argv[1], &vin);
    if (rc != SPFY_OK) { fprintf(stderr, "vin_load %d\n", rc); return 1; }

    spfy_unit_table_t ut;
    rc = spfy_unit_table_load(&vin, &ut);
    if (rc != SPFY_OK) { fprintf(stderr, "unit_table_load %d\n", rc); return 1; }
    const uint32_t n_units = ut.n_units;

    spfy_hash_t h;
    rc = spfy_hash_load(&vin, &h);
    if (rc != SPFY_OK) { fprintf(stderr, "hash_load %d\n", rc); return 1; }

    /* prsl key per unit, exactly as S4 reads it back.
     *
     * ⚠ S4 writes ONE key per unit and lets a later group overwrite an earlier
     * one. That is only sound if group membership is a partition. If
     * preselection BACKS OFF -- listing a unit under contexts other than the
     * one it was recorded in -- then a unit has several keys, the last writer
     * wins arbitrarily, and the enumerated relation is a slice of the real
     * one. `memberships` is the control for exactly that. */
    uint32_t *keys = (uint32_t *)calloc(n_units ? n_units : 1u, sizeof *keys);
    uint32_t *memb = (uint32_t *)calloc(n_units ? n_units : 1u, sizeof *memb);
    /* Every (unit, key) listing, CSR by unit: mk_off[u] .. mk_off[u+1]. */
    uint32_t *mk_off = NULL, *mk_key = NULL;
    if (!keys || !memb) return 1;
    {
        spfy_prsl_t prsl;
        rc = spfy_prsl_load(&vin, &prsl);
        if (rc != SPFY_OK) { fprintf(stderr, "prsl_load %d\n", rc); return 1; }
        uint32_t n_wide = 0;
        uint64_t n_listings = 0;
        for (uint32_t g = 0; g < prsl.n_groups; ++g) {
            uint32_t k = prsl.groups[g].context_key;
            uint32_t l = k / 10000u, r = k % 100u;
            if (l >= 92u || r >= 92u) { ++n_wide; continue; }
            for (uint32_t i = 0; i < prsl.groups[g].n_candidates; ++i) {
                uint32_t u = spfy_prsl_cand(prsl.groups[g].candidates, i);
                if (u < n_units) { keys[u] = k; ++memb[u]; ++n_listings; }
            }
        }
        uint32_t n_zero = 0, n_one = 0, n_many = 0, m_max = 0;
        for (uint32_t u = 0; u < n_units; ++u) {
            if (!memb[u]) ++n_zero;
            else if (memb[u] == 1u) ++n_one;
            else ++n_many;
            if (memb[u] > m_max) m_max = memb[u];
        }
        printf("  prsl: %u groups (%u wide/skipped), %llu listings over "
               "%u units\n", prsl.n_groups, n_wide,
               (unsigned long long)n_listings, n_units);
        printf("        memberships per unit: 0 -> %u (%.1f%%), "
               "1 -> %u (%.1f%%), >1 -> %u (%.1f%%), max %u\n",
               n_zero, 100.0 * n_zero / n_units, n_one, 100.0 * n_one / n_units,
               n_many, 100.0 * n_many / n_units, m_max);

        if (want_multi) {
            mk_off = (uint32_t *)calloc((size_t)n_units + 1u, sizeof *mk_off);
            mk_key = (uint32_t *)malloc((n_listings ? (size_t)n_listings : 1u)
                                        * sizeof *mk_key);
            if (!mk_off || !mk_key) return 1;
            for (uint32_t u = 0; u < n_units; ++u) mk_off[u + 1u] = memb[u];
            for (uint32_t u = 0; u < n_units; ++u) mk_off[u + 1u] += mk_off[u];
            uint32_t *fill = (uint32_t *)calloc(n_units ? n_units : 1u,
                                                sizeof *fill);
            if (!fill) return 1;
            for (uint32_t g = 0; g < prsl.n_groups; ++g) {
                uint32_t k = prsl.groups[g].context_key;
                uint32_t l = k / 10000u, r = k % 100u;
                if (l >= 92u || r >= 92u) continue;
                for (uint32_t i = 0; i < prsl.groups[g].n_candidates; ++i) {
                    uint32_t u = spfy_prsl_cand(prsl.groups[g].candidates, i);
                    if (u < n_units) mk_key[mk_off[u] + fill[u]++] = k;
                }
            }
            free(fill);
        }
        spfy_prsl_free(&prsl);
    }

    /* The prsl key per unit, so the demand side (which pairs the DP asks for)
     * can be classified by the same rule without a second prsl reader. */
    if (keys_out) {
        FILE *kf = fopen(keys_out, "wb");
        if (!kf) { fprintf(stderr, "cannot write %s\n", keys_out); return 1; }
        if (fwrite(keys, sizeof *keys, n_units, kf) != n_units) {
            fprintf(stderr, "short write on %s\n", keys_out);
            fclose(kf);
            return 1;
        }
        fclose(kf);
        printf("  keys: %u u32 -> %s\n", n_units, keys_out);
    }

    /* Bucket the left units, same two families as S4. */
    uint32_t *head = (uint32_t *)malloc((size_t)(NKA + NB) * sizeof *head);
    uint32_t *next = (uint32_t *)malloc((n_units ? n_units : 1u) * sizeof *next);
    if (!head || !next) return 1;
    uint32_t *headA = head, *headB = head + NKA;
    for (uint32_t i = 0; i < NKA + NB; ++i) head[i] = 0xFFFFFFFFu;
    for (uint32_t u = n_units; u-- > 0; ) {
        next[u] = 0xFFFFFFFFu;
        uint32_t k = keys[u];
        if (!k) continue;
        uint32_t L = k / 10000u, C = (k / 100u) % 100u, R = k % 100u;
        if (L >= 92u || C >= 92u || R >= 92u) continue;
        if ((C & 1u) == 0u) {
            if (k >= NKA) continue;
            next[u] = headA[k]; headA[k] = u;
        } else {
            uint32_t b = C * 92u + R;
            next[u] = headB[b]; headB[b] = u;
        }
    }

    /* --multi: the relation as PRESELECTION defines it, not as one key per
     * unit defines it. A pair (l, r) is admissible when SOME group listing l
     * and SOME group listing r are an adjacent-slot key pair -- which is what
     * the DP can actually request. With a partitioned prsl the two agree
     * exactly; with backoff they do not, and the difference is the part of
     * the vendors' table our rule cannot see. */
    uint32_t *bkt_off = NULL, *bkt_uid = NULL;
    if (want_multi) {
        size_t NBK = (size_t)NKA + NB;
        bkt_off = (uint32_t *)calloc(NBK + 1u, sizeof *bkt_off);
        if (!bkt_off) return 1;
        uint32_t n_list = mk_off[n_units];
        bkt_uid = (uint32_t *)malloc((n_list ? n_list : 1u) * sizeof *bkt_uid);
        if (!bkt_uid) return 1;
        for (uint32_t u = 0; u < n_units; ++u)
            for (uint32_t i = mk_off[u]; i < mk_off[u + 1u]; ++i) {
                uint32_t k = mk_key[i];
                uint32_t C = (k / 100u) % 100u, R = k % 100u;
                size_t b = (C & 1u) ? ((size_t)NKA + C * 92u + R) : k;
                bkt_off[b + 1u]++;
            }
        for (size_t b = 0; b < NBK; ++b) bkt_off[b + 1u] += bkt_off[b];
        uint32_t *fill = (uint32_t *)calloc(NBK, sizeof *fill);
        if (!fill) return 1;
        for (uint32_t u = 0; u < n_units; ++u)
            for (uint32_t i = mk_off[u]; i < mk_off[u + 1u]; ++i) {
                uint32_t k = mk_key[i];
                uint32_t C = (k / 100u) % 100u, R = k % 100u;
                size_t b = (C & 1u) ? ((size_t)NKA + C * 92u + R) : k;
                bkt_uid[bkt_off[b] + fill[b]++] = u;
            }
        free(fill);
    }

    /* Invert the shipped table into per-right-unit partner counts. A cell is
     * owned by row keys[i]; that is the same test the engine's lookup makes,
     * so a cell counted here is a cell that can actually resolve. */
    uint32_t *row_n = (uint32_t *)calloc(n_units ? n_units : 1u, sizeof *row_n);
    uint32_t *row_in = (uint32_t *)calloc(n_units ? n_units : 1u, sizeof *row_in);
    if (!row_n || !row_in) return 1;
    size_t n_live = 0, n_ghost = 0, n_cont_stored = 0;
    for (uint64_t i = 0; i < h.n_cells; ++i) {
        uint32_t r = spfy_hash_cell_a(&h, i);
        if (r >= n_units) continue;                 /* dead / out of domain */
        uint64_t base = spfy_hash_row(&h, r);
        if (i < base) { ++n_ghost; continue; }
        uint64_t l = i - base;
        if (l >= n_units) { ++n_ghost; continue; }
        ++n_live;
        ++row_n[r];
        if ((uint32_t)l + 1u == r) { ++n_cont_stored; continue; }
        /* Is this stored partner inside the relation? */
        if (want_multi) {
            int found = 0;
            for (uint32_t ir = mk_off[r]; ir < mk_off[r + 1u] && !found; ++ir) {
                uint32_t kr = mk_key[ir];
                uint32_t Lr = kr / 10000u, Cr = (kr / 100u) % 100u;
                for (uint32_t il = mk_off[l]; il < mk_off[l + 1u]; ++il) {
                    uint32_t kl = mk_key[il];
                    uint32_t Cl = (kl / 100u) % 100u, Rl = kl % 100u;
                    if ((Cr & 1u) ? (kl + 10101u == kr)
                                  : (Cl == Lr + 1u && Rl == Cr + 1u)) {
                        found = 1; break;
                    }
                }
            }
            row_in[r] += (uint32_t)found;
            continue;
        }
        uint32_t k = keys[r];
        if (!k) continue;
        uint32_t L = k / 10000u, C = (k / 100u) % 100u, R = k % 100u;
        if (L >= 92u || C >= 92u || R >= 92u) continue;
        if (((L ^ C) | (C ^ R)) & 1u) continue;
        uint32_t hd = (C & 1u) ? headA[k - 10101u]
                               : headB[(L + 1u) * 92u + (C + 1u)];
        for (uint32_t q = hd; q != 0xFFFFFFFFu; q = next[q])
            if (q == (uint32_t)l) { ++row_in[r]; break; }
    }

    /* Relation width per right unit. */
    uint32_t *rel = (uint32_t *)calloc(n_units ? n_units : 1u, sizeof *rel);
    if (!rel) return 1;
    uint64_t rel_total = 0;
    uint32_t n_keyed = 0;
    for (uint32_t r = 0; r < n_units; ++r) {
        if (want_multi) {
            /* Union over every group listing r. Counted as LISTINGS, so a
             * left unit reachable through two of r's groups counts twice --
             * an upper bound, flagged in the label rather than deduped, since
             * deduping 158 M of them would cost more than it informs. */
            if (mk_off[r + 1u] == mk_off[r]) continue;
            ++n_keyed;
            uint64_t m = 0;
            for (uint32_t ir = mk_off[r]; ir < mk_off[r + 1u]; ++ir) {
                uint32_t kr = mk_key[ir];
                uint32_t Lr = kr / 10000u, Cr = (kr / 100u) % 100u;
                size_t b;
                if (Cr & 1u) {
                    if (kr < 10101u) continue;
                    b = kr - 10101u;
                } else {
                    b = (size_t)NKA + (Lr + 1u) * 92u + (Cr + 1u);
                }
                m += bkt_off[b + 1u] - bkt_off[b];
            }
            rel[r] = (m > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)m;
            rel_total += m;
            continue;
        }
        uint32_t k = keys[r];
        if (!k) continue;
        uint32_t L = k / 10000u, C = (k / 100u) % 100u, R = k % 100u;
        if (L >= 92u || C >= 92u || R >= 92u) continue;
        if (((L ^ C) | (C ^ R)) & 1u) continue;
        ++n_keyed;
        uint32_t hd = (C & 1u) ? headA[k - 10101u]
                               : headB[(L + 1u) * 92u + (C + 1u)];
        uint32_t m = 0;
        for (uint32_t q = hd; q != 0xFFFFFFFFu; q = next[q])
            if (q != r && r != q + 1u) ++m;
        rel[r] = m;
        rel_total += m;
    }

    printf("%s\n", argv[1]);
    printf("  units %u   keyed %u (%.1f%%)   n_rows %u   n_cells %u\n",
           n_units, n_keyed, 100.0 * n_keyed / (n_units ? n_units : 1u),
           h.n_rows, h.n_cells);
    printf("  live cells %zu   ghost/unreachable %zu   stored continuations %zu\n",
           n_live, n_ghost, n_cont_stored);
    printf("  relation total %llu   mean over keyed units %.2f\n",
           (unsigned long long)rel_total,
           n_keyed ? (double)rel_total / n_keyed : 0.0);

    /* Restrict every distribution to units that HAVE a relation, so a unit
     * the rule simply cannot key does not drag the mean toward zero and make
     * a truncating table look complete. */
    uint32_t *a = (uint32_t *)malloc((size_t)n_units * sizeof *a);
    uint32_t *b = (uint32_t *)malloc((size_t)n_units * sizeof *b);
    uint32_t *c = (uint32_t *)malloc((size_t)n_units * sizeof *c);
    if (!a || !b || !c) return 1;
    size_t na = 0, nb = 0, nc = 0;
    uint64_t cov_num = 0, cov_den = 0;
    uint32_t n_full = 0, n_trunc = 0;
    for (uint32_t r = 0; r < n_units; ++r) {
        if (!rel[r]) continue;
        a[na++] = rel[r];
        b[nb++] = row_n[r];
        c[nc++] = row_in[r];
        cov_num += row_in[r];
        cov_den += rel[r];
        if (row_in[r] >= rel[r]) ++n_full; else ++n_trunc;
    }
    printf("\n  per right unit WITH a non-empty relation:\n");
    report("RELATION width", a, na, (double)rel_total);
    report("ROW stored", b, nb, (double)n_live);
    report("ROW inside relation", c, nc, (double)n_live);
    /* HOW the budget is spread. Our K-best cut gives every right unit the
     * same K; jill's rows run p50 5, p90 28, max 416 on a mean of 11. Same
     * mean, completely different distribution -- so the open question is what
     * she spends MORE on. Cross-tabbing row size against relation width says
     * whether the answer is simply "the units with more partners". */
    {
        printf("\n  row size by relation-width decile (is the budget spread "
               "by demand?):\n");
        printf("    %-6s %12s %12s %10s %10s %10s\n",
               "decile", "rel width", "rel width", "row mean", "row p50",
               "row max");
        printf("    %-6s %12s %12s %10s %10s %10s\n",
               "", "low", "high", "", "", "");
        /* a[] is already sorted ascending by relation width; walk the units
         * again and bucket by where their width falls. */
        for (int d = 0; d < 10; ++d) {
            size_t lo_i = (size_t)((double)na * d / 10.0);
            size_t hi_i = (size_t)((double)na * (d + 1) / 10.0);
            if (hi_i <= lo_i) continue;
            uint32_t wlo = a[lo_i], whi = a[hi_i - 1u];
            double sum = 0.0; size_t cnt = 0; uint32_t rmax = 0;
            uint32_t *rows_here = (uint32_t *)malloc(na * sizeof *rows_here);
            if (!rows_here) break;
            for (uint32_t r = 0; r < n_units; ++r) {
                if (!rel[r] || rel[r] < wlo || rel[r] > whi) continue;
                rows_here[cnt++] = row_n[r];
                sum += row_n[r];
                if (row_n[r] > rmax) rmax = row_n[r];
            }
            if (cnt) {
                qsort(rows_here, cnt, sizeof *rows_here, cmp_u32);
                printf("    %-6d %12u %12u %10.2f %10u %10u\n",
                       d, wlo, whi, sum / (double)cnt, rows_here[cnt / 2], rmax);
            }
            free(rows_here);
        }
    }

    /* The other candidate allocation rule, and the one that would be free to
     * implement: a unit listed in more groups is asked for more often, so it
     * is worth caching more of its joins. Unlike relation width this needs no
     * acoustics and no demand harvest -- prsl already knows it. */
    {
        printf("\n  row size by prsl MEMBERSHIP of the right unit:\n");
        printf("    %-10s %10s %10s %10s %10s\n",
               "listings", "units", "row mean", "row p50", "row max");
        static const uint32_t BK[] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 64, 0};
        uint32_t *rows_here = (uint32_t *)malloc((size_t)n_units * sizeof *rows_here);
        if (rows_here) {
            for (int bi = 0; BK[bi]; ++bi) {
                uint32_t lo = BK[bi], hi = BK[bi + 1] ? BK[bi + 1] - 1u : 0xFFFFFFFFu;
                double sum = 0.0; size_t cnt = 0; uint32_t rmax = 0;
                for (uint32_t r = 0; r < n_units; ++r) {
                    if (memb[r] < lo || memb[r] > hi) continue;
                    rows_here[cnt++] = row_n[r];
                    sum += row_n[r];
                    if (row_n[r] > rmax) rmax = row_n[r];
                }
                if (!cnt) continue;
                qsort(rows_here, cnt, sizeof *rows_here, cmp_u32);
                char lbl[32];
                if (hi == 0xFFFFFFFFu) snprintf(lbl, sizeof lbl, "%u+", lo);
                else if (lo == hi)     snprintf(lbl, sizeof lbl, "%u", lo);
                else                   snprintf(lbl, sizeof lbl, "%u-%u", lo, hi);
                printf("    %-10s %10zu %10.2f %10u %10u\n",
                       lbl, cnt, sum / (double)cnt, rows_here[cnt / 2], rmax);
            }
            free(rows_here);
        }
    }

    printf("\n  relation coverage  %llu / %llu = %.2f%%\n",
           (unsigned long long)cov_num, (unsigned long long)cov_den,
           cov_den ? 100.0 * (double)cov_num / (double)cov_den : 0.0);
    printf("  right units storing their relation WHOLE  %u / %zu (%.2f%%)\n",
           n_full, na, na ? 100.0 * n_full / na : 0.0);
    printf("  right units TRUNCATED                     %u / %zu (%.2f%%)\n",
           n_trunc, na, na ? 100.0 * n_trunc / na : 0.0);

    if (want_pairs) {
        /* Key components run 0..91, so every delta fits [-91,91] and a flat
         * 183^3 counter is exact with no hashing to get wrong. */
        const int D = 183, OFF = 91;
        const size_t DN = (size_t)(D * D * D);
        uint32_t *dh = (uint32_t *)calloc(DN, sizeof *dh);
        if (!dh) return 1;
        uint64_t n_pair = 0, n_cont = 0, n_r_unkeyed = 0, n_l_unkeyed = 0;
        uint64_t n_famA = 0, n_famB = 0, n_relA = 0, n_relB = 0, n_other = 0;
        uint64_t par[4] = {0, 0, 0, 0};        /* (C_l&1)*2 + (C_r&1) */
        /* Both strict families are a conjunction of TWO context matches. Our
         * rule demands both; the vendors clearly do not. Splitting the 2x2
         * says which half of the conjunction is the real constraint and which
         * is the one we invented. */
        uint64_t a22[4] = {0, 0, 0, 0};    /* even->odd: (dL==1)*2 + (dR==1) */
        uint64_t b22[4] = {0, 0, 0, 0};    /* odd->even: (Cl==Lr+1)*2 + (Rl==Cr+1) */
        uint64_t n_a_samephone = 0, n_a_offphone = 0;

        for (uint64_t i = 0; i < h.n_cells; ++i) {
            uint32_t r = spfy_hash_cell_a(&h, i);
            if (r >= n_units) continue;
            uint64_t base = spfy_hash_row(&h, r);
            if (i < base) continue;
            uint64_t l64 = i - base;
            if (l64 >= n_units) continue;
            uint32_t l = (uint32_t)l64;
            ++n_pair;
            if (l + 1u == r) { ++n_cont; continue; }

            uint32_t kr = keys[r], kl = keys[l];
            uint32_t Lr = kr / 10000u, Cr = (kr / 100u) % 100u, Rr = kr % 100u;
            uint32_t Ll = kl / 10000u, Cl = (kl / 100u) % 100u, Rl = kl % 100u;
            if (!kr || Lr >= 92u || Cr >= 92u || Rr >= 92u) { ++n_r_unkeyed; continue; }
            if (!kl || Ll >= 92u || Cl >= 92u || Rl >= 92u) { ++n_l_unkeyed; continue; }

            ++par[(Cl & 1u) * 2u + (Cr & 1u)];
            int is_A = (kl + 10101u == kr);
            int is_B = (Cl == Lr + 1u && Rl == Cr + 1u);
            /* The context-free versions of the same two families: A is
             * "L half then R half of the same phone", B is "R half then L
             * half of the next phone". If the relaxed rule matches where the
             * strict one does not, the miss is CONTEXT, not adjacency. */
            int is_rA = ((Cl & 1u) == 0u && Cr == Cl + 1u);
            int is_rB = ((Cl & 1u) == 1u && (Cr & 1u) == 0u);
            if (is_A) ++n_famA;
            else if (is_B) ++n_famB;
            else if (is_rA) ++n_relA;
            else if (is_rB) ++n_relB;
            else ++n_other;

            if ((Cl & 1u) == 0u) {
                if (Cr == Cl + 1u) ++n_a_samephone; else ++n_a_offphone;
                a22[(size_t)((Lr == Ll + 1u) * 2u + (Rr == Rl + 1u))]++;
            } else {
                b22[(size_t)((Cl == Lr + 1u) * 2u + (Rl == Cr + 1u))]++;
            }

            int dl = (int)Lr - (int)Ll;
            int dc = (int)Cr - (int)Cl;
            int dr = (int)Rr - (int)Rl;
            dh[(size_t)(((dl + OFF) * D + (dc + OFF)) * D + (dr + OFF))]++;
        }

        uint64_t keyed = n_pair - n_cont - n_r_unkeyed - n_l_unkeyed;
        printf("\n  --pairs: %llu stored, %llu continuations, "
               "%llu right-unkeyed, %llu left-unkeyed, %llu both-keyed\n",
               (unsigned long long)n_pair, (unsigned long long)n_cont,
               (unsigned long long)n_r_unkeyed,
               (unsigned long long)n_l_unkeyed, (unsigned long long)keyed);
        static const char *PN[4] = {"even->even", "even->odd",
                                    "odd ->even", "odd ->odd "};
        printf("  half-parity transition (of both-keyed):\n");
        for (int p = 0; p < 4; ++p)
            printf("    %-12s %10llu  %6.2f%%\n", PN[p],
                   (unsigned long long)par[p],
                   keyed ? 100.0 * (double)par[p] / (double)keyed : 0.0);
        const double kd = keyed ? (double)keyed : 1.0;
        printf("  family classification (of both-keyed):\n");
        printf("    %-26s %10llu  %6.2f%%\n", "A strict (dkey=+10101)",
               (unsigned long long)n_famA, 100.0 * (double)n_famA / kd);
        printf("    %-26s %10llu  %6.2f%%\n", "B strict (Cl=Lr+1,Rl=Cr+1)",
               (unsigned long long)n_famB, 100.0 * (double)n_famB / kd);
        printf("    %-26s %10llu  %6.2f%%\n", "A relaxed (Cr=Cl+1)",
               (unsigned long long)n_relA, 100.0 * (double)n_relA / kd);
        printf("    %-26s %10llu  %6.2f%%\n", "B relaxed (odd->even)",
               (unsigned long long)n_relB, 100.0 * (double)n_relB / kd);
        printf("    %-26s %10llu  %6.2f%%\n", "neither",
               (unsigned long long)n_other, 100.0 * (double)n_other / kd);

        uint64_t n_ao = a22[0] + a22[1] + a22[2] + a22[3];
        uint64_t n_bo = b22[0] + b22[1] + b22[2] + b22[3];
        double n_aod = n_ao ? (double)n_ao : 1.0, n_bod = n_bo ? (double)n_bo : 1.0;
        printf("  even->odd (%llu): same centre phone %llu (%.2f%%), "
               "off-phone %llu (%.2f%%)\n",
               (unsigned long long)n_ao, (unsigned long long)n_a_samephone,
               100.0 * (double)n_a_samephone / n_aod,
               (unsigned long long)n_a_offphone,
               100.0 * (double)n_a_offphone / n_aod);
        printf("    left ctx  right ctx        count       %%\n");
        for (int p = 0; p < 4; ++p)
            printf("    %-9s %-9s %12llu  %6.2f%%\n",
                   (p & 2) ? "match" : "differ", (p & 1) ? "match" : "differ",
                   (unsigned long long)a22[p], 100.0 * (double)a22[p] / n_aod);
        printf("  odd->even (%llu): Cl==Lr+1 is \"left unit's phone is the "
               "right unit's left context\"\n", (unsigned long long)n_bo);
        printf("    Cl==Lr+1  Rl==Cr+1         count       %%\n");
        for (int p = 0; p < 4; ++p)
            printf("    %-9s %-9s %12llu  %6.2f%%\n",
                   (p & 2) ? "yes" : "no", (p & 1) ? "yes" : "no",
                   (unsigned long long)b22[p], 100.0 * (double)b22[p] / n_bod);

        size_t n_d = 0;
        for (size_t i = 0; i < DN; ++i) if (dh[i]) ++n_d;
        delta_t *dv = (delta_t *)malloc((n_d ? n_d : 1u) * sizeof *dv);
        if (!dv) return 1;
        size_t j = 0;
        for (int x = 0; x < D; ++x)
            for (int y = 0; y < D; ++y)
                for (int z = 0; z < D; ++z) {
                    uint32_t cnt = dh[(size_t)((x * D + y) * D + z)];
                    if (!cnt) continue;
                    dv[j].dl = x - OFF; dv[j].dc = y - OFF; dv[j].dr = z - OFF;
                    dv[j].n = cnt; ++j;
                }
        qsort(dv, n_d, sizeof *dv, cmp_delta_n);
        printf("  top key deltas (Lr-Ll, Cr-Cl, Rr-Rl), %zu distinct:\n", n_d);
        for (size_t i = 0; i < n_d && i < 20; ++i)
            printf("    (%4d,%4d,%4d) %10u  %6.2f%%\n",
                   dv[i].dl, dv[i].dc, dv[i].dr, dv[i].n,
                   100.0 * (double)dv[i].n / kd);
        free(dv); free(dh);
    }

    free(a); free(b); free(c);
    free(rel); free(row_n); free(row_in);
    free(head); free(next); free(keys); free(memb);
    free(mk_off); free(mk_key); free(bkt_off); free(bkt_uid);
    spfy_hash_free(&h);
    spfy_vin_free(&vin);
    return 0;
}
