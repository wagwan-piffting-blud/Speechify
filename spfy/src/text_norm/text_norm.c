/* text_norm.c — see text_norm.h.
 *
 * Tokenization strategy: single-pass walk through the input. We classify
 * each character into one of {alpha, digit, punct-break, sentence-end,
 * whitespace, other}, and accumulate runs.
 *
 *   - Alpha runs become WORD tokens (lowercased).
 *   - Digit runs (with optional embedded '.') become number-expansion
 *     WORD tokens — multi-token output for "42" → "forty" + "two".
 *   - Sentence-end punct (. ! ?) emits a SENTENCE_BREAK token.
 *   - Phrase-break punct (, ; : ( )) emits a PHRASE_BREAK token.
 *   - Other chars are silently dropped (treat as whitespace).
 *
 * Number expansion is a small hand-rolled English cardinal/year decoder.
 * Covers 0 - 999,999,999. Year heuristic kicks in for 4-digit numbers in
 * [1000, 2999] — those expand as "nineteen ninety" rather than "one
 * thousand nine hundred ninety". */

#include "text_norm.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- output helpers ----------------------------------------------- */

typedef struct {
    spfy_token_t *out;
    size_t cap;
    size_t n;
    int    overflowed;
} sink_t;

static void push_word(sink_t *s, const char *word)
{
    if (s->overflowed || !word || !*word) return;
    if (s->n >= s->cap) { s->overflowed = 1; return; }
    spfy_token_t *t = &s->out[s->n++];
    t->type = SPFY_TOKEN_WORD;
    size_t k = strlen(word);
    if (k + 1 > SPFY_TOKEN_TEXT_MAX) k = SPFY_TOKEN_TEXT_MAX - 1;
    memcpy(t->text, word, k);
    t->text[k] = '\0';
}

static void push_break(sink_t *s, spfy_token_type_t type, char ch)
{
    if (s->overflowed) return;
    /* Collapse repeated breaks — "hello,,," yields one phrase break. */
    if (s->n > 0 && s->out[s->n - 1].type == type) return;
    if (s->n >= s->cap) { s->overflowed = 1; return; }
    spfy_token_t *t = &s->out[s->n++];
    t->type = type;
    /* Preserve the actual punctuation char so downstream can distinguish
     * `.` / `!` / `?` and `,` / `;` / `:` when emitting boundary tones
     * and opener tags. */
    t->text[0] = ch;
    t->text[1] = '\0';
}

/* ---- number expansion --------------------------------------------- */

static const char *under_20[] = {
    "zero","one","two","three","four","five","six","seven","eight","nine",
    "ten","eleven","twelve","thirteen","fourteen","fifteen","sixteen",
    "seventeen","eighteen","nineteen"
};
static const char *tens[] = {
    "", "", "twenty","thirty","forty","fifty","sixty","seventy","eighty",
    "ninety"
};

static void emit_under_1000(sink_t *s, int n)
{
    if (n >= 100) {
        push_word(s, under_20[n / 100]);
        push_word(s, "hundred");
        n %= 100;
        if (n == 0) return;
    }
    if (n >= 20) {
        push_word(s, tens[n / 10]);
        n %= 10;
        if (n == 0) return;
        push_word(s, under_20[n]);
    } else if (n > 0) {
        push_word(s, under_20[n]);
    } else {
        /* n == 0: only emit "zero" if we're at top-level (caller's job) */
    }
}

static void emit_year(sink_t *s, int year)
{
    int hi = year / 100;
    int lo = year % 100;
    if (lo == 0) {
        /* "1900" → "nineteen hundred" */
        emit_under_1000(s, hi);
        push_word(s, "hundred");
    } else if (lo < 10) {
        /* "1907" → "nineteen oh seven" */
        emit_under_1000(s, hi);
        push_word(s, "oh");
        push_word(s, under_20[lo]);
    } else {
        /* "1990" → "nineteen ninety", "2024" → "twenty twenty four" */
        emit_under_1000(s, hi);
        emit_under_1000(s, lo);
    }
}

