/* g2p.c - multi-stage word→phoneme lookup. */

#include "g2p.h"
#include "cmudict_data.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>


static int to_lower_ascii(int c)
{
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static int ascii_strcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = to_lower_ascii((unsigned char)*a);
        int cb = to_lower_ascii((unsigned char)*b);
        if (ca != cb) return ca - cb;
        ++a; ++b;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* Binsearch the CMU dict. */
static const char *dict_lookup(const char *word)
{
    size_t lo = 0, hi = cmudict_n_entries;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = ascii_strcasecmp(word, cmudict_entries[mid].word);
        if (cmp == 0) return cmudict_entries[mid].phonemes;
        if (cmp < 0) hi = mid;
        else         lo = mid + 1;
    }
    return NULL;
}

static void copy_out(const char *src, char *out, size_t out_n)
{
    size_t n = strlen(src);
    if (n + 1 > out_n) n = out_n - 1;
    memcpy(out, src, n);
    out[n] = '\0';
}

/* Append " <phonemes>" to out (with a leading space if out already has
 * content). */
static void append_phonemes(char *out, size_t out_n, const char *more)
{
    size_t cur = strlen(out);
    if (cur >= out_n - 1) return;
    if (cur > 0 && cur < out_n - 1) {
        out[cur++] = ' ';
        out[cur] = '\0';
    }
    size_t rem = out_n - cur;
    size_t n = strlen(more);
    if (n + 1 > rem) n = rem - 1;
    memcpy(out + cur, more, n);
    out[cur + n] = '\0';
}

/* Extract the final ARPAbet phoneme (no stress digit) from a CMU-style
 * string like "HH AH0 L OW1". */
static void last_phoneme(const char *phon, char *buf, size_t buf_n)
{
    buf[0] = '\0';
    if (!phon || !*phon) return;
    const char *end = phon + strlen(phon);
    const char *start = end;
    while (start > phon && start[-1] != ' ') --start;
    size_t n = (size_t)(end - start);
    if (n + 1 > buf_n) n = buf_n - 1;
    memcpy(buf, start, n);
    buf[n] = '\0';
    size_t L = strlen(buf);
    if (L > 0 && (buf[L-1] >= '0' && buf[L-1] <= '9')) buf[L-1] = '\0';
}

static int is_sibilant(const char *p)
{
    return strcmp(p, "S")  == 0 || strcmp(p, "Z")  == 0
        || strcmp(p, "SH") == 0 || strcmp(p, "ZH") == 0
        || strcmp(p, "CH") == 0 || strcmp(p, "JH") == 0;
}

static int is_voiceless_obstruent(const char *p)
{
    return strcmp(p, "P") == 0 || strcmp(p, "T") == 0
        || strcmp(p, "K") == 0 || strcmp(p, "F") == 0
        || strcmp(p, "TH") == 0 || strcmp(p, "S") == 0
        || strcmp(p, "SH") == 0 || strcmp(p, "CH") == 0
        || strcmp(p, "HH") == 0;
}

static int ends_with_t_or_d(const char *p)
{
    return strcmp(p, "T") == 0 || strcmp(p, "D") == 0;
}


/* Recover the stem of a possibly-inflected word and look it up. */
static const char *stem_lookup(const char *word, const char *suffix,
                                char *stem_buf, size_t stem_buf_n)
{
    size_t wn = strlen(word);
    size_t sn = strlen(suffix);
    if (sn >= wn || sn == 0) return NULL;

    for (size_t i = 0; i < sn; ++i) {
        if (to_lower_ascii((unsigned char)word[wn - sn + i])
            != to_lower_ascii((unsigned char)suffix[i])) return NULL;
    }

    size_t stem_len = wn - sn;
    if (stem_len == 0 || stem_len + 2 > stem_buf_n) return NULL;

    memcpy(stem_buf, word, stem_len);
    stem_buf[stem_len] = '\0';
    const char *hit = dict_lookup(stem_buf);
    if (hit) return hit;

    if (stem_len >= 2
        && to_lower_ascii((unsigned char)stem_buf[stem_len - 1])
           == to_lower_ascii((unsigned char)stem_buf[stem_len - 2])
        && strchr("bcdfgklmnprstvz",
                  to_lower_ascii((unsigned char)stem_buf[stem_len - 1]))) {
        stem_buf[stem_len - 1] = '\0';
        hit = dict_lookup(stem_buf);
        if (hit) return hit;
        stem_buf[stem_len - 1] = stem_buf[stem_len - 2];
        stem_buf[stem_len] = '\0';
    }

    if (stem_len + 1 < stem_buf_n) {
        stem_buf[stem_len] = 'e';
        stem_buf[stem_len + 1] = '\0';
        hit = dict_lookup(stem_buf);
        if (hit) return hit;
    }

    return NULL;
}

