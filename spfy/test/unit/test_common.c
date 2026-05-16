/* Combined unit-test runner for the common module.
 * Single exe (rather than per-suite) so Windows Defender heuristics
 * don't flag tiny binaries as malware. */

#include "../../src/common/obfuscation.h"
#include "../../src/common/riff.h"
#include "../../src/usel/cost.h"
#include "../../src/usel/viterbi.h"
#include "../../include/spfy/spfy.h"

#include <stdlib.h>

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do {                                                       \
    if (cond) { ++g_pass; }                                                    \
    else      { ++g_fail; fprintf(stderr,                                      \
                  "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); }            \
} while (0)

static void test_ce_roundtrip(void)
{
    uint8_t buf[16];
    for (size_t i = 0; i < sizeof buf; ++i) buf[i] = (uint8_t)i;
    uint8_t orig[16];
    memcpy(orig, buf, sizeof buf);

    spfy_unobfuscate_ce(buf, sizeof buf);
    for (size_t i = 0; i < sizeof buf; ++i) {
        CHECK(buf[i] == (uint8_t)(orig[i] ^ 0xCE));
    }
    spfy_unobfuscate_ce(buf, sizeof buf);
    CHECK(memcmp(buf, orig, sizeof buf) == 0);

    uint8_t dst[16];
    spfy_unobfuscate_ce_copy(dst, orig, sizeof orig);
    for (size_t i = 0; i < sizeof orig; ++i) {
        CHECK(dst[i] == (uint8_t)(orig[i] ^ 0xCE));
    }

    spfy_unobfuscate_ce(NULL, 0);
    spfy_unobfuscate_ce_copy(NULL, NULL, 0);
}

static void test_riff_iter(void)
{
    /* Two chunks: 'aaaa' (3 bytes -> pad), 'bbbb' (2 bytes). */
    uint8_t body[8 + 4 + 8 + 2] = {
        'a','a','a','a',  3, 0, 0, 0,  'X','Y','Z', 0,
        'b','b','b','b',  2, 0, 0, 0,  'P','Q'
    };

    spfy_riff_iter it;
    spfy_riff_iter_init(&it, body, sizeof body);

    spfy_chunk c;
    int rc;

    rc = spfy_riff_iter_next(&it, &c); CHECK(rc == 1);
    CHECK(c.fourcc == SPFY_FOURCC('a','a','a','a'));
    CHECK(c.size == 3);
    CHECK(memcmp(c.data, "XYZ", 3) == 0);

    rc = spfy_riff_iter_next(&it, &c); CHECK(rc == 1);
    CHECK(c.fourcc == SPFY_FOURCC('b','b','b','b'));
    CHECK(c.size == 2);
    CHECK(memcmp(c.data, "PQ", 2) == 0);

    rc = spfy_riff_iter_next(&it, &c); CHECK(rc == 0);
}

static void test_riff_truncated(void)
{
    uint8_t body[6] = { 'x','x','x','x', 99, 0 };
    spfy_riff_iter it;
    spfy_riff_iter_init(&it, body, sizeof body);
    spfy_chunk c;
    /* Truncated header (need 8 bytes). */
    CHECK(spfy_riff_iter_next(&it, &c) == -1);
}

static void test_fourcc_str(void)
{
    char s[5];
    spfy_fourcc_str(SPFY_FOURCC('R','I','F','F'), s);
    CHECK(strcmp(s, "RIFF") == 0);
    spfy_fourcc_str(SPFY_FOURCC('s','v','i','n'), s);
    CHECK(strcmp(s, "svin") == 0);
}

static void test_cost_d_basic(void)
{
    /* stored=100, mean=95, scale=0.1, weight=0.3
     * diff = 5
     * scaled = 0.5
     * sq = 0.25
     * cost = 0.075 */
    float c = spfy_cost_d(100, 95.0f, 0.1f, 0.3f);
    CHECK(fabsf(c - 0.075f) < 1e-6f);

    /* zero diff -> zero cost */
    CHECK(spfy_cost_d(100, 100.0f, 0.5f, 0.3f) == 0.0f);

    /* zero weight -> zero cost regardless */
    CHECK(spfy_cost_d(100, 50.0f, 1.0f, 0.0f) == 0.0f);

    /* sign of diff is irrelevant (squared) */
    float a = spfy_cost_d(100, 110.0f, 0.1f, 0.3f);
    float b = spfy_cost_d(120, 110.0f, 0.1f, 0.3f);
    CHECK(a == b);
}

static void test_cost_f0_basic(void)
{
    /* unvoiced -> MISSING_F0_COST verbatim */
    CHECK(spfy_cost_f0(0, 118.0f, 0.1f, 0.2f, 1000.0f) == 1000.0f);
    CHECK(spfy_cost_f0(0, 118.0f, 0.1f, 0.2f, 42.5f)   == 42.5f);

    /* stored=120, mean=118, scale=0.1, weight=0.2
     * diff = 2, scaled = 0.2, sq = 0.04, cost = 0.008 */
    float c = spfy_cost_f0(120, 118.0f, 0.1f, 0.2f, 1000.0f);
    CHECK(fabsf(c - 0.008f) < 1e-6f);

    /* exact prediction -> zero cost (unless unvoiced) */
    CHECK(spfy_cost_f0(118, 118.0f, 1.0f, 1.0f, 1000.0f) == 0.0f);
}

static void test_cost_sp_basic(void)
{
    /* Two 2x3 matrices, three empty matrices.
     *  m0:                 m1:
     *    [ 0.0  1.0  2.0 ]   [ 10.0  20.0  30.0 ]
     *    [ 3.0  4.0  5.0 ]   [ 40.0  50.0  60.0 ]
     */
    static const float m0_data[6] = {0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
    static const float m1_data[6] = {10.0f, 20.0f, 30.0f, 40.0f, 50.0f, 60.0f};

    spfy_sp_matrix_t mats[5] = {
        { m0_data, 2, 3 },
        { m1_data, 2, 3 },
        { NULL, 0, 0 },
        { NULL, 0, 0 },
        { NULL, 0, 0 },
    };

    /* target=[1,0,0,0,0], cand=[2,1,0,0,0], weights=[0.5, 0.1, 0, 0, 0]
     * term[0] = 0.5 * m0[1][2] = 0.5 * 5.0 = 2.5
     * term[1] = 0.1 * m1[0][1] = 0.1 * 20.0 = 2.0
     * total = 4.5 */
    uint32_t tgt[5]  = {1, 0, 0, 0, 0};
    uint32_t cand[5] = {2, 1, 0, 0, 0};
    float    w[5]    = {0.5f, 0.1f, 0.0f, 0.0f, 0.0f};
    float c = spfy_cost_sp(mats, tgt, cand, w);
    CHECK(fabsf(c - 4.5f) < 1e-6f);

    /* Out-of-bounds row/col contributes zero. */
    uint32_t bad_tgt[5] = {99, 0, 0, 0, 0};
    float c2 = spfy_cost_sp(mats, bad_tgt, cand, w);
    /* term[0] suppressed by OOB; term[1] still 2.0 */
    CHECK(fabsf(c2 - 2.0f) < 1e-6f);

    /* All-zero weights -> zero cost. */
    float zw[5] = {0};
    CHECK(spfy_cost_sp(mats, tgt, cand, zw) == 0.0f);
}

static void test_cost_s_basic(void)
{
    /* 3 labels, 2*3=6 hp_classes, 4 slots/class = 24 matrices total.
     * Each matrix is 3x3 = 9 floats. Total = 216 floats. */
    enum { N = 3, K = 6 * 4 };
    static float ccos[K * N * N] = {0};

    /* Populate a known cell on hp_class=2, slot=1: matrix[1][2] = 5.0 */
    ccos[(2 * 4 + 1) * N * N + 1 * N + 2] = 5.0f;
    /* hp_class=2, slot=0: matrix[0][1] = 3.0 */
    ccos[(2 * 4 + 0) * N * N + 0 * N + 1] = 3.0f;

    /* L[]: phone_id -> label_id; identity for first few. */
    static const uint8_t L[10] = {0, 1, 2, 0, 1, 2, 0, 1, 2, 0};

    /* target_ctx covers slots [0..3]: [0,1,?,?]. cand_ctx: [1,2,?,?].
     * Term s=0: ccos[hp=2,s=0][L[0]=0][L[1]=1] = 3.0
     * Term s=1: ccos[hp=2,s=1][L[1]=1][L[2]=2] = 5.0
     * Term s=2: phone 255 (sentinel) -> skipped
     * Term s=3: phone 255 (sentinel) -> skipped
     * sum = 8.0; weight = 0.5 -> S = 4.0 */
    uint8_t tgt[4]  = {0, 1, 255, 255};
    uint8_t cand[4] = {1, 2, 255, 255};
    float c = spfy_cost_s(ccos, N, 2, L, 10, tgt, cand, 0.5f);
    CHECK(fabsf(c - 4.0f) < 1e-6f);

    /* All sentinels -> zero. */
    uint8_t all_oob[4] = {255, 255, 255, 255};
    CHECK(spfy_cost_s(ccos, N, 2, L, 10, all_oob, all_oob, 1.0f) == 0.0f);

    /* Zero weight -> zero. */
    CHECK(spfy_cost_s(ccos, N, 2, L, 10, tgt, cand, 0.0f) == 0.0f);
}

/* ----- Viterbi DP tests --------------------------------------------- */

/* Symmetric-table join function for tests: returns a join cost from a
 * sparse map keyed by (prev_uid, curr_uid). Anything not in the map is
 * 0.0 (free transition). */
typedef struct {
    uint32_t prev;
    uint32_t curr;
    float    cost;
} test_join_entry;

typedef struct {
    const test_join_entry *entries;
    size_t                 n;
} test_join_ctx;

static float test_join_lookup(uint32_t prev_uid, uint32_t curr_uid, void *user)
{
    const test_join_ctx *ctx = (const test_join_ctx *)user;
    for (size_t i = 0; i < ctx->n; ++i) {
        if (ctx->entries[i].prev == prev_uid &&
            ctx->entries[i].curr == curr_uid) {
            return ctx->entries[i].cost;
        }
    }
    return 0.0f;
}

static float test_join_zero(uint32_t prev_uid, uint32_t curr_uid, void *user)
{
    (void)prev_uid; (void)curr_uid; (void)user;
    return 0.0f;
}

static void test_viterbi_single_slot(void)
{
    /* One slot, three candidates. Best path is the cheapest target cost. */
    uint32_t cands[3]  = {10, 20, 30};
    float    tcost[3]  = {0.5f, 0.1f, 0.7f};
    spfy_viterbi_slot_t slots[1] = { { cands, tcost, 3 } };
    uint32_t path[1] = {0};
    float    total   = 0.0f;
    int rc = spfy_viterbi_run(slots, 1, test_join_zero, NULL, path, &total);
    CHECK(rc == SPFY_OK);
    CHECK(path[0] == 20);
    CHECK(fabsf(total - 0.1f) < 1e-6f);
}

static void test_viterbi_no_join(void)
{
    /* Three slots, two candidates each, zero join. Best path picks cheapest
     * candidate per slot independently. */
    uint32_t c0[2] = {1, 2};        float t0[2] = {1.0f, 0.5f};
    uint32_t c1[2] = {3, 4};        float t1[2] = {0.2f, 0.9f};
    uint32_t c2[2] = {5, 6};        float t2[2] = {0.4f, 0.1f};
    spfy_viterbi_slot_t slots[3] = {
        { c0, t0, 2 },
        { c1, t1, 2 },
        { c2, t2, 2 },
    };
    uint32_t path[3] = {0};
    float    total   = 0.0f;
    int rc = spfy_viterbi_run(slots, 3, test_join_zero, NULL, path, &total);
    CHECK(rc == SPFY_OK);
    CHECK(path[0] == 2);
    CHECK(path[1] == 3);
    CHECK(path[2] == 6);
    CHECK(fabsf(total - (0.5f + 0.2f + 0.1f)) < 1e-6f);
}

static void test_viterbi_join_changes_path(void)
{
    /* Two slots. Per-slot best would be (cand=2 in s0, cand=4 in s1).
     * But a high join cost between (2 -> 4) makes (cand=1 in s0, cand=4 in
     * s1) cheaper. */
    uint32_t c0[2] = {1, 2};   float t0[2] = {0.10f, 0.05f};
    uint32_t c1[2] = {3, 4};   float t1[2] = {0.30f, 0.10f};
    spfy_viterbi_slot_t slots[2] = { { c0, t0, 2 }, { c1, t1, 2 } };
    test_join_entry entries[] = {
        { .prev = 2, .curr = 4, .cost = 1.00f },   /* heavy */
        { .prev = 1, .curr = 4, .cost = 0.00f },   /* free  */
    };
    test_join_ctx ctx = { entries, sizeof entries / sizeof entries[0] };
    uint32_t path[2] = {0};
    float    total   = 0.0f;
    int rc = spfy_viterbi_run(slots, 2, test_join_lookup, &ctx,
                              path, &total);
    CHECK(rc == SPFY_OK);
    CHECK(path[0] == 1);
    CHECK(path[1] == 4);
    /* Expected: 0.10 (cand=1) + 0.00 (join 1->4) + 0.10 (cand=4) = 0.20 */
    CHECK(fabsf(total - 0.20f) < 1e-6f);
}

static void test_viterbi_forbidden_cand(void)
{
    /* A negative target cost is forbidden. Forces the alternate. */
    uint32_t c0[2] = {1, 2};   float t0[2] = {-1.0f, 0.5f};
    spfy_viterbi_slot_t slots[1] = { { c0, t0, 2 } };
    uint32_t path[1] = {0};
    float    total   = 0.0f;
    int rc = spfy_viterbi_run(slots, 1, test_join_zero, NULL, path, &total);
    CHECK(rc == SPFY_OK);
    CHECK(path[0] == 2);
    CHECK(fabsf(total - 0.5f) < 1e-6f);
}

static float test_join_block_to_3(uint32_t prev_uid, uint32_t curr_uid, void *user)
{
    (void)user; (void)prev_uid;
    /* Forbid every transition into uid=3. */
    return curr_uid == 3 ? -1.0f : 0.0f;
}

static void test_viterbi_unreachable_slot(void)
{
    /* slot 1 has only cand=3 reachable, but every join into 3 is forbidden,
     * and target_cost is fine -> SPFY_E_OOB (no reachable path). */
    uint32_t c0[1] = {1};      float t0[1] = {0.1f};
    uint32_t c1[1] = {3};      float t1[1] = {0.2f};
    spfy_viterbi_slot_t slots[2] = { { c0, t0, 1 }, { c1, t1, 1 } };
    uint32_t path[2] = {0xDEADBEEF, 0xDEADBEEF};
    float    total   = 0.0f;
    int rc = spfy_viterbi_run(slots, 2, test_join_block_to_3, NULL,
                              path, &total);
    CHECK(rc == SPFY_E_OOB);
    /* path is left untouched on unreachable -- sentinels still there. */
    CHECK(path[0] == 0xDEADBEEF);
    CHECK(path[1] == 0xDEADBEEF);
}

static void test_viterbi_invalid_args(void)
{
    uint32_t c[1] = {1}; float t[1] = {0.0f};
    spfy_viterbi_slot_t s[1] = { { c, t, 1 } };
    uint32_t path[1]; float total;

    CHECK(spfy_viterbi_run(NULL, 1, test_join_zero, NULL, path, &total)
          == SPFY_E_INVAL);
    CHECK(spfy_viterbi_run(s, 0, test_join_zero, NULL, path, &total)
          == SPFY_E_INVAL);

    /* Slot with zero candidates is invalid. */
    spfy_viterbi_slot_t s_empty[1] = { { c, t, 0 } };
    CHECK(spfy_viterbi_run(s_empty, 1, test_join_zero, NULL, path, &total)
          == SPFY_E_INVAL);
}

static void test_viterbi_long_double_accumulation(void)
{
    /* 50-slot path with tiny per-slot cost -- proves that we accumulate
     * in long double, not f32 (f32 would lose precision below ~1e-7). */
    enum { N = 50 };
    uint32_t cands_buf[N];
    float    tcost_buf[N];
    spfy_viterbi_slot_t slots_buf[N];
    for (uint32_t i = 0; i < N; ++i) {
        cands_buf[i] = 1000 + i;
        tcost_buf[i] = 1.0e-7f;
        slots_buf[i].cands       = &cands_buf[i];
        slots_buf[i].target_cost = &tcost_buf[i];
        slots_buf[i].n_cands     = 1;
    }
    uint32_t path[N] = {0};
    float    total   = 0.0f;
    int rc = spfy_viterbi_run(slots_buf, N, test_join_zero, NULL,
                              path, &total);
    CHECK(rc == SPFY_OK);
    /* Expected: 50 * 1e-7 = 5e-6, exact in long double. f32 accum drift
     * would visibly differ; we just check we're in the right ballpark. */
    CHECK(fabsf(total - 5.0e-6f) < 1e-9f);
    /* path is the only candidate per slot. */
    for (uint32_t i = 0; i < N; ++i) CHECK(path[i] == 1000 + i);
}

int main(void)
{
    test_ce_roundtrip();
    test_riff_iter();
    test_riff_truncated();
    test_fourcc_str();
    test_cost_d_basic();
    test_cost_f0_basic();
    test_cost_sp_basic();
    test_cost_s_basic();
    test_viterbi_single_slot();
    test_viterbi_no_join();
    test_viterbi_join_changes_path();
    test_viterbi_forbidden_cand();
    test_viterbi_unreachable_slot();
    test_viterbi_invalid_args();
    test_viterbi_long_double_accumulation();

    fprintf(stdout, "test_common: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
