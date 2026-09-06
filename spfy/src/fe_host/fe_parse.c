/* spfy/src/fe_host/fe_parse.c - recursive-descent parser for the
 * tagged-text FE output captured at host/PROTOCOL.md. */

#include "fe_parse.h"
#include "env.h"
#include "engine_phoneid.h"
#include "fe_no_refine.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* ============================================================ Tokenizer
 * helpers (cursor-based, never look beyond .end)
 * ============================================================ */

typedef struct {
    const char *p;
    const char *end;
    int err;
    int err_line_off;
    const char *err_msg;
} parser_t;

static void p_skip_ws(parser_t *p) {
    while (p->p < p->end && isspace((unsigned char)*p->p)) p->p++;
}

static int p_peek(parser_t *p) {
    return (p->p < p->end) ? (unsigned char)*p->p : -1;
}

static int p_match_lit(parser_t *p, const char *lit) {
    p_skip_ws(p);
    size_t n = strlen(lit);
    if ((size_t)(p->end - p->p) < n) return 0;
    if (memcmp(p->p, lit, n) != 0) return 0;
    p->p += n;
    return 1;
}

static int p_expect_lit(parser_t *p, const char *lit) {
    if (!p_match_lit(p, lit)) {
        if (!p->err) {
            p->err = 1;
            p->err_msg = lit;
            p->err_line_off = (int)(p->p - (p->end - (p->end - p->p)));
        }
        return 0;
    }
    return 1;
}

static int p_parse_int(parser_t *p, int *out) {
    p_skip_ws(p);
    if (p->p >= p->end || !isdigit((unsigned char)*p->p)) return 0;
    int v = 0;
    while (p->p < p->end && isdigit((unsigned char)*p->p)) {
        v = v * 10 + (*p->p - '0');
        p->p++;
    }
    *out = v;
    return 1;
}

/* Identifier: starts with letter/underscore, continues with
 * alphanumeric/underscore. */
static int p_parse_ident(parser_t *p, char *buf, size_t buf_sz) {
    p_skip_ws(p);
    if (p->p >= p->end) return 0;
    int c = (unsigned char)*p->p;
    /* A byte >= 0x80 is an accented Latin-1 letter in a word name -- the FE
     * emits fr-CA/es-MX words like "días", "niño", "être" with their
     * accents intact. */
    if (!(isalpha(c) || c == '_' || c >= 0x80)) return 0;
    size_t n = 0;
    while (p->p < p->end) {
        c = (unsigned char)*p->p;
        if (!(isalnum(c) || c == '_' || c == '\'' || c >= 0x80)) break;
        if (n + 1 < buf_sz) buf[n] = (char)c;
        n++;
        p->p++;
    }
    if (buf_sz > 0) buf[(n < buf_sz) ? n : buf_sz - 1] = '\0';
    return (int)n;
}

/* Phoneme symbol: as p_parse_ident, plus '~'. */
static int p_parse_phone_ident(parser_t *p, char *buf, size_t buf_sz) {
    p_skip_ws(p);
    if (p->p >= p->end) return 0;
    int c = (unsigned char)*p->p;
    if (!(isalpha(c) || c == '_')) return 0;
    size_t n = 0;
    while (p->p < p->end) {
        c = (unsigned char)*p->p;
        if (!(isalnum(c) || c == '_' || c == '\'' || c == '~')) break;
        if (n + 1 < buf_sz) buf[n] = (char)c;
        n++;
        p->p++;
    }
    if (buf_sz > 0) buf[(n < buf_sz) ? n : buf_sz - 1] = '\0';
    return (int)n;
}

/* Accent tokens are not bare identifiers - they contain `*`, `+`, `;`, `-`,
 * `%`. */
static int p_parse_accent(parser_t *p, char *buf, size_t buf_sz) {
    p_skip_ws(p);
    size_t n = 0;
    while (p->p < p->end) {
        int c = (unsigned char)*p->p;
        if (isalnum(c) || c == '*' || c == '+' || c == ';' ||
            c == '-' || c == '%') {
            if (n + 1 < buf_sz) buf[n] = (char)c;
            n++; p->p++;
        } else break;
    }
    if (buf_sz > 0) buf[(n < buf_sz) ? n : buf_sz - 1] = '\0';
    return (int)n;
}

/* ============================================================ Grammar
 * productions ============================================================ */

static int parse_pau(parser_t *p, fe_parsed_t *out, int post_word,
                     int phrase_id, int post_in_utt) {
    /* "pau(p" int ")" - already saw "pau"; consume "(p", digits, ")". */
    if (!p_expect_lit(p, "(")) return 0;
    if (!p_expect_lit(p, "p")) return 0;
    int dur = 0, is_default = 0;
    if (p_peek(p) == '?') {
        p->p++;
        if (p_peek(p) == 'd') p->p++;
        dur = 0;
        is_default = 1;
    } else if (!p_parse_int(p, &dur)) {
        p->err = 1; p->err_msg = "pau duration"; return 0;
    }
    if (!p_expect_lit(p, ")")) return 0;
    if (out) {
        if (out->n_words == 0 && !post_word) {
            out->pause_before_ms = dur;
        } else if (post_word) {
            out->pause_after_ms = dur;
        } else {
            /* Inter-word pause - attach to the previous word's trailing pause field. */
            out->words[out->n_words - 1].pause_after_ms = dur;
        }
        /* Per-phrase copy, kept SEPARATE from the three fields above.
         *
         * Those key `post_word` off the GLOBAL word count, so in a
         * multi-phrase input every pau from phrase 1 onward looks like a
         * trailing pau and phrase N's LEADING value is lost (measured on
         * "Hello, world.": phrase 1's `pau(p100)` landed in
         * pause_after_ms). Existing consumers depend on that behaviour, so
         * record the per-phrase values alongside rather than changing it.
         *
         * `?d` resolves to 50. FUN_08ee2960 sizes a pau sub-unit from
         * sub+0x18, and a live capture of that field gives exactly p/2 ms
         * for every concrete p (p25 -> 12.5, p100 -> 50) and 25.0 for
         * `?d` -- i.e. `?d` behaves as p50. */
        if (phrase_id >= 0 && phrase_id < FE_PARSE_MAX_PHRASES) {
            int pv = is_default ? FE_PAU_DEFAULT_P : dur;
            float pms = is_default ? FE_PAU_DEFAULT_MS : (float)dur / 2.0f;
            if (post_in_utt) {
                out->phrase_pau_p_after [phrase_id] = (int16_t)pv;
                out->phrase_pau_ms_after[phrase_id] = pms;
            } else {
                out->phrase_pau_p_before [phrase_id] = (int16_t)pv;
                out->phrase_pau_ms_before[phrase_id] = pms;
            }
        }
    }
    return 1;
}