static void emit_cardinal(sink_t *s, long n)
{
    if (n == 0) { push_word(s, "zero"); return; }
    if (n < 0)  { push_word(s, "negative"); n = -n; }

    long billions = n / 1000000000;
    long rem = n % 1000000000;
    if (billions > 0) {
        emit_under_1000(s, (int)billions);
        push_word(s, "billion");
    }
    long millions = rem / 1000000;
    rem %= 1000000;
    if (millions > 0) {
        emit_under_1000(s, (int)millions);
        push_word(s, "million");
    }
    long thousands = rem / 1000;
    rem %= 1000;
    if (thousands > 0) {
        emit_under_1000(s, (int)thousands);
        push_word(s, "thousand");
    }
    if (rem > 0) {
        emit_under_1000(s, (int)rem);
    }
}

/* Cardinal → ordinal word mapping for the LAST word of a multi-word
 * cardinal expansion. "twenty six" + "th" → swap "six" → "sixth" =
 * "twenty sixth". "twenty" + "th" → "twentieth". */
static const char *cardinal_to_ordinal(const char *card)
{
    static const struct { const char *card; const char *ord; } map[] = {
        { "one",       "first"      },
        { "two",       "second"     },
        { "three",     "third"      },
        { "four",      "fourth"     },
        { "five",      "fifth"      },
        { "six",       "sixth"      },
        { "seven",     "seventh"    },
        { "eight",     "eighth"     },
        { "nine",      "ninth"      },
        { "ten",       "tenth"      },
        { "eleven",    "eleventh"   },
        { "twelve",    "twelfth"    },
        { "thirteen",  "thirteenth" },
        { "fourteen",  "fourteenth" },
        { "fifteen",   "fifteenth"  },
        { "sixteen",   "sixteenth"  },
        { "seventeen", "seventeenth"},
        { "eighteen",  "eighteenth" },
        { "nineteen",  "nineteenth" },
        { "twenty",    "twentieth"  },
        { "thirty",    "thirtieth"  },
        { "forty",     "fortieth"   },
        { "fifty",     "fiftieth"   },
        { "sixty",     "sixtieth"   },
        { "seventy",   "seventieth" },
        { "eighty",    "eightieth"  },
        { "ninety",    "ninetieth"  },
        { "hundred",   "hundredth"  },
        { "thousand",  "thousandth" },
        { "million",   "millionth"  },
        { "billion",   "billionth"  },
        { NULL, NULL }
    };
    for (int i = 0; map[i].card; ++i)
        if (strcmp(map[i].card, card) == 0) return map[i].ord;
    return NULL;
}

static void make_last_word_ordinal(sink_t *s)
{
    if (s->n == 0) return;
    spfy_token_t *last = &s->out[s->n - 1];
    if (last->type != SPFY_TOKEN_WORD) return;
    const char *ord = cardinal_to_ordinal(last->text);
    if (!ord) return;
    size_t k = strlen(ord);
    if (k + 1 > SPFY_TOKEN_TEXT_MAX) k = SPFY_TOKEN_TEXT_MAX - 1;
    memcpy(last->text, ord, k);
    last->text[k] = '\0';
}

/* Emit a digit-run as words. Detects 4-digit years and decimals. */
static void emit_number(sink_t *s, const char *digits, size_t n,
                         const char *frac, size_t frac_n)
{
    /* If there's a fractional part, read int part then "point" then
     * digit-by-digit on the fractional part. */
    if (frac_n > 0) {
        char int_buf[32];
        size_t ki = n < sizeof int_buf ? n : sizeof int_buf - 1;
        memcpy(int_buf, digits, ki); int_buf[ki] = '\0';
        long ival = atol(int_buf);
        emit_cardinal(s, ival);
        push_word(s, "point");
        for (size_t i = 0; i < frac_n; ++i) {
            char d = frac[i];
            if (d >= '0' && d <= '9') push_word(s, under_20[d - '0']);
        }
        return;
    }

    /* Year heuristic: 4-digit numbers in [1000, 2999] are pronounced
     * as years rather than as a cardinal. Exception: 2000-2009 reads
     * naturally as "two thousand [and N]" — fall through to the
     * cardinal branch for that decade. */
    if (n == 4) {
        char buf[8];
        memcpy(buf, digits, 4); buf[4] = '\0';
        int y = atoi(buf);
        if (y >= 1000 && y <= 2999 && !(y >= 2000 && y < 2010)) {
            emit_year(s, y);
            return;
        }
    }

    /* Otherwise: cardinal. */
    char buf[32];
    size_t k = n < sizeof buf - 1 ? n : sizeof buf - 1;
    memcpy(buf, digits, k); buf[k] = '\0';
    long val = atol(buf);
    emit_cardinal(s, val);
}

