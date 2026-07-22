/* spfy_hash_replay -- M3.3 acceptance harness.
 *
 * Replays a captured hash_probe JSONL trace through our C hash table and
 * verifies every (uid_left, uid_right) probe gives the same hit/miss
 * result and the same f32 cost as the engine.
 *
 *   spfy_hash_replay <voice.vin> <hash_probes.jsonl>
 *
 * The JSONL is produced by:
 *   python spfy/test/oracle/run_frida_capture.py --hook hash_lookup ...
 *
 * Each line is one engine probe with fields: uid_left, uid_right, hit,
 * cost, stored. We re-run the lookup in C and compare.
 */

#include <spfy/spfy.h>

#include "../voice/voice.h"
#include "../usel/hash.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *find_key(const char *buf, const char *end, const char *key)
{
    size_t kn = strlen(key);
    char needle[64];
    if (kn + 3 >= sizeof needle) return NULL;
    needle[0] = '"';
    memcpy(needle + 1, key, kn);
    needle[kn + 1] = '"';
    needle[kn + 2] = ':';
    for (const char *p = buf; p + kn + 3 <= end; ++p) {
        if (memcmp(p, needle, kn + 3) == 0) return p + kn + 3;
    }
    return NULL;
}

static int parse_long_at(const char *p, const char *end, long *out)
{
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    char *e = NULL;
    long v = strtol(p, &e, 10);
    if (e == p || e > end) return -1;
    *out = v;
    return 0;
}

static int parse_double_at(const char *p, const char *end, double *out)
{
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    /* "null" -> NaN sentinel (we don't care about value on miss) */
    if (p + 4 <= end && memcmp(p, "null", 4) == 0) { *out = 0.0; return 1; }
    char *e = NULL;
    double v = strtod(p, &e);
    if (e == p || e > end) return -1;
    *out = v;
    return 0;
}

static int floats_equal(float a, float b)
{
    union { float f; uint32_t u; } ua, ub;
    ua.f = a; ub.f = b;
    return ua.u == ub.u;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <voice.vin> <hash_probes.jsonl>\n",
                argv[0]);
        return 2;
    }

    spfy_vin_t vin = {0};
    int rc = spfy_vin_load(argv[1], &vin);
    if (rc != SPFY_OK) {
        fprintf(stderr, "vin_load: %s\n", spfy_strerror(rc));
        return 1;
    }
    spfy_hash_t h = {0};
    rc = spfy_hash_load(&vin, &h);
    if (rc != SPFY_OK) {
        fprintf(stderr, "hash_load: %s\n", spfy_strerror(rc));
        spfy_vin_free(&vin);
        return 1;
    }
    fprintf(stdout, "loaded hash: n_rows=%u n_cells=%u\n",
            h.n_rows, h.n_cells);

    FILE *fp = fopen(argv[2], "rb");
    if (!fp) { fprintf(stderr, "open %s\n", argv[2]); return 1; }

    char buf[4096];
    long n_probes = 0, n_match = 0;
    long n_hit_match = 0, n_miss_match = 0;
    long n_hit_mismatch = 0, n_miss_mismatch = 0;
    long n_cost_diff = 0;

    while (fgets(buf, sizeof buf, fp)) {
        size_t len = strlen(buf);
        const char *end = buf + len;

        long uid_left = -1, uid_right = -1, hit_int = -1;
        double cost_eng = 0.0;

        const char *p = find_key(buf, end, "uid_left");
        if (!p || parse_long_at(p, end, &uid_left)) continue;
        p = find_key(buf, end, "uid_right");
        if (!p || parse_long_at(p, end, &uid_right)) continue;
        p = find_key(buf, end, "hit");
        if (!p) continue;
        /* "true" / "false" */
        while (p < end && (*p == ' ' || *p == '\t')) ++p;
        if (p + 4 <= end && memcmp(p, "true", 4) == 0)        hit_int = 1;
        else if (p + 5 <= end && memcmp(p, "false", 5) == 0)  hit_int = 0;
        else continue;
        p = find_key(buf, end, "cost");
        if (!p) continue;
        parse_double_at(p, end, &cost_eng);

        ++n_probes;

        float native_cost = 0.0f;
        int native_hit = (spfy_hash_lookup(&h, (uint32_t)uid_left,
                                           (uint32_t)uid_right,
                                           &native_cost) == SPFY_OK);

        if (hit_int == 1 && native_hit) {
            if (floats_equal(native_cost, (float)cost_eng)) {
                ++n_hit_match; ++n_match;
            } else {
                ++n_cost_diff;
                if (n_cost_diff <= 5) {
                    fprintf(stderr,
                        "cost diff: (%ld,%ld) native=%.9g engine=%.9g\n",
                        uid_left, uid_right, native_cost, cost_eng);
                }
            }
        } else if (hit_int == 0 && !native_hit) {
            ++n_miss_match; ++n_match;
        } else if (hit_int == 1 && !native_hit) {
            ++n_hit_mismatch;
            if (n_hit_mismatch <= 8) {
                fprintf(stderr,
                    "engine-hit native-miss: (l=%ld r=%ld) "
                    "engine_cost=%.6g  rows[r]=%u\n",
                    uid_left, uid_right, cost_eng,
                    (uint32_t)uid_right < h.n_rows
                       ? spfy_hash_row(&h, (uint32_t)uid_right)
                       : 0xFFFFFFFFu);
            }
        } else {
            ++n_miss_mismatch;
        }
    }
    fclose(fp);

    fprintf(stdout, "\nprobes total      : %ld\n", n_probes);
    /* Explicit (double) casts: `long` is 64-bit on 64-bit hosts and
     * -Wconversion flags the implicit widening. Probe counts never reach
     * 2^53, so the cast is exact. */
    fprintf(stdout, "hit/miss match    : %ld  (%.2f%%)\n",
            n_match,
            100.0 * (double)n_match / (double)(n_probes ? n_probes : 1));
    fprintf(stdout, "  hit  matched    : %ld\n", n_hit_match);
    fprintf(stdout, "  miss matched    : %ld\n", n_miss_match);
    fprintf(stdout, "engine HIT, native MISS  : %ld\n", n_hit_mismatch);
    fprintf(stdout, "engine MISS, native HIT  : %ld\n", n_miss_mismatch);
    fprintf(stdout, "cost differences (f32 ULP-strict) : %ld\n", n_cost_diff);

    spfy_hash_free(&h);
    spfy_vin_free(&vin);
    return (n_match == n_probes && n_cost_diff == 0) ? 0 : 1;
}
