/* SSML -> embedded-tag translation. See ssml.h for why this is a translator
 * rather than a parser. */

#include "ssml.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Growable output buffer                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *p;
    size_t n;
    size_t cap;
    int    oom;
} sbuf_t;

static void sb_init(sbuf_t *b, size_t cap)
{
    b->p = (char *)malloc(cap);
    b->n = 0;
    b->cap = b->p ? cap : 0;
    b->oom = b->p ? 0 : 1;
}

static int sb_grow(sbuf_t *b, size_t need)
{
    if (b->oom) return 0;
    if (b->n + need + 1 <= b->cap) return 1;
    size_t want = b->cap ? b->cap : 256;
    while (want < b->n + need + 1) {
        if (want > (size_t)-1 / 2) { b->oom = 1; return 0; }
        want *= 2;
    }
    char *q = (char *)realloc(b->p, want);
    if (!q) { b->oom = 1; return 0; }
    b->p = q;
    b->cap = want;
    return 1;
}

static void sb_ch(sbuf_t *b, char c)
{
    if (!sb_grow(b, 1)) return;
    b->p[b->n++] = c;
}

static void sb_str(sbuf_t *b, const char *s)
{
    size_t l = strlen(s);
    if (!sb_grow(b, l)) return;
    memcpy(b->p + b->n, s, l);
    b->n += l;
}

static void sb_int(sbuf_t *b, int v)
{
    char t[16];
    snprintf(t, sizeof t, "%d", v);
    sb_str(b, t);
}

/* ------------------------------------------------------------------ */
/* Small text helpers                                                  */
/* ------------------------------------------------------------------ */

static int ci_eq_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        int x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return 0;
        if (x == 0) return 1;
    }
    return 1;
}

/* Element name at `s` of length `n` equals `kw` (whole word, case-blind). */
static int name_is(const char *s, size_t n, const char *kw)
{
    return strlen(kw) == n && ci_eq_n(s, kw, n);
}

/* Signed decimal at *pp, returned scaled by 1000 ("-3.5" -> -3500). Leaves
 * *pp on the first unconsumed character so a unit suffix can be read. */
