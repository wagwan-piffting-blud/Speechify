/* fe_internal.c — text → tagged-output assembler. See fe_internal.h.
 *
 * The interesting work is:
 *   1. Syllabification: CMU dict returns a flat phoneme stream with
 *      stress digits on vowels (e.g., "HH AH0 L OW1"); the tagged
 *      format wants explicit syllable boundaries (`.0 hh ah .1 l ow`).
 *   2. POS-aware emission: closed-class function words ("the", "of",
 *      "for") get a *reduced* pronunciation that carries no stress
 *      digit. Word headers emit the engine's POS string (det, aux,
 *      prep, …) from the embedded baked_pos dictionary. The rightmost
 *      content word in each utterance is marked stress=2 (focus) and
 *      gets a phrase-final boundary tone (L-L% for `.`/`!`, H-H% for
 *      `?`). H* pitch accent lands on the primary-stress syl of each
 *      content word.
 *
 * Reference reproduction target (DLL FE on the pangram):
 *   <the (0,3) det,0 [.0 dh ax]>
 *   <quick (4,5) adj,1 [.1,H* k w ih k]>
 *   ...
 *   <dog (40,4) noun,2 [.1,H*;L-L% d ao g]>
 */

#include "fe_internal.h"
#include "g2p.h"
#include "text_norm.h"
#include "baked_pos.h"
#include "pos_context.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----------------- string emit helpers ----------------- */

typedef struct {
    char *p;          /* write cursor */
    char *end;        /* one past last writable byte (reserves trailing NUL) */
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

/* ----------------- phoneme tokenization ----------------- */

/* Phoneme inventory — vowels carry stress digits, consonants don't.
 * Tag each emitted phoneme with whether it's a vowel so the syllabifier
 * knows where nuclei sit. We classify by name (first 1-2 chars). */
static int is_vowel_arpa(const char *p)
{
    /* CMU ARPAbet vowels: AA AE AH AO AW AY EH ER EY IH IY OW OY UH UW.
     * Phonemes in our internal storage are LOWERCASED by split_phonemes,
     * so accept both cases. */
    char c = p[0];
    if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
    return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
}

#define MAX_PHONS_PER_WORD 32

typedef struct {
    char  arpa[8];  /* lowercase, no stress digit */
    int   stress;   /* -1 = consonant; 0/1/2 = vowel with that stress */
} phon_t;

/* Split a CMU-style string like "HH AH0 L OW1" into phon_t[]. Lowercases
 * the ARPAbet name. Returns count (0 on empty / overflow). */
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
        /* Strip trailing stress digit if present. */
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
        /* CMU dict uses ARPAbet `AH` for both the stressed wedge vowel
         * (e.g. "cup" = K AH1 P) and the unstressed schwa (e.g. "about"
         * = AH0 B AW1 T). Tom's voice — and the Speechify FE in general
         * — distinguishes these as separate phone IDs: `ah` (stressed)
         * and `ax` (schwa). The downstream R3 refinement in fe_parse.c
         * (`ax → ix` in onset-present contexts) only fires on `ax`, not
         * `ah`, so leaving unstressed AH as `ah` cascades into wrong
         * ctx/sp inputs at every unit pool query — bigger than it
         * sounds, because Tom only has a few hundred true `ah` units
         * vs thousands of `ax` units. Map unstressed AH (stress 0 or 2)
         * to `ax` so downstream refinement and unit selection use the
         * right inventory. */
        if (out[n].arpa[0] == 'a' && out[n].arpa[1] == 'h'
            && out[n].arpa[2] == '\0' && stress != 1) {
            out[n].arpa[0] = 'a';
            out[n].arpa[1] = 'x';
        }
        ++n;
    }
    return n;
}

/* ----------------- syllabification ----------------- */

/* Allowed English multi-consonant onsets (in lowercase ARPAbet, space-
 * separated pairs/triples). The syllabifier checks LONGEST first. */