static int ensure_word_cap(fe_parsed_t *out) {
    if (out->n_words >= out->words_cap) {
        int nc = out->words_cap ? out->words_cap * 2 : 8;
        fe_parsed_word_t *p = (fe_parsed_word_t *)
            realloc(out->words, (size_t)nc * sizeof(*p));
        if (!p) return 0;
        out->words = p; out->words_cap = nc;
        memset(out->words + out->n_words, 0,
               (size_t)(nc - out->n_words) * sizeof(*p));
    }
    return 1;
}

static void apply_phoneme_refinement(fe_parsed_t *out);

/* fr-CA liaison stress inheritance. */
static int s_liaison_inherit_stress = 0;
/* SPFY_FE_LIAISON_LEGACY=1 restores the pre-fix behaviour, where a
 * marker-less leading phone opened a fresh syllable in its own word instead
 * of continuing the previous word's. */
static int s_liaison_legacy_syl = -1;

static int ensure_phoneme_cap(fe_parsed_word_t *w) {
    if (w->n_phonemes >= w->phonemes_cap) {
        int nc = w->phonemes_cap ? w->phonemes_cap * 2 : 16;
        fe_parsed_phoneme_t *p = (fe_parsed_phoneme_t *)
            realloc(w->phonemes, (size_t)nc * sizeof(*p));
        if (!p) return 0;
        w->phonemes = p; w->phonemes_cap = nc;
        memset(w->phonemes + w->n_phonemes, 0,
               (size_t)(nc - w->n_phonemes) * sizeof(*p));
    }
    return 1;
}

static int parse_phoneme(parser_t *p, fe_parsed_word_t *w,
                         int cur_stress, const char *cur_accent) {
    char name[16];
    if (p_parse_phone_ident(p, name, sizeof(name)) <= 0) return 0;
    if (!p_expect_lit(p, "(")) return 0;
    if (!p_expect_lit(p, "p")) return 0;
    int dur = 0;
    if (!p_parse_int(p, &dur)) { p->err = 1; p->err_msg = "phn duration"; return 0; }
    if (!p_expect_lit(p, ")")) return 0;

    if (!ensure_phoneme_cap(w)) { p->err = 1; p->err_msg = "oom"; return 0; }
    fe_parsed_phoneme_t *ph = &w->phonemes[w->n_phonemes++];
    strncpy(ph->arpabet, name, sizeof(ph->arpabet) - 1);
    ph->arpabet[sizeof(ph->arpabet) - 1] = '\0';
    ph->duration = dur;
    ph->syl_stress = (int8_t)cur_stress;
    strncpy(ph->accent, cur_accent ? cur_accent : "",
            sizeof(ph->accent) - 1);
    ph->accent[sizeof(ph->accent) - 1] = '\0';
    ph->syl_index = w->n_syllables - 1;
    return 1;
}

static int parse_word_body(parser_t *p, fe_parsed_word_t *w,
                           int prev_last_stress) {
    /* Inside the [ ... */
    int cur_stress = 0;
    char cur_accent[24] = "";

    p_skip_ws(p);
    while (p->p < p->end && p_peek(p) != ']') {
        if (p_peek(p) == '.') {
            p->p++;
            int st;
            if (!p_parse_int(p, &st)) {
                p->err = 1; p->err_msg = "syl stress digit"; return 0;
            }
            cur_stress = st;
            cur_accent[0] = '\0';
            p_skip_ws(p);
            /* Accent introducer is comma for pitch-accented syllables
             * (.1,H*) and semicolon for boundary-tone-only syllables on
             * otherwise unstressed positions (.0;L-L%, observed on
             * word-final unstressed syllables of pronouns like... */
            if (p_peek(p) == ',' || p_peek(p) == ';') {
                p->p++;
                if (p_parse_accent(p, cur_accent, sizeof(cur_accent)) <= 0) {
                    p->err = 1; p->err_msg = "accent"; return 0;
                }
            }
            w->n_syllables++;
            continue;
        }
        /* Liaison / enchainement (fr-CA): a word's phone list can begin
         * WITHOUT a leading `.N` marker, because its first phone belongs to
         * the syllable whose nucleus sits in the PREVIOUS word -- <un (0,2)
         * art,0 [.0 oe~ .0 n ]>... */
        if (w->n_syllables == 0) {
            w->n_syllables++;
            w->first_syl_implicit = 1;
            /* These bare leading phones belong to the PREVIOUS word's final
             * syllable (e.g. */
            if (s_liaison_inherit_stress && cur_stress == 0)
                cur_stress = prev_last_stress;
        }
        if (!parse_phoneme(p, w, cur_stress, cur_accent)) return 0;
        p_skip_ws(p);
    }
    return 1;
}

static int parse_word(parser_t *p, fe_parsed_t *out) {
    fe_parsed_word_t *w = NULL;
    if (!ensure_word_cap(out)) { p->err = 1; p->err_msg = "oom"; return 0; }
    w = &out->words[out->n_words++];
    memset(w, 0, sizeof(*w));

    if (p_parse_ident(p, w->text, sizeof(w->text)) <= 0) {
        p->err = 1; p->err_msg = "word name"; return 0;
    }
    if (!p_expect_lit(p, "(")) return 0;
    /* Numbers and other text expanded by the FE produce auxiliary words
     * (e.g. */
    p_skip_ws(p);
    if (p_peek(p) == ')') {
        w->char_start = 0;
        w->char_len = 0;
    } else {
        /* The FE emits `?d` (its "default/unspecified" marker) instead of a
         * numeric char_start when the DLL didn't compute one - happens with
         * the plain-text feedConfigA path used by both fe_host.c and
         * fe_host_emu.c. */
        if (p_peek(p) == '?') {
            p->p++;
            if (p_peek(p) == 'd') p->p++;
            w->char_start = 0;
        } else if (!p_parse_int(p, &w->char_start)) {
            p->err = 1; p->err_msg = "char_start"; return 0;
        }
        if (!p_expect_lit(p, ",")) return 0;
        if (!p_parse_int(p, &w->char_len)) {
            p->err = 1; p->err_msg = "char_len"; return 0;
        }
    }
    if (!p_expect_lit(p, ")")) return 0;
    if (p_parse_ident(p, w->pos, sizeof(w->pos)) <= 0) {
        p->err = 1; p->err_msg = "pos"; return 0;
    }
    if (!p_expect_lit(p, ",")) return 0;
    if (!p_parse_int(p, &w->stress_level)) {
        p->err = 1; p->err_msg = "stress_level"; return 0;
    }
    /* Optional SSML/Balabolka prosody trailers ",p=N" and/or ",r=M". */
    p_skip_ws(p);
    while (p_peek(p) == ',') {
        ++p->p;
        p_skip_ws(p);
        char key[8] = {0};
        int  ki = 0;
        while (p->p < p->end && ki + 1 < (int)sizeof(key)) {
            int c = (unsigned char)*p->p;
            if (!(isalpha(c) || c == '_')) break;
            key[ki++] = (char)c;
            ++p->p;
        }
        key[ki] = '\0';
        if (p_peek(p) != '=') break;
        ++p->p;
        p_skip_ws(p);
        int sign = 1;
        if (p_peek(p) == '-') { sign = -1; ++p->p; }
        else if (p_peek(p) == '+')         { ++p->p; }
        int uval = 0;
        if (!p_parse_int(p, &uval)) {
            p->err = 1; p->err_msg = "prosody value"; return 0;
        }
        int val = sign * uval;
        if (val < -127) val = -127; else if (val > 127) val = 127;
        if      (strcmp(key, "p") == 0) w->pitch_st = (int8_t)val;
        else if (strcmp(key, "r") == 0) w->rate_pct = (int8_t)val;
        p_skip_ws(p);
    }
    if (!p_expect_lit(p, "[")) return 0;
    /* Stress of the previous word's final syllable, for the fr-CA liaison
     * inheritance in parse_word_body. */
    int prev_last_stress = 0;
    if (out->n_words >= 2) {
        const fe_parsed_word_t *prev = &out->words[out->n_words - 2];
        if (prev->n_phonemes > 0)
            prev_last_stress = prev->phonemes[prev->n_phonemes - 1].syl_stress;
    }
    if (!parse_word_body(p, w, prev_last_stress)) return 0;
    if (!p_expect_lit(p, "]")) return 0;
    if (!p_expect_lit(p, ">")) return 0;
    return 1;
}

