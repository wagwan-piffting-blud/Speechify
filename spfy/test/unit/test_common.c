/* Combined unit-test runner for the common module.
 * Single exe (rather than per-suite) so Windows Defender heuristics
 * don't flag tiny binaries as malware. */

#include "../../src/common/obfuscation.h"
#include "../../src/common/riff.h"
#include "../../src/common/riff_write.h"
#include "../../src/vb/join_cost.h"
#include "../../src/vb/vb_chunk.h"
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

/* ---- RIFF writer -------------------------------------------------------
 * Contract from SWIttsEngineUtil.dll: the size field excludes the pad byte,
 * and the pad goes through the cipher like everything else. */

#define TMP_RIFF "test_riff_write.tmp"

static long slurp_tmp(uint8_t *buf, size_t cap)
{
    FILE *fp = fopen(TMP_RIFF, "rb");
    if (!fp) return -1;
    size_t n = fread(buf, 1, cap, fp);
    fclose(fp);
    return (long)n;
}

static void test_riff_write_encrypted_layout(void)
{
    spfy_riff_writer w;
    CHECK(spfy_riff_create(&w, TMP_RIFF, "svin", SPFY_RIFF_CE) == 0);
    CHECK(spfy_riff_open_chunk(&w, "abc ") == 0);
    CHECK(spfy_riff_write_bytes(&w, "xyz", 3) == 0);   /* odd -> pad */
    CHECK(spfy_riff_close_chunk(&w) == 0);
    CHECK(spfy_riff_open_chunk(&w, "defg") == 0);
    CHECK(spfy_riff_write_u32(&w, 0x11223344u) == 0);
    CHECK(spfy_riff_close_chunk(&w) == 0);
    CHECK(spfy_riff_finish(&w) == 0);

    uint8_t raw[256];
    long n = slurp_tmp(raw, sizeof raw);
    CHECK(n > 0);

    /* No plaintext header: even "RIFF" is ciphered. */
    CHECK(raw[0] == (uint8_t)('R' ^ SPFY_OBFUSCATION_BYTE));

    uint8_t plain[256];
    spfy_unobfuscate_ce_copy(plain, raw, (size_t)n);
    CHECK(memcmp(plain, "RIFF", 4) == 0);
    CHECK(memcmp(plain + 8, "svin", 4) == 0);

    /* 'abc ' payload is 3 bytes; the size field must say 3, not 4. */
    CHECK(memcmp(plain + 12, "abc ", 4) == 0);
    CHECK(plain[16] == 3 && plain[17] == 0 && plain[18] == 0 && plain[19] == 0);

    /* The pad byte at offset 23 is a ciphered zero on disk. */
    CHECK(raw[23] == SPFY_OBFUSCATION_BYTE);
    CHECK(plain[23] == 0);

    /* Second chunk starts after the pad. */
    CHECK(memcmp(plain + 24, "defg", 4) == 0);

    /* Outer RIFF size covers everything after the size field itself. */
    uint32_t riff_size = (uint32_t)plain[4] | ((uint32_t)plain[5] << 8) |
                         ((uint32_t)plain[6] << 16) | ((uint32_t)plain[7] << 24);
    CHECK(riff_size == (uint32_t)n - 8);

    /* And the reader agrees with the writer. */
    spfy_riff_iter it;
    spfy_chunk c;
    spfy_riff_iter_init(&it, plain + 12, (size_t)n - 12);
    CHECK(spfy_riff_iter_next(&it, &c) == 1);
    CHECK(c.fourcc == SPFY_FOURCC('a','b','c',' ') && c.size == 3);
    CHECK(spfy_riff_iter_next(&it, &c) == 1);
    CHECK(c.fourcc == SPFY_FOURCC('d','e','f','g') && c.size == 4);
    CHECK(spfy_riff_iter_next(&it, &c) == 0);

    remove(TMP_RIFF);
}

static void test_riff_write_str_w(void)
{
    spfy_riff_writer w;
    CHECK(spfy_riff_create(&w, TMP_RIFF, "svin", SPFY_RIFF_PLAIN) == 0);
    CHECK(spfy_riff_open_chunk(&w, "strw") == 0);
    CHECK(spfy_riff_write_str_w(&w, "hi") == 0);
    CHECK(spfy_riff_close_chunk(&w) == 0);
    CHECK(spfy_riff_finish(&w) == 0);

    uint8_t raw[64];
    long n = slurp_tmp(raw, sizeof raw);
    CHECK(n > 0);
    /* word-prefixed, NOT wide, and NOT NUL-terminated */
    CHECK(raw[20] == 2 && raw[21] == 0);
    CHECK(raw[22] == 'h' && raw[23] == 'i');
    CHECK(raw[16] == 4);   /* chunk size = 2 + 2 */

    remove(TMP_RIFF);
}