static int is_allowed_onset_pair(const char *a, const char *b)
{
    /* "stop + liquid/glide" and a few other common clusters. */
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

/* Assign each phoneme to its syllable index.
 *
 * Algorithm:
 *   1. Vowels = nuclei. Number them 0..nuclei-1.
 *   2. All phonemes before the FIRST vowel → syllable 0 (the onset).
 *   3. Between vowel V_k and V_{k+1}: walk consonants RIGHT-to-LEFT to
 *      build the longest allowed-onset cluster for syllable k+1. The
 *      rest go to syllable k (as coda).
 *   4. All phonemes after the LAST vowel → last syllable's coda.
 *
 * Returns number of syllables (= number of vowels). Writes syl-index
 * for each phoneme into out_syl[]. */
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
     * PREVIOUS vowel (so they're coda by default). Vowels carry their
     * own syllable index. Walk left-to-right. */
    for (int i = 0; i < n; ++i) {
        if (phs[i].stress >= 0) {
            out_syl[i] = syl++;
            last_vowel = i;
        } else {
            out_syl[i] = last_vowel < 0 ? 0 : out_syl[last_vowel];
        }
    }
    /* Pre-first-vowel consonants are already 0 (initialised). */

    /* Second pass: for each between-vowel consonant cluster, move as
     * many trailing consonants as form an allowed onset to the FOLLOWING
     * syllable. */
    last_vowel = -1;
    for (int i = 0; i < n; ++i) {
        if (phs[i].stress < 0) continue;
        if (last_vowel < 0) { last_vowel = i; continue; }

        /* Cluster between last_vowel and i, exclusive. */
        int cstart = last_vowel + 1;
        int cend = i;       /* one past last consonant */
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
            /* Single consonant always to the following onset. */
            out_syl[cend - 1] = next_syl;
        }
    adv:
        last_vowel = i;
    }
    return n_syllables;
}

/* ----------------- POS + function-word reduction ----------------- */

/* Map spfy_pos_class_t → string the DLL emits in word headers. Order
 * matches the enum in baked_pos.h. */
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
        case POS_UNKNOWN:        return "noun";   /* OOV default */
        default:                 return "noun";
    }
}

/* Function-word reduced pronunciations. When the word lemma matches AND
 * the POS tag is closed-class, override the CMU-dict-derived phoneme
 * string with these stress-less forms — this is what makes the DLL's
 * tagged output show `[.0 dh ax]` for "the" instead of `[.1 dh ah]`.
 *
 * Source: empirical scan of DLL captures. Stress-less ARPAbet (no `0`/
 * `1` suffix) — split_phonemes() treats stress-less vowel names as
 * stress=-1, which then drops to stress=0 in syl emission via the loop
 * that initialises s_stress from `phs[j].stress >= 0`.
 *
 * Convention: vowels here are written WITHOUT a stress digit so the
 * downstream syllabifier produces stress=0 syl markers. */
typedef struct {
    const char *word;       /* lowercase lemma */
    const char *phonemes;   /* space-separated ARPAbet, no stress digits */
} func_red_t;

static const func_red_t FUNC_RED[] = {
    /* Articles */
    { "the",  "DH AX0" },
    { "a",    "AX0"    },
    { "an",   "AX0 N"  },
    /* Prepositions (frequent reduced forms) */
    { "to",   "T AX0"  },
    { "for",  "F ER0"  },
    { "of",   "AX0 V"  },
    { "in",   "IH0 N"  },
    { "on",   "AA0 N"  },
    { "at",   "AX0 T"  },
    { "by",   "B AY0"  },
    { "from", "F R AH0 M" },
    { "with", "W IH0 TH" },
    /* Conjunctions */
    { "and",  "AX0 N D" },
    { "but",  "B AH0 T" },
    { "or",   "ER0"    },
    { "as",   "AE0 Z"  },
    { "than", "DH AX0 N" },
    /* Aux verbs */
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
    /* Pronouns (subject + possessive) */
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
    /* Demonstratives. Engine keeps lexical stress on the syl marker
     * (`.1 dh ih s`) but the word-header stress level is still 0
     * because the POS is closed-class (dem). analyze_word handles the
     * dem,0 derivation via pos_is_closed_class. */
    { "this", "DH IH1 S" },
    { "that", "DH AE1 T" },
    { "these","DH IY1 Z" },
    { "those","DH OW1 Z" },
    /* Function-word reductions for words that CMU pronounces with a
     * stressed vowel but the engine reduces (syl `.0`). */
    { "if",    "IH0 F"   },
    { "it",    "IH0 T"   },
    /* "until" — engine has [.0 ax n .1 t ih l] — first syl reduced,
     * second syl keeps lexical stress. */
    { "until", "AX0 N T IH1 L" },
    { NULL, NULL }
};