/* ⭐⭐⭐ THE FE'S OWN DURATION CLOCK, REPRODUCED EXACTLY.
 *
 * SWIttsUSel's duration extractor (FUN_08e8fb20 @ 0x08E8FB20, named by the
 * static feature-name table at PTR_DAT_08e999e8) does NOT read a "duration"
 * feature. It reads this Segment's `end` and the PREVIOUS Segment's `end` --
 * absolute float32 seconds on the FE utterance -- splits at the midpoint,
 * and hands USel the two halfphone lengths:
 *
 *     prev = prev_segment ? feat(prev, "end") : 0.0f
 *     end  = feat(seg, "end")
 *     mid  = (end + prev) * 0.5f
 *     hp1  = mid - prev
 *     hp2  = end - mid
 *
 * and the FE builds those end times by keeping the running time in whole
 * MILLISECONDS and converting with a MULTIPLY:
 *
 *     ms  += nominal_ms                 (exact integer, per utterance)
 *     end  = (float)(ms * 0.001f)       rounded once
 *
 * ⛔ `ms / 1000.0f` IS NOT THE SAME and is wrong. 825/1000 is exact in real
 * arithmetic and rounds to 0.82499998807907104; the engine has
 * 0.82500004768371582, which only the multiply by 0.001f produces. That one
 * segment is what separates the two models.
 *
 * ⚠ `mid` and the two differences MUST stay in double. The engine computes
 * them on the x87 stack in 80-bit and rounds once on the store; the operands
 * are float32 so their sum, its half, and both differences are all EXACT in
 * double -- which makes double bit-identical to 80-bit here, but a float
 * intermediate is not.
 *
 * This is where the FE's "25 ms" pause acquires up to 50 float32 ULPs of
 * error, and why the value tracks neither the phone nor the text family:
 * it is a difference of absolute times, so it depends only on how much time
 * has already accumulated. Six distinct values across the 221-text corpus.
 *
 * Verified bit-exact against the engine: 219/219 captured `end` features
 * (traces/fe_tree) and 10168/10168 halfphone durations over all 221 master
 * texts (traces/slice_dur, via viz/frida_hooks/slice_dur_hook.js). */
typedef struct {
    int   ms;        /* running whole milliseconds, per utterance */
    float prev_end;  /* previous segment's stored `end`, seconds */
} fe_dur_clock_t;

/* 0.001f as a double, by BIT PATTERN.
 *
 * ⛔ `(double)0.001f` does NOT do this. Under -fexcess-precision=standard on
 * x87 the literal keeps extended precision, so the cast hands back double
 * 0.001 (0.0010000000000000000208) instead of the float32 value
 * 0.001000000047497451. Measured: end(2875 ms) then comes out 2.875 where
 * the engine has 2.8750002384185791, and the trailing pause lands 12 ULPs
 * BELOW 25 ms instead of 50 above -- the entire defect, from one cast. */
static double fe_ms_to_sec_k(void) {
    union { uint32_t u; float f; } v;
    v.u = 0x3A83126Fu;                      /* 0.001f */
    return (double)v.f;
}

static void fe_dur_step(fe_dur_clock_t *c, int nominal_ms,
                        float *out_h1, float *out_h2) {
    c->ms += nominal_ms;
    float end = (float)((double)c->ms * fe_ms_to_sec_k());
    double mid = ((double)end + (double)c->prev_end) * 0.5;
    if (out_h1) *out_h1 = (float)(mid - (double)c->prev_end);
    if (out_h2) *out_h2 = (float)((double)end - mid);
    c->prev_end = end;
}

/* Target ms for a pau NODE from its two halfphone lengths -- the mean USel
 * writes to wsola unit+0x10 (FUN_08e8de20: f32-rounded running add, divided
 * by the halfphone count, times 1000.0f). */
static float fe_pau_target_ms(float h1, float h2) {
    float acc = (float)(0.0f + h1);
    acc = (float)(acc + h2);
    return (float)(acc / 2.0f) * 1000.0f;
}

/* Walk every phrase's segments in emission order and record the exact pau
 * targets. Non-pau segments only advance the clock: under rate the engine
 * scales them by their own natural length and never consults the duration
 * model (see build_unit_plan in spfy_synth.c), so only pauses need a value. */
static void fe_compute_pau_targets(fe_parsed_t *out) {
    if (!out) return;
    int max_pid = 0;
    for (int i = 0; i < out->n_words; ++i) {
        if (out->words[i].phrase_id > max_pid) max_pid = out->words[i].phrase_id;
    }
    if (max_pid >= FE_PARSE_MAX_PHRASES) max_pid = FE_PARSE_MAX_PHRASES - 1;

    for (int pid = 0; pid <= max_pid; ++pid) {
        fe_dur_clock_t c = { 0, 0.0f };
        float h1 = 0.0f, h2 = 0.0f;

        int pb = out->phrase_pau_p_before[pid];
        if (pb <= 0) pb = FE_PAU_DEFAULT_P;
        fe_dur_step(&c, pb, &h1, &h2);
        out->phrase_pau_ms_before[pid] = fe_pau_target_ms(h1, h2);

        /* A leading `\!pN` is a pau unit pair right after the pad. */
        if (out->phrase_head_pau_ms[pid] > 0) {
            fe_dur_step(&c, out->phrase_head_pau_ms[pid], &h1, &h2);
            out->phrase_head_pau_target_ms[pid] = fe_pau_target_ms(h1, h2);
        }

        for (int wi = 0; wi < out->n_words; ++wi) {
            fe_parsed_word_t *w = &out->words[wi];
            if (w->phrase_id != pid) continue;
            for (int ph = 0; ph < w->n_phonemes; ++ph)
                fe_dur_step(&c, w->phonemes[ph].duration, NULL, NULL);
            if (w->pause_after_ms != 0 && wi + 1 < out->n_words
                && out->words[wi + 1].phrase_id == pid) {
                fe_dur_step(&c, w->pause_after_ms, &h1, &h2);
                w->pause_after_target_ms = fe_pau_target_ms(h1, h2);
            }
        }

        int pa = out->phrase_pau_p_after[pid];
        if (pa <= 0) pa = FE_PAU_DEFAULT_P;
        fe_dur_step(&c, pa, &h1, &h2);
        out->phrase_pau_ms_after[pid] = fe_pau_target_ms(h1, h2);

        if (spfy_env("SPFY_FE_DUR_MODEL")) {
            fprintf(stderr, "[dur_model] phrase %d: total_ms=%d "
                    "lead=%.9g trail=%.9g (nominal p %d/%d) "
                    "h1=%.9g h2=%.9g prev_end=%.9g\n",
                    pid, c.ms, (double)out->phrase_pau_ms_before[pid],
                    (double)out->phrase_pau_ms_after[pid], pb, pa,
                    (double)h1, (double)h2, (double)c.prev_end);
        }
    }
}

