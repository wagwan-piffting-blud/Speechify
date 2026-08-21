/* S4 acceptance gate: rebuild a voice's join-cost table from its own contents.
 *
 * Extracts the live (uid_right, uid_left, cost) set out of the shipped `hash`
 * chunk by inverting the addressing rule, re-packs it with the first-fit-
 * decreasing row-displacement packer in src/usel/hash_build.c, and compares.
 *
 * Two verdicts, and the weaker one is still worth having:
 *   SEMANTIC  every original pair resolves to the same cost in the rebuilt
 *             table, and nothing resolves that should not. This is what the
 *             engine actually depends on.
 *   BYTE      the serialised chunk is identical. This additionally pins the
 *             vendor's packing ORDER, which is not implied by the format.
 *
 * Spec: spfy/src/vb/SPEC_S4_hash.md
 *
 *   spfy_hash_roundtrip <voice.vin> [--dump-pairs <out.tsv>]
 *
 * --dump-pairs writes the recovered domain as TSV. Paired with
 * `spfy_dump_voice --index`, that is what lets the vendors' own DOMAIN RULE be
 * read off their tables instead of guessed -- which matters, because a guessed
 * rule produced a table that hit 0 of 601,768 lookups while tom hit 8.65%.
 */

#include "../usel/hash.h"
#include "../usel/hash_build.h"
#include "../voice/voice.h"
#include "../voice/unit_table.h"
#include "../../include/spfy/spfy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv)
{
    const char *vin_path = NULL, *dump_pairs = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--dump-pairs") && i + 1 < argc) {
            dump_pairs = argv[++i];
        } else if (argv[i][0] != '-' && !vin_path) {
            vin_path = argv[i];
        } else {
            fprintf(stderr, "usage: %s <voice.vin> [--dump-pairs <out.tsv>]\n",
                    argv[0]);
            return 2;
        }
    }
    if (!vin_path) {
        fprintf(stderr, "usage: %s <voice.vin> [--dump-pairs <out.tsv>]\n",
                argv[0]);
        return 2;
    }

    spfy_vin_t vin;
    int rc = spfy_vin_load(vin_path, &vin);
    if (rc != SPFY_OK) {
        fprintf(stderr, "cannot load %s (%d)\n", vin_path, rc);
        return 1;
    }

    spfy_hash_t h;
    rc = spfy_hash_load(&vin, &h);
    if (rc != SPFY_OK) {
        fprintf(stderr, "cannot load hash (%d)\n", rc);
        spfy_vin_free(&vin);
        return 1;
    }

    /* n_units bounds uid_left. Without it the inversion accepts any cell whose
     * key names a row placed at or below it, which lets an uninitialised cell
     * -- key 0, a perfectly valid uid_right -- be counted as a recovered pair.
     * On donnart that inflated 4.22 M reachable pairs to 5.98 M. */
    spfy_unit_table_t ut;
    rc = spfy_unit_table_load(&vin, &ut);
    if (rc != SPFY_OK) {
        fprintf(stderr, "cannot load unit table (%d)\n", rc);
        spfy_vin_free(&vin);
        return 1;
    }
    const uint32_t n_units = ut.n_units;

    printf("in   : %s\n", vin_path);
    printf("       n_rows=%u  n_cells=%u  n_units=%u\n",
           h.n_rows, h.n_cells, n_units);

    /* --- invert the addressing rule to recover the pair set --- */
    size_t live = 0;
    for (uint32_t i = 0; i < h.n_cells; ++i)
        if (spfy_hash_cell_a(&h, i) != SPFY_HASH_EMPTY_KEY) ++live;

    spfy_hash_pair *pairs = (spfy_hash_pair *)malloc(live * sizeof *pairs);
    if (!pairs) {
        fprintf(stderr, "out of memory for %zu pairs\n", live);
        spfy_vin_free(&vin);
        return 1;
    }

    size_t np = 0, bad = 0, ghost_in = 0;
    for (uint32_t i = 0; i < h.n_cells; ++i) {
        uint32_t r = spfy_hash_cell_a(&h, i);
        if (r == SPFY_HASH_EMPTY_KEY) continue;
        if (r >= h.n_rows || r >= n_units) { ++bad; continue; }
        uint32_t base = spfy_hash_row(&h, r);
        if (i < base) { ++bad; continue; }
        uint32_t l = i - base;
        if (l >= n_units) { ++bad; continue; }
        /* A zero cost that is not an adjacency is not a pair any vendor
         * writes: cost == 0 <=> r == l + 1, exactly, on both jill and tom.
         * Counting them separately is what distinguishes "we packed a domain
         * we did not intend" from "we packed it badly". */
        if (r != l + 1u && spfy_hash_cell_b(&h, i) == 0.0f) ++ghost_in;
        pairs[np].uid_right = r;
        pairs[np].uid_left  = l;
        pairs[np].cost      = spfy_hash_cell_b(&h, i);
        ++np;
    }
    printf("       live cells=%zu  recovered pairs=%zu  unreachable=%zu\n",
           live, np, bad);
    if (bad) printf("       WARNING: %zu live cells no lookup can reach "
                    "(%.1f%% of live, %.1f MB)\n",
                    bad, 100.0 * (double)bad / (double)(live ? live : 1),
                    (double)bad * 8.0 / (1024.0 * 1024.0));
    if (ghost_in) printf("       WARNING: %zu resolvable cells break "
                         "cost==0 <=> r==l+1 -- free joins the vendors "
                         "never grant\n", ghost_in);

    if (dump_pairs) {
        FILE *f = fopen(dump_pairs, "wb");
        if (!f) {
            fprintf(stderr, "cannot write %s\n", dump_pairs);
        } else {
            fprintf(f, "#uid_left\tuid_right\tcost\n");
            for (size_t k = 0; k < np; ++k)
                fprintf(f, "%u\t%u\t%.9g\n", pairs[k].uid_left,
                        pairs[k].uid_right, (double)pairs[k].cost);
            fclose(f);
            printf("       dumped %zu pairs to %s\n", np, dump_pairs);
        }
    }

    /* Which rows the pair set actually populates. */
    uint8_t *occupied = (uint8_t *)calloc(h.n_rows, 1);
    if (!occupied) {
        fprintf(stderr, "out of memory\n");
        free(pairs); spfy_vin_free(&vin);
        return 1;
    }
    size_t n_occupied = 0;
    for (size_t k = 0; k < np; ++k) {
        if (!occupied[pairs[k].uid_right]) { occupied[pairs[k].uid_right] = 1; ++n_occupied; }
    }
    printf("       non-empty rows=%zu of %u\n", n_occupied, h.n_rows);

    /* --- repack, once per candidate ordering --- */
    const spfy_hash_order orders[2] = { SPFY_HASH_ORDER_FFD, SPFY_HASH_ORDER_ROW };
    const char *order_name[2] = { "ffd (largest row first)", "row (ascending uid_right)" };
    spfy_hash_table t;
    int best = 0;
    memset(&t, 0, sizeof t);

    for (int oi = 0; oi < 2; ++oi) {
        spfy_hash_table cand;
        clock_t t0 = clock();
        rc = spfy_hash_build(pairs, np, h.n_rows, orders[oi], &cand);
        double secs = (double)(clock() - t0) / (double)CLOCKS_PER_SEC;
        if (rc != SPFY_OK) {
            fprintf(stderr, "pack failed for %s (%d)\n", order_name[oi], rc);
            continue;
        }
        /* Only non-empty rows are informative: an empty row carries
         * displacement 0 in both tables and would match by construction. */
        size_t rows_same = 0;
        for (uint32_t r = 0; r < cand.n_rows; ++r)
            if (occupied[r] && cand.rows[r] == spfy_hash_row(&h, r)) ++rows_same;

        printf("pack : %-26s %6.2fs  n_cells=%u (%+ld)  fill=%.1f%%  "
               "non-empty rows[] match %zu/%zu (%.2f%%)\n",
               order_name[oi], secs, cand.n_cells,
               (long)cand.n_cells - (long)h.n_cells,
               100.0 * (double)np / (double)cand.n_cells,
               rows_same, n_occupied,
               100.0 * (double)rows_same / (double)n_occupied);

        if (t.rows == NULL || cand.n_cells < t.n_cells) {
            spfy_hash_table_free(&t);
            t = cand;
            best = oi;
        } else {
            spfy_hash_table_free(&cand);
        }
    }
    if (!t.rows) {
        fprintf(stderr, "no packing succeeded\n");
        free(pairs);
        spfy_vin_free(&vin);
        return 1;
    }
    printf("out  : using %s\n", order_name[best]);
    printf("       orig fill = %.1f%%\n", 100.0 * (double)np / (double)h.n_cells);

    /* --- semantic check --- */
    size_t miss = 0, wrong = 0;
    for (size_t k = 0; k < np; ++k) {
        uint32_t r = pairs[k].uid_right, l = pairs[k].uid_left;
        size_t idx = (size_t)t.rows[r] + l;
        if (idx >= t.n_cells || t.key[idx] != r) { ++miss; continue; }
        if (memcmp(&t.cost[idx], &pairs[k].cost, sizeof(float)) != 0) ++wrong;
    }
    printf("check: pairs resolving=%zu/%zu  wrong cost=%zu\n", np - miss, np, wrong);

    /* No cell may claim a row that does not own it. */
    size_t ghosts = 0;
    for (uint32_t i = 0; i < t.n_cells; ++i) {
        uint32_t r = t.key[i];
        if (r == SPFY_HASH_EMPTY_KEY) continue;
        if (r >= t.n_rows || i < t.rows[r]) { ++ghosts; continue; }
    }
    printf("       ghost cells=%zu\n", ghosts);

    int semantic_ok = (miss == 0 && wrong == 0 && ghosts == 0 && bad == 0
                       && ghost_in == 0);

    /* --- byte check --- */
    uint8_t *ser = NULL;
    size_t   ser_n = 0;
    rc = spfy_hash_serialise(&t, &ser, &ser_n);
    if (rc != SPFY_OK) {
        fprintf(stderr, "serialise failed (%d)\n", rc);
        free(pairs);
        spfy_hash_table_free(&t);
        spfy_vin_free(&vin);
        return 1;
    }

    int byte_ok = (ser_n == vin.hash_n) && !memcmp(ser, vin.hash, ser_n);
    printf("       serialised=%zu bytes (orig %zu)\n", ser_n, vin.hash_n);

    if (!byte_ok && ser_n == vin.hash_n) {
        size_t first = 0;
        while (first < ser_n && ser[first] == vin.hash[first]) ++first;
        printf("       first byte difference at 0x%zx\n", first);
        size_t rows_same = 0;
        for (uint32_t r = 0; r < t.n_rows; ++r)
            if (occupied[r] && t.rows[r] == spfy_hash_row(&h, r)) ++rows_same;
        printf("       non-empty rows[] identical: %zu/%zu (%.2f%%)\n",
               rows_same, n_occupied,
               100.0 * (double)rows_same / (double)n_occupied);
    }

    printf("RESULT: SEMANTIC %s, BYTE %s\n",
           semantic_ok ? "PASS" : "FAIL",
           byte_ok ? "IDENTICAL" : "DIFFERS");

    free(ser);
    free(occupied);
    free(pairs);
    spfy_hash_table_free(&t);
    spfy_vin_free(&vin);
    return semantic_ok ? 0 : 1;
}