static int scan_milli(const char **pp, long *out)
{
    const char *p = *pp;
    int sign = 1;
    if (*p == '+') ++p;
    else if (*p == '-') { sign = -1; ++p; }
    if (!isdigit((unsigned char)*p) && *p != '.') return 0;

    long whole = 0;
    int  any = 0;
    while (isdigit((unsigned char)*p)) { whole = whole * 10 + (*p - '0'); ++p; any = 1; }
    long frac = 0, scale = 1000;
    if (*p == '.') {
        ++p;
        while (isdigit((unsigned char)*p)) {
            if (scale > 1) { scale /= 10; frac += (*p - '0') * scale; }
            ++p;
            any = 1;
        }
    }
    if (!any) return 0;
    *out = sign * (whole * 1000 + frac);
    *pp = p;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Pitch percent <-> semitones                                         */
/* ------------------------------------------------------------------ */

/* Round-trips over the whole clamped range: +8 st -> 159% -> +8 st,
 * -8 st -> 63% -> -8 st, +-1 st -> 106% / 94% -> +-1 st. */
int spfy_ssml_pct_to_semitones(int pct)
{
    if (pct <= 0) return 0;
    double st = 12.0 * log((double)pct / 100.0) / log(2.0);
    int r = (int)lround(st);
    if (r < -24) r = -24;
    if (r >  24) r =  24;
    return r;
}

int spfy_ssml_semitones_to_pct(int st)
{
    if (st < -24) st = -24;
    if (st >  24) st =  24;
    int r = (int)lround(100.0 * pow(2.0, (double)st / 12.0));
    if (r < 25)  r = 25;
    if (r > 400) r = 400;
    return r;
}

/* ------------------------------------------------------------------ */
/* SSML attribute-value parsing                                        */
/* ------------------------------------------------------------------ */

/* <prosody volume>. Percent of the CURRENT value, so nesting composes. */
static int volume_pct(const char *v, int cur)
{
    if (!v || !*v) return cur;
    if (name_is(v, strlen(v), "silent")) return 0;
    if (name_is(v, strlen(v), "x-soft")) return 25;
    if (name_is(v, strlen(v), "soft"))   return 50;
    if (name_is(v, strlen(v), "medium")) return 100;
    if (name_is(v, strlen(v), "loud"))   return 150;
    if (name_is(v, strlen(v), "x-loud")) return 200;

    const char *p = v;
    int relative = (*p == '+' || *p == '-');
    long m = 0;
    if (!scan_milli(&p, &m)) return cur;

    if (ci_eq_n(p, "db", 2)) {
        /* SSML volume in decibels is always relative. */
        double f = pow(10.0, (double)m / 1000.0 / 20.0);
        int r = (int)lround((double)cur * f);
        return r < 0 ? 0 : (r > 400 ? 400 : r);
    }
    if (*p == '%') {
        /* `m` is the value scaled by 1000, so "+20%" arrives as 20000 and a
         * relative factor is (100 + m/1000) / 100 == (100000 + m) / 100000.
         * An UNSIGNED percentage is read as a percentage OF THE DEFAULT, not
         * of the enclosing element: SSML leaves this ambiguous, and every
         * engine a user is likely to have written for -- Polly, Azure --
         * reads `rate="50%"` as half the default rather than half of
         * whatever a parent tag happened to set. */
        if (relative) {
            int r = (int)lround((double)cur * (100000.0 + (double)m) / 100000.0);
            return r < 0 ? 0 : (r > 400 ? 400 : r);
        }
        int r = (int)(m / 1000);        /* "50%" -> 50 */
        return r < 0 ? 0 : (r > 400 ? 400 : r);
    }
    /* Bare number: SSML 1.0 gives volume on a 0..100 scale where 100 is the
     * maximum, which is the same axis our percent map already uses. */
    {
        int r = (int)(m / 1000);
        return r < 0 ? 0 : (r > 400 ? 400 : r);
    }
}

/* <prosody rate>. 100 = unchanged, larger is faster.
 *
 * Emitted as `\!rp`, which is now what the vendor engine's own rate control
 * does: scale the duration targets and time-scale each selected unit onto
 * them, plosives passed through untouched and pauses scaled with the speech.
 *
 * ⚠ This used to emit `\!wp` on the reasoning that `\!rp` "biases selection
 * and saturates around +9% in the slow direction". That was true of spfy's
 * OLD `\!rp` and it was never true of the engine's: measured on the vendor
 * binary, `\!rp50` gives 1.969x and `\!rp33` gives 2.991x. The saturation
 * was spfy's bug, not the tag's meaning, so the workaround goes with it.
 * `\!wp` remains available as a literal time-scale at any factor. */
static int rate_pct(const char *v, int cur)
{
    if (!v || !*v) return cur;
    if (name_is(v, strlen(v), "x-slow")) return 50;
    if (name_is(v, strlen(v), "slow"))   return 70;
    if (name_is(v, strlen(v), "medium")) return 100;
    if (name_is(v, strlen(v), "fast"))   return 130;
    if (name_is(v, strlen(v), "x-fast")) return 160;
    if (name_is(v, strlen(v), "default")) return 100;

    const char *p = v;
    int relative = (*p == '+' || *p == '-');
    long m = 0;
    if (!scan_milli(&p, &m)) return cur;

    if (*p == '%') {
        if (relative)
            return (int)lround((double)cur * (100000.0 + (double)m) / 100000.0);
        return (int)(m / 1000);
    }
    /* SSML 1.1 non-negative MULTIPLIER: rate="1.5" means half again as fast.
     * Note the divisor differs from the `%` branch above and that is not a
     * slip -- "150%" is 150 on this scale, "1.5" is also 150, and `m` holds
     * 150000 in the first case and 1500 in the second. */
    if (m < 0) return cur;
    return (int)(m / 10);
}

/* <prosody pitch>, carried as a percentage of the base F0 so it shares the
 * `\!vp`/`\!rp` tag shape; the consumer converts back to semitones. */
static int pitch_pct(const char *v, int cur)
{
    if (!v || !*v) return cur;
    if (name_is(v, strlen(v), "x-low"))  return spfy_ssml_semitones_to_pct(-8);
    if (name_is(v, strlen(v), "low"))    return spfy_ssml_semitones_to_pct(-4);
    if (name_is(v, strlen(v), "medium")) return 100;
    if (name_is(v, strlen(v), "high"))   return spfy_ssml_semitones_to_pct(4);
    if (name_is(v, strlen(v), "x-high")) return spfy_ssml_semitones_to_pct(8);
    if (name_is(v, strlen(v), "default")) return 100;

    const char *p = v;
    int relative = (*p == '+' || *p == '-');
    long m = 0;
    if (!scan_milli(&p, &m)) return cur;

    if (ci_eq_n(p, "st", 2)) {
        /* Semitones compose by ADDITION, which is multiplication in percent. */
        int base_st = spfy_ssml_pct_to_semitones(cur);
        return spfy_ssml_semitones_to_pct(base_st + (int)lround((double)m / 1000.0));
    }
    if (ci_eq_n(p, "hz", 2)) {
        /* An absolute target needs the voice's base F0, which is not known
         * this far upstream. Ignored rather than guessed. */
        return cur;
    }
    if (*p == '%') {
        if (relative)
            return (int)lround((double)cur * (100000.0 + (double)m) / 100000.0);
        return (int)(m / 1000);
    }
    return cur;
}

/* <break time="..."> */
static int break_time_ms(const char *v)
{
    if (!v || !*v) return 0;
    const char *p = v;
    long m = 0;
    if (!scan_milli(&p, &m)) return 0;
    if (m < 0) return 0;
    /* "1s" and "1500ms" both land in milliseconds. `ms` must be tested
     * first -- a prefix test for "s" matches the 's' in "ms". */
    if (ci_eq_n(p, "ms", 2)) return (int)(m / 1000);
    if (*p == 's' || *p == 'S') return (int)m;
    return (int)(m / 1000);          /* bare number: milliseconds */
}

/* <break strength="..."> */
static int break_strength_ms(const char *v)
{
    if (!v || !*v) return 250;
    size_t n = strlen(v);
    if (name_is(v, n, "none"))     return 0;
    if (name_is(v, n, "x-weak"))   return 50;
    if (name_is(v, n, "weak"))     return 100;
    if (name_is(v, n, "medium"))   return 250;
    if (name_is(v, n, "strong"))   return 500;
    if (name_is(v, n, "x-strong")) return 1000;
    return 250;
}

/* ------------------------------------------------------------------ */
/* Phonetic alphabets -> the `<pron sym=...>` dialect                   */
/* ------------------------------------------------------------------ */

/* text_norm.c's balabolka_to_arpabet() reads space-separated lowercase
 * ARPAbet and attaches a LONE DIGIT token to the vowel before it, so
 * "hh ax 0 l ow 1" is how "HH AX0 L OW1" has to be spelled going in. */
static void arpabet_to_sym(const char *ph, sbuf_t *out)
{
    const char *p = ph;
    int wrote = 0;
    while (*p) {
        while (*p && !isalnum((unsigned char)*p)) ++p;
        if (!*p) break;
        char tok[16];
        size_t k = 0;
        while (*p && isalpha((unsigned char)*p) && k + 1 < sizeof tok) {
            char c = *p++;
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            tok[k++] = c;
        }
        tok[k] = '\0';
        /* The stress digit rides on the phone in ARPAbet ("OW1") but is its
         * own token here. */
        char stress = 0;
        if (isdigit((unsigned char)*p)) stress = *p++;
        while (*p && isalnum((unsigned char)*p)) ++p;   /* junk after a digit */

        if (k) {
            if (wrote) sb_ch(out, ' ');
            sb_str(out, strcmp(tok, "h") == 0 ? "hh" : tok);
            wrote = 1;
        }
        if (stress) {
            if (wrote) sb_ch(out, ' ');
            sb_ch(out, stress);
            wrote = 1;
        }
    }
}

/* Longest-match first: the digraphs must precede their own first characters
 * or "a" swallows the "a" of "aʊ". */
static const struct { const char *ipa; const char *arpa; int vowel; } IPA_MAP[] = {
    { "a\xCA\x8A", "aw", 1 },   /* aʊ */
    { "a\xC9\xAA", "ay", 1 },   /* aɪ */
    { "o\xCA\x8A", "ow", 1 },   /* oʊ */
    { "e\xC9\xAA", "ey", 1 },   /* eɪ */
    { "\xC9\x94\xC9\xAA", "oy", 1 }, /* ɔɪ */
    { "t\xCA\x83", "ch", 0 },   /* tʃ */
    { "d\xCA\x92", "jh", 0 },   /* dʒ */
    { "\xC9\x91", "aa", 1 },    /* ɑ */
    { "\xC3\xA6", "ae", 1 },    /* æ */
    { "\xCA\x8C", "ah", 1 },    /* ʌ */
    { "\xC9\x94", "ao", 1 },    /* ɔ */
    { "\xC9\x99", "ax", 1 },    /* ə */
    { "\xC9\x9B", "eh", 1 },    /* ɛ */
    { "\xC9\x9C", "er", 1 },    /* ɜ */
    { "\xC9\x9D", "er", 1 },    /* ɝ */
    { "\xC9\x9A", "er", 1 },    /* ɚ */
    { "\xC9\xAA", "ih", 1 },    /* ɪ */
    { "\xCA\x8A", "uh", 1 },    /* ʊ */
    { "\xC5\x8B", "ng", 0 },    /* ŋ */
    { "\xC3\xB0", "dh", 0 },    /* ð */
    { "\xCE\xB8", "th", 0 },    /* θ */
    { "\xCA\x83", "sh", 0 },    /* ʃ */
    { "\xCA\x92", "zh", 0 },    /* ʒ */
    { "\xC9\xB9", "r",  0 },    /* ɹ */
    { "\xC9\xA1", "g",  0 },    /* ɡ (U+0261, not ASCII g) */
    { "i", "iy", 1 }, { "u", "uw", 1 }, { "o", "ow", 1 }, { "e", "ey", 1 },
    { "a", "aa", 1 },
    { "b", "b", 0 }, { "d", "d", 0 }, { "f", "f", 0 }, { "g", "g", 0 },
    { "h", "hh", 0 }, { "j", "y", 0 }, { "k", "k", 0 }, { "l", "l", 0 },
    { "m", "m", 0 }, { "n", "n", 0 }, { "p", "p", 0 }, { "r", "r", 0 },
    { "s", "s", 0 }, { "t", "t", 0 }, { "v", "v", 0 }, { "w", "w", 0 },
    { "z", "z", 0 },
};

/* IPA stress marks PRECEDE their syllable; the sym dialect wants the digit
 * AFTER the vowel, so a mark is held until the next vowel is emitted. */
static void ipa_to_sym(const char *ph, sbuf_t *out)
{
    const char *p = ph;
    int wrote = 0;
    char pending = 0;
    while (*p) {
        if (strncmp(p, "\xCB\x88", 2) == 0) { pending = '1'; p += 2; continue; }
        if (strncmp(p, "\xCB\x8C", 2) == 0) { pending = '2'; p += 2; continue; }
        if (*p == '.' || *p == ' ' || *p == '-') { ++p; continue; }
        /* Length marks and ties carry no ARPAbet distinction. */
        if (strncmp(p, "\xCB\x90", 2) == 0) { p += 2; continue; }
        if (strncmp(p, "\xCD\xA1", 2) == 0) { p += 2; continue; }

        size_t hit = (size_t)-1;
        for (size_t i = 0; i < sizeof IPA_MAP / sizeof IPA_MAP[0]; ++i) {
            size_t l = strlen(IPA_MAP[i].ipa);
            if (strncmp(p, IPA_MAP[i].ipa, l) == 0) { hit = i; break; }
        }
        if (hit == (size_t)-1) {
            /* Unmapped codepoint: skip its whole UTF-8 sequence rather than
             * one byte, or the continuation bytes become garbage phones. */
            ++p;
            while ((*p & 0xC0) == 0x80) ++p;
            continue;
        }
        if (wrote) sb_ch(out, ' ');
        sb_str(out, IPA_MAP[hit].arpa);
        wrote = 1;
        if (IPA_MAP[hit].vowel) {
            sb_ch(out, ' ');
            sb_ch(out, pending ? pending : '0');
            pending = 0;
        }
        p += strlen(IPA_MAP[hit].ipa);
    }
}

/* SSML makes `alphabet` required; plenty of real documents omit it. Anything
 * outside ASCII is IPA, because ARPAbet has no non-ASCII spelling. */
static int ph_looks_like_ipa(const char *ph)
{
    for (const char *p = ph; *p; ++p)
        if ((unsigned char)*p >= 0x80) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* XML scaffolding                                                     */
/* ------------------------------------------------------------------ */

/* Copy the value of attribute `want` from the attribute region [s, e) into
 * `out`. Returns 1 when found. */
static int get_attr(const char *s, const char *e, const char *want,
                    char *out, size_t cap)
{
    size_t wl = strlen(want);
    const char *p = s;
    while (p < e) {
        while (p < e && (isspace((unsigned char)*p) || *p == '/')) ++p;
        if (p >= e) break;
        const char *ns = p;
        while (p < e && *p != '=' && !isspace((unsigned char)*p) && *p != '/') ++p;
        size_t nl = (size_t)(p - ns);
        while (p < e && isspace((unsigned char)*p)) ++p;
        if (p >= e || *p != '=') continue;
        ++p;
        while (p < e && isspace((unsigned char)*p)) ++p;
        char quote = 0;
        if (p < e && (*p == '"' || *p == '\'')) quote = *p++;
        const char *vs = p;
        if (quote) while (p < e && *p != quote) ++p;
        else       while (p < e && !isspace((unsigned char)*p) && *p != '/') ++p;
        size_t vl = (size_t)(p - vs);
        if (p < e && quote) ++p;

        /* An `xml:` or namespace prefix is not part of the name we match. */
        const char *nc = (const char *)memchr(ns, ':', nl);
        if (nc) { nl -= (size_t)(nc + 1 - ns); ns = nc + 1; }

        if (nl == wl && ci_eq_n(ns, want, wl)) {
            if (vl >= cap) vl = cap - 1;
            memcpy(out, vs, vl);
            out[vl] = '\0';
            return 1;
        }
    }
    return 0;
}

/* Decode one entity at `*pp` (which points at '&'); 0 when it is not one. */
static int decode_entity(const char **pp, sbuf_t *out)
{
    const char *p = *pp + 1;
    const char *semi = strchr(p, ';');
    if (!semi || semi - p > 10) return 0;
    size_t n = (size_t)(semi - p);

    if (*p == '#') {
        long cp = 0;
        if (p[1] == 'x' || p[1] == 'X') {
            for (const char *q = p + 2; q < semi; ++q) {
                int d;
                if (isdigit((unsigned char)*q)) d = *q - '0';
                else if (*q >= 'a' && *q <= 'f') d = *q - 'a' + 10;
                else if (*q >= 'A' && *q <= 'F') d = *q - 'A' + 10;
                else return 0;
                cp = cp * 16 + d;
            }
        } else {
            for (const char *q = p + 1; q < semi; ++q) {
                if (!isdigit((unsigned char)*q)) return 0;
                cp = cp * 10 + (*q - '0');
            }
        }
        if (cp <= 0 || cp > 0x10FFFF) return 0;
        /* Back to UTF-8 -- the FE is fed bytes, not codepoints. */
        if (cp < 0x80) {
            sb_ch(out, (char)cp);
        } else if (cp < 0x800) {
            sb_ch(out, (char)(0xC0 | (cp >> 6)));
            sb_ch(out, (char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            sb_ch(out, (char)(0xE0 | (cp >> 12)));
            sb_ch(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
            sb_ch(out, (char)(0x80 | (cp & 0x3F)));
        } else {
            sb_ch(out, (char)(0xF0 | (cp >> 18)));
            sb_ch(out, (char)(0x80 | ((cp >> 12) & 0x3F)));
            sb_ch(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
            sb_ch(out, (char)(0x80 | (cp & 0x3F)));
        }
        *pp = semi + 1;
        return 1;
    }

    static const struct { const char *name; const char *text; } ENT[] = {
        { "amp", "&" }, { "lt", "<" }, { "gt", ">" },
        { "quot", "\"" }, { "apos", "'" }, { "nbsp", " " },
    };
    for (size_t i = 0; i < sizeof ENT / sizeof ENT[0]; ++i) {
        if (strlen(ENT[i].name) == n && ci_eq_n(p, ENT[i].name, n)) {
            sb_str(out, ENT[i].text);
            *pp = semi + 1;
            return 1;
        }
    }
    return 0;
}

/* Element names this translator claims. `pron` is NOT here on purpose: it is
 * an existing engine feature that build_inline_mixed_tagged() handles, and
 * consuming it here would delete it. */
static const char *const SSML_NAMES[] = {
    "speak", "prosody", "break", "emphasis", "phoneme", "say-as", "sub",
    "voice", "audio", "mark", "p", "s", "lexicon", "meta", "metadata", "desc",
};

static int is_ssml_name(const char *s, size_t n)
{
    for (size_t i = 0; i < sizeof SSML_NAMES / sizeof SSML_NAMES[0]; ++i)
        if (name_is(s, n, SSML_NAMES[i])) return 1;
    return 0;
}

int spfy_ssml_detect(const char *text)
{
    if (!text) return 0;
    for (const char *p = strchr(text, '<'); p; p = strchr(p + 1, '<')) {
        const char *q = p + 1;
        if (*q == '/') ++q;
        const char *ns = q;
        while (*q && (isalnum((unsigned char)*q) || *q == '-' || *q == ':')) ++q;
        size_t nl = (size_t)(q - ns);
        if (!nl) continue;
        /* A real tag boundary. `a < b` and `x<3` never reach here. */
        if (*q != '>' && *q != '/' && !isspace((unsigned char)*q)) continue;
        const char *nc = (const char *)memchr(ns, ':', nl);
        if (nc) { nl -= (size_t)(nc + 1 - ns); ns = nc + 1; }
        if (is_ssml_name(ns, nl)) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* The translator                                                      */
/* ------------------------------------------------------------------ */

#define SSML_DEPTH_MAX 32

typedef struct {
    char  name[16];
    int   vol, rate, pitch;      /* state to restore on close  */
    int   spell, year;           /* ditto for \!ts* and \!ny*  */
    char  emph[8];               /* ToBI tag active in this span, "" if none */
    int   restores_prosody;
    int   restores_speech;
    int   restores_emph;
    int   closes_pron;           /* <phoneme> emitted a `<pron ...>` open   */
    int   skip_content;          /* <sub>, <desc>: drop until the close tag */
} frame_t;

/* One-shot per word, so it is re-emitted at every word start inside the span
 * rather than once for the span. */
static void emit_emph(sbuf_t *out, const char *tobi)
{
    sb_str(out, "\\![ToBI:");
    sb_str(out, tobi);
    sb_ch(out, ']');
}

static const char *emph_to_tobi(const char *level)
{
    if (!level || !*level) return "H*";
    size_t n = strlen(level);
    if (name_is(level, n, "strong"))   return "L+H*";
    if (name_is(level, n, "moderate")) return "H*";
    if (name_is(level, n, "reduced"))  return "NONE";
    if (name_is(level, n, "none"))     return "NONE";
    return "H*";
}

char *spfy_ssml_to_etags(const char *ssml)
{
    if (!ssml) return NULL;

    sbuf_t out;
    sb_init(&out, strlen(ssml) * 2 + 256);
    if (out.oom) return NULL;

    frame_t stack[SSML_DEPTH_MAX];
    int top = 0;

    int vol = 100, rate = 100, pitch = 100;
    int spell = 0, year = 0;
    char emph[8] = "";
    int  at_word_start = 1;        /* next non-space char begins a word */
    int  skip_depth = 0;           /* >0 while inside <sub>/<desc> content */

    const char *p = ssml;
    while (*p) {
        if (*p == '&' && !skip_depth) {
            const char *q = p;
            if (decode_entity(&q, &out)) { p = q; at_word_start = 0; continue; }
        }
        if (*p != '<') {
            if (skip_depth) { ++p; continue; }
            if (isspace((unsigned char)*p)) {
                at_word_start = 1;
            } else {
                if (at_word_start && emph[0]) emit_emph(&out, emph);
                at_word_start = 0;
            }
            sb_ch(&out, *p++);
            continue;
        }

        /* ---- non-element markup ---------------------------------- */
        if (strncmp(p, "<!--", 4) == 0) {
            const char *e = strstr(p + 4, "-->");
            p = e ? e + 3 : p + strlen(p);
            continue;
        }
        if (strncmp(p, "<![CDATA[", 9) == 0) {
            const char *e = strstr(p + 9, "]]>");
            const char *stop = e ? e : p + strlen(p);
            if (!skip_depth)
                for (const char *q = p + 9; q < stop; ++q) sb_ch(&out, *q);
            p = e ? e + 3 : stop;
            at_word_start = 0;
            continue;
        }
        if (p[1] == '?' || p[1] == '!') {
            const char *e = strchr(p, '>');
            p = e ? e + 1 : p + strlen(p);
            continue;
        }

        /* ---- element ---------------------------------------------- */
        const char *q = p + 1;
        int closing = 0;
        if (*q == '/') { closing = 1; ++q; }
        const char *ns = q;
        while (*q && (isalnum((unsigned char)*q) || *q == '-' || *q == ':')) ++q;
        size_t nl = (size_t)(q - ns);
        const char *gt = strchr(q, '>');
        if (!nl || !gt) {                       /* not a tag after all */
            if (!skip_depth) sb_ch(&out, *p);
            ++p;
            at_word_start = 0;
            continue;
        }
        const char *nc = (const char *)memchr(ns, ':', nl);
        if (nc) { nl -= (size_t)(nc + 1 - ns); ns = nc + 1; }
        int self_closing = (gt > p && gt[-1] == '/');
        const char *attrs = q, *attrs_end = self_closing ? gt - 1 : gt;

        /* Anything we do not claim passes through byte-for-byte, which is
         * what keeps `<pron sym="...">` working. */
        if (!is_ssml_name(ns, nl)) {
            if (!skip_depth) { while (p <= gt) sb_ch(&out, *p++); }
            else p = gt + 1;
            at_word_start = 0;
            continue;
        }
        p = gt + 1;

        if (closing) {
            /* Unwind to the matching frame; an unbalanced close is ignored
             * rather than allowed to corrupt the stack. */
            int i;
            for (i = top - 1; i >= 0; --i)
                if (name_is(ns, nl, stack[i].name)) break;
            if (i < 0) continue;
            for (int j = top - 1; j >= i; --j) {
                frame_t *f = &stack[j];
                if (f->closes_pron) { sb_str(&out, "</pron>"); at_word_start = 1; }
                if (f->skip_content && skip_depth) --skip_depth;
                if (f->restores_emph) {
                    snprintf(emph, sizeof emph, "%s", f->emph);
                }
                if (f->restores_speech) {
                    if (spell != f->spell) {
                        spell = f->spell;
                        sb_str(&out, spell == 'c' ? "\\!tsc " :
                                     spell == 'a' ? "\\!tsa " :
                                     spell == 'r' ? "\\!tsr " : "\\!ts0 ");
                    }
                    if (year != f->year) {
                        year = f->year;
                        sb_str(&out, year ? "\\!ny0 " : "\\!ny1 ");
                    }
                }
                if (f->restores_prosody) {
                    if (vol != f->vol)   { vol = f->vol;     sb_str(&out, "\\!vp"); sb_int(&out, vol);   sb_ch(&out, ' '); }
                    if (rate != f->rate) { rate = f->rate;   sb_str(&out, "\\!rp"); sb_int(&out, rate);  sb_ch(&out, ' '); }
                    if (pitch != f->pitch){ pitch = f->pitch; sb_str(&out, "\\!pp"); sb_int(&out, pitch); sb_ch(&out, ' '); }
                }
                if (name_is(ns, nl, "p") || name_is(ns, nl, "s")) {
                    /* A paragraph or sentence element IS a phrase boundary,
                     * whether or not the author typed the full stop.
                     *
                     * ⚠ Look past TRAILING WHITESPACE, not just at the last
                     * byte. `</s></p>` closes both in a row, and the `</s>`
                     * has already written ". " -- testing out.p[n-1] sees the
                     * space, decides there is no terminator, and `<p><s>one
                     * </s></p>` comes out as "one. . " with a phantom empty
                     * phrase the synth then renders as a pause. */
                    size_t k = out.n;
                    while (k && isspace((unsigned char)out.p[k - 1])) --k;
                    if (k && !strchr(".!?,;", out.p[k - 1])) {
                        out.n = k;
                        sb_ch(&out, '.');
                    }
                    if (out.n && !isspace((unsigned char)out.p[out.n - 1]))
                        sb_ch(&out, ' ');
                    at_word_start = 1;
                }
            }
            top = i;
            continue;
        }

        /* ---- opening (or empty) element --------------------------- */
        frame_t f;
        memset(&f, 0, sizeof f);
        snprintf(f.name, sizeof f.name, "%.*s", (int)nl, ns);
        f.vol = vol; f.rate = rate; f.pitch = pitch;
        f.spell = spell; f.year = year;
        snprintf(f.emph, sizeof f.emph, "%s", emph);

        char av[128];

        if (name_is(ns, nl, "break")) {
            int ms = 0;
            if (get_attr(attrs, attrs_end, "time", av, sizeof av))
                ms = break_time_ms(av);
            else if (get_attr(attrs, attrs_end, "strength", av, sizeof av))
                ms = break_strength_ms(av);
            else
                ms = 250;
            if (ms > 0) {
                if (ms > 32767) ms = 32767;
                sb_str(&out, " \\!p");
                sb_int(&out, ms);
                sb_ch(&out, ' ');
            }
            at_word_start = 1;
            continue;                       /* always empty */
        }

        if (name_is(ns, nl, "mark") || name_is(ns, nl, "lexicon")
            || name_is(ns, nl, "meta") || name_is(ns, nl, "metadata")) {
            if (!self_closing && !name_is(ns, nl, "mark")) {
                /* <metadata> has content nobody should hear. */
                f.skip_content = 1;
                ++skip_depth;
                if (top < SSML_DEPTH_MAX) stack[top++] = f;
            }
            continue;
        }

        if (name_is(ns, nl, "desc")) {
            /* Alternate text for <audio>; never spoken when the audio is. */
            if (!self_closing) {
                f.skip_content = 1;
                ++skip_depth;
                if (top < SSML_DEPTH_MAX) stack[top++] = f;
            }
            continue;
        }

        if (name_is(ns, nl, "sub")) {
            /* The alias replaces the content outright. */
            if (get_attr(attrs, attrs_end, "alias", av, sizeof av)) {
                if (at_word_start && emph[0]) emit_emph(&out, emph);
                sb_str(&out, av);
                at_word_start = 0;
            }
            if (!self_closing) {
                f.skip_content = 1;
                ++skip_depth;
                if (top < SSML_DEPTH_MAX) stack[top++] = f;
            }
            continue;
        }

        if (name_is(ns, nl, "phoneme")) {
            if (get_attr(attrs, attrs_end, "ph", av, sizeof av) && av[0]) {
                char alpha[32] = "";
                get_attr(attrs, attrs_end, "alphabet", alpha, sizeof alpha);
                int ipa = alpha[0]
                        ? (ci_eq_n(alpha, "ipa", 3) && strlen(alpha) == 3)
                        : ph_looks_like_ipa(av);
                sbuf_t sym;
                sb_init(&sym, 256);
                if (!sym.oom) {
                    if (ipa) ipa_to_sym(av, &sym);
                    else     arpabet_to_sym(av, &sym);
                    sym.p[sym.n] = '\0';
                    if (sym.n) {
                        sb_str(&out, "<pron sym=\"");
                        sb_str(&out, sym.p);
                        sb_str(&out, "\">");
                        f.closes_pron = 1;
                        at_word_start = 0;
                    }
                }
                free(sym.p);
            }
            if (!self_closing && top < SSML_DEPTH_MAX) stack[top++] = f;
            else if (f.closes_pron) sb_str(&out, "</pron>");
            continue;
        }

        if (name_is(ns, nl, "say-as")) {
            char how[32] = "";
            get_attr(attrs, attrs_end, "interpret-as", how, sizeof how);
            size_t hn = strlen(how);
            int new_spell = spell, new_year = year;
            if (name_is(how, hn, "characters") || name_is(how, hn, "spell-out")) {
                new_spell = 'c';
            } else if (name_is(how, hn, "digits")
                       || name_is(how, hn, "telephone")
                       || name_is(how, hn, "verbatim")) {
                new_spell = 'a';
            } else if (name_is(how, hn, "date") || name_is(how, hn, "year")) {
                /* \!ny0 is what turns "1985" into "nineteen eighty five". */
                new_year = 1;
            }
            if (new_spell != spell) {
                spell = new_spell;
                sb_str(&out, spell == 'c' ? "\\!tsc " :
                             spell == 'a' ? "\\!tsa " :
                             spell == 'r' ? "\\!tsr " : "\\!ts0 ");
                f.restores_speech = 1;
            }
            if (new_year != year) {
                year = new_year;
                sb_str(&out, year ? "\\!ny0 " : "\\!ny1 ");
                f.restores_speech = 1;
            }
            at_word_start = 1;
            if (!self_closing && top < SSML_DEPTH_MAX) stack[top++] = f;
            continue;
        }

        if (name_is(ns, nl, "emphasis")) {
            char level[16] = "";
            get_attr(attrs, attrs_end, "level", level, sizeof level);
            snprintf(emph, sizeof emph, "%s", emph_to_tobi(level));
            f.restores_emph = 1;
            at_word_start = 1;
            if (!self_closing && top < SSML_DEPTH_MAX) stack[top++] = f;
            else snprintf(emph, sizeof emph, "%s", f.emph);
            continue;
        }

        if (name_is(ns, nl, "prosody")) {
            if (get_attr(attrs, attrs_end, "volume", av, sizeof av)) {
                int nv = volume_pct(av, vol);
                if (nv != vol) {
                    vol = nv;
                    sb_str(&out, "\\!vp"); sb_int(&out, vol); sb_ch(&out, ' ');
                    f.restores_prosody = 1;
                }
            }
            if (get_attr(attrs, attrs_end, "rate", av, sizeof av)) {
                int nv = rate_pct(av, rate);
                if (nv < 33) nv = 33;
                if (nv > 300) nv = 300;
                if (nv != rate) {
                    rate = nv;
                    sb_str(&out, "\\!rp"); sb_int(&out, rate); sb_ch(&out, ' ');
                    f.restores_prosody = 1;
                }
            }
            if (get_attr(attrs, attrs_end, "pitch", av, sizeof av)) {
                int nv = pitch_pct(av, pitch);
                if (nv < 25) nv = 25;
                if (nv > 400) nv = 400;
                if (nv != pitch) {
                    pitch = nv;
                    sb_str(&out, "\\!pp"); sb_int(&out, pitch); sb_ch(&out, ' ');
                    f.restores_prosody = 1;
                }
            }
            /* `range` and `contour` describe an F0 SHAPE, which the engine
             * takes from its own f0tr CART; there is no lever to hand them
             * to, so they are dropped rather than approximated. */
            at_word_start = 1;
            if (!self_closing && top < SSML_DEPTH_MAX) stack[top++] = f;
            continue;
        }

        if (name_is(ns, nl, "p") || name_is(ns, nl, "s")) {
            if (out.n && !isspace((unsigned char)out.p[out.n - 1])) sb_ch(&out, ' ');
            at_word_start = 1;
            if (!self_closing && top < SSML_DEPTH_MAX) stack[top++] = f;
            continue;
        }

        /* <speak>, <voice>, <audio>: structural only. A voice change cannot
         * be honoured -- the engine is one voice per process (see
         * SPFY_GUI_HANDOFF.md) -- so the content is spoken in the voice
         * already loaded rather than dropped. */
        if (!self_closing && top < SSML_DEPTH_MAX) stack[top++] = f;
    }

    /* Close anything the document left open, so a truncated paste still
     * ends with neutral prosody rather than carrying it into the next call. */
    while (top > 0) {
        frame_t *f = &stack[--top];
        if (f->closes_pron) sb_str(&out, "</pron>");
        if (f->skip_content && skip_depth) --skip_depth;
    }
    if (spell) sb_str(&out, " \\!ts0");
    if (year)  sb_str(&out, " \\!ny1");

    if (out.oom) { free(out.p); return NULL; }
    out.p[out.n] = '\0';
    return out.p;
}