/* Append the suffix realization for "-s" given the stem's final phone. */
static void append_s_suffix(const char *last_ph, char *out, size_t out_n)
{
    if (is_sibilant(last_ph))                append_phonemes(out, out_n, "IH0 Z");
    else if (is_voiceless_obstruent(last_ph))append_phonemes(out, out_n, "S");
    else                                     append_phonemes(out, out_n, "Z");
}

static void append_ed_suffix(const char *last_ph, char *out, size_t out_n)
{
    if (ends_with_t_or_d(last_ph))           append_phonemes(out, out_n, "IH0 D");
    else if (is_voiceless_obstruent(last_ph))append_phonemes(out, out_n, "T");
    else                                     append_phonemes(out, out_n, "D");
}

/* Suffix table - order matters: try LONGER suffixes first so "-ness" beats
 * "-s". */
typedef struct {
    const char *suffix;
    const char *phon;
    int         min_stem;
} suffix_rule_t;

static const suffix_rule_t g_suffix_rules[] = {
    { "ization", "AH0 Z EY1 SH AH0 N", 3 },
    { "ational", "EY1 SH AH0 N AH0 L", 3 },
    { "tional",  "SH AH0 N AH0 L",     3 },
    { "ation",   "EY1 SH AH0 N",       3 },
    { "ness",    "N AH0 S",            3 },
    { "ment",    "M AH0 N T",          3 },
    { "tion",    "SH AH0 N",           3 },
    { "sion",    "ZH AH0 N",           3 },
    { "able",    "AH0 B AH0 L",        3 },
    { "ible",    "AH0 B AH0 L",        3 },
    { "ity",     "AH0 T IY0",          3 },
    { "ous",     "AH0 S",              3 },
    { "ful",     "F AH0 L",            3 },
    { "ly",      "L IY0",              3 },
    { "ing",     "IH0 NG",             2 },
    { "est",     "AH0 S T",            2 },
    { "er",      "ER0",                2 },
    { "ed",      NULL,                 2 },
    { "s",       NULL,                 2 },
};
static const size_t g_n_suffix_rules =
    sizeof(g_suffix_rules) / sizeof(g_suffix_rules[0]);

static int try_suffix_strip(const char *word, char *out, size_t out_n)
{
    size_t wn = strlen(word);
    char stem_buf[64];
    char last_ph[16];

    for (size_t i = 0; i < g_n_suffix_rules; ++i) {
        const suffix_rule_t *r = &g_suffix_rules[i];
        size_t sn = strlen(r->suffix);
        if (wn < sn + (size_t)r->min_stem) continue;
        const char *stem_ph = stem_lookup(word, r->suffix,
                                           stem_buf, sizeof stem_buf);
        if (!stem_ph) continue;
        last_phoneme(stem_ph, last_ph, sizeof last_ph);
        copy_out(stem_ph, out, out_n);
        if (r->phon) {
            append_phonemes(out, out_n, r->phon);
        } else if (strcmp(r->suffix, "ed") == 0) {
            append_ed_suffix(last_ph, out, out_n);
        } else if (strcmp(r->suffix, "s") == 0) {
            append_s_suffix(last_ph, out, out_n);
        }
        return 1;
    }
    return 0;
}


/* The LTS step is intentionally simple - better than silence on truly
 * unknown words like "zyzzyva", but it's not going to win any quality
 * awards. */
typedef struct {
    const char *pat;
    const char *phon;
} lts_rule_t;

