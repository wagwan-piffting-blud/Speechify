/* g2p.c — multi-stage word→phoneme lookup. See g2p.h for the contract.
 *
 * Stages:
 *   1. CMU dict binsearch (fastest, exact)
 *   2. Suffix stripping with consonant-doubling collapse
 *   3. Letter-to-sound rules (last-resort synthesis)
 *
 * Stage 2 covers the common English inflection patterns -s / -ed / -ing
 * / -ly / -er / -est / -ness / -tion / -ity. The suffix's phonemes are
 * context-dependent on the stem's final phoneme: e.g., "-s" after a
 * voiceless consonant is "S", after voiced is "Z", after a sibilant is
 * "IH0 Z". We decode the stem's final phoneme from its lookup result
 * to pick the right realisation. */

#include "g2p.h"
#include "cmudict_data.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* ---- shared helpers ------------------------------------------------ */

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

/* Binsearch the CMU dict. Returns the phoneme string on hit, NULL on
 * miss. The pointer is into the static dict — do NOT free. */
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

/* Copy a phoneme string into out, truncating if needed. */
static void copy_out(const char *src, char *out, size_t out_n)
{
    size_t n = strlen(src);
    if (n + 1 > out_n) n = out_n - 1;
    memcpy(out, src, n);
    out[n] = '\0';
}

/* Append " <phonemes>" to out (with a leading space if out already has
 * content). Truncates safely. */
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
 * string like "HH AH0 L OW1". Returns "" if the string is empty. */
static void last_phoneme(const char *phon, char *buf, size_t buf_n)
{
    buf[0] = '\0';
    if (!phon || !*phon) return;
    /* Find last token (space-separated). */
    const char *end = phon + strlen(phon);
    const char *start = end;
    while (start > phon && start[-1] != ' ') --start;
    size_t n = (size_t)(end - start);
    if (n + 1 > buf_n) n = buf_n - 1;
    memcpy(buf, start, n);
    buf[n] = '\0';
    /* Strip trailing stress digit. */
    size_t L = strlen(buf);
    if (L > 0 && (buf[L-1] >= '0' && buf[L-1] <= '9')) buf[L-1] = '\0';
}

/* Voicing / sibilant classification for the suffix-phoneme picker. */
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

/* ---- Stage 2: suffix stripping ------------------------------------- */

/* Recover the stem of a possibly-inflected word and look it up.
 *
 *   word         the OOV word
 *   suffix       the literal suffix to strip ("ed", "ing", ...)
 *   out_stem_ph  on success, points into the static dict (don't free)
 *   stem_buf     scratch for stem candidates; required, >= 64 bytes
 *
 * Handles two stem-form heuristics:
 *   (a) plain strip:        "running" → "runn"     (stem "runn" rarely
 *                                                  exists, fall through)
 *   (b) consonant collapse: "running" → "run"      (doubled final
 *                                                  consonant in stem)
 *   (c) drop-e restore:     "lik-ing" → "like"     (silent-e stem)
 *
 * Tries each variant and returns the first dict hit. */
static const char *stem_lookup(const char *word, const char *suffix,
                                char *stem_buf, size_t stem_buf_n)
{
    size_t wn = strlen(word);
    size_t sn = strlen(suffix);
    if (sn >= wn || sn == 0) return NULL;

    /* Word must end in suffix (case-insensitive). */
    for (size_t i = 0; i < sn; ++i) {
        if (to_lower_ascii((unsigned char)word[wn - sn + i])
            != to_lower_ascii((unsigned char)suffix[i])) return NULL;
    }

    size_t stem_len = wn - sn;
    if (stem_len == 0 || stem_len + 2 > stem_buf_n) return NULL;

    /* (a) plain strip */
    memcpy(stem_buf, word, stem_len);
    stem_buf[stem_len] = '\0';
    const char *hit = dict_lookup(stem_buf);
    if (hit) return hit;

    /* (b) consonant-doubling collapse: "runn-" → "run-" */
    if (stem_len >= 2
        && to_lower_ascii((unsigned char)stem_buf[stem_len - 1])
           == to_lower_ascii((unsigned char)stem_buf[stem_len - 2])
        && strchr("bcdfgklmnprstvz",
                  to_lower_ascii((unsigned char)stem_buf[stem_len - 1]))) {
        stem_buf[stem_len - 1] = '\0';
        hit = dict_lookup(stem_buf);
        if (hit) return hit;
        stem_buf[stem_len - 1] = stem_buf[stem_len - 2];  /* restore */
        stem_buf[stem_len] = '\0';
    }

    /* (c) drop-e restore: "lik-" + "ing" → "like" */
    if (stem_len + 1 < stem_buf_n) {
        stem_buf[stem_len] = 'e';
        stem_buf[stem_len + 1] = '\0';
        hit = dict_lookup(stem_buf);
        if (hit) return hit;
    }

    return NULL;
}

/* Append the suffix realization for "-s" given the stem's final phone.
 * English plural / 3sg / possessive: sibilant → IH0 Z, voiceless → S,
 * else → Z. */
static void append_s_suffix(const char *last_ph, char *out, size_t out_n)
{
    if (is_sibilant(last_ph))                append_phonemes(out, out_n, "IH0 Z");
    else if (is_voiceless_obstruent(last_ph))append_phonemes(out, out_n, "S");
    else                                     append_phonemes(out, out_n, "Z");
}

