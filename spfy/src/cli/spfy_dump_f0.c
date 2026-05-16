/* spfy_dump_f0 — print Tom's per-unit F0 byte distribution.
 *
 * Uses the production unit-table loader so we never desync from the
 * actual on-disk row format. Output is a one-line summary of f0_start,
 * f0_end, f0_mid byte stats (min/max/median + 1/5/50/95/99 percentile)
 * plus the semitone range available above and below median. Used once
 * to size the unit-selection-only pitch range before falling back to
 * PSOLA for wider shifts. */

#include <spfy/spfy.h>
#include "../voice/voice.h"
#include "../voice/unit_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int u8cmp(const void *a, const void *b) {
    return (int)*(const uint8_t *)a - (int)*(const uint8_t *)b;
}

static void stats(const char *label, uint8_t *vals, size_t n) {
    /* Filter out 0 (no F0 marker = unvoiced unit). */
    size_t k = 0;
    for (size_t i = 0; i < n; ++i) if (vals[i]) vals[k++] = vals[i];
    if (k == 0) { printf("%s: empty\n", label); return; }
    qsort(vals, k, 1, u8cmp);
    uint8_t lo1  = vals[k /  100];
    uint8_t lo5  = vals[k /  20];
    uint8_t med  = vals[k /   2];
    uint8_t hi95 = vals[(k * 19) / 20];
    uint8_t hi99 = vals[(k * 99) / 100];
    double sem_above = 12.0 * log2((double)hi99 / (double)med);
    double sem_below = 12.0 * log2((double)med / (double)lo1);
    printf("%s: n_nz=%zu  min=%u  1%%=%u  5%%=%u  med=%u  95%%=%u  99%%=%u  max=%u  "
           "(sem_above_med=%.2f, sem_below_med=%.2f)\n",
           label, k, vals[0], lo1, lo5, med, hi95, hi99, vals[k - 1],
           sem_above, sem_below);
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <voice.vin>\n", argv[0]); return 2; }
    spfy_vin_t vin = {0};
    if (spfy_vin_load(argv[1], &vin) != SPFY_OK) {
        fprintf(stderr, "vin load failed\n"); return 1;
    }
    spfy_unit_table_t ut = {0};
    if (spfy_unit_table_load(&vin, &ut) != SPFY_OK) {
        fprintf(stderr, "unit load failed\n"); return 1;
    }
    printf("n_units = %u\n", ut.n_units);

    uint8_t *fs = (uint8_t *)malloc(ut.n_units);
    uint8_t *fe = (uint8_t *)malloc(ut.n_units);
    uint8_t *fm = (uint8_t *)malloc(ut.n_units);
    for (uint32_t u = 0; u < ut.n_units; ++u) {
        spfy_unit_record_t r;
        if (spfy_unit_record_get(&ut, u, &r) != SPFY_OK) {
            fs[u] = fe[u] = fm[u] = 0; continue;
        }
        fs[u] = r.f0_start; fe[u] = r.f0_end; fm[u] = r.f0_mid;
    }
    stats("f0_start", fs, ut.n_units);
    stats("f0_end  ", fe, ut.n_units);
    stats("f0_mid  ", fm, ut.n_units);
    free(fs); free(fe); free(fm);
    return 0;
}