/* Lexical pronunciation overrides for open-class words where CMU and
 * the engine's dict disagree. Applied irrespective of POS. */
typedef struct { const char *word; const char *phonemes; } lex_override_t;

static const lex_override_t LEX_OVERRIDE[] = {
    /* CMU: HH AH0 L OW1 — engine uses HH EH0 L OW1 */
    { "hello",  "HH EH0 L OW1" },
    /* CMU "today" with AH0; engine uses AX0 */
    { "today",  "T AX0 D EY1" },
    /* ARPAbet phoneme names that engine treats as known SINGLE-phoneme
     * words in its dict. Verified against engine output for "The X sound"
     * probes: aa/ao/ey/ih emit one phoneme each. Other ARPAbet 2-letter
     * combos (ch, dh, dx, hh, jh, ng, sh, zh) are SPELLED OUT by engine
     * — see SPELL_OUT_LETTER_WORDS below. */
    { "aa", "AA1" }, { "ao", "AO1" }, { "ey", "EY1" }, { "ih", "IH1" },
    /* "Dr." abbreviation. CMU dict has "dr" → "D R AY1 V" (drive), but
     * engine expands to "doctor". Other title abbreviations (mr / mrs /
     * etc) already pronounce correctly in CMU; we only need to consume
     * the trailing period at the tokenization layer (see ABBREV_TAKES_PERIOD
     * below). */
    { "dr", "D AA1 K T ER0" },
    /* nat_036 "usual": CMU has Y UW1 ZH AH0 W AH0 L (4 syls); engine
     * collapses the medial schwa: Y UW1 ZH W AX0 L (3 syls, 6 phonemes). */
    { "usual",     "Y UW1 ZH W AX0 L" },
    /* nat_027 "arrive": CMU has ER0 AY1 V (3 phon); engine splits ER →
     * AX + R: AX0 R AY1 V (4 phonemes). */
    { "arrive",    "AX0 R AY1 V" },
    /* nat_040 "galleries": CMU "G AE1 L ER0 IY0 Z" — engine emits the
     * "gallerys" base form with ER → AX + R: G AE1 L AX0 R IY0 Z. */
    { "galleries", "G AE1 L AX0 R IY0 Z" },
    /* nat_049 "duration": CMU "D UH1 R EY1 SH AH0 N" — engine collapses
     * the unstressed first syl (UH+R → ER) and lowers AH0→IH0:
     * D ER0 EY1 SH IH0 N. */
    { "duration",  "D ER0 EY1 SH IH0 N" },
    { NULL, NULL }
};

/* Abbreviations whose trailing "." is part of the abbreviation, not a
 * sentence terminator. When the period is followed by another WORD token
 * we consume it; if followed by end-of-input or a phrase break, we leave
 * it as the actual sentence end (e.g. "Apples, oranges, etc." — "etc"
 * expands via CMU but the dot stays as sentence-final). */
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

/* Letter-name pronunciations (CMU ARPAbet, primary stress on nucleus).
 * Engine uses these when expanding short unknown words into letter-by-
 * letter spell-out (e.g. "dx" → "dee eks"). Indexed by (letter - 'a'). */
static const char *LETTER_PHONEMES[26] = {
    /* a */ "EY1",
    /* b */ "B IY1",
    /* c */ "S IY1",
    /* d */ "D IY1",
    /* e */ "IY1",
    /* f */ "EH1 F",
    /* g */ "JH IY1",
    /* h */ "EY1 CH",
    /* i */ "AY1",
    /* j */ "JH EY1",
    /* k */ "K EY1",
    /* l */ "EH1 L",
    /* m */ "EH1 M",
    /* n */ "EH1 N",
    /* o */ "OW1",
    /* p */ "P IY1",
    /* q */ "K Y UW1",
    /* r */ "AA1 R",
    /* s */ "EH1 S",
    /* t */ "T IY1",
    /* u */ "Y UW1",
    /* v */ "V IY1",
    /* w */ "D AH1 B AH0 L Y UW1",
    /* x */ "EH1 K S",
    /* y */ "W AY1",
    /* z */ "Z IY1",
};