int fe_parse_tagged_output(const char *tagged, fe_parsed_t *out) {
    if (!tagged || !out) return -1;
    memset(out, 0, sizeof(*out));

    parser_t pp = {
        .p = tagged,
        .end = tagged + strlen(tagged),
        .err = 0,
        .err_msg = NULL,
    };
    parser_t *p = &pp;

    p_skip_ws(p);
    p_match_lit(p, "%%");
    p_skip_ws(p);

    /* Outer loop over utterances. */
    int utt_count = 0;
    while (p->p < p->end) {
        int phrase_id_for_this_utt = utt_count;
        p_skip_ws(p);
        if (p->p >= p->end) break;
        if (p_match_lit(p, "%%")) { p_skip_ws(p); continue; }
        /* Single `%` is the inter-utterance separator the FE emits between
         * consecutive `{...}` blocks for ';' / multi-clause boundaries
         * (e.g. */
        if (p_peek(p) == '%') { p->p++; p_skip_ws(p); continue; }
        /* Utterance opener: first one is "#{", subsequent inter-utterance
         * separators are just "{". */
        if (!(p_match_lit(p, "#{") || p_match_lit(p, "{"))) {
            if (utt_count == 0) {
                p->err = 1; p->err_msg = "missing #{ opener"; goto fail;
            }
            break;
        }
        /* Capture per-utterance terminator marker. */
        {
            /* The FE's phrase-type vocabulary is not all single characters:
             * wh-questions open `#{wh?` and alternative questions `#{alt?`. */
            static const char *const PHRASE_TYPES[] = {
                "alt?", "wh?", ",", ".", "?", "!", ";"
            };
            char term_marker = 0;
            p_skip_ws(p);
            for (size_t ti = 0;
                 ti < sizeof PHRASE_TYPES / sizeof PHRASE_TYPES[0]; ++ti) {
                if (p_match_lit(p, PHRASE_TYPES[ti])) {
                    term_marker = PHRASE_TYPES[ti][0];
                    break;
                }
            }
            if (utt_count < (int)(sizeof(out->phrase_terms) /
                                  sizeof(out->phrase_terms[0]))) {
                out->phrase_terms[utt_count] = term_marker ? term_marker : '.';
                if (utt_count + 1 > out->n_phrase_terms)
                    out->n_phrase_terms = utt_count + 1;
            }
        }
        utt_count++;

        /* Word count at this utterance's start, so a pau can be classed as
         * leading/trailing WITHIN the utterance rather than globally. */
        int words_at_utt_start = out->n_words;

        for (;;) {
            p_skip_ws(p);
            if (p_peek(p) == '}') { p->p++; break; }
            if (p->p >= p->end) { p->err = 1; p->err_msg = "unterminated #{"; goto fail; }

            if (p->end - p->p >= 3 && p->p[0] == 'p' && p->p[1] == 'a' && p->p[2] == 'u') {
                p->p += 3;
                /* `pau(uN)` - USER pause from a `\!pN` embedded tag
                 * (emitted by build_inline_mixed_tagged). */
                if (p->end - p->p >= 2 && p->p[0] == '(' && p->p[1] == 'u') {
                    p->p += 2;
                    int dur = 0;
                    if (!p_parse_int(p, &dur)) { p->err = 1; p->err_msg = "user pause"; goto fail; }
                    if (!p_expect_lit(p, ")")) goto fail;
                    /* An INLINE pau unit hanging off the preceding word, not
                     * injected silence and not a phrase break -- the engine
                     * keeps `\!p` inside one utterance as a single unit pair.
                     * With no `\!pN` in the text nothing emits `pau(u...)`,
                     * so `pause_after_ms` stays 0 everywhere and the untagged
                     * path is untouched. */
                    int in_utt = (out->n_words > words_at_utt_start);
                    if (!spfy_env("SPFY_INLINE_PAU_LEGACY") && in_utt) {
                        out->words[out->n_words - 1].pause_after_ms = dur;
                    } else if (phrase_id_for_this_utt >= 0
                               && phrase_id_for_this_utt
                                  < FE_PARSE_MAX_PHRASES) {
                        /* Before this utterance's first word. The engine
                         * keeps it INSIDE the utterance as a pau pair after
                         * the leading pad, so it is head_pau, not the
                         * inject-silence-between-phrases lead_pause. ⚠ The
                         * test is per-UTTERANCE: keying it off the global
                         * n_words made phrase 1's leading `\!p` look like a
                         * trailing one on phrase 0's last word. */
                        if (spfy_env("SPFY_INLINE_PAU_LEGACY"))
                            out->phrase_lead_pause_ms
                                [phrase_id_for_this_utt] += dur;
                        else
                            out->phrase_head_pau_ms
                                [phrase_id_for_this_utt] += dur;
                    }
                    continue;
                }
                int post = (out->n_words > 0);
                if (!parse_pau(p, out, (post != 0),
                               phrase_id_for_this_utt,
                               (out->n_words > words_at_utt_start)))
                    goto fail;
                continue;
            }
            if (p_peek(p) == '<') {
                p->p++;
                if (!parse_word(p, out)) goto fail;
                /* Tag the just-parsed word with the current utterance index. */
                if (out->n_words > 0) {
                    int pid = spfy_env("SPFY_FE_HOST_PHRASE_MERGE")
                                ? 0 : phrase_id_for_this_utt;
                    out->words[out->n_words - 1].phrase_id = pid;
                }
                continue;
            }
            p->p++;
        }
        /* A `{X pau(pN) }` block with no words at all -- what a TRAILING
         * `\!pN` becomes. The engine renders it as its own 2-unit utterance,
         * so it must survive into the phrase loop, which otherwise skips any
         * phrase holding no words. */
        if (out->n_words == words_at_utt_start
            && phrase_id_for_this_utt >= 0
            && phrase_id_for_this_utt < FE_PARSE_MAX_PHRASES
            && out->phrase_pau_ms_before[phrase_id_for_this_utt] > 0.0f) {
            out->phrase_pau_only[phrase_id_for_this_utt] = 1u;
        }
        p_skip_ws(p);
    }
    apply_phoneme_refinement(out);
    fe_compute_pau_targets(out);
    return 0;

fail:
    fprintf(stderr, "[fe_parse] error at offset %td: %s\n",
            (ptrdiff_t)(p->p - tagged),
            p->err_msg ? p->err_msg : "(unknown)");
    fe_parsed_free(out);
    return -1;
}