static const lts_rule_t g_lts_rules[] = {
    { "ough",  "AO1 F" },
    { "augh",  "AO1 F" },
    { "tion",  "SH AH0 N" },
    { "sion",  "ZH AH0 N" },
    { "ing",  "IH0 NG" },
    { "ang",  "AE1 NG" },
    { "ong",  "AO1 NG" },
    { "ung",  "AH1 NG" },
    { "ch",   "CH" },
    { "sh",   "SH" },
    { "th",   "TH" },
    { "ph",   "F"  },
    { "gh",   ""   },
    { "wh",   "W"  },
    { "qu",   "K W" },
    { "ck",   "K"  },
    { "ng",   "NG" },
    { "ai",   "EY1" },
    { "ay",   "EY1" },
    { "ee",   "IY1" },
    { "ea",   "IY1" },
    { "ie",   "AY1" },
    { "oa",   "OW1" },
    { "oe",   "OW1" },
    { "oi",   "OY1" },
    { "oy",   "OY1" },
    { "oo",   "UW1" },
    { "ou",   "AW1" },
    { "ow",   "AW1" },
    { "ue",   "UW1" },
    { "ui",   "UW1" },
    { "a",    "AE1" },
    { "e",    "EH1" },
    { "i",    "IH1" },
    { "o",    "AA1" },
    { "u",    "AH1" },
    { "y",    "IY1" },          /* "y" as vowel - final position; we don't distinguish "y" as consonant
 * here, dropping to AY1 would also be defensible */
    { "b",    "B"  }, { "c",    "K"  }, { "d",    "D"  },
    { "f",    "F"  }, { "g",    "G"  }, { "h",    "HH" },
    { "j",    "JH" }, { "k",    "K"  }, { "l",    "L"  },
    { "m",    "M"  }, { "n",    "N"  }, { "p",    "P"  },
    { "q",    "K"  }, { "r",    "R"  }, { "s",    "S"  },
    { "t",    "T"  }, { "v",    "V"  }, { "w",    "W"  },
    { "x",    "K S" },{ "z",    "Z"  },
};
static const size_t g_n_lts_rules =
    sizeof(g_lts_rules) / sizeof(g_lts_rules[0]);

static int starts_with_ci(const char *s, const char *prefix)
{
    while (*prefix) {
        if (!*s) return 0;
        if (to_lower_ascii((unsigned char)*s)
            != to_lower_ascii((unsigned char)*prefix)) return 0;
        ++s; ++prefix;
    }
    return 1;
}

static void lts_synthesize(const char *word, char *out, size_t out_n)
{
    out[0] = '\0';
    int stress_assigned = 0;
    const char *p = word;
    while (*p) {
        const lts_rule_t *match = NULL;
        for (size_t i = 0; i < g_n_lts_rules; ++i) {
            if (starts_with_ci(p, g_lts_rules[i].pat)) {
                match = &g_lts_rules[i]; break;
            }
        }
        if (!match) { ++p; continue; }

        const char *phon = match->phon;
        size_t plen = strlen(match->pat);
        if (*phon == '\0') { p += plen; continue; }

        /* The LTS table marks every primary stress as 1 (we don't actually
         * know where stress belongs in an unseen word). */
        char buf[32];
        size_t n = strlen(phon);
        if (n + 1 > sizeof buf) n = sizeof buf - 1;
        memcpy(buf, phon, n); buf[n] = '\0';
        if (stress_assigned) {
            for (char *q = buf; *q; ++q) if (*q == '1') *q = '0';
        } else {
            for (char *q = buf; *q; ++q) {
                if (*q == '1') { stress_assigned = 1; break; }
            }
        }
        append_phonemes(out, out_n, buf);
        p += plen;
    }
    /* If we never assigned stress (all-consonant word?), promote the first
     * vowel we wrote to '1'. */
    if (!stress_assigned) {
        for (char *q = out; *q; ++q) if (*q == '0') { *q = '1'; break; }
    }
}


int spfy_g2p_word_lookup_ex(const char *word, char *out, size_t out_n,
                             spfy_g2p_origin_t *origin)
{
    if (!word || !out || out_n == 0) return -2;
    if (!*word) { out[0] = '\0'; return -1; }
    out[0] = '\0';

    const char *hit = dict_lookup(word);
    if (hit) {
        copy_out(hit, out, out_n);
        if (origin) *origin = SPFY_G2P_HIT_DICT;
        return 0;
    }

    if (try_suffix_strip(word, out, out_n)) {
        if (origin) *origin = SPFY_G2P_HIT_SUFFIX;
        return 0;
    }

    lts_synthesize(word, out, out_n);
    if (origin) *origin = SPFY_G2P_HIT_LTS;
    return 0;
}

int spfy_g2p_word_lookup(const char *word, char *out, size_t out_n)
{
    if (!word || !out || out_n == 0) return -2;
    out[0] = '\0';

    /* Legacy path - dict-only; preserves the original "-1 on miss" contract
     * callers that pre-date stage 2/3 relied on. */
    const char *hit = dict_lookup(word);
    if (!hit) return -1;
    copy_out(hit, out, out_n);
    return 0;
}

size_t spfy_g2p_dict_size(void)
{
    return cmudict_n_entries;
}
