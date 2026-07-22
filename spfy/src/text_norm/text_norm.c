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

/* Zero the SSML-related extension fields on a freshly-claimed token. */
static void zero_ext(spfy_token_t *t)
{
    t->phonemes[0] = '\0';
    t->pause_ms    = 0;
    t->pitch_st    = 0;
    t->rate_pct    = 0;
}

/* Apply the currently-active <prosody>/<rate>/<pitch> overrides to a
 * freshly-pushed WORD token. Called by both push_word and
 * push_word_with_phonemes after the token is in place. */
static void apply_prosody(spfy_token_t *t, int8_t pitch_st, int8_t rate_pct)
{
    if (!t || t->type != SPFY_TOKEN_WORD) return;
    if (pitch_st) t->pitch_st = pitch_st;
    if (rate_pct) t->rate_pct = rate_pct;
}

static void push_word(sink_t *s, const char *word)
{
    if (s->overflowed || !word || !*word) return;
    if (s->n >= s->cap) { s->overflowed = 1; return; }
    spfy_token_t *t = &s->out[s->n++];
    t->type = SPFY_TOKEN_WORD;
    zero_ext(t);
    size_t k = strlen(word);
    if (k + 1 > SPFY_TOKEN_TEXT_MAX) k = SPFY_TOKEN_TEXT_MAX - 1;
    memcpy(t->text, word, k);
    t->text[k] = '\0';
}

/* Variant for SSML <phoneme ph="...">word</phoneme>: pushes the WORD
 * with the phoneme override attached so fe_internal bypasses lookup. */
static void push_word_with_phonemes(sink_t *s, const char *word, const char *phonemes)
{
    if (s->overflowed || !word || !*word) return;
    if (s->n >= s->cap) { s->overflowed = 1; return; }
    spfy_token_t *t = &s->out[s->n++];
    t->type = SPFY_TOKEN_WORD;
    zero_ext(t);
    size_t k = strlen(word);
    if (k + 1 > SPFY_TOKEN_TEXT_MAX) k = SPFY_TOKEN_TEXT_MAX - 1;
    memcpy(t->text, word, k);
    t->text[k] = '\0';
    if (phonemes && *phonemes) {
        size_t pk = strlen(phonemes);
        if (pk + 1 > SPFY_TOKEN_PHONEMES_MAX) pk = SPFY_TOKEN_PHONEMES_MAX - 1;
        memcpy(t->phonemes, phonemes, pk);
        t->phonemes[pk] = '\0';
    }
}

static void push_break(sink_t *s, spfy_token_type_t type, char ch)
{
    if (s->overflowed) return;
    /* Collapse repeated breaks — "hello,,," yields one phrase break. */
    if (s->n > 0 && s->out[s->n - 1].type == type) return;
    if (s->n >= s->cap) { s->overflowed = 1; return; }
    spfy_token_t *t = &s->out[s->n++];
    t->type = type;
    zero_ext(t);
    /* Preserve the actual punctuation char so downstream can distinguish
     * `.` / `!` / `?` and `,` / `;` / `:` when emitting boundary tones
     * and opener tags. */
    t->text[0] = ch;
    t->text[1] = '\0';
}