/* Words engine spells out letter-by-letter (it lacks a dict entry, and
 * its FE writes one noun-word per letter). Verified by inspecting engine
 * tagged-text for "The X sound" probes — these eight emit two noun
 * letter-name words; the aa/ao/ey/ih four above emit a single noun. */
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

/* "Mmm.", "Sssss." — engine spells repeated-CONSONANT onomatopoeias
 * letter by letter ("em em em" / "ess ess ess ess ess"). Restricted to
 * consonants because repeated vowels (e.g. "aa" — phn_001 single ARPAbet
 * symbol; "eee" — edge_016 engine treats as a unique undef-word with
 * collapsed phonemes [iy ih]) follow different engine rules. */
static int is_repeated_consonant_word(const char *word_lower)
{
    size_t L = strlen(word_lower);
    if (L < 2) return 0;
    char c0 = word_lower[0];
    if (c0 < 'a' || c0 > 'z') return 0;
    /* Vowels (and 'y' which is ambiguous) are excluded. */
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

/* Force-tag the core function words to their canonical closed-class
 * POS even when baked_pos returns a different (rare nominal) sense.
 * Without this, "a"/"in"/"i"/"to" get tagged `noun` because the dict
 * also has those words as nouns (the letter A, the noun "in" as in
 * "ins and outs"). The DLL's POS tagger uses syntactic context we
 * don't replicate; hardcoding the canonical form is good enough for
 * the 90% case. */
typedef struct { const char *word; spfy_pos_class_t pos; } pos_override_t;

static const pos_override_t POS_OVERRIDE[] = {
    /* Articles + det */
    { "a",     POS_DET  }, { "an",    POS_DET  }, { "the",   POS_DET  },
    /* Prepositions */
    { "to",    POS_PREP }, { "for",   POS_PREP }, { "of",    POS_PREP },
    { "in",    POS_PREP }, { "on",    POS_PREP }, { "at",    POS_PREP },
    { "by",    POS_PREP }, { "from",  POS_PREP }, { "with",  POS_PREP },
    { "about", POS_PREP }, { "into",  POS_PREP }, { "onto",  POS_PREP },
    { "upon",  POS_PREP }, { "after", POS_PREP }, { "until", POS_PREP },
    /* Conjunctions */
    { "and",   POS_CONJ }, { "but",   POS_CONJ }, { "or",    POS_CONJ },
    { "as",    POS_CONJ }, { "if",    POS_CONJ }, { "while", POS_CONJ },
    { "because", POS_CONJ }, { "although", POS_CONJ }, { "since", POS_CONJ },
    { "unless", POS_CONJ }, { "when",  POS_CONJ }, { "before", POS_CONJ },
    /* "after"/"until" can be either prep or conj in English; the DLL's
     * empirical tagging on this corpus prefers prep (likely picks the
     * preposition reading by default). Match that. */
    /* Aux verbs */
    { "is",    POS_AUX  }, { "are",   POS_AUX  }, { "was",   POS_AUX  },
    { "were",  POS_AUX  }, { "be",    POS_AUX  }, { "been",  POS_AUX  },
    { "has",   POS_AUX  }, { "have",  POS_AUX  }, { "had",   POS_AUX  },
    { "do",    POS_AUX  }, { "does",  POS_AUX  }, { "did",   POS_AUX  },
    { "will",  POS_AUX  }, { "would", POS_AUX  }, { "can",   POS_AUX  },
    { "could", POS_AUX  }, { "should", POS_AUX }, { "might", POS_AUX  },
    /* Pronouns */
    { "i",     POS_PRO  }, { "you",   POS_PRO  }, { "he",    POS_PRO  },
    { "she",   POS_PRO  }, { "we",    POS_PRO  }, { "they",  POS_PRO  },
    { "me",    POS_PRO  }, { "him",   POS_PRO  }, { "us",    POS_PRO  },
    { "them",  POS_PRO  }, { "my",    POS_PRO2 }, { "your",  POS_PRO2 },
    { "his",   POS_PRO2 }, { "her",   POS_PRO2 }, { "our",   POS_PRO2 },
    { "their", POS_PRO2 }, { "its",   POS_PRO2 },
    /* DLL tags pre-nominal possessive "your" as `det` (e.g. "your
     * message" = determiner usage, predominant in the corpus). */
    { "your",  POS_DET  },
    { "it",    POS_PRO  }, { "one",   POS_PRO },
    /* Demonstratives */
    { "this",  POS_DEM  }, { "that",  POS_DEM  }, { "these", POS_DEM  },
    { "those", POS_DEM  },
    /* Wh-words */
    { "who",   POS_WH   }, { "what",  POS_WH   }, { "when",  POS_WH   },
    { "where", POS_WH   }, { "why",   POS_WH   }, { "how",   POS_WH   },
    /* Negation */
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

/* Return reduced-form phoneme string if (word, pos) matches the table,
 * else NULL. */
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

/* ----------------- per-utt word buffer ----------------- */

#define MAX_WORDS_PER_UTT 96

typedef struct {
    char              text[64];      /* original spelling */
    int               char_off;
    int               char_len;
    spfy_pos_class_t  pos;
    int               stress_lvl;    /* 0/1/2 — 2 only on focus */
    int               primary_syl;   /* syl idx carrying stress=1; -1 if none */
    int               is_focus;
    phon_t            phs[MAX_PHONS_PER_WORD];
    int               n_phs;
    int               syl[MAX_PHONS_PER_WORD];
    int               n_syl;
} word_rec_t;

typedef struct {
    word_rec_t  words[MAX_WORDS_PER_UTT];
    int         n_words;
    char        terminator;    /* '.', '!', '?', 0 (none/comma) */
} utt_buf_t;

static void utt_init(utt_buf_t *u) { u->n_words = 0; u->terminator = 0; }

/* Lowercase a word into out_lower. Returns strlen written. */
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

/* Strip a trailing 's apostrophe-s from `word_lower` for POS lookup —
 * baked_pos has lemma forms ("today") not possessives ("today's"). */
static void strip_possessive(char *w)
{
    size_t L = strlen(w);
    if (L >= 2 && w[L-2] == '\'' && (w[L-1] == 's' || w[L-1] == 'd')) {
        w[L-2] = '\0';
    }
}

/* Cheap POS-only lookup for auto-break lookahead. Returns the raw POS
 * BEFORE the multi-class collapse so callers can ask "can this word be a
 * verb?" (NOUN_VERB / VERB_ADJ / NOUN_VERB_ADJ should all answer yes). */
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

/* Resolve word → (pos, phonemes, syllabification). Returns 0 on success,
 * non-zero if the word's pronunciation can't be looked up. */
static int analyze_word(const char *raw_word, word_rec_t *w)
{
    /* Lowercased lemma for POS + reduction lookup. */
    char low[64];
    lower_copy(raw_word, low, sizeof low);
    strip_possessive(low);

    /* POS lookup: try the small override table first (canonical form
     * for ambiguous high-frequency function words), then the baked_pos
     * dict. Multi-class results (noun_verb / noun_adj / verb_adj /
     * noun_verb_adj / adj_adv) get collapsed to the single class the
     * DLL picks most often on this corpus. The DLL itself uses a real
     * syntactic tagger; without one we just take the empirical default. */
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

    /* Get phonemes. Priority order: function-word reduction (closed-class
     * lemmas) → small lexical override table (CMU/engine disagreements
     * on open-class lemmas) → CMU dict / suffix / LTS. */
    const char *phon_str = lookup_function_reduction(low, pos);
    char cmu_buf[160];
    if (!phon_str) phon_str = lookup_lex_override(low);
    if (!phon_str) {
        spfy_g2p_origin_t origin;
        int rc = spfy_g2p_word_lookup_ex(low, cmu_buf, sizeof cmu_buf, &origin);
        if (rc != 0) return rc;
        phon_str = cmu_buf;
    }

    /* Possessive 's / contraction 'd suffix. strip_possessive() above
     * trimmed the apostrophe + final letter before lookup so we get the
     * lemma's phonemes; append the inflectional phoneme back here.
     *
     * Possessive/plural 's voicing rule:
     *   sibilant coda (s z sh zh ch jh) → IH0 Z  ("horse's")
     *   voiceless coda (p t k f th)     → S      ("cat's")
     *   voiced / vowel coda             → Z      ("dog's", "today's")
     *
     * Contraction 'd appends a single D ("he'd", "we'd"). */
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
            const char *suffix = has_contr_d ? " D" : " Z";   /* default 'd → D; 's voiced → Z */
            if (has_poss_s) {
                /* Read last phoneme from phon_str to pick allomorph. */
                size_t pl = strlen(phon_str);
                size_t last_start = pl;
                while (last_start > 0 && phon_str[last_start - 1] != ' ') last_start--;
                char arpa_lc[8] = {0};
                for (int j = 0; j < 7 && phon_str[last_start + j]; ++j) {
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

    /* Tokenize phonemes + syllabify. */
    w->n_phs = split_phonemes(phon_str, w->phs, MAX_PHONS_PER_WORD);
    if (w->n_phs == 0) return -1;
    w->n_syl = syllabify(w->phs, w->n_phs, w->syl);

    /* Per-word stress level: max stress across syllables. Reduced forms
     * have all stress=-1, so max remains 0 (stored as `stress_lvl=0`).
     * `primary_syl` is the first syl whose nucleus carries stress=1.
     *
     * Word stress level decoupled from lexical syl stress: engine emits
     * `<this (...) dem,0 [.1 dh ih s]>` — word stress=0 (POS-driven)
     * but syllable .1 (lexical stress kept).
     *
     * Empirically the engine ALWAYS deaccents (word stress=0) for
     * DET/AUX/CONJ/DEM/NOT — these classes are fully closed and engine
     * never bumps them to content stress. PREP/PRO/PRO2/WH are more
     * variable: monosyllabic instances like "to"/"in"/"if" reduce, but
     * multi-syllable preps ("over") and pronouns in focus ("i"/"why")
     * keep stress=1 or 2. For those, fall back to lexical max so words
     * like "over" (CMU OW1 V ER0) correctly get stress=1.
     *
     * Open-class words get 1 when their max lexical stress > 0; flush_utt
     * promotes the rightmost stressed word to 2 (focus). */
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

/* ----------------- emission ----------------- */

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
    emit_str(e, " [");

    /* Boundary tone goes on the LAST syllable of the focus word,
     * regardless of stress. Engine: `<morning … [.1,H* m ao r .0;L-H%
     * n ih ng]>` — H* on primary-stress syl, L-* on LAST syl. When the
     * primary and last syl coincide (single-syl words like "dog", "men"),
     * both fire on the same syllable: `.1,H*;L-L%`.
     *
     * Tone selection by utt terminator:
     *   `?`  → H-H% (question rise)
     *   `,`  → L-H% (continuation rise — phrase break, more content follows)
     *   else → L-L% (sentence-final fall) */
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
                /* Engine uses `;X-Y%` directly after the stress digit
                 * (or after `,H*` when both fire on the same syl). */
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

/* Apply the before-vowel allomorph for "the" and "to": when followed
 * by a word starting with a vowel phoneme, "the" → DH IY0 and "to" →
 * T UW0. The DLL captures show this rule fires consistently. We must
 * do it AFTER analyze_word has filled w->phs for every word so the
 * lookahead can inspect the next word's first phoneme. */
static int starts_with_vowel(const word_rec_t *w)
{
    return w->n_phs > 0 && is_vowel_arpa(w->phs[0].arpa);
}

static void rewrite_phs(word_rec_t *w, const char *phon_str)
{
    w->n_phs = split_phonemes(phon_str, w->phs, MAX_PHONS_PER_WORD);
    w->n_syl = syllabify(w->phs, w->n_phs, w->syl);
    /* Stress is irrelevant for these (all stress=0); reset focus. */
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
        /* "a"→"an" is a spelling change the writer is expected to do
         * (the engine doesn't substitute). Skip. */
    }
}

/* Flush a buffered utterance: pick the focus word, then emit. Resets
 * the buffer for the next utterance. */
/* Refine each word's POS using the engine-empirical context table
 * (pos_context.c, captured from the DLL FE). The lookup is three-tier:
 *   1. (word, prev_word, next_word) — most specific
 *   2. (word, prev_word)
 *   3. word alone
 * Falls through to the analyze_word default on miss. Skips words whose
 * POS came from POS_OVERRIDE (the hardcoded core function-word pin map)
 * since those are the canonical-form decisions we want to keep. */
static void refine_utt_pos(utt_buf_t *u)
{
    for (int i = 0; i < u->n_words; ++i) {
        word_rec_t *w = &u->words[i];
        char low[64], prev[64] = "^", nxt[64] = "$";
        lower_copy(w->text, low, sizeof low);
        strip_possessive(low);
        /* Honour the POS_OVERRIDE pin for canonical function-word forms. */
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
 * (e.g. nat_036 utt 2 "...volume } { and apologize..."). Intra-utt
 * occurrences keep the regular AX0 N D from FUNC_RED. */
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

    /* Pick focus: the LAST word with stress_lvl ≥ 1 (i.e. the last
     * content word in the utterance). If none, leave is_focus = 0
     * everywhere — utt has no pitch accent. */
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

/* ----------------- public API ----------------- */

int spfy_fe_internal_text_to_tagged(const char *text,
                                     char *out, size_t out_n)
{
    if (!text || !out || out_n == 0) return -1;
    out[0] = '\0';
    emit_t e = { out, out + out_n - 1, 0 };

    /* Tokenize. */
    spfy_token_t toks[512];
    size_t nt = 0;
    int rc = spfy_text_normalize(text, toks, sizeof toks / sizeof toks[0], &nt);
    if (rc < 0) return -1;

    /* The DLL emits one `#{X ... pau(p50) }` block per phrase/sentence
     * with X = the boundary punctuation (`.`/`,`/`?`/`!`/`;`). Only the
     * FIRST utt's opener carries the leading `#`; subsequent utts open
     * with bare `{X`. The whole stream is closed with `} %%`. We defer
     * opener emission until each utt's terminator is known, so we keep
     * the words buffered and flush them with the right wrapper.
     *
     * Inter-utt opening pause length is terminator-driven:
     *   sentence-final (`.`/`!`/`?`) → pau(p350)  (~350 ms)
     *   phrase-only    (`,`/`;`/`:`) → pau(p100)  (~100 ms)
     * Matching what the DLL emits empirically. The 200 ms uniform
     * value we used before sounded too long at commas and too short
     * at sentence ends — bimodal cadence is what makes long passages
     * not feel stuttery.
     *
     *   #{<term0> pau(p25)  <word>* pau(p50) }
     *           {<term1> pau(p<X>) <word>* pau(p50) }
     *           {<term2> pau(p<X>) <word>* pau(p50) } %%
     */

    int char_off = 0;
    int is_first_utt = 1;
    utt_buf_t utt;
    utt_init(&utt);

    #define WRITE_UTT(term_char) do { \
        if (utt.n_words > 0) { \
            const char *open_pau = \
                ((term_char) == '.' || (term_char) == '!' \
                 || (term_char) == '?') \
                ? " pau(p350) " : " pau(p100) "; \
            int was_first_utt = is_first_utt; \
            if (is_first_utt) { \
                emit_str(&e, "#{"); \
                { char tmp[2] = { (term_char), 0 }; emit_str(&e, tmp); } \
                emit_str(&e, " pau(p25) "); \
                is_first_utt = 0; \
            } else { \
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

    /* Auto-phrase-break heuristic. Engine inserts a mid-sentence
     * prosodic break (renders as two consecutive `pau` phonemes between
     * utterances) when a long sentence has no internal punctuation but
     * crosses a natural prosodic boundary — most commonly:
     *
     *   - before subordinating conjunctions  (while, because, if, …)
     *   - before an infinitive "to"           ("…men || to come…")
     *
     * Implementation: as each word arrives, if the current utt already
     * has >= AUTO_BREAK_MIN_WORDS content words AND the incoming word
     * matches a trigger, flush the utt with a `,` boundary BEFORE
     * adding the word. WRITE_UTT(',') resets utt.n_words to 0, so the
     * threshold check naturally prevents re-firing in quick succession.
     *
     * Threshold tuned empirically: engine breaks observed after 7-9
     * preceding words on the corpus (text_029 has 8 before "to";
     * nat_042 has 9 before "while"). 7 catches both. */
    #define AUTO_BREAK_MIN_WORDS 7

    for (size_t i = 0; i < nt; ++i) {
        switch (toks[i].type) {
        case SPFY_TOKEN_WORD: {
            if (utt.n_words >= MAX_WORDS_PER_UTT) break;
            /* Auto-break check (only fires when the current utt has
             * accumulated enough content). See big comment above.
             * Some triggers use lookahead at the next WORD token to
             * disambiguate: "to+verb" is infinitive (break), "to+noun"
             * is preposition (no break); "and+verb" starts a coordinate
             * clause (break), "and+pronoun/noun" is in-phrase (no break). */
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
                /* Subordinating conjunctions that reliably signal a
                 * clause boundary. Excludes "before"/"after"/"until"/
                 * "since" — those are usually prepositions in the
                 * audit corpus (e.g. "from nine in the morning until
                 * five"), not clause-starting conjunctions; including
                 * them caused false-positive auto-breaks on nat_030
                 * (until), nat_037 (until), and nat_041 (before). */
                static const char *subs[] = {
                    "while", "because", "if", "unless", "when",
                    "although", "though", "whereas", NULL
                };
                for (int si = 0; subs[si]; ++si) {
                    if (strcmp(low_check, subs[si]) == 0) { trigger = 1; break; }
                }
                /* Infinitive "to" after a content word AND followed by a
                 * word that can be a verb. Without the lookahead this fired
                 * on prepositional "to + noun" (e.g. "switched to airplane"
                 * in nat_049 — engine doesn't break there). */
                if (!trigger && strcmp(low_check, "to") == 0
                    && utt.words[utt.n_words - 1].stress_lvl >= 1
                    && pos_can_be_verb(next_pos)) {
                    trigger = 1;
                }
                /* Coordinator "and" / "or" starting a new VERB-headed
                 * clause (lookahead). Catches nat_036 "...volume and
                 * apologize..."; declines to fire on intra-NP coords
                 * like "Tom and Jerry" or "name and number". */
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
            /* Complementizer "that" after a verb (no length threshold —
             * engine breaks even after short prefixes like "Our records
             * indicate that..." in nat_035). Fires only when the prior
             * word is verbal; "that book" (det) / "stop that" (dem) /
             * "this is the case that..." (relative pronoun in NP) are
             * out of scope here. */
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

            /* Letter-by-letter spell-out for ARPAbet 2-letter combos the
             * engine lacks in its dict (ch, dh, dx, hh, jh, ng, sh, zh).
             * Engine emits one noun-word per letter using the letter-name
             * pronunciation ("dx" → <dee><eks>). We mirror that by
             * synthesizing additional word_rec_t entries in place of the
             * original token, sharing the same char_off/len. */
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
                    break;   /* out of WORD case */
                }
            }

            if (analyze_word(w->text, w) != 0) {
                memset(w, 0, sizeof *w);
                break;
            }
            ++utt.n_words;

            /* Abbreviation period swallow: "Dr.", "Mr.", "Mrs.", "etc.", …
             * When followed by another WORD token, the period belongs to
             * the abbreviation (not a sentence boundary). Skip it so
             * "Mr. Jones is here." stays one utt instead of two. */
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
                        break;   /* only inspect the very next token */
                    }
                    if (next_is_word) ++i;   /* consume the period */
                }
            }
            break;
        }
        case SPFY_TOKEN_PHRASE_BREAK:
            /* Comma / semicolon — separate utt in the DLL's convention,
             * with `{,` opener and L-L% boundary on the focus word. */
            WRITE_UTT(',');
            break;
        case SPFY_TOKEN_SENTENCE_BREAK: {
            char t = (toks[i].text[0] == '?' || toks[i].text[0] == '!')
                     ? toks[i].text[0] : '.';
            WRITE_UTT(t);
            break;
        }
        }
    }

    /* Tail utt — input didn't end in a sentence break. */
    if (utt.n_words > 0) {
        WRITE_UTT('.');
    }
    emit_str(&e, "%%");
    #undef WRITE_UTT

    /* Suppress unused-static warnings. */
    (void)is_vowel_arpa;

    return e.truncated ? 1 : 0;
}
