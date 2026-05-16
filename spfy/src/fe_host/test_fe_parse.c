/*
 * spfy/src/fe_host/test_fe_parse.c — exercise the FE-output parser
 * against the empirically captured tagged streams.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Builds a tiny stand-alone executable independent of the rest of the
 * spfy stack. Compile with:
 *
 *   gcc -Wall -Wextra -O2 -I../../include -I../fe \
 *       test_fe_parse.c fe_parse.c -o test_fe_parse
 *
 * Runs all bundled test cases and reports PASS/FAIL counts.
 */

#include "fe_parse.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * Captured fixtures
 * ============================================================ */

/* Capture 1 — "Hello, world." (host/PROTOCOL.md / vt_001.jsonl
 * concatenated). The trailing "%% " is just the stream-end marker. */
static const char FIXTURE_HELLO[] =
    "%% #{, pau(p25) <hello (0,6) interj,2 "
    "[.1,H* hh(p100) eh(p100) l(p100) ow(p100) ] >"
    " pau(p50) <world (7,5) noun,2 [.1,H*;L-L% w(p100) er(p100) l(p100) d(p100) ] >"
    " pau(p50) } %%";

/* Capture 2 — the pangram, full text reconstructed from vt_long.jsonl. */
static const char FIXTURE_PANGRAM[] =
    "%% #{. pau(p25) "
    "<the (0,3) det,0 [.0 dh(p100) ix(p100) ] > "
    "<quick (4,5) adj,1 [.1,H* k(p100) w(p100) ih(p100) k(p100) ] > "
    "<brown (10,5) adj,1 [.1,H* b(p100) r(p100) aw(p100) n(p100) ] > "
    "<fox (16,3) noun,1 [.1,H* f(p100) aa(p100) k(p100) s(p100) ] > "
    "<jumps (20,5) noun_verb,1 [.1,H* jh(p100) ah(p100) m(p100) p(p100) s(p100) ] > "
    "<over (26,4) prep,1 [.1,H* ow(p100) .0 v(p100) er(p100) ] > "
    "<the (31,3) det,0 [.0 dh(p100) ix(p100) ] > "
    "<lazy (35,4) adj,1 [.1,H* l(p100) ey(p100) .0 z(p100) iy(p100) ] > "
    "<dog (40,4) noun,2 [.1,H*;L-L% d(p100) ao(p100) g(p100) ] > "
    "pau(p50) } %%";

/* ============================================================
 * Tiny assertion harness
 * ============================================================ */

static int g_fail = 0;
static int g_pass = 0;

#define ASSERT_EQI(label, got, want) do {                                \
    long long _g = (long long)(got), _w = (long long)(want);             \
    if (_g != _w) {                                                      \
        fprintf(stderr, "  FAIL %s: got %lld, want %lld\n",              \
                (label), _g, _w);                                        \
        g_fail++;                                                        \
    } else { g_pass++; }                                                 \
} while (0)

#define ASSERT_EQS(label, got, want) do {                                \
    const char *_g = (got), *_w = (want);                                \
    if (strcmp(_g, _w) != 0) {                                           \
        fprintf(stderr, "  FAIL %s: got %s, want %s\n",                  \
                (label), _g, _w);                                        \
        g_fail++;                                                        \
    } else { g_pass++; }                                                 \
} while (0)

/* ============================================================
 * Cases
 * ============================================================ */

static void test_hello(void) {
    fe_parsed_t p;
    fprintf(stderr, "test_hello\n");
    int rc = fe_parse_tagged_output(FIXTURE_HELLO, &p);
    ASSERT_EQI("hello.rc", rc, 0);
    ASSERT_EQI("hello.n_words", p.n_words, 2);
    if (p.n_words >= 2) {
        ASSERT_EQS("hello.w0.text", p.words[0].text, "hello");
        ASSERT_EQS("hello.w0.pos",  p.words[0].pos,  "interj");
        ASSERT_EQI("hello.w0.start", p.words[0].char_start, 0);
        ASSERT_EQI("hello.w0.len",   p.words[0].char_len,   6);
        ASSERT_EQI("hello.w0.stress", p.words[0].stress_level, 2);
        ASSERT_EQI("hello.w0.n_syl",  p.words[0].n_syllables, 1);
        ASSERT_EQI("hello.w0.n_ph",   p.words[0].n_phonemes,  4);
        ASSERT_EQS("hello.w0.ph0",    p.words[0].phonemes[0].arpabet, "hh");
        ASSERT_EQI("hello.w0.ph0.dur", p.words[0].phonemes[0].duration, 100);
        ASSERT_EQI("hello.w0.ph0.stress", p.words[0].phonemes[0].syl_stress, 1);
        ASSERT_EQS("hello.w0.ph0.acc",    p.words[0].phonemes[0].accent, "H*");
        ASSERT_EQS("hello.w1.text",  p.words[1].text, "world");
        ASSERT_EQS("hello.w1.pos",   p.words[1].pos,  "noun");
        ASSERT_EQS("hello.w1.ph0.acc",
                   p.words[1].phonemes[0].accent, "H*;L-L%");
    }
    fe_parsed_free(&p);
}