/* ============================================================
 * Phoneme refinement (engine-side post-process emulation)
 *
 * The FE's drained ESPR text gives coarse phoneme names (`ih`, `t`,
 * `d`). The real engine's Festival utterance (captured via fe_tree)
 * has these refined to `ix`, `dx` per standard English post-lexical
 * rules. The refinement lives inside SWIttsUSel.dll (not accessible
 * here) but the rules can be derived empirically - see
 * memory/project_phoneme_refinement_rules_2026_05_12.md.
 *
 * Two rules:
 *   1. Unstressed `ih` → `ix` (vowel reduction)
 *   2. `t`/`d` in unstressed syllable-onset, between sonorant/vowel and
 *      vowel → `dx` (flapping)
 *
 * Applied here so downstream engine_phoneid_lookup sees the refined
 * names (ih=20 → ix=21, d=9 → dx=11, t=? → dx=11).
 *
 * Toggle off via SPFY_FE_HOST_NO_PHONEME_REFINE=1.
 * ============================================================ */

static int is_arpa_vowel(const char *p) {
    static const char *VOWELS[] = {
        "aa","ae","ah","ao","aw","ax","ay",
        "eh","er","ey",
        "ih","ix","iy",
        "ow","oy",
        "uh","uw",
        NULL
    };
    for (const char **v = VOWELS; *v; v++)
        if (strcmp(p, *v) == 0) return 1;
    return 0;
}
/* `is_arpa_sonorant_not_vowel` was the prev-context predicate for an
 * earlier (over-broad) version of R2 that included nasals as flap triggers. */

/* Refinement toggle. */
static int s_refine_enabled = 1;

void fe_parse_set_refine(int mode)
{
    /* Historical callers pass 0/1; FE_REFINE_FLAP_ONLY (2) is additive. */
    s_refine_enabled = (mode == FE_REFINE_NONE) ? FE_REFINE_NONE
                     : (mode == FE_REFINE_FLAP_ONLY) ? FE_REFINE_FLAP_ONLY
                     : FE_REFINE_ALL;
}
int  fe_parse_get_refine(void)        { return s_refine_enabled; }

void fe_parse_set_liaison_inherit(int enabled)
{
    s_liaison_inherit_stress = enabled ? 1 : 0;
}