/* SSML <break time="500ms"/> — custom-duration pause token. */
static void push_custom_pause(sink_t *s, uint16_t ms)
{
    if (s->overflowed) return;
    if (s->n >= s->cap) { s->overflowed = 1; return; }
    spfy_token_t *t = &s->out[s->n++];
    t->type     = SPFY_TOKEN_CUSTOM_PAUSE;
    zero_ext(t);
    t->pause_ms = ms;
    t->text[0]  = '\0';
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

/* ---- SSML scanner -------------------------------------------------- *
 *
 * Minimal subset that covers the high-value tags. NOT a conforming
 * XML parser: we don't validate nesting, don't track namespaces, and
 * unknown tags get silently stripped (their inner text still flows
 * through the normal tokenizer). Recognized:
 *
 *   <speak>…</speak>, <p>…</p>, <s>…</s>     wrappers — strip
 *   <break time="Nms" / strength="strong|medium|weak" />
 *                                            custom-pause token
 *   <sub alias="X">Y</sub>                   speak X, skip Y
 *   <say-as interpret-as="characters">ABC</say-as>
 *                                            spell-out per char
 *   <phoneme alphabet="arpabet" ph="..">w</phoneme>
 *                                            attach phoneme override
 *                                            to next WORD token(s)
 *   &amp; &lt; &gt; &apos; &quot;            entity decode
 *
 * `<phoneme alphabet="ipa" ph="...">` is also accepted; the ph value
 * is left as-is (we don't translate IPA → ARPAbet yet — engine will
 * fall back to LTS on unknown phones, which is the conservative move
 * until an IPA table is added). */

/* Case-insensitive prefix match (ASCII). Returns 1 if s starts with pre. */
static int starts_with_ci(const char *s, const char *pre)
{
    while (*pre) {
        char a = *s++; char b = *pre++;
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return 0;
    }
    return 1;
}

/* Strip surrounding whitespace, lowercase, copy into dst. */
static void trim_lower(const char *src, size_t n, char *dst, size_t cap)
{
    while (n > 0 && isspace((unsigned char)*src)) { ++src; --n; }
    while (n > 0 && isspace((unsigned char)src[n - 1])) --n;
    size_t k = 0;
    while (k + 1 < cap && k < n) {
        char c = src[k];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        dst[k++] = c;
    }
    dst[k] = '\0';
}

/* Locate attribute `name` inside the tag body [start, end). Returns 1
 * on hit and fills `val` with the attribute value (unquoted, no length
 * cap beyond `val_cap`); 0 on miss.
 *
 * Tolerates both `attr="value"` and `attr='value'`. Whitespace around
 * `=` is allowed. */
static int ssml_get_attr(const char *start, const char *end,
                         const char *name, char *val, size_t val_cap)
{
    size_t name_len = strlen(name);
    const char *p = start;
    while (p < end) {
        /* Walk to the next char that starts an identifier. */
        while (p < end && !isalpha((unsigned char)*p)) ++p;
        if (p >= end) break;
        const char *attr_start = p;
        while (p < end && (isalnum((unsigned char)*p) || *p == '-' || *p == ':')) ++p;
        size_t attr_len = (size_t)(p - attr_start);
        /* Skip whitespace before `=`. */
        while (p < end && isspace((unsigned char)*p)) ++p;
        if (p >= end || *p != '=') continue;
        ++p;
        while (p < end && isspace((unsigned char)*p)) ++p;
        if (p >= end) break;
        char quote = *p;
        if (quote != '"' && quote != '\'') continue;
        ++p;
        const char *val_start = p;
        while (p < end && *p != quote) ++p;
        size_t val_len = (size_t)(p - val_start);
        if (p < end) ++p;   /* skip closing quote */

        if (attr_len == name_len
            && starts_with_ci(attr_start, name)
            && !isalnum((unsigned char)attr_start[name_len])
            && attr_start[name_len] != '-' && attr_start[name_len] != ':') {
            size_t k = (val_len < val_cap - 1) ? val_len : val_cap - 1;
            memcpy(val, val_start, k);
            val[k] = '\0';
            return 1;
        }
    }
    return 0;
}

/* Parse a duration spec from a `time="500ms"` value, returning ms.
 * Accepts `Nms`, `Ns` (seconds), or bare integer (interpreted as ms).
 * Returns 0 on malformed input. */
static uint16_t parse_break_time_ms(const char *val)
{
    if (!val || !*val) return 0;
    char *end = NULL;
    long n = strtol(val, &end, 10);
    if (n < 0)     n = 0;
    if (n > 65535) n = 65535;
    if (end && *end) {
        while (*end && isspace((unsigned char)*end)) ++end;
        if (starts_with_ci(end, "s") && !isalpha((unsigned char)end[1])) {
            n *= 1000;
        }
        /* Anything else (ms, or unknown unit) leaves n as ms. */
    }
    if (n > 65535) n = 65535;
    return (uint16_t)n;
}

/* Map <break strength="..."> to a millisecond budget. SSML spec
 * suggests these rough values; tuned to match this engine's existing
 * comma vs sentence-end pause durations. */
static uint16_t break_strength_to_ms(const char *val)
{
    if (!val || !*val) return 250;                  /* default = medium */
    if (starts_with_ci(val, "none"))     return 0;
    if (starts_with_ci(val, "x-weak"))   return 60;
    if (starts_with_ci(val, "weak"))     return 100;
    if (starts_with_ci(val, "medium"))   return 250;
    if (starts_with_ci(val, "strong"))   return 500;
    if (starts_with_ci(val, "x-strong")) return 1000;
    return 250;
}

/* ---- prosody value parsing --------------------------------------- *
 *
 * Parses rate / pitch attribute strings from both SSML and Balabolka
 * conventions into our internal int8 representation:
 *
 *   pitch_st  signed semitones. 0 = neutral. Clamped to [-12, +12].
 *   rate_pct  signed percent. 0 = neutral. Positive = faster (durations
 *             shrink). -50 = half speed, +100 = double speed. Clamped
 *             to [-90, +127] so int8 range holds.
 *
 * Conventions handled:
 *
 *   SSML <prosody rate="...">
 *     "x-slow"  → -60   (~0.5x)
 *     "slow"    → -30   (~0.7x)
 *     "medium"  →   0
 *     "fast"    → +30   (~1.3x)
 *     "x-fast"  → +100  (~2.0x)
 *     "+50%" / "150%" → +50
 *     "+10%" → +10, "-25%" → -25
 *
 *   SSML <prosody pitch="...">
 *     "+5st" / "-5st"  → ±5 semitones
 *     "+5%" / "-5%"    → ±~0.85 semitones (5/12 mapping)
 *     "x-low" → -6, "low" → -3, "medium" → 0, "high" → +3, "x-high" → +6
 *
 *   Balabolka <rate absspeed="N"> / <pitch absmiddle="N">
 *     SAPI -10..+10 range. Rate factor = 3^(N/10), so:
 *       absspeed +10 → factor 3.0  → rate_pct = +200 (clamped to +127)
 *       absspeed  0  → factor 1.0  → rate_pct = 0
 *       absspeed -10 → factor 0.333 → rate_pct = -66
 *     Pitch is ~1:1 semitones (Balabolka uses a slightly different
 *     curve but for practical purposes absmiddle N ≈ N semitones).
 */

static int8_t clamp_int8(int v, int lo, int hi)
{
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (int8_t)v;
}

/* Parse SSML rate attribute. Returns rate_pct (signed percent delta). */
static int8_t parse_ssml_rate(const char *val)
{
    if (!val || !*val) return 0;
    if (starts_with_ci(val, "x-slow")) return -60;
    if (starts_with_ci(val, "slow"))   return -30;
    if (starts_with_ci(val, "medium")) return 0;
    if (starts_with_ci(val, "default")) return 0;
    if (starts_with_ci(val, "x-fast")) return clamp_int8(100, -90, 127);
    if (starts_with_ci(val, "fast"))   return 30;
    /* Numeric: "+50%", "-25%", "150%", "1.5" (multiplier). */
    char *end = NULL;
    double n = strtod(val, &end);
    if (end == val) return 0;
    while (*end == ' ') ++end;
    if (*end == '%') {
        /* "+50%" → +50, "150%" → +50 (relative). If no sign, treat
         * 100% as neutral. */
        int rel = (val[0] == '+' || val[0] == '-') ? (int)n : (int)(n - 100);
        return clamp_int8(rel, -90, 127);
    }
    /* Bare multiplier: 1.0 = neutral, 1.5 = +50, 0.5 = -50. */
    if (n > 0.0) {
        int rel = (int)((n - 1.0) * 100.0);
        return clamp_int8(rel, -90, 127);
    }
    return 0;
}

/* Parse SSML pitch attribute. Returns pitch_st (signed semitones). */
static int8_t parse_ssml_pitch(const char *val)
{
    if (!val || !*val) return 0;
    if (starts_with_ci(val, "x-low"))   return -6;
    if (starts_with_ci(val, "low"))     return -3;
    if (starts_with_ci(val, "medium"))  return 0;
    if (starts_with_ci(val, "default")) return 0;
    if (starts_with_ci(val, "x-high"))  return +6;
    if (starts_with_ci(val, "high"))    return +3;
    /* Numeric. Suffix tells us unit. */
    char *end = NULL;
    double n = strtod(val, &end);
    if (end == val) return 0;
    while (*end == ' ') ++end;
    if (!*end) return clamp_int8((int)n, -12, 12);    /* bare = semitones */
    if (starts_with_ci(end, "st"))
        return clamp_int8((int)n, -12, 12);
    if (starts_with_ci(end, "hz")) {
        /* Hz delta: assume neutral ~120 Hz; +12 Hz ≈ +1 ST.
         * Rough but better than ignoring the attribute. */
        return clamp_int8((int)(n / 12.0), -12, 12);
    }
    if (*end == '%') {
        /* +N% of frequency ≈ N/8 semitones (since 2^(1/12) ≈ 1.06). */
        return clamp_int8((int)(n / 8.0), -12, 12);
    }
    return clamp_int8((int)n, -12, 12);
}

/* Balabolka SAPI absspeed (-10..+10) → rate_pct.
 * Factor curve = 3^(absspeed/10); rate_pct = (factor - 1) * 100. */
static int8_t balabolka_absspeed_to_rate_pct(int absspeed)
{
    if (absspeed < -10) absspeed = -10;
    if (absspeed >  10) absspeed =  10;
    /* Precomputed (factor - 1) × 100 for integer absspeed values to
     * avoid pulling in expf at this layer. */
    static const int LUT[21] = {
        /* -10..-1 */ -66, -61, -54, -47, -38, -29, -19, -7, +7, +21,
        /*    0   */   0,
        /* +1..+10*/ +27, +35, +44, +54, +64, +75, +88,+102,+117,+127
    };
    /* LUT indexing: -10 → 0, 0 → 10 (NOT the middle index — it's
     * positioned at the end of the negative bank). Recompute index. */
    int idx;
    if (absspeed <= -1)      idx = absspeed + 10;        /* -10..-1 → 0..9 */
    else if (absspeed == 0)  idx = 10;
    else                     idx = absspeed + 10;        /* +1..+10 → 11..20 */
    return clamp_int8(LUT[idx], -90, 127);
}

/* Balabolka SAPI absmiddle (-10..+10) → pitch_st. Roughly 1:1. */
static int8_t balabolka_absmiddle_to_pitch_st(int absmiddle)
{
    if (absmiddle < -12) absmiddle = -12;
    if (absmiddle >  12) absmiddle =  12;
    return (int8_t)absmiddle;
}

/* Convert a Balabolka-style `<pron sym="...">` value to engine-native
 * ARPAbet. Balabolka conventions vs our engine:
 *
 *   - Tokens are lowercase ("p aa t ax").
 *   - Stress is a SEPARATE token (1 = primary, 2 = secondary) following
 *     the vowel: "p aa 1 t ax" = P AA1 T AX0.
 *   - Uses `h` for /h/ where the engine uses `hh`.
 *
 * We produce uppercase ARPAbet with stress digits suffixed onto vowels
 * (the format `split_phonemes` expects). Unstressed vowels default to
 * stress 0. Consonants get no digit. Unknown tokens pass through as-is
 * uppercased — fe_internal's split_phonemes will skip / LTS them.
 *
 * `out_cap` includes the trailing NUL. */
static void balabolka_to_arpabet(const char *sym, char *out, size_t out_cap)
{
    if (out_cap == 0) return;
    out[0] = '\0';
    if (!sym) return;

    /* Vowel set used to decide whether a stress digit applies. */
    static const char *VOWELS[] = {
        "aa","ae","ah","ao","aw","ax","ay",
        "eh","er","ey","ih","iy","ow","oy","uh","uw", NULL
    };

    size_t off = 0;
    const char *p = sym;
    char prev_token[8] = {0};   /* lowercase, no stress */
    int  prev_is_vowel  = 0;
    int  prev_emitted   = 0;    /* how many chars of prev are already in out */
    while (*p) {
        while (*p && isspace((unsigned char)*p)) ++p;
        if (!*p) break;

        /* Pull the next whitespace-delimited token, lowercase. */
        char tok[8] = {0};
        size_t k = 0;
        while (*p && !isspace((unsigned char)*p) && k + 1 < sizeof tok) {
            char c = *p++;
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            tok[k++] = c;
        }
        /* Discard the rest of an oversized token. */
        while (*p && !isspace((unsigned char)*p)) ++p;

        /* Standalone digit → stress for the previous vowel. */
        if (k == 1 && tok[0] >= '0' && tok[0] <= '9') {
            if (prev_is_vowel && prev_emitted > 0
                && off + 1 < out_cap) {
                out[off++] = tok[0];
                out[off]   = '\0';
            }
            /* Even if the previous wasn't a vowel, swallow the digit. */
            prev_is_vowel = 0;
            prev_emitted  = 0;
            continue;
        }

        /* Token is a phoneme. If it's a vowel WITHOUT an explicit stress
         * digit, the previous vowel was already finalized — backfill
         * a 0 onto it if we haven't written one yet. */
        if (prev_is_vowel && prev_emitted > 0 && off + 1 < out_cap) {
            out[off++] = '0';
            out[off]   = '\0';
        }
        prev_is_vowel = 0;
        prev_emitted  = 0;

        /* h → hh. */
        const char *arpa = tok;
        if (strcmp(tok, "h") == 0) arpa = "hh";

        /* Append separator + uppercased token. */
        if (off > 0 && off + 1 < out_cap) { out[off++] = ' '; out[off] = '\0'; }
        size_t a_start = off;
        for (size_t j = 0; arpa[j] && off + 1 < out_cap; ++j) {
            char c = arpa[j];
            if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
            out[off++] = c;
        }
        out[off] = '\0';
        prev_emitted = (int)(off - a_start);

        /* Track whether the just-emitted token is a vowel. */
        for (int v = 0; VOWELS[v]; ++v) {
            if (strcmp(tok, VOWELS[v]) == 0) { prev_is_vowel = 1; break; }
        }
        /* snprintf rather than strncpy: same truncate-and-terminate
         * semantics, but without GCC's -Wstringop-truncation complaint
         * about strncpy not NUL-terminating on a full-length copy. */
        snprintf(prev_token, sizeof prev_token, "%s", tok);
    }

    /* Trailing unstressed vowel — finalize with 0. */
    if (prev_is_vowel && prev_emitted > 0 && off + 1 < out_cap) {
        out[off++] = '0';
        out[off]   = '\0';
    }
}

/* Decode a single XML entity at `*p` (which must point at '&'). On hit
 * returns the decoded ASCII char and advances `*p` past the entity.
 * On miss returns 0 and leaves `*p` unchanged. */
static int xml_decode_entity(const char **p_in_out)
{
    const char *p = *p_in_out;
    if (*p != '&') return 0;
    const char *q = p + 1;
    const char *semi = strchr(q, ';');
    if (!semi || semi - q > 6) return 0;
    char buf[8] = {0};
    size_t L = (size_t)(semi - q);
    for (size_t i = 0; i < L && i < 7; ++i) {
        char c = q[i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        buf[i] = c;
    }
    int out = 0;
    if      (!strcmp(buf, "amp"))   out = '&';
    else if (!strcmp(buf, "lt"))    out = '<';
    else if (!strcmp(buf, "gt"))    out = '>';
    else if (!strcmp(buf, "apos"))  out = '\'';
    else if (!strcmp(buf, "quot"))  out = '"';
    else return 0;
    *p_in_out = semi + 1;
    return out;
}

/* ---- public API --------------------------------------------------- */

int spfy_text_normalize(const char *input,
                        spfy_token_t *out, size_t cap, size_t *out_n)
{
    if (!input || !out || cap == 0 || !out_n) return -1;
    sink_t s = { out, cap, 0, 0 };

    /* SSML state. Updated when we enter/exit known tags; consulted when
     * the main loop emits a WORD token. Prosody state is a small stack
     * (depth 4) so nested <prosody>/<rate>/<pitch> regions restore the
     * outer value on close. Overflow saturates at the cap — pragmatic
     * for hand-authored SSML, which rarely nests 4+ deep. */
    char ssml_phonemes[SPFY_TOKEN_PHONEMES_MAX] = {0};
    int  ssml_spell_chars   = 0;
    int  ssml_spell_digits  = 0;

    #define SSML_PROSODY_STACK_MAX 4
    int8_t prosody_pitch_stack[SSML_PROSODY_STACK_MAX] = {0};
    int8_t prosody_rate_stack [SSML_PROSODY_STACK_MAX] = {0};
    int    prosody_depth = 0;
    /* current_* are the running totals (sum down the stack) — applied
     * additively so nested tags compose: outer "fast" + inner "+5st"
     * yields both. */
    int8_t cur_pitch_st = 0;
    int8_t cur_rate_pct = 0;

    const char *p = input;
    while (*p) {
        int c = (unsigned char)*p;

        /* ---- XML entity decode (&amp; &lt; …) --------------------- */
        if (c == '&') {
            const char *save = p;
            int dec = xml_decode_entity(&p);
            if (dec) { c = dec; /* fall through with decoded char */ }
            else     { p = save; /* malformed — leave & in place */ }
        }

        /* ---- SSML tag handling ------------------------------------- */
        if (c == '<') {
            const char *tag_end = strchr(p, '>');
            if (!tag_end) { ++p; continue; }    /* malformed: drop the '<' */
            const char *tag_body = p + 1;
            int is_close = (tag_body < tag_end && *tag_body == '/');
            if (is_close) ++tag_body;

            /* Extract tag name. */
            const char *name = tag_body;
            const char *name_end = name;
            while (name_end < tag_end
                   && (isalnum((unsigned char)*name_end) || *name_end == '-'))
                ++name_end;
            size_t name_len = (size_t)(name_end - name);

            /* Dispatch. Self-closing tags are detected by `/>`. */
            int self_close = (tag_end > tag_body && tag_end[-1] == '/');

            if (name_len == 5 && starts_with_ci(name, "speak")) {
                /* root wrapper — nothing to do */
            } else if ((name_len == 1 && (name[0] == 'p' || name[0] == 's'))) {
                /* <p>/<s> wrappers — engine treats as paragraph/sentence;
                 * we map close tags to sentence breaks for prosody. */
                if (is_close) push_break(&s, SPFY_TOKEN_SENTENCE_BREAK, '.');
            } else if (name_len == 5 && starts_with_ci(name, "break")) {
                /* Custom-duration pause. `time="Nms"` wins over `strength`. */
                char val[32];
                uint16_t ms = 0;
                if (ssml_get_attr(name_end, tag_end, "time", val, sizeof val))
                    ms = parse_break_time_ms(val);
                else if (ssml_get_attr(name_end, tag_end, "strength", val, sizeof val))
                    ms = break_strength_to_ms(val);
                else
                    ms = 250;   /* spec default — medium */
                if (ms > 0) push_custom_pause(&s, ms);
                (void)self_close;
            } else if (name_len == 4 && starts_with_ci(name, "pron")) {
                /* Balabolka pronunciation. Two forms:
                 *   <pron sym="..."/>                   — self-closing
                 *   <pron sym="...">annotation</pron>   — with text content
                 *
                 * Self-closing form uses "_pron_" as the WORD text (an
                 * underscore-padded placeholder won't accidentally hit
                 * fe_internal's auto-break / abbreviation / POS heuristics).
                 * The annotated form uses the inner text — useful when
                 * the user wants the tagged-text dump (and any future
                 * downstream consumer) to see the intended spelling. */
                if (is_close) {
                    /* nothing — already emitted at open */
                } else {
                    char sym[160];
                    if (ssml_get_attr(name_end, tag_end, "sym", sym, sizeof sym)) {
                        char converted[SPFY_TOKEN_PHONEMES_MAX];
                        balabolka_to_arpabet(sym, converted, sizeof converted);
                        if (converted[0]) {
                            char word_text[SPFY_TOKEN_TEXT_MAX] = "_pron_";
                            const char *skip_to = NULL;
                            if (!self_close) {
                                const char *close = strstr(tag_end + 1, "</pron>");
                                if (close) {
                                    /* Capture trimmed lowercase annotation. */
                                    const char *cs = tag_end + 1, *ce = close;
                                    while (cs < ce && isspace((unsigned char)*cs)) ++cs;
                                    while (ce > cs && isspace((unsigned char)ce[-1])) --ce;
                                    size_t L = (size_t)(ce - cs);
                                    if (L > 0) {
                                        if (L >= sizeof word_text) L = sizeof word_text - 1;
                                        for (size_t j = 0; j < L; ++j) {
                                            char ch = cs[j];
                                            if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
                                            word_text[j] = ch;
                                        }
                                        word_text[L] = '\0';
                                    }
                                    /* Resume after </pron>. */
                                    const char *gt = strchr(close, '>');
                                    if (gt) skip_to = gt + 1;
                                }
                            }
                            push_word_with_phonemes(&s, word_text, converted);
                            if (skip_to) { p = skip_to; continue; }
                        }
                    }
                }
            } else if (name_len == 7 && starts_with_ci(name, "phoneme")) {
                if (is_close) {
                    ssml_phonemes[0] = '\0';
                } else {
                    char ph[SPFY_TOKEN_PHONEMES_MAX];
                    if (ssml_get_attr(name_end, tag_end, "ph", ph, sizeof ph)) {
                        size_t k = strlen(ph);
                        if (k + 1 > sizeof ssml_phonemes)
                            k = sizeof ssml_phonemes - 1;
                        memcpy(ssml_phonemes, ph, k);
                        ssml_phonemes[k] = '\0';
                    }
                }
            } else if (name_len == 3 && starts_with_ci(name, "sub")) {
                if (is_close) {
                    /* nothing — alias was already emitted at open */
                } else {
                    /* Speak the alias instead of the inner content.
                     * We find </sub>, emit tokens for the alias text,
                     * then jump past the inner content. */
                    char alias[128];
                    if (ssml_get_attr(name_end, tag_end, "alias", alias, sizeof alias)) {
                        /* Tokenize alias inline — recurse via a local
                         * mini-loop so we don't blow the call stack on
                         * nested subs. The alias is treated as plain
                         * text (no further SSML parsing). */
                        const char *ap = alias;
                        while (*ap) {
                            unsigned char ac = (unsigned char)*ap;
                            if (isspace(ac)) { ++ap; continue; }
                            if (is_word_char(ac)) {
                                char wbuf[SPFY_TOKEN_TEXT_MAX]; size_t wk = 0;
                                while (*ap && is_word_char((unsigned char)*ap)
                                       && wk + 1 < sizeof wbuf)
                                    wbuf[wk++] = *ap++;
                                wbuf[wk] = '\0';
                                to_lower_buf(wbuf);
                                push_word(&s, wbuf);
                            } else {
                                ++ap;
                            }
                        }
                    }
                    /* Skip past inner content to </sub>. */
                    const char *close = strstr(tag_end + 1, "</sub>");
                    if (close) {
                        const char *gt = strchr(close, '>');
                        if (gt) { p = gt + 1; continue; }
                    }
                }
            } else if (name_len == 6 && starts_with_ci(name, "say-as")) {
                if (is_close) {
                    ssml_spell_chars = 0;
                    ssml_spell_digits = 0;
                } else {
                    char mode[32];
                    if (ssml_get_attr(name_end, tag_end, "interpret-as", mode, sizeof mode)) {
                        char m[32];
                        trim_lower(mode, strlen(mode), m, sizeof m);
                        if (!strcmp(m, "characters") || !strcmp(m, "spell-out"))
                            ssml_spell_chars = 1;
                        else if (!strcmp(m, "digits"))
                            ssml_spell_digits = 1;
                    }
                }
            } else if (name_len == 7 && starts_with_ci(name, "prosody")) {
                /* SSML <prosody rate="..." pitch="..."> — push the
                 * deltas onto the prosody stack on open, pop on close. */
                if (is_close) {
                    if (prosody_depth > 0) {
                        --prosody_depth;
                        cur_pitch_st = (int8_t)(cur_pitch_st - prosody_pitch_stack[prosody_depth]);
                        cur_rate_pct = (int8_t)(cur_rate_pct - prosody_rate_stack [prosody_depth]);
                    }
                } else if (prosody_depth < SSML_PROSODY_STACK_MAX) {
                    int8_t dp = 0, dr = 0;
                    char val[32];
                    if (ssml_get_attr(name_end, tag_end, "pitch", val, sizeof val))
                        dp = parse_ssml_pitch(val);
                    if (ssml_get_attr(name_end, tag_end, "rate", val, sizeof val))
                        dr = parse_ssml_rate(val);
                    prosody_pitch_stack[prosody_depth] = dp;
                    prosody_rate_stack [prosody_depth] = dr;
                    ++prosody_depth;
                    cur_pitch_st = (int8_t)clamp_int8(cur_pitch_st + dp, -12, 12);
                    cur_rate_pct = (int8_t)clamp_int8(cur_rate_pct + dr, -90, 127);
                }
            } else if (name_len == 4 && starts_with_ci(name, "rate")) {
                /* Balabolka <rate absspeed="N"> / <rate speed="N"> */
                if (is_close) {
                    if (prosody_depth > 0) {
                        --prosody_depth;
                        cur_pitch_st = (int8_t)(cur_pitch_st - prosody_pitch_stack[prosody_depth]);
                        cur_rate_pct = (int8_t)(cur_rate_pct - prosody_rate_stack [prosody_depth]);
                    }
                } else if (prosody_depth < SSML_PROSODY_STACK_MAX) {
                    int8_t dr = 0;
                    char val[32];
                    if (ssml_get_attr(name_end, tag_end, "absspeed", val, sizeof val))
                        dr = balabolka_absspeed_to_rate_pct(atoi(val));
                    else if (ssml_get_attr(name_end, tag_end, "speed", val, sizeof val))
                        /* Relative SAPI form — treat the same as absspeed
                         * since we don't track an absolute current rate. */
                        dr = balabolka_absspeed_to_rate_pct(atoi(val));
                    prosody_pitch_stack[prosody_depth] = 0;
                    prosody_rate_stack [prosody_depth] = dr;
                    ++prosody_depth;
                    cur_rate_pct = (int8_t)clamp_int8(cur_rate_pct + dr, -90, 127);
                }
            } else if (name_len == 5 && starts_with_ci(name, "pitch")) {
                /* Balabolka <pitch absmiddle="N"> / <pitch middle="N"> */
                if (is_close) {
                    if (prosody_depth > 0) {
                        --prosody_depth;
                        cur_pitch_st = (int8_t)(cur_pitch_st - prosody_pitch_stack[prosody_depth]);
                        cur_rate_pct = (int8_t)(cur_rate_pct - prosody_rate_stack [prosody_depth]);
                    }
                } else if (prosody_depth < SSML_PROSODY_STACK_MAX) {
                    int8_t dp = 0;
                    char val[32];
                    if (ssml_get_attr(name_end, tag_end, "absmiddle", val, sizeof val))
                        dp = balabolka_absmiddle_to_pitch_st(atoi(val));
                    else if (ssml_get_attr(name_end, tag_end, "middle", val, sizeof val))
                        dp = balabolka_absmiddle_to_pitch_st(atoi(val));
                    prosody_pitch_stack[prosody_depth] = dp;
                    prosody_rate_stack [prosody_depth] = 0;
                    ++prosody_depth;
                    cur_pitch_st = (int8_t)clamp_int8(cur_pitch_st + dp, -12, 12);
                }
            }
            /* Unknown tag — silently strip. Inner text flows through. */
            p = tag_end + 1;
            continue;
        }

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
            /* SSML <say-as interpret-as="characters"> — emit one WORD
             * per letter so the existing letter-spell-out path in
             * fe_internal handles each as a noun letter-name. */
            if (ssml_spell_chars) {
                char one[2] = {0, 0};
                for (size_t j = 0; j < k; ++j) {
                    one[0] = buf[j];
                    push_word(&s, one);
                    if ((cur_pitch_st || cur_rate_pct) && s.n > 0)
                        apply_prosody(&s.out[s.n - 1], cur_pitch_st, cur_rate_pct);
                }
            } else if (ssml_phonemes[0]) {
                push_word_with_phonemes(&s, buf, ssml_phonemes);
                if ((cur_pitch_st || cur_rate_pct) && s.n > 0)
                    apply_prosody(&s.out[s.n - 1], cur_pitch_st, cur_rate_pct);
            } else {
                push_word(&s, buf);
                if ((cur_pitch_st || cur_rate_pct) && s.n > 0)
                    apply_prosody(&s.out[s.n - 1], cur_pitch_st, cur_rate_pct);
            }
            continue;
        }

        /* Number run: digits + optional .frac, with optional ordinal
         * suffix (1st / 2nd / 3rd / 4th / 21st / 26th…). */
        if (isdigit(c)) {
            const char *digits = p;
            while (*p && isdigit((unsigned char)*p)) ++p;
            size_t n_int = (size_t)(p - digits);
            /* SSML <say-as interpret-as="digits">123</say-as>:
             * emit "one two three" instead of "one hundred twenty three". */
            if (ssml_spell_digits) {
                static const char *digit_names[] = {
                    "zero","one","two","three","four",
                    "five","six","seven","eight","nine"
                };
                size_t n_before = s.n;
                for (size_t j = 0; j < n_int; ++j) {
                    int d = digits[j] - '0';
                    if (d >= 0 && d <= 9) push_word(&s, digit_names[d]);
                }
                if (cur_pitch_st || cur_rate_pct) {
                    for (size_t j = n_before; j < s.n; ++j)
                        apply_prosody(&s.out[j], cur_pitch_st, cur_rate_pct);
                }
                continue;
            }
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
            size_t num_n_before = s.n;
            emit_number(&s, digits, n_int, frac, n_frac);
            if (is_ordinal) make_last_word_ordinal(&s);
            if (cur_pitch_st || cur_rate_pct) {
                for (size_t j = num_n_before; j < s.n; ++j)
                    apply_prosody(&s.out[j], cur_pitch_st, cur_rate_pct);
            }
            continue;
        }

        /* Punctuation. */
        if (is_sentence_end(c)) { push_break(&s, SPFY_TOKEN_SENTENCE_BREAK, (char)c); ++p; continue; }
        if (is_phrase_break(c)) { push_break(&s, SPFY_TOKEN_PHRASE_BREAK,   (char)c); ++p; continue; }

        /* Anything else: drop. */
        ++p;
    }

    *out_n = s.n;
    return s.overflowed ? 1 : 0;
}