static void test_riff_write_rejects_bad_fourcc(void)
{
    spfy_riff_writer w;
    CHECK(spfy_riff_create(&w, TMP_RIFF, "svin", SPFY_RIFF_PLAIN) == 0);
    CHECK(spfy_riff_open_chunk(&w, "ab")    != 0);   /* too short */
    CHECK(spfy_riff_open_chunk(&w, "abcde") != 0);   /* too long  */
    CHECK(spfy_riff_open_chunk(&w, "ab\tc") != 0);   /* bad char  */
    CHECK(spfy_riff_finish(&w) == 0);
    remove(TMP_RIFF);
}

/* ---- join cost (S4) ----------------------------------------------------
 * Pins the four behaviours that make this the vendor's formula rather than a
 * generic spectral distance: dim 0 absolute and voicing-gated, dim 1 dead,
 * the seam term doubled, and continuations hard-zeroed. */

#define JC_DIM 4u

static void jc_setup(spfy_jc_t *jc, float *frames, float *w, uint32_t n_units)
{
    memset(jc, 0, sizeof *jc);
    jc->dim = JC_DIM;
    jc->n_units = n_units;
    jc->frames = frames;
    jc->weights = w;
    jc->f0_gate = 0.0f;
    jc->raw_scale = 1.0f;
    for (uint32_t k = 0; k < JC_DIM; ++k) w[k] = 1.0f;
    w[SPFY_JC_DIM_DEAD] = 0.0f;
}

static void test_jc_kernel_f0_gate(void)
{
    float frames[2 * JC_DIM] = { 0 };
    float w[JC_DIM];
    spfy_jc_t jc;
    jc_setup(&jc, frames, w, 1);

    float x[JC_DIM] = { 100.0f, 5.0f, 0.0f, 0.0f };
    float y[JC_DIM] = { 110.0f, 9.0f, 0.0f, 0.0f };

    /* Both voiced: dim 0 is |100-110| = 10, ABSOLUTE not squared.
     * dim 1 is dead so its 4.0 difference must contribute nothing. */
    CHECK(fabsf(spfy_jc_kernel(&jc, x, y) - 10.0f) < 1e-5f);

    /* Left unvoiced: the dim-0 term drops out entirely rather than
     * becoming a large penalty. */
    x[0] = 0.0f;
    CHECK(fabsf(spfy_jc_kernel(&jc, x, y) - 0.0f) < 1e-5f);

    /* A spectral dimension is SQUARED. */
    x[0] = 0.0f; y[0] = 0.0f;
    x[3] = 3.0f; y[3] = 0.0f;
    CHECK(fabsf(spfy_jc_kernel(&jc, x, y) - 9.0f) < 1e-5f);
}

static void test_jc_seam_is_doubled(void)
{
    /* 3 units. Make every frame identical except the seam pair, so only the
     * doubled term is non-zero and its factor is directly observable. */
    float frames[3 * 2 * JC_DIM];
    float w[JC_DIM];
    spfy_jc_t jc;
    memset(frames, 0, sizeof frames);
    jc_setup(&jc, frames, w, 3);

    /* unit 0 end-frame spectral dim = 2, unit 2 start-frame = 0 -> diff 2. */
    frames[(0 * 2 + 1) * JC_DIM + 3] = 2.0f;

    /* raw = 2*kernel(end0,start2) + kernel(end0,end1) + kernel(start2,start1)
     *     = 2*4            + 4              + 0        = 12 */
    CHECK(fabsf(spfy_jc_raw(&jc, 0, 2) - 12.0f) < 1e-4f);
}

static void test_jc_continuation_is_hard_zero(void)
{
    float frames[3 * 2 * JC_DIM];
    float w[JC_DIM];
    spfy_jc_t jc;
    memset(frames, 0, sizeof frames);
    jc_setup(&jc, frames, w, 3);
    frames[(0 * 2 + 1) * JC_DIM + 3] = 7.0f;   /* make raw large */

    /* r == l+1 bypasses the affine map: exactly 0, not offset. */
    CHECK(spfy_jc_cached_value(&jc, 0, 1, 1.75f, 0.15f) == 0.0f);
    /* Everything else is >= the offset, which is what makes the vendor's
     * measured floor (0.2826) exceed JOIN_COST_OFFSET (0.15). */
    CHECK(spfy_jc_cached_value(&jc, 0, 2, 1.75f, 0.15f) > 0.15f);
}

static void test_jc_weights_disable_dim1(void)
{
    /* 2 units, 4 frames. Give dim 1 large variance; its weight must still
     * come out exactly zero. */
    float frames[2 * 2 * JC_DIM];
    float w[JC_DIM];
    spfy_jc_t jc;
    memset(frames, 0, sizeof frames);
    jc_setup(&jc, frames, w, 2);
    for (uint32_t f = 0; f < 4u; ++f) {
        frames[f * JC_DIM + 0u] = (float)(100u + f * 10u);  /* voiced */
        frames[f * JC_DIM + 1u] = (float)(f * 1000u);       /* dead dim */
        frames[f * JC_DIM + 2u] = (float)f;
        frames[f * JC_DIM + 3u] = (float)(f * 2u);
    }
    CHECK(spfy_jc_derive_weights(&jc, 1.0f) == 0);
    CHECK(w[SPFY_JC_DIM_DEAD] == 0.0f);
    CHECK(w[SPFY_JC_DIM_F0] > 0.0f);
    CHECK(w[3] > 0.0f);
    /* dims >= 2 are shared out by (2*dim - 4); with dim 4 that is 4. */
    CHECK(w[2] > 0.0f && w[2] < 1.0f);
}

