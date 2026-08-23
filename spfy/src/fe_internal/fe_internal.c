/* fe_internal.c - text → tagged-output assembler. */

#include "fe_internal.h"
#include "g2p.h"
#include "text_norm.h"
#include "baked_pos.h"
#include "pos_context.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct {
    char *p;
    char *end;
    int   truncated;
} emit_t;

static void emit_str(emit_t *e, const char *s)
{
    if (e->truncated) return;
    size_t n = strlen(s);
    if (e->p + n >= e->end) {
        n = (size_t)(e->end - e->p);
        e->truncated = 1;
    }
    memcpy(e->p, s, n);
    e->p += n;
    *e->p = '\0';
}

static void emit_int(emit_t *e, int v)
{
    char buf[16];
    snprintf(buf, sizeof buf, "%d", v);
    emit_str(e, buf);
}


/* Phoneme inventory - vowels carry stress digits, consonants don't. */
static int is_vowel_arpa(const char *p)
{
    /* CMU ARPAbet vowels: AA AE AH AO AW AY EH ER EY IH IY OW OY UH UW. */
    char c = p[0];
    if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
    return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
}

#define MAX_PHONS_PER_WORD 32

typedef struct {
    char  arpa[8];
    int   stress;
} phon_t;

/* Split a CMU-style string like "HH AH0 L OW1" into phon_t[]. */
static int split_phonemes(const char *s, phon_t *out, int cap)
{
    int n = 0;
    while (*s && n < cap) {
        while (*s == ' ') ++s;
        if (!*s) break;
        const char *start = s;
        while (*s && *s != ' ') ++s;
        size_t L = (size_t)(s - start);
        if (L == 0) continue;
        int stress = -1;
        if (L > 0 && start[L-1] >= '0' && start[L-1] <= '2') {
            stress = start[L-1] - '0';
            L--;
        }
        if (L >= sizeof out[n].arpa) L = sizeof out[n].arpa - 1;
        for (size_t i = 0; i < L; ++i) {
            char c = start[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
            out[n].arpa[i] = c;
        }
        out[n].arpa[L] = '\0';
        out[n].stress = stress;
        /* CMU dict uses ARPAbet `AH` for both the stressed wedge vowel (e.g. */
        if (out[n].arpa[0] == 'a' && out[n].arpa[1] == 'h'
            && out[n].arpa[2] == '\0' && stress != 1) {
            out[n].arpa[0] = 'a';
            out[n].arpa[1] = 'x';
        }
        ++n;
    }
    return n;
}


/* Allowed English multi-consonant onsets (in lowercase ARPAbet, space-
 * separated pairs/triples). */
static int is_allowed_onset_pair(const char *a, const char *b)
{
    static const char *PAIRS[] = {
        "b r","b l","b y","b w",
        "p r","p l","p y","p w",
        "d r","d w","d y",
        "t r","t w","t y",
        "g r","g l","g w","g y",
        "k r","k l","k w","k y",
        "f r","f l","f y",
        "v r","v l","v y",
        "th r","th w","th y",
        "sh r","sh w",
        "s p","s t","s k","s m","s n","s l","s w",
        "hh y","hh w","hh r","hh l",
        "m y","n y","l y", NULL
    };
    char buf[16];
    snprintf(buf, sizeof buf, "%s %s", a, b);
    for (int i = 0; PAIRS[i]; ++i) {
        if (strcmp(buf, PAIRS[i]) == 0) return 1;
    }
    return 0;
}
static int is_allowed_onset_triple(const char *a, const char *b, const char *c)
{
    static const char *TRIPLES[] = {
        "s p r","s p l","s p y",
        "s t r","s t y",
        "s k r","s k w","s k y",
        NULL
    };
    char buf[24];
    snprintf(buf, sizeof buf, "%s %s %s", a, b, c);
    for (int i = 0; TRIPLES[i]; ++i) {
        if (strcmp(buf, TRIPLES[i]) == 0) return 1;
    }
    return 0;
}

/* Assign each phoneme to its syllable index. */
static int syllabify(const phon_t *phs, int n, int *out_syl)
{
    int syl = 0;
    int last_vowel = -1;
    int n_syllables = 0;
    for (int i = 0; i < n; ++i) {
        if (phs[i].stress >= 0) ++n_syllables;
    }
    if (n_syllables == 0) {
        for (int i = 0; i < n; ++i) out_syl[i] = 0;
        return 1;
    }

    /* First pass: mark every consonant with the syllable index of the
     * PREVIOUS vowel (so they're coda by default). */
    for (int i = 0; i < n; ++i) {
        if (phs[i].stress >= 0) {
            out_syl[i] = syl++;
            last_vowel = i;
        } else {
            out_syl[i] = last_vowel < 0 ? 0 : out_syl[last_vowel];
        }
    }

    /* Second pass: for each between-vowel consonant cluster, move as many
     * trailing consonants as form an allowed onset to the FOLLOWING
     * syllable. */
    last_vowel = -1;
    for (int i = 0; i < n; ++i) {
        if (phs[i].stress < 0) continue;
        if (last_vowel < 0) { last_vowel = i; continue; }

        int cstart = last_vowel + 1;
        int cend = i;
        int next_syl = out_syl[i];
        if (cend - cstart >= 3) {
            const char *a = phs[cend - 3].arpa;
            const char *b = phs[cend - 2].arpa;
            const char *c = phs[cend - 1].arpa;
            if (is_allowed_onset_triple(a, b, c)) {
                out_syl[cend - 3] = next_syl;
                out_syl[cend - 2] = next_syl;
                out_syl[cend - 1] = next_syl;
                goto adv;
            }
        }
        if (cend - cstart >= 2) {
            const char *a = phs[cend - 2].arpa;
            const char *b = phs[cend - 1].arpa;
            if (is_allowed_onset_pair(a, b)) {
                out_syl[cend - 2] = next_syl;
                out_syl[cend - 1] = next_syl;
                goto adv;
            }
        }
        if (cend - cstart >= 1) {
            out_syl[cend - 1] = next_syl;
        }
    adv:
        last_vowel = i;
    }
    return n_syllables;
}


/* Map spfy_pos_class_t → string the DLL emits in word headers. */
static const char *pos_name(spfy_pos_class_t p)
{
    switch (p) {
        case POS_NOUN:           return "noun";
        case POS_ADJ:            return "adj";
        case POS_VERB:           return "verb";
        case POS_ADV:            return "adv";
        case POS_INTERJ:         return "interj";
        case POS_QUANT:          return "quant";
        case POS_NOUN_ADJ:       return "noun_adj";
        case POS_NOUN_VERB:      return "noun_verb";
        case POS_VERB_ADJ:       return "verb_adj";
        case POS_NOUN_VERB_ADJ:  return "noun_verb_adj";
        case POS_ADJ_ADV:        return "adj_adv";
        case POS_DET:            return "det";
        case POS_AUX:            return "aux";
        case POS_PREP:           return "prep";
        case POS_PRO:            return "pro";
        case POS_PRO2:           return "pro2";
        case POS_WH:             return "wh";
        case POS_CONJ:           return "conj";
        case POS_DEM:            return "dem";
        case POS_THERE:          return "there";
        case POS_NOT:            return "not";
        case POS_POSTPOS:        return "postpos";
        case POS_DISAMBIG:       return "disambig";
        case POS_OTHER:          return "other";
        case POS_UNDEF:          return "undef";
        case POS_UNKNOWN:        return "noun";
        default:                 return "noun";
    }
}

/* Function-word reduced pronunciations. */
typedef struct {
    const char *word;
    const char *phonemes;
} func_red_t;

static const func_red_t FUNC_RED[] = {
    { "the",  "DH AX0" },
    { "a",    "AX0"    },
    { "an",   "AX0 N"  },
    { "to",   "T AX0"  },
    { "for",  "F ER0"  },
    { "of",   "AX0 V"  },
    { "in",   "IH0 N"  },
    { "on",   "AA0 N"  },
    { "at",   "AX0 T"  },
    { "by",   "B AY0"  },
    { "from", "F R AH0 M" },
    { "with", "W IH0 TH" },
    { "and",  "AX0 N D" },
    { "but",  "B AH0 T" },
    { "or",   "ER0"    },
    { "as",   "AE0 Z"  },
    { "than", "DH AX0 N" },
    { "is",   "IH0 Z"  },
    { "are",  "AA0 R"  },
    { "was",  "W AH0 Z" },
    { "were", "W ER0"  },
    { "be",   "B IY0"  },
    { "been", "B IH0 N" },
    { "has",  "HH AE0 Z" },
    { "have", "HH AH0 V" },
    { "had",  "HH AE0 D" },
    { "do",   "D UW0"  },
    { "does", "D AH0 Z" },
    { "did",  "D IH0 D" },
    { "will", "W IH0 L" },
    { "would","W UH0 D" },
    { "can",  "K AE0 N" },
    { "could","K UH0 D" },
    { "should","SH UH0 D" },
    { "i",    "AY0"    },
    { "you",  "Y UW0"  },
    { "he",   "HH IY0" },
    { "she",  "SH IY0" },
    { "we",   "W IY0"  },
    { "they", "DH EY0" },
    { "your", "Y ER0"  },
    { "his",  "HH IH0 Z" },
    { "her",  "HH ER0" },
    { "their","DH EH0 R" },
    /* Demonstratives. */
    { "this", "DH IH1 S" },
    { "that", "DH AE1 T" },
    { "these","DH IY1 Z" },
    { "those","DH OW1 Z" },
    /* Function-word reductions for words that CMU pronounces with a
     * stressed vowel but the engine reduces (syl `.0`). */
    { "if",    "IH0 F"   },
    { "it",    "IH0 T"   },
    /* "until" - engine has [.0 ax n .1 t ih l] - first syl reduced, second
     * syl keeps lexical stress. */
    { "until", "AX0 N T IH1 L" },
    { NULL, NULL }
};

/* Lexical pronunciation overrides for open-class words where CMU and the
 * engine's dict disagree. */
typedef struct { const char *word; const char *phonemes; } lex_override_t;

static const lex_override_t LEX_OVERRIDE[] = {
    { "hello",  "HH EH0 L OW1" },
    { "today",  "T AX0 D EY1" },
    /* ARPAbet phoneme names that engine treats as known SINGLE-phoneme
     * words in its dict. */
    { "aa", "AA1" }, { "ao", "AO1" }, { "ey", "EY1" }, { "ih", "IH1" },
    /* "Dr." abbreviation. */
    { "dr", "D AA1 K T ER0" },
    /* nat_036 "usual": CMU has Y UW1 ZH AH0 W AH0 L (4 syls); engine
     * collapses the medial schwa: Y UW1 ZH W AX0 L (3 syls, 6 phonemes). */
    { "usual",     "Y UW1 ZH W AX0 L" },
    /* nat_027 "arrive": CMU has ER0 AY1 V (3 phon); engine splits ER → AX +
     * R: AX0 R AY1 V (4 phonemes). */
    { "arrive",    "AX0 R AY1 V" },
    /* nat_040 "galleries": CMU "G AE1 L ER0 IY0 Z" - engine emits the
     * "gallerys" base form with ER → AX + R: G AE1 L AX0 R IY0 Z. */
    { "galleries", "G AE1 L AX0 R IY0 Z" },
    /* nat_049 "duration": CMU "D UH1 R EY1 SH AH0 N" - engine collapses the
     * unstressed first syl (UH+R → ER) and lowers AH0→IH0: D ER0 EY1 SH IH0
     * N. */
    { "duration",  "D ER0 EY1 SH IH0 N" },
    { NULL, NULL }
};

/* Abbreviations whose trailing "." is part of the abbreviation, not a
 * sentence terminator. */
static const char *ABBREV_TAKES_PERIOD[] = {
    "dr", "mr", "mrs", "ms", "jr", "sr", "etc", "vs",
    NULL
};

static int abbrev_swallows_period(const char *word_lower)
{
    for (int i = 0; ABBREV_TAKES_PERIOD[i]; ++i)
        if (strcmp(ABBREV_TAKES_PERIOD[i], word_lower) == 0) return 1;
    return 0;
}

/* Letter-name pronunciations (CMU ARPAbet, primary stress on nucleus). */
static const char *LETTER_PHONEMES[26] = {
     "EY1",
     "B IY1",
     "S IY1",
     "D IY1",
     "IY1",
     "EH1 F",
     "JH IY1",
     "EY1 CH",
     "AY1",
     "JH EY1",
     "K EY1",
     "EH1 L",
     "EH1 M",
     "EH1 N",
     "OW1",
     "P IY1",
     "K Y UW1",
     "AA1 R",
     "EH1 S",
     "T IY1",
     "Y UW1",
     "V IY1",
     "D AH1 B AH0 L Y UW1",
     "EH1 K S",
     "W AY1",
     "Z IY1",
};

/* Words engine spells out letter-by-letter (it lacks a dict entry, and its
 * FE writes one noun-word per letter). */
static const char *SPELL_OUT_LETTER_WORDS[] = {
    "ch", "dh", "dx", "hh", "jh", "ng", "sh", "zh",
    NULL
};

static int is_spell_out_letter_word(const char *word_lower)
{
    for (int i = 0; SPELL_OUT_LETTER_WORDS[i]; ++i)
        if (strcmp(SPELL_OUT_LETTER_WORDS[i], word_lower) == 0) return 1;
    return 0;
}

/* "Mmm.", "Sssss." - engine spells repeated-CONSONANT onomatopoeias letter
 * by letter ("em em em" / "ess ess ess ess ess"). */
static int is_repeated_consonant_word(const char *word_lower)
{
    size_t L = strlen(word_lower);
    if (L < 2) return 0;
    char c0 = word_lower[0];
    if (c0 < 'a' || c0 > 'z') return 0;
    if (c0 == 'a' || c0 == 'e' || c0 == 'i' || c0 == 'o'
     || c0 == 'u' || c0 == 'y') return 0;
    for (size_t k = 1; k < L; ++k) {
        if (word_lower[k] != c0) return 0;
    }
    return 1;
}

static const char *lookup_lex_override(const char *word_lower)
{
    for (int i = 0; LEX_OVERRIDE[i].word; ++i) {
        if (strcmp(LEX_OVERRIDE[i].word, word_lower) == 0)
            return LEX_OVERRIDE[i].phonemes;
    }
    return NULL;
}

/* Force-tag the core function words to their canonical closed-class POS
 * even when baked_pos returns a different (rare nominal) sense. */
typedef struct { const char *word; spfy_pos_class_t pos; } pos_override_t;

static const pos_override_t POS_OVERRIDE[] = {
    { "a",     POS_DET  }, { "an",    POS_DET  }, { "the",   POS_DET  },
    { "to",    POS_PREP }, { "for",   POS_PREP }, { "of",    POS_PREP },
    { "in",    POS_PREP }, { "on",    POS_PREP }, { "at",    POS_PREP },
    { "by",    POS_PREP }, { "from",  POS_PREP }, { "with",  POS_PREP },
    { "about", POS_PREP }, { "into",  POS_PREP }, { "onto",  POS_PREP },
    { "upon",  POS_PREP }, { "after", POS_PREP }, { "until", POS_PREP },
    { "and",   POS_CONJ }, { "but",   POS_CONJ }, { "or",    POS_CONJ },
    { "as",    POS_CONJ }, { "if",    POS_CONJ }, { "while", POS_CONJ },
    { "because", POS_CONJ }, { "although", POS_CONJ }, { "since", POS_CONJ },
    { "unless", POS_CONJ }, { "when",  POS_CONJ }, { "before", POS_CONJ },
    /* "after"/"until" can be either prep or conj in English; the DLL's
     * empirical tagging on this corpus prefers prep (likely picks the
     * preposition reading by default). */
    { "is",    POS_AUX  }, { "are",   POS_AUX  }, { "was",   POS_AUX  },
    { "were",  POS_AUX  }, { "be",    POS_AUX  }, { "been",  POS_AUX  },
    { "has",   POS_AUX  }, { "have",  POS_AUX  }, { "had",   POS_AUX  },
    { "do",    POS_AUX  }, { "does",  POS_AUX  }, { "did",   POS_AUX  },
    { "will",  POS_AUX  }, { "would", POS_AUX  }, { "can",   POS_AUX  },
    { "could", POS_AUX  }, { "should", POS_AUX }, { "might", POS_AUX  },
    { "i",     POS_PRO  }, { "you",   POS_PRO  }, { "he",    POS_PRO  },
    { "she",   POS_PRO  }, { "we",    POS_PRO  }, { "they",  POS_PRO  },
    { "me",    POS_PRO  }, { "him",   POS_PRO  }, { "us",    POS_PRO  },
    { "them",  POS_PRO  }, { "my",    POS_PRO2 }, { "your",  POS_PRO2 },
    { "his",   POS_PRO2 }, { "her",   POS_PRO2 }, { "our",   POS_PRO2 },
    { "their", POS_PRO2 }, { "its",   POS_PRO2 },
    /* DLL tags pre-nominal possessive "your" as `det` (e.g. */
    { "your",  POS_DET  },
    { "it",    POS_PRO  }, { "one",   POS_PRO },
    { "this",  POS_DEM  }, { "that",  POS_DEM  }, { "these", POS_DEM  },
    { "those", POS_DEM  },
    { "who",   POS_WH   }, { "what",  POS_WH   }, { "when",  POS_WH   },
    { "where", POS_WH   }, { "why",   POS_WH   }, { "how",   POS_WH   },
    { "not",   POS_NOT  },
    { NULL,    POS_UNKNOWN }
};

static int try_pos_override(const char *word_lower, spfy_pos_class_t *out)
{
    for (int i = 0; POS_OVERRIDE[i].word; ++i) {
        if (strcmp(POS_OVERRIDE[i].word, word_lower) == 0) {
            *out = POS_OVERRIDE[i].pos;
            return 1;
        }
    }
    return 0;
}

static const char *pos_is_closed_class(spfy_pos_class_t p)
{
    switch (p) {
        case POS_DET: case POS_AUX: case POS_PREP: case POS_PRO:
        case POS_PRO2: case POS_WH: case POS_CONJ: case POS_DEM:
        case POS_THERE: case POS_NOT: case POS_POSTPOS:
            return "y";
        default:
            return NULL;
    }
}

/* Return reduced-form phoneme string if (word, pos) matches the table, else
 * NULL. */
static const char *lookup_function_reduction(const char *word_lower,
                                              spfy_pos_class_t pos)
{
    if (!pos_is_closed_class(pos)) return NULL;
    for (int i = 0; FUNC_RED[i].word; ++i) {
        if (strcmp(FUNC_RED[i].word, word_lower) == 0)
            return FUNC_RED[i].phonemes;
    }
    return NULL;
}


#define MAX_WORDS_PER_UTT 96

typedef struct {
    char              text[64];
    int               char_off;
    int               char_len;
    spfy_pos_class_t  pos;
    int               stress_lvl;
    int               primary_syl;
    int               is_focus;
    phon_t            phs[MAX_PHONS_PER_WORD];
    int               n_phs;
    int               syl[MAX_PHONS_PER_WORD];
    int               n_syl;
    int8_t            pitch_st;
    int8_t            rate_pct;
} word_rec_t;

typedef struct {
    word_rec_t  words[MAX_WORDS_PER_UTT];
    int         n_words;
    char        terminator;
} utt_buf_t;

static void utt_init(utt_buf_t *u) { u->n_words = 0; u->terminator = 0; }

static int lower_copy(const char *src, char *dst, size_t cap)
{
    size_t i = 0;
    while (src[i] && i + 1 < cap) {
        char c = src[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
        dst[i] = c;
        ++i;
    }
    dst[i] = '\0';
    return (int)i;
}

/* Strip a trailing 's apostrophe-s from `word_lower` for POS lookup -
 * baked_pos has lemma forms ("today") not possessives ("today's"). */
static void strip_possessive(char *w)
{
    size_t L = strlen(w);
    if (L >= 2 && w[L-2] == '\'' && (w[L-1] == 's' || w[L-1] == 'd')) {
        w[L-2] = '\0';
    }
}

/* Cheap POS-only lookup for auto-break lookahead. */
static spfy_pos_class_t peek_word_pos_raw(const char *raw_word)
{
    char low[64];
    lower_copy(raw_word, low, sizeof low);
    strip_possessive(low);
    spfy_pos_class_t pos = POS_UNKNOWN;
    if (!try_pos_override(low, &pos))
        spfy_baked_pos_lookup(low, &pos);
    return pos;
}

static int pos_can_be_verb(spfy_pos_class_t p)
{
    return p == POS_VERB || p == POS_NOUN_VERB
        || p == POS_VERB_ADJ || p == POS_NOUN_VERB_ADJ;
}

/* Resolve word → (pos, phonemes, syllabification). */
static int analyze_word(const char *raw_word, word_rec_t *w)
{
    char low[64];
    lower_copy(raw_word, low, sizeof low);
    strip_possessive(low);

    /* POS lookup: try the small override table first (canonical form for
     * ambiguous high-frequency function words), then the baked_pos dict. */
    spfy_pos_class_t pos = POS_UNKNOWN;
    if (!try_pos_override(low, &pos)) {
        spfy_baked_pos_lookup(low, &pos);
        switch (pos) {
            case POS_NOUN_VERB:     pos = POS_NOUN; break;
            case POS_NOUN_ADJ:      pos = POS_ADJ;  break;
            case POS_VERB_ADJ:      pos = POS_ADJ;  break;
            case POS_NOUN_VERB_ADJ: pos = POS_ADJ;  break;
            case POS_ADJ_ADV:       pos = POS_ADJ;  break;
            default: break;
        }
    }
    w->pos = pos;

    /* Get phonemes. */
    const char *phon_str = lookup_function_reduction(low, pos);
    char cmu_buf[160];
    if (!phon_str) phon_str = lookup_lex_override(low);
    if (!phon_str) {
        spfy_g2p_origin_t origin;
        int rc = spfy_g2p_word_lookup_ex(low, cmu_buf, sizeof cmu_buf, &origin);
        if (rc != 0) return rc;
        phon_str = cmu_buf;
    }

    /* Possessive 's / contraction 'd suffix. */
    char with_suffix[200];
    {
        size_t raw_len = strlen(raw_word);
        char last_ch = (raw_len >= 1) ? raw_word[raw_len - 1] : 0;
        if (last_ch >= 'A' && last_ch <= 'Z') last_ch = (char)(last_ch - 'A' + 'a');
        int has_poss_s = (raw_len >= 2 && raw_word[raw_len - 2] == '\''
                          && last_ch == 's');
        int has_contr_d = (raw_len >= 2 && raw_word[raw_len - 2] == '\''
                           && last_ch == 'd');
        if (has_poss_s || has_contr_d) {
            const char *suffix = has_contr_d ? " D" : " Z";
            if (has_poss_s) {
                size_t pl = strlen(phon_str);
                size_t last_start = pl;
                while (last_start > 0 && phon_str[last_start - 1] != ' ') last_start--;
                char arpa_lc[8] = {0};
                for (size_t j = 0; j < 7 && phon_str[last_start + j]; ++j) {
                    char c = phon_str[last_start + j];
                    if (c == '0' || c == '1' || c == '2') break;
                    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                    arpa_lc[j] = c;
                }
                if (strcmp(arpa_lc, "s")  == 0 || strcmp(arpa_lc, "z")  == 0
                 || strcmp(arpa_lc, "sh") == 0 || strcmp(arpa_lc, "zh") == 0
                 || strcmp(arpa_lc, "ch") == 0 || strcmp(arpa_lc, "jh") == 0) {
                    suffix = " IH0 Z";
                } else if (strcmp(arpa_lc, "p")  == 0 || strcmp(arpa_lc, "t") == 0
                        || strcmp(arpa_lc, "k")  == 0 || strcmp(arpa_lc, "f") == 0
                        || strcmp(arpa_lc, "th") == 0) {
                    suffix = " S";
                }
            }
            int n = snprintf(with_suffix, sizeof with_suffix, "%s%s",
                             phon_str, suffix);
            if (n > 0 && (size_t)n < sizeof with_suffix) phon_str = with_suffix;
        }
    }

    w->n_phs = split_phonemes(phon_str, w->phs, MAX_PHONS_PER_WORD);
    if (w->n_phs == 0) return -1;
    w->n_syl = syllabify(w->phs, w->n_phs, w->syl);

    /* Per-word stress level: max stress across syllables. */
    int max_stress = 0;
    int primary    = -1;
    for (int i = 0; i < w->n_phs; ++i) {
        int s = w->phs[i].stress;
        if (s > max_stress) max_stress = s;
        if (s == 1 && primary < 0) primary = w->syl[i];
    }
    int always_deaccent = (pos == POS_DET || pos == POS_AUX
                          || pos == POS_CONJ || pos == POS_DEM
                          || pos == POS_NOT || pos == POS_THERE
                          || pos == POS_POSTPOS);
    if (always_deaccent) {
        w->stress_lvl = 0;
    } else {
        w->stress_lvl = (max_stress > 0) ? 1 : 0;
    }
    w->primary_syl = primary;
    w->is_focus    = 0;
    return 0;
}


static void emit_word_rec(emit_t *e, const word_rec_t *w, char boundary_term)
{
    emit_str(e, "<");
    emit_str(e, w->text);
    emit_str(e, "(");
    emit_int(e, w->char_off);
    emit_str(e, ",");
    emit_int(e, w->char_len);
    emit_str(e, ") ");
    emit_str(e, pos_name(w->pos));
    emit_str(e, ",");
    emit_int(e, w->stress_lvl);
    /* SSML / Balabolka prosody overrides. */
    if (w->pitch_st != 0) {
        emit_str(e, ",p=");
        emit_int(e, w->pitch_st);
    }
    if (w->rate_pct != 0) {
        emit_str(e, ",r=");
        emit_int(e, w->rate_pct);
    }
    emit_str(e, " [");

    /* Boundary tone goes on the LAST syllable of the focus word, regardless
     * of stress. */
    int boundary_syl_idx = -1;
    if (w->is_focus && boundary_term != 0 && w->n_syl > 0)
        boundary_syl_idx = w->n_syl - 1;
    const char *boundary_tone =
        (boundary_term == '?') ? "H-H%" :
        (boundary_term == ',') ? "L-H%" : "L-L%";

    int cur_syl = -1;
    for (int i = 0; i < w->n_phs; ++i) {
        if (w->syl[i] != cur_syl) {
            int s_stress = 0;
            for (int j = i; j < w->n_phs && w->syl[j] == w->syl[i]; ++j) {
                if (w->phs[j].stress >= 0) { s_stress = w->phs[j].stress; break; }
            }
            emit_str(e, " .");
            emit_int(e, s_stress);

            int is_accent_syl   = (w->stress_lvl >= 1
                                   && w->syl[i] == w->primary_syl);
            int is_boundary_syl = (w->syl[i] == boundary_syl_idx);
            if (is_accent_syl) emit_str(e, ",H*");
            if (is_boundary_syl) {
                /* Engine uses `;X-Y%` directly after the stress digit (or
                 * after `,H*` when both fire on the same syl). */
                emit_str(e, ";");
                emit_str(e, boundary_tone);
            }
            cur_syl = w->syl[i];
        }
        emit_str(e, " ");
        emit_str(e, w->phs[i].arpa);
        emit_str(e, "(p100)");
    }
    emit_str(e, " ]>");
}

/* Apply the before-vowel allomorph for "the" and "to": when followed by a
 * word starting with a vowel phoneme, "the" → DH IY0 and "to" → T UW0. */
static int starts_with_vowel(const word_rec_t *w)
{
    return w->n_phs > 0 && is_vowel_arpa(w->phs[0].arpa);
}

static void rewrite_phs(word_rec_t *w, const char *phon_str)
{
    w->n_phs = split_phonemes(phon_str, w->phs, MAX_PHONS_PER_WORD);
    w->n_syl = syllabify(w->phs, w->n_phs, w->syl);
    int max_stress = 0, primary = -1;
    for (int i = 0; i < w->n_phs; ++i) {
        int s = w->phs[i].stress;
        if (s > max_stress) max_stress = s;
        if (s == 1 && primary < 0) primary = w->syl[i];
    }
    w->stress_lvl  = max_stress;
    w->primary_syl = primary;
}

static void apply_before_vowel_allomorph(utt_buf_t *u)
{
    for (int i = 0; i + 1 < u->n_words; ++i) {
        word_rec_t *cur = &u->words[i];
        const word_rec_t *nxt = &u->words[i + 1];
        if (!starts_with_vowel(nxt)) continue;
        if (cur->pos != POS_DET && cur->pos != POS_PREP) continue;
        char low[64];
        lower_copy(cur->text, low, sizeof low);
        strip_possessive(low);
        if (strcmp(low, "the") == 0)      rewrite_phs(cur, "DH IY0");
        else if (strcmp(low, "to") == 0)  rewrite_phs(cur, "T UW0");
        /* "a"→"an" is a spelling change the writer is expected to do (the
         * engine doesn't substitute). */
    }
}

/* Flush a buffered utterance: pick the focus word, then emit. */
/* Refine each word's POS using the engine-empirical context table
 * (pos_context.c, captured from the DLL FE). */
static void refine_utt_pos(utt_buf_t *u)
{
    for (int i = 0; i < u->n_words; ++i) {
        word_rec_t *w = &u->words[i];
        char low[64], prev[64] = "^", nxt[64] = "$";
        lower_copy(w->text, low, sizeof low);
        strip_possessive(low);
        spfy_pos_class_t pinned = POS_UNKNOWN;
        if (try_pos_override(low, &pinned)) continue;
        if (i > 0) {
            lower_copy(u->words[i - 1].text, prev, sizeof prev);
            strip_possessive(prev);
        }
        if (i + 1 < u->n_words) {
            lower_copy(u->words[i + 1].text, nxt, sizeof nxt);
            strip_possessive(nxt);
        }
        spfy_pos_class_t ctx = spfy_pos_context_lookup(low, prev, nxt);
        if (ctx == POS_UNKNOWN) continue;
        /* Collapse residual multi-class results the same way analyze_word
         * does (the captured table may carry a multi-class label when the
         * engine emitted one in the only context we observed). */
        switch (ctx) {
            case POS_NOUN_VERB:     ctx = POS_NOUN; break;
            case POS_NOUN_ADJ:      ctx = POS_ADJ;  break;
            case POS_VERB_ADJ:      ctx = POS_ADJ;  break;
            case POS_NOUN_VERB_ADJ: ctx = POS_ADJ;  break;
            case POS_ADJ_ADV:       ctx = POS_ADJ;  break;
            default: break;
        }
        w->pos = ctx;
    }
}

/* "and" / "or" utt-initial allomorph: engine reduces to .0 AE N (two
 * phonemes, no trailing D) when the coordinator starts a new utterance
 * (e.g. */
static void apply_utt_initial_and_allomorph(utt_buf_t *u, int is_first_utt)
{
    if (is_first_utt) return;
    if (u->n_words == 0) return;
    word_rec_t *w = &u->words[0];
    char low[64];
    lower_copy(w->text, low, sizeof low);
    strip_possessive(low);
    if (strcmp(low, "and") == 0 || strcmp(low, "or") == 0) {
        rewrite_phs(w, "AE0 N");
    }
}

static void flush_utt(emit_t *e, utt_buf_t *u, int is_first_utt_in_stream)
{
    apply_before_vowel_allomorph(u);
    apply_utt_initial_and_allomorph(u, is_first_utt_in_stream);
    refine_utt_pos(u);

    /* Pick focus: the LAST word with stress_lvl ≥ 1 (i.e. */
    int focus = -1;
    for (int i = u->n_words - 1; i >= 0; --i) {
        if (u->words[i].stress_lvl >= 1) { focus = i; break; }
    }
    if (focus >= 0) {
        u->words[focus].is_focus    = 1;
        u->words[focus].stress_lvl  = 2;
    }

    for (int i = 0; i < u->n_words; ++i) {
        char term = 0;
        if (i == focus) term = u->terminator ? u->terminator : '.';
        emit_word_rec(e, &u->words[i], term);
        emit_str(e, " ");
    }
}


int spfy_fe_internal_text_to_tagged(const char *text,
                                     char *out, size_t out_n)
{
    if (!text || !out || out_n == 0) return -1;
    out[0] = '\0';
    emit_t e = { out, out + out_n - 1, 0 };

    spfy_token_t toks[512];
    size_t nt = 0;
    int rc = spfy_text_normalize(text, toks, sizeof toks / sizeof toks[0], &nt);
    if (rc < 0) return -1;

    /* The DLL emits one `#{X ... */

    int char_off = 0;
    int is_first_utt = 1;
    /* SSML <break time="..."> - when set, overrides the next inter-utt
     * opener pause. */
    uint16_t pending_custom_pause_ms = 0;
    utt_buf_t utt;
    utt_init(&utt);

    #define WRITE_UTT(term_char) do { \
        if (utt.n_words > 0) { \
            const char *open_pau = \
                ((term_char) == '.' || (term_char) == '!' \
                 || (term_char) == '?') \
                ? " pau(p350) " : " pau(p100) "; \
            char custom_pau[32]; \
            int was_first_utt = is_first_utt; \
            if (is_first_utt) { \
                emit_str(&e, "#{"); \
                { char tmp[2] = { (term_char), 0 }; emit_str(&e, tmp); } \
                emit_str(&e, " pau(p25) "); \
                is_first_utt = 0; \
                /* Pending pause is NOT cleared here - the first utt's
                 * leading pau is fixed at p25, so any SSML break that
                 * triggered THIS flush is semantically about the gap
                 * BETWEEN this utt and the next. */ \
            } else { \
                /* Only consume pending here - this opener represents the \
                 * silence between the previous utt's end and this utt's \
                 * start, which is exactly what SSML <break time> means. */ \
                if (pending_custom_pause_ms > 0) { \
                    snprintf(custom_pau, sizeof custom_pau, \
                             " pau(p%u) ", (unsigned)pending_custom_pause_ms); \
                    open_pau = custom_pau; \
                    pending_custom_pause_ms = 0; \
                } \
                emit_str(&e, "{"); \
                { char tmp[2] = { (term_char), 0 }; emit_str(&e, tmp); } \
                emit_str(&e, open_pau); \
            } \
            utt.terminator = (term_char); \
            flush_utt(&e, &utt, was_first_utt); \
            emit_str(&e, "pau(p50) } "); \
            utt_init(&utt); \
            char_off = 0; \
        } \
    } while (0)

    /* Auto-phrase-break heuristic. */
    #define AUTO_BREAK_MIN_WORDS 7

    for (size_t i = 0; i < nt; ++i) {
        switch (toks[i].type) {
        case SPFY_TOKEN_WORD: {
            if (utt.n_words >= MAX_WORDS_PER_UTT) break;
            /* Auto-break check (only fires when the current utt has
             * accumulated enough content). */
            spfy_pos_class_t next_pos = POS_UNKNOWN;
            {
                for (size_t j = i + 1; j < nt; ++j) {
                    if (toks[j].type == SPFY_TOKEN_WORD) {
                        next_pos = peek_word_pos_raw(toks[j].text);
                        break;
                    }
                }
            }
            if (utt.n_words >= AUTO_BREAK_MIN_WORDS) {
                char low_check[64];
                lower_copy(toks[i].text, low_check, sizeof low_check);
                strip_possessive(low_check);
                int trigger = 0;
                /* Subordinating conjunctions that reliably signal a clause boundary. */
                static const char *subs[] = {
                    "while", "because", "if", "unless", "when",
                    "although", "though", "whereas", NULL
                };
                for (int si = 0; subs[si]; ++si) {
                    if (strcmp(low_check, subs[si]) == 0) { trigger = 1; break; }
                }
                /* Infinitive "to" after a content word AND followed by a
                 * word that can be a verb. */
                if (!trigger && strcmp(low_check, "to") == 0
                    && utt.words[utt.n_words - 1].stress_lvl >= 1
                    && pos_can_be_verb(next_pos)) {
                    trigger = 1;
                }
                /* Coordinator "and" / "or" starting a new VERB-headed
                 * clause (lookahead). */
                if (!trigger
                    && (strcmp(low_check, "and") == 0
                        || strcmp(low_check, "or") == 0)
                    && pos_can_be_verb(next_pos)) {
                    trigger = 1;
                }
                if (trigger) {
                    WRITE_UTT(',');
                }
            }
            /* Complementizer "that" after a verb (no length threshold -
             * engine breaks even after short prefixes like "Our records
             * indicate that..." in nat_035). */
            if (utt.n_words >= 2) {
                char low_check2[64];
                lower_copy(toks[i].text, low_check2, sizeof low_check2);
                strip_possessive(low_check2);
                if (strcmp(low_check2, "that") == 0
                    && pos_can_be_verb(utt.words[utt.n_words - 1].pos)
                    && next_pos != POS_UNKNOWN
                    && next_pos != POS_NOUN
                    && next_pos != POS_NOUN_VERB
                    && next_pos != POS_ADJ
                    && next_pos != POS_NOUN_ADJ) {
                    WRITE_UTT(',');
                }
            }
            word_rec_t *w = &utt.words[utt.n_words];
            memset(w, 0, sizeof *w);
            size_t wl = strlen(toks[i].text);
            if (wl >= sizeof w->text) wl = sizeof w->text - 1;
            memcpy(w->text, toks[i].text, wl);
            w->text[wl] = '\0';
            w->char_off = char_off;
            w->char_len = (int)wl;
            char_off += (int)wl + 1;
            w->pitch_st = toks[i].pitch_st;
            w->rate_pct = toks[i].rate_pct;

            /* SSML <phoneme ph="..."> - caller-supplied ARPAbet override. */
            if (toks[i].phonemes[0]) {
                w->n_phs = split_phonemes(toks[i].phonemes, w->phs,
                                          MAX_PHONS_PER_WORD);
                if (w->n_phs == 0) { memset(w, 0, sizeof *w); break; }
                w->n_syl = syllabify(w->phs, w->n_phs, w->syl);
                w->pos   = POS_NOUN;
                int ms = 0, pr = -1;
                for (int k = 0; k < w->n_phs; ++k) {
                    int s = w->phs[k].stress;
                    if (s > ms) ms = s;
                    if (s == 1 && pr < 0) pr = w->syl[k];
                }
                w->stress_lvl  = (ms > 0) ? 1 : 0;
                w->primary_syl = pr;
                w->is_focus    = 0;
                ++utt.n_words;
                break;
            }

            /* Letter-by-letter spell-out for ARPAbet 2-letter combos the
             * engine lacks in its dict (ch, dh, dx, hh, jh, ng, sh, zh). */
            {
                char low_sp[64];
                lower_copy(w->text, low_sp, sizeof low_sp);
                strip_possessive(low_sp);
                if (is_spell_out_letter_word(low_sp)
                    || is_repeated_consonant_word(low_sp)) {
                    int orig_off = w->char_off;
                    int orig_len = w->char_len;
                    memset(w, 0, sizeof *w);
                    int n_letters = (int)strlen(low_sp);
                    for (int li = 0; li < n_letters; ++li) {
                        if (utt.n_words >= MAX_WORDS_PER_UTT) break;
                        char ch = low_sp[li];
                        if (ch < 'a' || ch > 'z') continue;
                        word_rec_t *lw = &utt.words[utt.n_words];
                        memset(lw, 0, sizeof *lw);
                        lw->text[0] = ch; lw->text[1] = '\0';
                        lw->char_off = orig_off;
                        lw->char_len = orig_len;
                        lw->pos = POS_NOUN;
                        lw->n_phs = split_phonemes(
                            LETTER_PHONEMES[ch - 'a'],
                            lw->phs, MAX_PHONS_PER_WORD);
                        lw->n_syl = syllabify(lw->phs, lw->n_phs, lw->syl);
                        int ms = 0, pr = -1;
                        for (int k = 0; k < lw->n_phs; ++k) {
                            int s = lw->phs[k].stress;
                            if (s > ms) ms = s;
                            if (s == 1 && pr < 0) pr = lw->syl[k];
                        }
                        lw->stress_lvl  = (ms > 0) ? 1 : 0;
                        lw->primary_syl = pr;
                        lw->is_focus    = 0;
                        ++utt.n_words;
                    }
                    break;
                }
            }

            if (analyze_word(w->text, w) != 0) {
                memset(w, 0, sizeof *w);
                break;
            }
            ++utt.n_words;

            /* Abbreviation period swallow: "Dr.", "Mr.", "Mrs.", "etc.", …
             * When followed by another WORD token, the period belongs to
             * the abbreviation (not a sentence boundary). */
            {
                char low_ab[64];
                lower_copy(w->text, low_ab, sizeof low_ab);
                strip_possessive(low_ab);
                if (abbrev_swallows_period(low_ab)
                    && i + 1 < nt
                    && toks[i+1].type == SPFY_TOKEN_SENTENCE_BREAK
                    && toks[i+1].text[0] == '.') {
                    int next_is_word = 0;
                    for (size_t j = i + 2; j < nt; ++j) {
                        if (toks[j].type == SPFY_TOKEN_WORD) { next_is_word = 1; }
                        break;
                    }
                    if (next_is_word) ++i;
                }
            }
            break;
        }
        case SPFY_TOKEN_PHRASE_BREAK:
            /* Comma / semicolon - separate utt in the DLL's convention,
             * with `{,` opener and L-L% boundary on the focus word. */
            WRITE_UTT(',');
            break;
        case SPFY_TOKEN_SENTENCE_BREAK: {
            char t = (toks[i].text[0] == '?' || toks[i].text[0] == '!')
                     ? toks[i].text[0] : '.';
            WRITE_UTT(t);
            break;
        }
        case SPFY_TOKEN_CUSTOM_PAUSE:
            /* SSML <break time="..." /> - stash the explicit duration so
             * the next utt's opener emits pau(p<ms>) instead of the
             * terminator-class default, then close the current utt as a
             * phrase break. */
            pending_custom_pause_ms = toks[i].pause_ms;
            WRITE_UTT(',');
            break;
        }
    }

    if (utt.n_words > 0) {
        WRITE_UTT('.');
    }
    emit_str(&e, "%%");
    #undef WRITE_UTT

    (void)is_vowel_arpa;

    return e.truncated ? 1 : 0;
}