static void apply_phoneme_refinement(fe_parsed_t *out) {
    if (!out || out->n_words == 0) return;
    if (!s_refine_enabled) return;
    if (spfy_env("SPFY_FE_HOST_NO_PHONEME_REFINE")) return;

    /* Pass 1: unstressed ih → ix AND unstressed ax → ix (onset-present). */
    int no_refine_enabled = (spfy_env("SPFY_FE_HOST_NO_LEXICAL_OVERRIDE") == NULL);
    /* FLAP_ONLY skips every VOWEL pass (1, 2.5, 3) and keeps only the flap
     * (pass 2). */
    const int refine_vowels = (s_refine_enabled == FE_REFINE_ALL);
    for (int wi = 0; refine_vowels && wi < out->n_words; wi++) {
        fe_parsed_word_t *w = &out->words[wi];
        /* Lowercase a copy of the word text for case-insensitive lookup
         * against the engine-derived (word, syl_idx) override table. */
        char wlow[64];
        size_t wl_n = strlen(w->text);
        if (wl_n >= sizeof(wlow)) wl_n = sizeof(wlow) - 1;
        for (size_t i = 0; i < wl_n; i++) wlow[i] = (char)tolower((unsigned char)w->text[i]);
        wlow[wl_n] = '\0';
        for (int pi = 0; pi < w->n_phonemes; pi++) {
            fe_parsed_phoneme_t *ph = &w->phonemes[pi];
            /* Stress gate: always fire on stress=0; for stress=2 (secondary
             * stress), fire ONLY when the phoneme is a singleton syllable
             * (no onset, no coda) - i.e. */
            int singleton_syl = 0;
            if (ph->syl_stress == 2) {
                int prev_same = (pi > 0
                    && w->phonemes[pi - 1].syl_index == ph->syl_index);
                int next_same = (pi + 1 < w->n_phonemes
                    && w->phonemes[pi + 1].syl_index == ph->syl_index);
                singleton_syl = (!prev_same && !next_same);
            }
            if (ph->syl_stress != 0 && !singleton_syl) continue;
            /* Lexical override: engine keeps `ih`/`ax` unrefined in
             * specific (word, syl_idx) pairs that our R1/R3 rules would
             * otherwise over-refine to `ix`. */
            static const struct { const char *w; int s; } HARD_SKIP[] = {
                { "this", 0 },
                { NULL, 0 }
            };
            int hard_skip = 0;
            for (int hi = 0; HARD_SKIP[hi].w; ++hi) {
                if (HARD_SKIP[hi].s == ph->syl_index
                    && strcmp(HARD_SKIP[hi].w, wlow) == 0) {
                    hard_skip = 1; break;
                }
            }
            if (hard_skip) continue;
            if (no_refine_enabled
                && spfy_fe_should_skip_refinement(wlow, ph->syl_index)) {
                continue;
            }
            if (strcmp(ph->arpabet, "ih") == 0) {
                /* Exception: -ing endings (`ih` followed by `ng` in the
                 * same syllable) keep `ih`. */
                int next_is_ng_same_syl = (pi + 1 < w->n_phonemes
                    && w->phonemes[pi + 1].syl_index == ph->syl_index
                    && strcmp(w->phonemes[pi + 1].arpabet, "ng") == 0);
                if (next_is_ng_same_syl) continue;
                strncpy(ph->arpabet, "ix", sizeof(ph->arpabet) - 1);
                ph->arpabet[sizeof(ph->arpabet) - 1] = '\0';
                continue;
            }
            if (strcmp(ph->arpabet, "ax") == 0) {
                /* Onset present iff a phoneme of the same syl_index
                 * precedes ax in this word. */
                int has_onset = (pi > 0
                    && w->phonemes[pi - 1].syl_index == ph->syl_index);
                if (has_onset) {
                    strncpy(ph->arpabet, "ix", sizeof(ph->arpabet) - 1);
                    ph->arpabet[sizeof(ph->arpabet) - 1] = '\0';
                }
            }
        }
    }

    /* Pass 2: flap rule (uses post-rule-1 names; runs after pass 1
     * so flap-context lookup sees ix where ih became ix).
     *
     * Predicate (derived 2026-05-13 evening from full-corpus
     * empirical pivot of 485 alveolar-stop slots vs engine's
     * fe_tree decisions; 97.7% agreement on the rule below):
     *   flap iff:
     *     phone in {t, d}
     *     AND curr_syl_stress == 0          (engine NEVER flaps cs>=1)
     *     AND prev in {VOWEL, 'r'}         (NOT n/m/ng/l - engine
     *                                         doesn't flap after nasals
     *                                         or 'l'; the 'twenty' nt
     *                                         keep cluster, "London" nd,
     *                                         etc.)
     *     AND next in {VOWEL, 'l', 'el'}   (NOT 'r' - onset cluster
     *                                         tr/dr blocks flap; NOT
     *                                         nasal - "wanted" nt-axn)
     *     AND NOT word-initial             (handled by prev_ph=NULL at
     *                                         word boundary, so the
     *                                         prev-class check fails)
     *
     * Prior rule used `prev_sonorant_or_vowel` (included nasals) and
     * `next_vowel` (excluded l/el). Net effect of the tightening:
     * stops over-flapping after nasals (twenty, holiday, calendar)
     * AND over-flapping into 'r' (no cases hit that direction here),
     * plus catches engine-flap into 'l' contexts.
     *
     * See project_flap_predicate_2026_05_13.md for the empirical
     * derivation (full pivot table, drilldown of outliers).
     *
     * Cross-word LOOK-AHEAD is applied (added 2026-05-13 evening,
     * after the tightened predicate landed): word-FINAL t/d looks at
     * the next word's first phoneme to detect intervocalic-across-
     * word-boundary cases the engine flaps ("road ahead", "att add").
     * Critically, look-BEHIND remains within-word only -- prev_ph is
     * reset at word boundaries -- because engine does NOT flap word-
     * initial t/d (e.g. "today" word-initial /t/ after vowel-final
     * prev word stays /t/, never /dx/). Word-initial t/d therefore
     * fails the predicate (prev_ph=NULL) regardless of prev word.
     * Pause boundary blocks cross-word lookahead. */
    fe_parsed_phoneme_t *prev_ph = NULL;
    for (int wi = 0; wi < out->n_words; wi++) {
        fe_parsed_word_t *w = &out->words[wi];
        prev_ph = NULL;
        for (int pi = 0; pi < w->n_phonemes; pi++) {
            fe_parsed_phoneme_t *curr = &w->phonemes[pi];

            /* Look-ahead: same-word next, with cross-word fallback for the
             * word-final position (no pause after this word). */
            fe_parsed_phoneme_t *next = NULL;
            int xword_next = 0;
            if (pi + 1 < w->n_phonemes) {
                next = &w->phonemes[pi + 1];
            } else if (w->pause_after_ms == 0 && wi + 1 < out->n_words) {
                fe_parsed_word_t *nw = &out->words[wi + 1];
                /* Cross-word lookahead is bounded to the SAME phrase --
                 * comma/period boundaries that split into separate `#{...}`
                 * blocks (phrase_id increments) block the flap. */
                if (nw->n_phonemes > 0 && nw->phrase_id == w->phrase_id) {
                    next = &nw->phonemes[0];
                    xword_next = 1;
                }
            }

            int is_onset = 0;
            if (pi == 0) is_onset = 1;
            else if (w->phonemes[pi - 1].syl_index != curr->syl_index)
                is_onset = 1;

            int is_t_or_d = (strcmp(curr->arpabet, "t") == 0
                             || strcmp(curr->arpabet, "d") == 0);
            int prev_v_or_r = prev_ph &&
                (is_arpa_vowel(prev_ph->arpabet)
                 || strcmp(prev_ph->arpabet, "r") == 0);
            int next_is_vowel = next && is_arpa_vowel(next->arpabet);
            int next_v_or_l = next &&
                (next_is_vowel
                 || strcmp(next->arpabet, "l") == 0
                 || strcmp(next->arpabet, "el") == 0);

            /* Cross-word resyllabification: when the t/d is word-final
             * before a VOWEL-initial next word in the SAME phrase, the
             * engine treats the flap as ONSET of the next word's first
             * syllable. */
            int eff_stress     = xword_next ? 0 : curr->syl_stress;
            int eff_onset      = xword_next ? 1 : is_onset;
            int eff_next_match = xword_next ? next_is_vowel : next_v_or_l;
            if (is_t_or_d && eff_stress == 0 && eff_onset
                && prev_v_or_r && eff_next_match) {
                strncpy(curr->arpabet, "dx", sizeof(curr->arpabet) - 1);
                curr->arpabet[sizeof(curr->arpabet) - 1] = '\0';
            }

            prev_ph = curr;
        }
        if (w->pause_after_ms != 0) prev_ph = NULL;
    }

    /* Pass 2.5: word-"to" vowel override. */
    for (int wi = 0; refine_vowels && wi < out->n_words; wi++) {
        fe_parsed_word_t *w = &out->words[wi];
        if (strcmp(w->text, "to") != 0
            && strcmp(w->text, "To") != 0) continue;
        if (w->n_phonemes < 2) continue;
        fe_parsed_phoneme_t *vowel = &w->phonemes[1];

        /* Find next word's first phoneme (cross-word, same phrase, no pause). */
        const char *next_phone = NULL;
        if (w->pause_after_ms == 0 && wi + 1 < out->n_words) {
            fe_parsed_word_t *nw = &out->words[wi + 1];
            if (nw->phrase_id == w->phrase_id && nw->n_phonemes > 0) {
                next_phone = nw->phonemes[0].arpabet;
            }
        }
        if (!next_phone) continue;

        const char *new_vowel;
        if (is_arpa_vowel(next_phone))            new_vowel = "uw";
        else if (strcmp(next_phone, "dh") == 0)   new_vowel = "ix";
        else                                       new_vowel = "ax";

        strncpy(vowel->arpabet, new_vowel,
                sizeof(vowel->arpabet) - 1);
        vowel->arpabet[sizeof(vowel->arpabet) - 1] = '\0';
    }

    /* Pass 3 (R4): syllabic-L collapse. */
    if (refine_vowels && !spfy_env("SPFY_FE_HOST_NO_EL_COLLAPSE")) {
        for (int wi = 0; wi < out->n_words; wi++) {
            fe_parsed_word_t *w = &out->words[wi];
            int syl_start = 0;
            while (syl_start < w->n_phonemes) {
                int sx = w->phonemes[syl_start].syl_index;
                int stress = w->phonemes[syl_start].syl_stress;
                int syl_end = syl_start + 1;
                while (syl_end < w->n_phonemes
                       && w->phonemes[syl_end].syl_index == sx) {
                    syl_end++;
                }
                int last = syl_end - 1;
                int word_final_syl = (sx == w->n_syllables - 1);
                int multi_syl = (w->n_syllables > 1);
                int can_collapse =
                    (stress == 0)
                    || (stress == 1 && word_final_syl && multi_syl);
                if (can_collapse && last > syl_start
                    && strcmp(w->phonemes[last].arpabet, "l") == 0) {
                    int vidx = last - 1;
                    const char *v = w->phonemes[vidx].arpabet;
                    if (strcmp(v, "ax") == 0
                        || strcmp(v, "ih") == 0
                        || strcmp(v, "ix") == 0) {
                        strncpy(w->phonemes[last].arpabet, "el",
                                sizeof(w->phonemes[last].arpabet) - 1);
                        w->phonemes[last].arpabet[
                            sizeof(w->phonemes[last].arpabet) - 1] = '\0';
                        if (vidx + 1 < w->n_phonemes) {
                            memmove(&w->phonemes[vidx],
                                    &w->phonemes[vidx + 1],
                                    sizeof(fe_parsed_phoneme_t)
                                    * (size_t)(w->n_phonemes - vidx - 1));
                        }
                        w->n_phonemes--;
                        syl_end--;
                    }
                }
                syl_start = syl_end;
            }
        }
    }
}