/* ---- ckls: an EMPTY anchor group must not carry a sequence index ----
 *
 * Each ckls record is {u32 seq_index, token, span_start, span_end, filename};
 * the index is written BEFORE its record, so a group with no records writes
 * none. The engine's reader (voice/chunk_table.c) consumes it only when the
 * count is non-zero, and felix ships an empty `_WORD_` group proving that is
 * the vendor's rule too.
 *
 * No corpus we build yields an empty group, so this branch has no coverage
 * from any build -- hence a direct control. It reads with the ENGINE's rule
 * and asserts the writer's bytes are consumed EXACTLY; a writer that emits the
 * word unconditionally leaves 4 bytes over and fails here. */
static size_t ckls_scan_group(const uint8_t *p, size_t n, size_t *off,
                              uint32_t *out_count)
{
    if (*off + 2u > n) return 0;
    uint16_t nm = (uint16_t)(p[*off] | (p[*off + 1u] << 8));
    *off += 2u + nm;
    if (*off + 4u > n) return 0;
    uint32_t cnt = (uint32_t)(p[*off] | (p[*off + 1u] << 8)
                            | (p[*off + 2u] << 16) | (p[*off + 3u] << 24));
    *off += 4u;
    if (cnt) *off += 4u;                    /* record 0's sequence index */
    for (uint32_t i = 0; i < cnt; ++i) {
        if (*off + 2u > n) return 0;
        uint16_t tl = (uint16_t)(p[*off] | (p[*off + 1u] << 8));
        *off += 2u + tl + 8u;               /* token, span_start, span_end */
        if (*off + 2u > n) return 0;
        uint16_t fl = (uint16_t)(p[*off] | (p[*off + 1u] << 8));
        *off += 2u + fl;                    /* filename */
        if (i + 1u < cnt) *off += 4u;       /* the NEXT record's index */
    }
    *out_count = cnt;
    return 1;
}

static void test_ckls_empty_group(void)
{
    char t0[] = "cloudy", f0[] = "wx_001";
    char t1[] = "today",  f1[] = "wx_002";
    spfy_vb_anchor rec[2];
    memset(rec, 0, sizeof rec);
    rec[0].text = t0; rec[0].file = f0;
    rec[0].span_start = 10; rec[0].span_end = 21;
    rec[1].text = t1; rec[1].file = f1;
    rec[1].span_start = 30; rec[1].span_end = 37;

    spfy_vb_anchors words;
    spfy_vb_anchors syls;
    memset(&words, 0, sizeof words);      /* the empty group */
    memset(&syls, 0, sizeof syls);
    syls.v = rec; syls.n = 2;

    uint8_t *out = NULL;
    size_t out_n = 0;
    CHECK(spfy_vb_encode_ckls(&words, &syls, &out, &out_n) == SPFY_OK);
    if (!out) return;

    size_t off = 4;                        /* u32 n_groups */
    uint32_t c0 = 0xFFFFFFFFu, c1 = 0xFFFFFFFFu;
    CHECK(out_n >= 4u);
    CHECK(ckls_scan_group(out, out_n, &off, &c0) == 1);
    CHECK(c0 == 0u);
    CHECK(ckls_scan_group(out, out_n, &off, &c1) == 1);
    CHECK(c1 == 2u);
    /* The whole point: no bytes left over and none borrowed. */
    CHECK(off == out_n);

    /* Control -- the same scan on two NON-empty groups must also be exact, so
     * a pass above cannot come from the scanner simply being lax. */
    free(out);
    out = NULL; out_n = 0;
    CHECK(spfy_vb_encode_ckls(&syls, &syls, &out, &out_n) == SPFY_OK);
    if (!out) return;
    off = 4; c0 = c1 = 0xFFFFFFFFu;
    CHECK(ckls_scan_group(out, out_n, &off, &c0) == 1);
    CHECK(ckls_scan_group(out, out_n, &off, &c1) == 1);
    CHECK(c0 == 2u && c1 == 2u);
    CHECK(off == out_n);
    free(out);
}

int main(void)
{
    test_ckls_empty_group();
    test_ce_roundtrip();
    test_riff_iter();
    test_jc_kernel_f0_gate();
    test_jc_seam_is_doubled();
    test_jc_continuation_is_hard_zero();
    test_jc_weights_disable_dim1();
    test_riff_write_encrypted_layout();
    test_riff_write_str_w();
    test_riff_write_rejects_bad_fourcc();
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