/* Append "-ed" realization: post-T/D → IH0 D, voiceless → T, else D. */
static void append_ed_suffix(const char *last_ph, char *out, size_t out_n)
{
    if (ends_with_t_or_d(last_ph))           append_phonemes(out, out_n, "IH0 D");
    else if (is_voiceless_obstruent(last_ph))append_phonemes(out, out_n, "T");
    else                                     append_phonemes(out, out_n, "D");
}

/* Suffix table — order matters: try LONGER suffixes first so "-ness"
 * beats "-s". `phon` is a fixed appendage when the suffix doesn't
 * depend on the stem's final phone; for the variable ones (-s, -ed)
 * we set phon=NULL and dispatch in the loop. */
typedef struct {
    const char *suffix;      /* lowercase letters */
    const char *phon;        /* fixed phonemes, or NULL for variable */
    int         min_stem;    /* refuse if stem would be shorter */
} suffix_rule_t;

static const suffix_rule_t g_suffix_rules[] = {
    /* longest first to avoid -s eating "-ness" etc. */
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
    { "ed",      NULL,                 2 },  /* T/D/IH0 D — context */
    { "s",       NULL,                 2 },  /* S/Z/IH0 Z   — context */
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

/* ---- Stage 3: letter-to-sound rules -------------------------------- */

/* The LTS step is intentionally simple — better than silence on truly
 * unknown words like "zyzzyva", but it's not going to win any quality
 * awards. Patterns are processed in order; longer ones first.
 *
 * Format:  pattern → phoneme string (uppercase ARPAbet, may be multi-
 *          phoneme like "AH0 NG" for "ung").
 *
 * Each pattern is matched against the current position in the word.
 * On hit we emit its phonemes and advance by the pattern length. */
typedef struct {
    const char *pat;
    const char *phon;
} lts_rule_t;

static const lts_rule_t g_lts_rules[] = {
    /* === 4-letter digraphs / trigraphs first === */
    { "ough",  "AO1 F" },   /* "cough" path — many "ough" forms; this is a guess */
    { "augh",  "AO1 F" },
    { "tion",  "SH AH0 N" },
    { "sion",  "ZH AH0 N" },
    /* === 3-letter === */
    { "ing",  "IH0 NG" },
    { "ang",  "AE1 NG" },
    { "ong",  "AO1 NG" },
    { "ung",  "AH1 NG" },
    /* === 2-letter consonant digraphs === */
    { "ch",   "CH" },
    { "sh",   "SH" },
    { "th",   "TH" },
    { "ph",   "F"  },
    { "gh",   ""   },           /* mostly silent in modern English */
    { "wh",   "W"  },
    { "qu",   "K W" },
    { "ck",   "K"  },
    { "ng",   "NG" },
    /* === 2-letter vowel digraphs === */
    { "ai",   "EY1" },
    { "ay",   "EY1" },
    { "ee",   "IY1" },
    { "ea",   "IY1" },          /* mostly; "head" is an exception */
    { "ie",   "AY1" },          /* "die"; not great for "field" */
    { "oa",   "OW1" },
    { "oe",   "OW1" },
    { "oi",   "OY1" },
    { "oy",   "OY1" },
    { "oo",   "UW1" },
    { "ou",   "AW1" },
    { "ow",   "AW1" },
    { "ue",   "UW1" },
    { "ui",   "UW1" },
    /* === single vowels === */
    { "a",    "AE1" },
    { "e",    "EH1" },
    { "i",    "IH1" },
    { "o",    "AA1" },
    { "u",    "AH1" },
    { "y",    "IY1" },          /* "y" as vowel — final position; we
                                 * don't distinguish "y" as consonant
                                 * here, dropping to AY1 would also be
                                 * defensible */
    /* === single consonants === */
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
        if (!match) { ++p; continue; }   /* unknown char — skip */

        const char *phon = match->phon;
        size_t plen = strlen(match->pat);
        if (*phon == '\0') { p += plen; continue; }   /* silent (gh) */

        /* The LTS table marks every primary stress as 1 (we don't
         * actually know where stress belongs in an unseen word). Keep
         * the first stressed vowel as primary, demote the rest to 0.
         * This produces a single-stress prosody that's at least
         * acceptable. */
        char buf[32];
        size_t n = strlen(phon);
        if (n + 1 > sizeof buf) n = sizeof buf - 1;
        memcpy(buf, phon, n); buf[n] = '\0';
        if (stress_assigned) {
            for (char *q = buf; *q; ++q) if (*q == '1') *q = '0';
        } else {
            /* Mark stress_assigned if any digit '1' is present. */
            for (char *q = buf; *q; ++q) {
                if (*q == '1') { stress_assigned = 1; break; }
            }
        }
        append_phonemes(out, out_n, buf);
        p += plen;
    }
    /* If we never assigned stress (all-consonant word?), promote the
     * first vowel we wrote to '1'. */
    if (!stress_assigned) {
        for (char *q = out; *q; ++q) if (*q == '0') { *q = '1'; break; }
    }
}

/* ---- Public API ---------------------------------------------------- */

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

    /* Legacy path — dict-only; preserves the original "-1 on miss"
     * contract callers that pre-date stage 2/3 relied on. */
    const char *hit = dict_lookup(word);
    if (!hit) return -1;
    copy_out(hit, out, out_n);
    return 0;
}

size_t spfy_g2p_dict_size(void)
{
    return cmudict_n_entries;
}