static void test_pangram(void) {
    fe_parsed_t p;
    fprintf(stderr, "test_pangram\n");
    int rc = fe_parse_tagged_output(FIXTURE_PANGRAM, &p);
    ASSERT_EQI("pangram.rc", rc, 0);
    ASSERT_EQI("pangram.n_words", p.n_words, 9);
    ASSERT_EQI("pangram.pause_before", p.pause_before_ms, 25);

    const char *expect_words[] = {
        "the","quick","brown","fox","jumps","over","the","lazy","dog"};
    const char *expect_pos[] = {
        "det","adj","adj","noun","noun_verb","prep","det","adj","noun"};
    int expect_n_ph[]  = { 2, 4, 4, 4, 5, 3, 2, 4, 3 };
    int expect_n_syl[] = { 1, 1, 1, 1, 1, 2, 1, 2, 1 };
    for (int i = 0; i < 9 && i < p.n_words; i++) {
        char label[32]; snprintf(label, sizeof(label), "pangram.w%d.text", i);
        ASSERT_EQS(label, p.words[i].text, expect_words[i]);
        snprintf(label, sizeof(label), "pangram.w%d.pos", i);
        ASSERT_EQS(label, p.words[i].pos,  expect_pos[i]);
        snprintf(label, sizeof(label), "pangram.w%d.n_ph", i);
        ASSERT_EQI(label, p.words[i].n_phonemes, expect_n_ph[i]);
        snprintf(label, sizeof(label), "pangram.w%d.n_syl", i);
        ASSERT_EQI(label, p.words[i].n_syllables, expect_n_syl[i]);
    }
    /* The pangram has 31 phonemes total (2+4+4+4+5+3+2+4+3). */
    ASSERT_EQI("pangram.n_phonemes_total", fe_parsed_count_phonemes(&p), 31);

    /* The two-syllable "over" should have phoneme 0 in syl 0 stressed,
     * and phonemes 2..3 in syl 1 unstressed. */
    {
        const fe_parsed_word_t *over = &p.words[5];
        ASSERT_EQI("over.ph0.syl",    over->phonemes[0].syl_index, 0);
        ASSERT_EQI("over.ph0.stress", over->phonemes[0].syl_stress, 1);
        ASSERT_EQI("over.ph2.syl",    over->phonemes[2].syl_index, 1);
        ASSERT_EQI("over.ph2.stress", over->phonemes[2].syl_stress, 0);
    }
    /* "dog" has the boundary tone L-L% combined with the pitch accent. */
    ASSERT_EQS("dog.ph0.acc", p.words[8].phonemes[0].accent, "H*;L-L%");

    /* Boundary-tone-only accent strings should not raise emphasis. The
     * pitch-accent H* alone is emphasis 1; H*;L-L% is also 1 (single *).
     * L+H* would be 2 (compound). Verify the flatten helper produces
     * sensible emphasis values. */
    spfy_fe_slot_t slots[64];
    fe_parsed_flatten_to_slots(&p, slots, 64);
    ASSERT_EQI("flatten.slot0.emph", slots[0].emphasis_level, 0); /* "the": no accent */
    ASSERT_EQI("flatten.slot2.emph", slots[2].emphasis_level, 1); /* "quick" H* */

    fe_parsed_free(&p);
}

static void test_malformed(void) {
    fe_parsed_t p;
    fprintf(stderr, "test_malformed\n");
    /* Missing close-bracket — should fail cleanly without crashing. */
    int rc = fe_parse_tagged_output(
        "%% #{ <oops (0,4) noun,1 [.1,H* x(p100) > } %%", &p);
    ASSERT_EQI("malformed.rc", rc, -1);
    /* fe_parsed_free should be safe to call on a zeroed struct too. */
    fe_parsed_free(&p);
}

static void test_chunk_seam_cleanup(void) {
    fprintf(stderr, "test_chunk_seam_cleanup\n");
    /* Simulates the live "Hello, world." capture where the FE flushed
     * mid-identifier and inserted 2-space padding. After cleanup the
     * parser should see "interj" as one identifier. */
    char buf[512];
    snprintf(buf, sizeof(buf),
        "%%%%  #{, pau(p25) <hello (0,6) inte  rj,2 "
        "[.1,H* hh(p100) eh(p100) l(p100) ow(p100) ] >"
        " pau(p50) <world (7,5) noun,2 [.1,H*;L-L%% w(p100) er(p100) l(p100) d(p100) ] >"
        " pau(p50) } %%%%");
    fe_clean_stream_inplace(buf);
    /* The 2-space seam between "inte" and "rj" should be GONE. */
    if (strstr(buf, "interj") == NULL) {
        fprintf(stderr, "  FAIL chunk_seam: did not merge 'inte  rj' -> 'interj'\n  buf=%s\n", buf);
        g_fail++; return;
    }
    fe_parsed_t p;
    int rc = fe_parse_tagged_output(buf, &p);
    ASSERT_EQI("seam.rc", rc, 0);
    ASSERT_EQI("seam.n_words", p.n_words, 2);
    if (p.n_words >= 1) {
        ASSERT_EQS("seam.w0.text", p.words[0].text, "hello");
        ASSERT_EQS("seam.w0.pos",  p.words[0].pos,  "interj");
    }
    fe_parsed_free(&p);
}

int main(void) {
    test_hello();
    test_pangram();
    test_malformed();
    test_chunk_seam_cleanup();
    fprintf(stderr, "\n[%d PASS, %d FAIL]\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