/* ---- character classification ------------------------------------- */

static int is_word_char(int c)
{
    /* Apostrophes are kept inside words: "don't", "speechify's" — only
     * the dict / suffix-strip stage decides what to do with them. */
    return isalpha((unsigned char)c) || c == '\'';
}

static int is_sentence_end(int c)
{
    return c == '.' || c == '!' || c == '?';
}

static int is_phrase_break(int c)
{
    return c == ',' || c == ';' || c == ':'
        || c == '(' || c == ')' || c == '"';
}

/* Lowercase an ASCII word in place, with length cap. */
static void to_lower_buf(char *s)
{
    for (; *s; ++s) {
        if (*s >= 'A' && *s <= 'Z') *s += ('a' - 'A');
    }
}

/* ---- public API --------------------------------------------------- */

int spfy_text_normalize(const char *input,
                        spfy_token_t *out, size_t cap, size_t *out_n)
{
    if (!input || !out || cap == 0 || !out_n) return -1;
    sink_t s = { out, cap, 0, 0 };

    const char *p = input;
    while (*p) {
        int c = (unsigned char)*p;

        /* Whitespace: skip. */
        if (isspace(c)) { ++p; continue; }

        /* Word run: alpha + apostrophe. */
        if (is_word_char(c)) {
            char buf[SPFY_TOKEN_TEXT_MAX];
            size_t k = 0;
            while (*p && is_word_char((unsigned char)*p)
                   && k + 1 < sizeof buf) {
                buf[k++] = *p++;
            }
            buf[k] = '\0';
            to_lower_buf(buf);
            push_word(&s, buf);
            continue;
        }

        /* Number run: digits + optional .frac, with optional ordinal
         * suffix (1st / 2nd / 3rd / 4th / 21st / 26th…). */
        if (isdigit(c)) {
            const char *digits = p;
            while (*p && isdigit((unsigned char)*p)) ++p;
            size_t n_int = (size_t)(p - digits);
            const char *frac = NULL;
            size_t n_frac = 0;
            if (*p == '.' && isdigit((unsigned char)p[1])) {
                frac = p + 1;
                ++p;
                while (*p && isdigit((unsigned char)*p)) ++p;
                n_frac = (size_t)((p) - frac);
            }
            /* Ordinal suffix — only when there's no fractional part. */
            int is_ordinal = 0;
            if (n_frac == 0 && p[0] && p[1]) {
                int s1 = (p[0] >= 'A' && p[0] <= 'Z') ? p[0] + ('a' - 'A') : p[0];
                int s2 = (p[1] >= 'A' && p[1] <= 'Z') ? p[1] + ('a' - 'A') : p[1];
                /* Require word boundary after suffix so "5that" doesn't
                 * eat "th" as ordinal. */
                int after = (unsigned char)p[2];
                int at_boundary = !after || !is_word_char(after);
                if (at_boundary
                    && ((s1 == 's' && s2 == 't')      /* Nst  */
                     || (s1 == 'n' && s2 == 'd')      /* Nnd  */
                     || (s1 == 'r' && s2 == 'd')      /* Nrd  */
                     || (s1 == 't' && s2 == 'h'))) {  /* Nth  */
                    is_ordinal = 1;
                    p += 2;
                }
            }
            emit_number(&s, digits, n_int, frac, n_frac);
            if (is_ordinal) make_last_word_ordinal(&s);
            continue;
        }

        /* Punctuation. */
        if (is_sentence_end(c)) { push_break(&s, SPFY_TOKEN_SENTENCE_BREAK, c); ++p; continue; }
        if (is_phrase_break(c)) { push_break(&s, SPFY_TOKEN_PHRASE_BREAK,   c); ++p; continue; }

        /* Anything else: drop. */
        ++p;
    }

    *out_n = s.n;
    return s.overflowed ? 1 : 0;
}