void fe_clean_stream_inplace(char *s) {
    if (!s) return;
    char *r = s, *w = s;
    while (*r) {
        if (isspace((unsigned char)*r)) {
            const char *ws_start = r;
            while (*r && isspace((unsigned char)*r)) r++;
            ptrdiff_t ws_len = r - ws_start;
            int prev = (w > s) ? (unsigned char)*(w - 1) : 0;
            int next = (unsigned char)*r;
            if (ws_len >= 2 && isalnum(prev) && isalnum(next)) {
            } else {
                *w++ = ' ';
            }
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

int fe_parsed_count_phonemes(const fe_parsed_t *out) {
    int n = 0;
    if (!out) return 0;
    for (int i = 0; i < out->n_words; i++) n += out->words[i].n_phonemes;
    return n;
}

void fe_parsed_free(fe_parsed_t *out) {
    if (!out) return;
    if (out->words) {
        for (int i = 0; i < out->n_words; i++) {
            free(out->words[i].phonemes);
        }
        free(out->words);
    }
    memset(out, 0, sizeof(*out));
}

static uint8_t accent_to_emphasis(const char *acc) {
    if (!acc || !*acc) return 0;
    /* Boundary tones alone (no asterisk) do not raise emphasis. */
    if (strchr(acc, '*')) {
        if (strchr(acc, '+')) return 2;
        return 1;
    }
    return 0;
}

void fe_parsed_flatten_to_slots(const fe_parsed_t *parsed,
                                spfy_fe_slot_t *slots,
                                int slots_cap) {
    if (!parsed || !slots) return;
    int k = 0;
    for (int i = 0; i < parsed->n_words && k < slots_cap; i++) {
        const fe_parsed_word_t *w = &parsed->words[i];
        for (int j = 0; j < w->n_phonemes && k < slots_cap; j++) {
            const fe_parsed_phoneme_t *ph = &w->phonemes[j];
            spfy_fe_slot_t *s = &slots[k++];
            memset(s, 0, sizeof(*s));
            s->emphasis_level = accent_to_emphasis(ph->accent);
            /* ctx, sp, is_voiced and the durt / f0tr fields stay zero; they
             * require a phoneset crosswalk (ARPAbet to engine HP-class IDs)
             * that lives downstream. */
        }
    }
}

/* phoneInSyl: the target-side row index into the VCF's phoneInSylCosts
 * matrix, whose row/column vocabulary is 0 UNDEF 1 WordInitial 2
 * SyllInitial 3 SyllMedial 4 SyllFinal 5 WordFinal 6 SyllUnknown The rule
 * below... */
static uint16_t classify_phone_in_syl(int phi, int n_phon_in_word,
                                      int pi_in_syl, int syl_n) {
    if (phi <= 0)                    return 1;
    if (phi >= n_phon_in_word - 1)   return 5;
    if (pi_in_syl >= syl_n - 1)      return 4;
    if (pi_in_syl <= 0)              return 2;
    return 3;
}

/* Engine phone id for a symbol. */
static uint8_t engine_phone_id(const fe_phone_names_t *pn, const char *sym) {
    if (pn && pn->names) {
        for (uint32_t i = 0; i < pn->n; ++i) {
            if (pn->names[i] && strcmp(pn->names[i], sym) == 0)
                return (uint8_t)i;
        }
        return 0xff;
    }
    return spfy_engine_phoneid_lookup(sym);
}

int fe_parsed_to_full_slots(const fe_parsed_t       *parsed,
                            const spfy_phoneset_t   *ps,
                            const fe_phone_names_t  *pn,
                            spfy_fe_slot_t         **slots_out,
                            uint32_t                *n_slots_out) {
    if (!parsed || !slots_out || !n_slots_out) return -1;
    *slots_out = NULL; *n_slots_out = 0;

    uint32_t n_phons = (uint32_t)fe_parsed_count_phonemes(parsed);
    if (n_phons == 0) return 0;

    /* Each `\!pN` becomes one extra pau PHONE (two halfphone slots) inside
     * the utterance, matching the engine's 66->68 units / 19->20 spans on the
     * wx sentence. A pause after the LAST word is the phrase's trailing pad,
     * which already exists below, so it is not counted here. Nothing sets
     * pause_after_ms unless a `\!pN` was present, so untagged input keeps
     * n_inline_pau == 0 and every index below is unchanged. */
    uint32_t n_inline_pau = 0;
    for (int wi = 0; wi + 1 < parsed->n_words; wi++)
        if (parsed->words[wi].pause_after_ms != 0) n_inline_pau++;

    uint32_t n_slots = (n_phons + n_inline_pau + 2u) * 2u;
    if (spfy_env("SPFY_FE_HOST_DEBUG"))
        fprintf(stderr, "[fe_parse] slots: phons=%u inline_pau=%u n_slots=%u\n",
                n_phons, n_inline_pau, n_slots);
    spfy_fe_slot_t *slots = (spfy_fe_slot_t *)calloc(n_slots, sizeof *slots);
    if (!slots) return -1;

    /* Engine-faithful pau id (32 in en-US per the empirical table). */
    uint8_t pau_id = engine_phone_id(pn, "pau");
    if (pau_id == 0xff) {
        pau_id = (ps && ps->silence_phone_id != 0xff)
                   ? ps->silence_phone_id : 0u;
    }
    int32_t pau_side0 = (int32_t)pau_id * 2;
    int32_t pau_side1 = pau_side0 + 1;

    slots[0].ctx[2] = pau_side0;
    slots[1].ctx[2] = pau_side1;

    if (s_liaison_legacy_syl < 0)
        s_liaison_legacy_syl = (spfy_env("SPFY_FE_LIAISON_LEGACY") != NULL);

    uint32_t global_pi = 0;
    uint32_t syl_in_utt = 0;
    int prev_word = -1;
    int prev_syl_global_index = -1;
    int prev_syl_in_word = -1;

    for (int wi = 0; wi < parsed->n_words; wi++) {
        const fe_parsed_word_t *w = &parsed->words[wi];

        for (int phi = 0; phi < w->n_phonemes; phi++) {
            const fe_parsed_phoneme_t *ph = &w->phonemes[phi];

            /* fr-CA liaison/enchainement: a leading marker-less phone
             * belongs to the PREVIOUS word's final syllable, so it must NOT
             * open a syllable here. */
            const fe_parsed_word_t *pw = (wi > 0) ? &parsed->words[wi - 1] : NULL;
            int cont = (!s_liaison_legacy_syl
                        && w->first_syl_implicit && ph->syl_index == 0
                        && pw && pw->n_syllables > 0 && pw->n_phonemes > 0);

            int this_syl_global = cont
                ? (wi - 1) * 1000 + (pw->n_syllables - 1)
                : wi * 1000 + ph->syl_index;
            if (this_syl_global != prev_syl_global_index) {
                syl_in_utt++;
                prev_syl_global_index = this_syl_global;
                if (wi != prev_word) prev_syl_in_word = -1;
                prev_syl_in_word++;
                prev_word = wi;
            }

            int syl_start = phi, syl_end = phi;
            while (syl_start > 0
                   && w->phonemes[syl_start - 1].syl_index == ph->syl_index)
                syl_start--;
            while (syl_end + 1 < w->n_phonemes
                   && w->phonemes[syl_end + 1].syl_index == ph->syl_index)
                syl_end++;
            int syl_n = syl_end - syl_start + 1;
            int pi_in_syl = phi - syl_start;
            /* The run extends BACKWARDS into the previous word, so the
             * phone's position within its syllable is offset by however
             * many phones that word contributed. */
            if (cont) {
                int prev_tail = 0;
                for (int k = pw->n_phonemes - 1; k >= 0; --k) {
                    if (pw->phonemes[k].syl_index == pw->n_syllables - 1)
                        prev_tail++;
                    else
                        break;
                }
                syl_n += prev_tail;
                pi_in_syl += prev_tail;
            }
            uint16_t phon_in_syl = classify_phone_in_syl(
                phi, w->n_phonemes, pi_in_syl, syl_n);

            /* phone_id (for ctx[2]) comes from the engine's empirical
             * table; voiced still comes from the VCF since the engine table
             * doesn't carry that flag. */
            uint8_t phone_id = 0, voiced = 0;
            uint8_t vid = ps ? spfy_phoneset_lookup(ps, ph->arpabet) : 0xff;
            if (vid != 0xff && vid < ps->n_phones) {
                voiced = ps->entries[vid].is_voiced;
            }
            uint8_t eid = engine_phone_id(pn, ph->arpabet);
            if (eid != 0xff) {
                phone_id = eid;
            } else if (vid != 0xff && vid < ps->n_phones) {
                phone_id = vid;
                fprintf(stderr,
                        "[fe_parse] warn: '%s' not in engine phone-id "
                        "table; falling back to VCF id=%u\n",
                        ph->arpabet, vid);
            }
            uint8_t emph = 0;
            if (ph->accent[0]) {
                if (strchr(ph->accent, '*')) {
                    emph = (strchr(ph->accent, '+')) ? 2u : 1u;
                }
            }

            uint32_t base = (global_pi + 1u) * 2u;
            for (int side = 0; side < 2; side++) {
                spfy_fe_slot_t *s = &slots[base + (uint32_t)side];
                s->ctx[2] = (int32_t)((uint32_t)phone_id * 2u + (uint32_t)side);
                s->is_voiced = voiced;
                s->emphasis_level = emph;
                /* SSML / Balabolka per-word prosody overrides flow from
                 * fe_parsed_word_t into every slot under this word. */
                s->pitch_offset_st = w->pitch_st;
                s->rate_offset_pct = w->rate_pct;
                s->sp[0] = syl_in_utt;
                s->sp[1] = (uint32_t)ph->syl_stress;
                s->sp[2] = (uint32_t)(prev_syl_in_word + 1);
                s->sp[3] = (uint32_t)(wi + 1);
                s->sp[4] = phon_in_syl;
            }

            global_pi++;
        }

        /* Inline pau unit for a `\!pN` that sat between two words. Only
         * ctx[2] is set, exactly like the leading and trailing pads -- the
         * sp[]/emphasis fields stay zero so it selects as a plain pause. */
        if (w->pause_after_ms != 0 && wi + 1 < parsed->n_words) {
            uint32_t pbase = (global_pi + 1u) * 2u;
            slots[pbase + 0].ctx[2] = pau_side0;
            slots[pbase + 1].ctx[2] = pau_side1;
            global_pi++;
        }
    }

    uint32_t tail = (n_phons + n_inline_pau + 1u) * 2u;
    slots[tail + 0].ctx[2] = pau_side0;
    slots[tail + 1].ctx[2] = pau_side1;

    /* Same-side neighbour fill (i ± 2, i ± 4 with pau-encoded edge sentinel
     * matching slot side). */
    for (uint32_t i = 0; i < n_slots; i++) {
        int32_t sentinel = (i & 1u) ? pau_side1 : pau_side0;
        slots[i].ctx[0] = (i >= 4)             ? slots[i - 4].ctx[2] : sentinel;
        slots[i].ctx[1] = (i >= 2)             ? slots[i - 2].ctx[2] : sentinel;
        slots[i].ctx[3] = (i + 2 < n_slots)    ? slots[i + 2].ctx[2] : sentinel;
        slots[i].ctx[4] = (i + 4 < n_slots)    ? slots[i + 4].ctx[2] : sentinel;
    }

    *slots_out = slots;
    *n_slots_out = n_slots;
    return 0;
}

void fe_parsed_debug_dump(const fe_parsed_t *p, FILE *out) {
    if (!p || !out) return;
    fprintf(out, "[fe_parse] pre_pause=%d post_pause=%d words=%d phonemes=%d\n",
            p->pause_before_ms, p->pause_after_ms,
            p->n_words, fe_parsed_count_phonemes(p));
    for (int i = 0; i < p->n_words; i++) {
        const fe_parsed_word_t *w = &p->words[i];
        fprintf(out, "  [%2d] '%s' (%d,%d) %s,%d  syls=%d phns=%d  pau_after=%d\n",
                i, w->text, w->char_start, w->char_len,
                w->pos, w->stress_level, w->n_syllables, w->n_phonemes,
                w->pause_after_ms);
        for (int j = 0; j < w->n_phonemes; j++) {
            const fe_parsed_phoneme_t *ph = &w->phonemes[j];
            fprintf(out, "        %2d: %-4s (p%d) syl=%d stress=%d acc='%s'\n",
                    j, ph->arpabet, ph->duration, ph->syl_index,
                    ph->syl_stress, ph->accent);
        }
    }
}
