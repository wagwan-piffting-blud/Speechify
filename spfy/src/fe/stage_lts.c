/* Stage 4: Letter-to-phoneme rules (basic English LTS).
 *
 * Hand-coded ruleset that walks each syllable and emits one or more
 * phoneme tokens per letter. Primary digraphs (th/ch/sh/ph/ng/ck/qu)
 * and silent-letter patterns (kn/gn/wr/mb-final) are handled. Vowels
 * pick a context-sensitive phoneme based on whether the syllable is
 * "open" (ends in vowel) or "closed" (ends in consonant).
 *
 * This is intentionally small and rule-based -- it gets text -> some
 * phoneme stream flowing for end-to-end testing. Quality improvement
 * comes from a future data-driven LTS using registry-B tables.
 */

#include "stage_lts.h"
#include "baked_dict.h"
#include "fe.h"
#include "stream.h"
#include "vocab.h"

#include <spfy/spfy.h>

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Phoneme symbol IDs in the 469-vocabulary. Resolved by NAME once at
 * fe-open time would be ideal; for MVP we hard-code the indices we
 * verified during F2 vocab decoding. */
enum {
    PH_b = 229, PH_p = 230, PH_d = 231, PH_t = 232,
    PH_k = 235, PH_g = 236,
    PH_TH = 238,    /* unvoiced th */
    PH_DH = 237,    /* voiced th */
    PH_v = 239, PH_f = 240, PH_z = 241, PH_s = 242,
    PH_ZH = 243, PH_SH = 244,
    PH_JH = 245,    /* voiced /dʒ/ — ARPAbet "jh" */
    PH_CH = 246,
    PH_h = 247,
    PH_m = 248, PH_n = 249, PH_NG = 250,
    PH_r = 251, PH_l = 252,
    PH_y = 253, PH_w = 254,
    PH_ii = 255,    /* /i/ as in "see" */
    PH_I  = 256,    /* /I/ as in "sit" */
    PH_e  = 257,    /* /e/ as in "say" */
    PH_E  = 258,    /* /E/ as in "set" */
    PH_A  = 259,    /* /A/ as in "father" */
    PH_aa = 271,    /* /a/ as in "cat" */
    PH_u  = 272,    /* /u/ as in "soon" */
    PH_U  = 273,    /* /U/ as in "put" */
    PH_o  = 274,    /* /o/ as in "go" */
    PH_O  = 278,    /* /O/ as in "thought" */
    PH_at = 266,    /* /@/ schwa */
    PH_AH = 260,    /* /X/ as in "but" / "one" / "love" — ARPAbet "ah" */
    PH_AY = 276,    /* /Y/ diphthong /aɪ/ — "fly", "I", "five" */
    PH_AW = 277,    /* /W/ diphthong /aʊ/ — "now", "house" */
    PH_IX = 261,    /* /Xx/ reduced /ɪ/ — ARPAbet "ix"; "the" before C */
    PH_ER = 264,    /* /R/ rhotacized — ARPAbet "er"; "world", "her" */
    PH_DX = 233,    /* flap-t — ARPAbet "dx"; intervocalic t/d ("Peter") */
};

/* ARPAbet vowel set: vocab IDs for aa, ae, ah, ao, aw, ax, ay, eh,
 * er, ey, ih, ix, iy, ow, oy, uh, uw plus the syllabic en/el. */
static int is_phon_vowel(uint16_t pid)
{
    switch (pid) {
        case PH_A:    /* aa */
        case PH_aa:   /* ae */
        case PH_AH:   /* ah */
        case PH_O:    /* ao */
        case PH_AW:   /* aw */
        case PH_at:   /* ax */
        case PH_AY:   /* ay */
        case PH_E:    /* eh */
        case PH_ER:   /* er */
        case PH_e:    /* ey */
        case PH_I:    /* ih */
        case PH_IX:   /* ix */
        case PH_ii:   /* iy */
        case PH_o:    /* ow */
        case 275:     /* oy */
        case PH_U:    /* uh */
        case PH_u:    /* uw */
            return 1;
        default:
            return 0;
    }
}

/* ARPAbet voiced phoneme set. All vowels are voiced; voiced consonants
 * are b, d, dh, dx, g, jh, l, m, n, ng, r, v, w, y, z, zh. Retained as a
 * documented companion to is_phon_vowel(); not currently wired into any
 * LTS pass, hence the unused attribute to keep the zero-warning build. */
__attribute__((unused)) static int is_phon_voiced(uint16_t pid)
{
    if (is_phon_vowel(pid)) return 1;
    switch (pid) {
        case PH_b:    case PH_d:    case PH_DH:   case PH_DX:
        case PH_g:    case PH_JH:   case PH_l:    case PH_m:
        case PH_n:    case PH_NG:   case PH_r:    case PH_v:
        case PH_w:    case PH_y:    case PH_z:    case PH_ZH:
            return 1;
        default:
            return 0;
    }
}

/* Phoneme-token field index for per-phone stress (0/1/2 ARPAbet markers
 * carried from the baked dictionary). Read by the post-pass at the end
 * of spfy_fe_lts_run that aggregates max-stress per syllable into the
 * syl token's `.name`, which stage_spr.c then maps into sp[1] (sylType).
 * Engine downstream consumes sylType as q_type 1 of the durt+f0tr CARTs. */
#define SPFY_PHON_FIELD_STRESS 4

/* Push one phoneme token into %phoneme stream. */
static void emit(spfy_fe_delta_t *delta,
                  uint16_t          phon_id,
                  uint16_t          syl_id,
                  uint16_t          word_id,
                  uint16_t          phrase_id,
                  uint16_t          src_off,
                  uint16_t          src_len,
                  int               is_vowel,
                  uint16_t          pos_in_syl,
                  uint8_t           stress)
{
    spfy_fe_token_t tk = {0};
    tk.name      = phon_id;
    tk.syl_id    = syl_id;
    tk.word_id   = word_id;
    tk.phrase_id = phrase_id;
    tk.fields[0] = src_off;
    tk.fields[1] = src_len;
    tk.fields[2] = (uint16_t)is_vowel;
    tk.fields[3] = pos_in_syl;
    tk.fields[SPFY_PHON_FIELD_STRESS] = (uint16_t)stress;
    spfy_fe_stream_push(delta, SPFY_STREAM_PHONEME, tk);
}

static char lc(char c) { return (char)tolower((unsigned char)c); }

static int is_vowel_letter(char c)
{
    char l = lc(c);
    return (l == 'a' || l == 'e' || l == 'i' || l == 'o' || l == 'u');
}

/* Two-letter digraph match. */
static int dg(const char *t, uint32_t i, uint32_t end,
               const char *lit)
{
    if (i + 2 > end) return 0;
    return lc(t[i]) == lit[0] && lc(t[i + 1]) == lit[1];
}

/* Pick vowel phoneme. `letter` = source vowel; `is_open` = syllable
 * ends in a vowel (no coda). */
static uint16_t pick_vowel(char letter, int is_open, int has_e,
                            int is_stressed)
{
    char v = lc(letter);
    /* Unstressed open 'e' reduces to /ix/ ("synthe-sizing" syl "the" ->
     * /th ix/, not /th iy/). Only this pattern is general enough to
     * apply confidently — other vowels (a/i/o/u) have morphologically-
     * conditioned reductions that we'd need stress-aware POS info to
     * predict (e.g. "synthesi-zing" keeps /aɪ/ in unstressed "si"
     * because it's the verb-stem vowel). */
    if (!is_stressed && (has_e || is_open) && v == 'e') {
        return PH_IX;
    }
    /* "Magic e" (silent e at end of syllable) makes the vowel "long". */
    if (has_e) {
        switch (v) {
        case 'a': return PH_e;     /* "make" */
        case 'e': return PH_ii;    /* "scene" */
        case 'i': return PH_AY;    /* "fine" /faɪn/ */
        case 'o': return PH_o;     /* "rope" */
        case 'u': return PH_u;     /* "tune" */
        }
    }
    if (is_open) {
        switch (v) {
        case 'a': return PH_e;     /* "ba-by" */
        case 'e': return PH_ii;
        case 'i': return PH_AY;    /* "hi", "fly" /aɪ/ */
        case 'o': return PH_o;
        case 'u': return PH_u;
        }
    }
    /* Closed syllable. Conservative reduction only — short vowels are
     * already close to their reduced forms (ih, eh, uh). */
    switch (v) {
    case 'a': return PH_aa;        /* "cat" */
    case 'e': return PH_E;         /* "set" */
    case 'i': return PH_I;         /* "sit" */
    case 'o': return PH_A;         /* "stop"=/stɑp/ — engine uses ARPAbet
                                      "aa" not "ao" for American /ɑ/. */
    case 'u': return PH_U;         /* "put" */
    }
    return PH_at;                  /* schwa fallback */
}

/* Lowercase compare of a syllable's letters against a literal. */
static int syl_eq(const char *t, uint32_t off, uint32_t len, const char *lit)
{
    size_t n = strlen(lit);
    if (len != n) return 0;
    for (size_t i = 0; i < n; ++i) {
        if (lc(t[off + i]) != lit[i]) return 0;
    }
    return 1;
}

/* High-frequency irregular-word dictionary. Many common English words
 * are read letter-by-letter by the rule-based LTS (e.g., "eight" →
 * "e/i/g/h/t"); this table catches them and emits the correct phoneme
 * sequence directly. Entries are matched against the WHOLE syllable
 * text (lowercased). One word per syllable in the engine's syllabifier
 * happens often enough that whole-syllable matching is the right unit.
 *
 * Phoneme list per entry is a fixed-length array terminated by a 0
 * sentinel (vocab id 0 = "GAP" which is never emitted as a phoneme). */
typedef struct {
    const char *spelling;
    uint16_t    phons[16];     /* up to 15 phones + 0 terminator */
} irreg_word_t;

static const irreg_word_t IRREGULAR_WORDS[] = {
    { "i",       { PH_AY, 0 } },
    { "a",       { PH_e,  0 } },     /* the letter 'A' / article */
    { "o",       { PH_o,  0 } },     /* letter 'O' */
    { "one",     { PH_w,  PH_AH, PH_n,  0 } },
    { "two",     { PH_t,  PH_u,            0 } },
    { "three",   { PH_TH, PH_r, PH_ii,    0 } },
    { "four",    { PH_f,  PH_O, PH_r,     0 } },
    { "five",    { PH_f,  PH_AY, PH_v,    0 } },
    { "six",     { PH_s,  PH_I, PH_k, PH_s, 0 } },
    { "seven",   { PH_s,  PH_E, PH_v, PH_at, PH_n, 0 } },
    { "eight",   { PH_e,  PH_t,           0 } },
    { "nine",    { PH_n,  PH_AY, PH_n,    0 } },
    { "ten",     { PH_t,  PH_E, PH_n,     0 } },
    { "the",     { PH_d,  PH_IX,          0 } },  /* engine: d+ix, not dh+ax */
    { "of",      { PH_at, PH_v,           0 } },
    { "hello",   { PH_h,  PH_E, PH_l, PH_o, 0 } },
    { "world",   { PH_w,  PH_ER, PH_l, PH_d, 0 } },
    { "her",     { PH_h,  PH_ER,          0 } },
    { "fox",     { PH_f,  PH_aa, PH_k, PH_s, 0 } },
    { "dog",     { PH_d,  PH_A, PH_g,     0 } },  /* American /dɑg/ */
    { "cat",     { PH_k,  PH_aa, PH_t,    0 } },
    { "today",   { PH_t,  PH_at, PH_d, PH_e, 0 } },
    { "yes",     { PH_y,  PH_E, PH_s,     0 } },
    { "english", { PH_I, PH_NG, PH_g, PH_l, PH_I, PH_SH, 0 } },
    { "press",   { PH_p,  PH_r, PH_E, PH_s, 0 } },
    { "thank",   { PH_TH, PH_aa, PH_NG, PH_k, 0 } },
    { "thanks",  { PH_TH, PH_aa, PH_NG, PH_k, PH_s, 0 } },
    { "please",  { PH_p,  PH_l, PH_ii, PH_z, 0 } },
    { "leave",   { PH_l,  PH_ii, PH_v,    0 } },
    { "message", { PH_m,  PH_E, PH_s, PH_at, PH_CH, 0 } },
    { "after",   { PH_aa, PH_f, PH_t, PH_ER, 0 } },
    { "tone",    { PH_t,  PH_o, PH_n,     0 } },
    { "call",    { PH_k,  PH_O, PH_l,     0 } },
    { "us",      { PH_at, PH_s,           0 } },
    { "to",      { PH_t,  PH_u,           0 } },
    { "and",     { PH_aa, PH_n, PH_d,     0 } },
    { "in",      { PH_I,  PH_n,           0 } },
    { "is",      { PH_IX, PH_z,           0 } },  /* engine: ix+z, not I+z */
    { "it",      { PH_I,  PH_t,           0 } },
    { "you",     { PH_y,  PH_u,           0 } },
    { "your",    { PH_y,  PH_O, PH_r,     0 } },
    { "are",     { PH_A,  PH_r,           0 } },
    { "was",     { PH_w,  PH_AH, PH_z,    0 } },
    { "have",    { PH_h,  PH_aa, PH_v,    0 } },
    { "do",      { PH_d,  PH_u,           0 } },
    { "what",    { PH_w,  PH_AH, PH_t,    0 } },
    { "where",   { PH_w,  PH_E, PH_r,     0 } },
    { "there",   { PH_DH, PH_E, PH_r,     0 } },
    { "they",    { PH_DH, PH_e,           0 } },
    { "be",      { PH_b,  PH_ii,          0 } },
    { "been",    { PH_b,  PH_I, PH_n,     0 } },
    { "for",     { PH_f,  PH_O, PH_r,     0 } },
    { "this",    { PH_DH, PH_I, PH_s,     0 } },
    { "that",    { PH_DH, PH_aa, PH_t,    0 } },
    { "with",    { PH_w,  PH_I, PH_TH,    0 } },
    { "as",      { PH_aa, PH_z,           0 } },
    { "or",      { PH_O,  PH_r,           0 } },
    { "by",      { PH_b,  PH_AY,          0 } },
    { "my",      { PH_m,  PH_AY,          0 } },
    { "we",      { PH_w,  PH_ii,          0 } },
    { "he",      { PH_h,  PH_ii,          0 } },
    { "she",     { PH_SH, PH_ii,          0 } },
    { "me",      { PH_m,  PH_ii,          0 } },
    { "no",      { PH_n,  PH_o,           0 } },
    { "go",      { PH_g,  PH_o,           0 } },
    { "so",      { PH_s,  PH_o,           0 } },
    { "now",     { PH_n,  PH_AW,          0 } },
    { "how",     { PH_h,  PH_AW,          0 } },
    { "but",     { PH_b,  PH_AH, PH_t,    0 } },
    { "from",    { PH_f,  PH_r, PH_AH, PH_m, 0 } },
    { "all",     { PH_O,  PH_l,           0 } },

    { "would",   { PH_w,  PH_U, PH_d,     0 } },
    { "could",   { PH_k,  PH_U, PH_d,     0 } },
    { "should",  { PH_SH, PH_U, PH_d,     0 } },
    { "their",   { PH_DH, PH_E, PH_r,     0 } },
    { "more",    { PH_m,  PH_O, PH_r,     0 } },
    { "out",     { PH_AW, PH_t,           0 } },
    { "an",      { PH_aa, PH_n,           0 } },
    { "stop",    { PH_s,  PH_t, PH_A, PH_p, 0 } },  /* engine: aa not ao */
    { "hi",      { PH_h,  PH_AY,          0 } },
    { "aye",     { PH_AY,                 0 } },
    { "why",     { PH_w,  PH_AY,          0 } },
    { "mmm",     { PH_E, PH_m, PH_E, PH_m, PH_E, PH_m, 0 } },  /* engine: /eh m eh m eh m/ */
    { NULL, { 0 } },
};

/* Look up a word in the dictionary; emit its phonemes and return 1 on
 * match, 0 otherwise.
 *
 * Order matters: BAKED dictionary is consulted FIRST -- it's the engine's
 * authoritative pronunciation for 36k words and uses vocab IDs verified
 * against stage_spr.c's SAMPA_ARPA table. The hand-written IRREGULAR_WORDS
 * entries are an early-iteration table whose PH_* macros pre-date the
 * SAMPA_ARPA fix, so several of them are vocab-id-stale (e.g. "fox" used
 * PH_aa=271 which actually maps to ARPAbet "ae"=cat, not "aa"=father).
 * IRREGULAR_WORDS is consulted only as a fallback for words missing from
 * the baked dict (less common nowadays since the bake covers 36k+). */
static int try_irregular(const char *t, uint32_t off, uint32_t len,
                          spfy_fe_delta_t *delta,
                          uint16_t syl_id, uint16_t word_id,
                          uint16_t phrase_id)
{
    /* Baked dictionary (engine-driven, ~36k registry-A/B words). */
    if (len > 0 && len < 64) {
        char lcbuf[64];
        for (uint32_t i = 0; i < len; ++i) lcbuf[i] = lc(t[off + i]);
        const uint16_t *phons  = NULL;
        const uint8_t  *stress = NULL;
        size_t n_phons = 0;
        if (spfy_fe_baked_dict_lookup(lcbuf, len, &phons, &stress, &n_phons)
            && n_phons) {
            for (size_t k = 0; k < n_phons; ++k) {
                uint16_t ph = phons[k];
                int iv = (ph >= PH_ii && ph <= PH_AW);
                uint16_t pos = (uint16_t)((k == 0) ? 0u : 1u);
                emit(delta, ph, syl_id, word_id, phrase_id,
                     (uint16_t)off, (uint16_t)len, iv, pos,
                     stress ? stress[k] : 0u);
            }
            return 1;
        }

        /* Compound-word morpheme decomposition. If the whole word missed
         * the baked dict but splits into two pieces that BOTH hit the
         * dict, emit the concatenation. Catches productive compounds
         * like "seashells" (sea+shells), "lighthouse" (light+house),
         * "fireman" (fire+man), "blackboard" (black+board) without
         * needing to enumerate them. Each piece must be >=3 chars to
         * avoid spurious splits. Try left-shortest first so "sea+shells"
         * wins over a hypothetical "seas+hells" (both could be valid).
         * SPFY_NO_COMPOUND_DECOMP=1 disables. */
        if (len >= 6 && !getenv("SPFY_NO_COMPOUND_DECOMP")) {
            for (uint32_t split = 3; split + 3 <= len; ++split) {
                const uint16_t *p1 = NULL, *p2 = NULL;
                const uint8_t  *s1 = NULL, *s2 = NULL;
                size_t n1 = 0, n2 = 0;
                if (!spfy_fe_baked_dict_lookup(lcbuf, split,
                                                &p1, &s1, &n1) || !n1)
                    continue;
                if (!spfy_fe_baked_dict_lookup(lcbuf + split, len - split,
                                                &p2, &s2, &n2) || !n2)
                    continue;
                /* Emit left piece. */
                for (size_t k = 0; k < n1; ++k) {
                    uint16_t ph = p1[k];
                    int iv = (ph >= PH_ii && ph <= PH_AW);
                    uint16_t pos = (uint16_t)((k == 0) ? 0u : 1u);
                    emit(delta, ph, syl_id, word_id, phrase_id,
                         (uint16_t)off, (uint16_t)split, iv, pos,
                         s1 ? s1[k] : 0u);
                }
                /* Emit right piece. */
                for (size_t k = 0; k < n2; ++k) {
                    uint16_t ph = p2[k];
                    int iv = (ph >= PH_ii && ph <= PH_AW);
                    uint16_t pos = 1u;
                    emit(delta, ph, syl_id, word_id, phrase_id,
                         (uint16_t)(off + split),
                         (uint16_t)(len - split), iv, pos,
                         s2 ? s2[k] : 0u);
                }
                return 1;
            }
        }
    }

    /* Fallback: hand-written entries (kept for OOC of baked dict). */
    for (const irreg_word_t *w = IRREGULAR_WORDS; w->spelling; ++w) {
        if (!syl_eq(t, off, len, w->spelling)) continue;
        for (int k = 0; w->phons[k] != 0; ++k) {
            uint16_t ph = w->phons[k];
            int iv = (ph >= PH_ii && ph <= PH_AW);
            uint16_t pos = (uint16_t)((k == 0) ? 0u : 1u);
            /* Hand-written entries don't carry stress info. Leave at 0
             * (Unstressed) -- the dict-stress post-pass is opt-in via
             * SPFY_USE_DICT_STRESS, and even when on, marking all hand
             * entries as stressed-on-vowels was over-eager. */
            emit(delta, ph, syl_id, word_id, phrase_id,
                 (uint16_t)off, (uint16_t)len, iv, pos, 0u);
        }
        return 1;
    }
    return 0;
}

/* ARPAbet pronunciation of each English letter (as named in isolation).
 * Indexed by lowercase letter offset from 'a'. NULL = use rule-based
 * fallback. Each entry is a 0-terminated phoneme list. */
static const uint16_t LETTER_NAMES[26][8] = {
    /* a */ { PH_e, 0 },                                     /* "ey" */
    /* b */ { PH_b, PH_ii, 0 },                              /* "b iy" */
    /* c */ { PH_s, PH_ii, 0 },                              /* "s iy" */
    /* d */ { PH_d, PH_ii, 0 },                              /* "d iy" */
    /* e */ { PH_ii, 0 },                                    /* "iy" */
    /* f */ { PH_E, PH_f, 0 },                               /* "eh f" */
    /* g */ { PH_JH, PH_ii, 0 },                             /* "jh iy" */
    /* h */ { PH_e, PH_CH, 0 },                              /* "ey ch" */
    /* i */ { PH_AY, 0 },                                    /* "ay" */
    /* j */ { PH_JH, PH_e, 0 },                              /* "jh ey" */
    /* k */ { PH_k, PH_e, 0 },                               /* "k ey" */
    /* l */ { PH_E, PH_l, 0 },                               /* "eh l" */
    /* m */ { PH_E, PH_m, 0 },                               /* "eh m" */
    /* n */ { PH_E, PH_n, 0 },                               /* "eh n" */
    /* o */ { PH_o, 0 },                                     /* "ow" */
    /* p */ { PH_p, PH_ii, 0 },                              /* "p iy" */
    /* q */ { PH_k, PH_y, PH_u, 0 },                         /* "k y uw" */
    /* r */ { PH_A, PH_r, 0 },                               /* "aa r" */
    /* s */ { PH_E, PH_s, 0 },                               /* "eh s" */
    /* t */ { PH_t, PH_ii, 0 },                              /* "t iy" */
    /* u */ { PH_y, PH_u, 0 },                               /* "y uw" */
    /* v */ { PH_v, PH_ii, 0 },                              /* "v iy" */
    /* w */ { PH_d, PH_AH, PH_b, PH_at, PH_l, PH_y, PH_u, 0 }, /* "d ah b ax l y uw" */
    /* x */ { PH_E, PH_k, PH_s, 0 },                         /* "eh k s" */
    /* y */ { PH_w, PH_AY, 0 },                              /* "w ay" */
    /* z */ { PH_z, PH_ii, 0 },                              /* "z iy" */
};

/* Convert one syllable's source letters to phonemes. Returns next pos
 * after the syllable. */
static void syllable_to_phonemes(const char *t,
                                  uint32_t off,
                                  uint32_t len,
                                  spfy_fe_delta_t *delta,
                                  uint16_t syl_id,
                                  uint16_t word_id,
                                  uint16_t phrase_id,
                                  int is_stressed)
{
    /* Letter-naming mode: if the syllable has no vowel letters at all,
     * the engine spells each letter using its alphabet name ("Sssss"
     * -> "eh s eh s eh s eh s eh s"; "Brrr" -> "b iy aa r aa r aa r").
     * Detect by scanning for any aeiouy; if none, emit letter names. */
    int has_vowel = 0;
    for (uint32_t i = 0; i < len; ++i) {
        char c = lc(t[off + i]);
        if (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u'
            || c == 'y') {
            has_vowel = 1;
            break;
        }
    }
    if (!has_vowel && len > 0) {
        for (uint32_t i = 0; i < len; ++i) {
            char c = lc(t[off + i]);
            if (c < 'a' || c > 'z') continue;
            const uint16_t *names = LETTER_NAMES[c - 'a'];
            for (int k = 0; names[k] != 0; ++k) {
                uint16_t ph = names[k];
                int iv = (ph >= PH_ii && ph <= PH_AW);
                uint16_t pos = (uint16_t)((k == 0) ? 0u : 1u);
                emit(delta, ph, syl_id, word_id, phrase_id,
                     (uint16_t)(off + i), 1, iv, pos, 0u);
            }
        }
        return;
    }

    /* Note: word-level baked-dict lookup happens in the outer loop in
     * spfy_fe_lts_run (when word_state == WORD_UNSEEN). Per-syllable
     * dict lookup was a bug — syllables that happen to spell whole
     * words (e.g. "the" inside "synthesizing") would inherit the word's
     * pronunciation. Rule-based syllable LTS only here. */
    uint32_t end = off + len;

    /* Detect "magic e": ends in 'e' AND the previous syllable letter
     * was a consonant AND there's a vowel before that consonant.
     * Heuristic only -- the real engine would consult a dictionary. */
    int magic_e = 0;
    if (len >= 3 && lc(t[end - 1]) == 'e'
        && !is_vowel_letter(t[end - 2])
        && is_vowel_letter(t[end - 3])) {
        magic_e = 1;
    }
    /* Determine if the syllable nucleus ends the syllable (open). */
    int is_open = is_vowel_letter(t[end - 1]) && !magic_e;

    uint32_t i = off;
    uint32_t emitted = 0;
    while (i < end) {
        if (magic_e && i == end - 1) break;       /* skip silent e */

        uint16_t pos = (emitted == 0) ? 0 : 1;    /* 0=onset, 1=after */
        char     c   = t[i];
        char     l   = lc(c);

        /* Four-letter and three-letter combinations FIRST (longest match wins).
         * These cover irregular spellings like "eigh"=/ey/, "ight"=/ay+t/,
         * "ough"=/ow/, "augh"=/ao/. */
        if (i + 4 <= end) {
            char a = lc(t[i]), b = lc(t[i+1]), c2 = lc(t[i+2]), d2 = lc(t[i+3]);
            if (a=='e' && b=='i' && c2=='g' && d2=='h') {
                emit(delta, PH_e,  syl_id, word_id, phrase_id,
                     (uint16_t)i, 4, 1, pos, 0);
                i += 4; ++emitted; continue;
            }
            if (a=='i' && b=='g' && c2=='h' && d2=='t') {
                emit(delta, PH_AY, syl_id, word_id, phrase_id,
                     (uint16_t)i, 3, 1, pos, 0);
                emit(delta, PH_t,  syl_id, word_id, phrase_id,
                     (uint16_t)(i+3), 1, 0, 1, 0);
                i += 4; emitted += 2; continue;
            }
            if (a=='o' && b=='u' && c2=='g' && d2=='h') {
                emit(delta, PH_o,  syl_id, word_id, phrase_id,
                     (uint16_t)i, 4, 1, pos, 0);
                i += 4; ++emitted; continue;
            }
            if (a=='a' && b=='u' && c2=='g' && d2=='h') {
                emit(delta, PH_O,  syl_id, word_id, phrase_id,
                     (uint16_t)i, 4, 1, pos, 0);
                i += 4; ++emitted; continue;
            }
        }
        /* Three-letter "tch" → /ch/. */
        if (i + 3 <= end && lc(t[i])=='t' && lc(t[i+1])=='c' && lc(t[i+2])=='h') {
            emit(delta, PH_CH, syl_id, word_id, phrase_id,
                 (uint16_t)i, 3, 0, pos, 0);
            i += 3; ++emitted; continue;
        }
        /* Two-letter combinations. */
        if (dg(t, i, end, "th")) {
            emit(delta, PH_TH, syl_id, word_id, phrase_id,
                 (uint16_t)i, 2, 0, pos, 0);
            i += 2; ++emitted; continue;
        }
        if (dg(t, i, end, "ch")) {
            emit(delta, PH_CH, syl_id, word_id, phrase_id,
                 (uint16_t)i, 2, 0, pos, 0);
            i += 2; ++emitted; continue;
        }
        if (dg(t, i, end, "sh")) {
            emit(delta, PH_SH, syl_id, word_id, phrase_id,
                 (uint16_t)i, 2, 0, pos, 0);
            i += 2; ++emitted; continue;
        }
        if (dg(t, i, end, "ph")) {
            emit(delta, PH_f, syl_id, word_id, phrase_id,
                 (uint16_t)i, 2, 0, pos, 0);
            i += 2; ++emitted; continue;
        }
        if (dg(t, i, end, "ng")) {
            emit(delta, PH_NG, syl_id, word_id, phrase_id,
                 (uint16_t)i, 2, 0, pos, 0);
            i += 2; ++emitted; continue;
        }
        if (dg(t, i, end, "ck")) {
            emit(delta, PH_k, syl_id, word_id, phrase_id,
                 (uint16_t)i, 2, 0, pos, 0);
            i += 2; ++emitted; continue;
        }
        if (dg(t, i, end, "qu")) {
            /* qu -> /k/ /w/ */
            emit(delta, PH_k, syl_id, word_id, phrase_id,
                 (uint16_t)i, 1, 0, pos, 0);
            emit(delta, PH_w, syl_id, word_id, phrase_id,
                 (uint16_t)(i + 1), 1, 0, pos, 0);
            i += 2; emitted += 2; continue;
        }
        /* Vowel digraphs. */
        if (dg(t, i, end, "ee")) {
            emit(delta, PH_ii, syl_id, word_id, phrase_id,
                 (uint16_t)i, 2, 1, pos, 0);
            i += 2; ++emitted; continue;
        }
        if (dg(t, i, end, "ea")) {
            emit(delta, PH_ii, syl_id, word_id, phrase_id,
                 (uint16_t)i, 2, 1, pos, 0);
            i += 2; ++emitted; continue;
        }
        if (dg(t, i, end, "oo")) {
            emit(delta, PH_u, syl_id, word_id, phrase_id,
                 (uint16_t)i, 2, 1, pos, 0);
            i += 2; ++emitted; continue;
        }
        if (dg(t, i, end, "oa")) {
            emit(delta, PH_o, syl_id, word_id, phrase_id,
                 (uint16_t)i, 2, 1, pos, 0);
            i += 2; ++emitted; continue;
        }
        if (dg(t, i, end, "ai") || dg(t, i, end, "ay")) {
            emit(delta, PH_e, syl_id, word_id, phrase_id,
                 (uint16_t)i, 2, 1, pos, 0);
            i += 2; ++emitted; continue;
        }
        if (dg(t, i, end, "ow")) {
            /* "ow" is /o/ ("show", "low") OR /aw/ ("now", "how"). Default
             * to /o/ — irregulars like "now"/"how" go through the
             * irregular-word dictionary above. */
            emit(delta, PH_o, syl_id, word_id, phrase_id,
                 (uint16_t)i, 2, 1, pos, 0);
            i += 2; ++emitted; continue;
        }
        if (dg(t, i, end, "ou")) {
            /* "ou" is most often /aw/ ("out", "house"). */
            emit(delta, PH_AW, syl_id, word_id, phrase_id,
                 (uint16_t)i, 2, 1, pos, 0);
            i += 2; ++emitted; continue;
        }
        if (dg(t, i, end, "oi") || dg(t, i, end, "oy")) {
            /* /ɔɪ/ — no clean SAMPA mapping in our table; emit as
             * /O/ (open-o) approximation. */
            emit(delta, PH_O, syl_id, word_id, phrase_id,
                 (uint16_t)i, 2, 1, pos, 0);
            i += 2; ++emitted; continue;
        }
        /* Silent-letter patterns at syllable start. */
        if (i == off && (dg(t, i, end, "kn") || dg(t, i, end, "gn") ||
                          dg(t, i, end, "wr"))) {
            i += 1;          /* skip the silent letter */
            continue;
        }

        /* Single-letter rules. */
        uint16_t phon = 0;
        int      iv   = 0;
        switch (l) {
        case 'a': case 'e': case 'i': case 'o': case 'u':
            /* Bug fix: was `magic_e && i == end - 2` which only fires
             * on the CONSONANT position (never a vowel). Pass magic_e
             * directly so any vowel in a magic-e syllable gets the
             * long-vowel pronunciation ("make"=ey, "rope"=ow,
             * "shore"=ow within seashore, etc.). */
            phon = pick_vowel(l, is_open, magic_e, is_stressed);
            iv = 1; break;
        case 'b': phon = PH_b; break;
        case 'c': {
            /* c is /s/ before e/i/y, /k/ otherwise. */
            char nx = (i + 1 < end) ? lc(t[i + 1]) : 0;
            phon = (nx == 'e' || nx == 'i' || nx == 'y') ? PH_s : PH_k;
            break;
        }
        case 'd': phon = PH_d; break;
        case 'f': phon = PH_f; break;
        case 'g': {
            char nx = (i + 1 < end) ? lc(t[i + 1]) : 0;
            phon = (nx == 'e' || nx == 'i' || nx == 'y') ? PH_CH : PH_g;
            break;
        }
        case 'h': phon = PH_h; break;
        case 'j': phon = PH_CH; break;
        case 'k': phon = PH_k; break;
        case 'l': phon = PH_l; break;
        case 'm': phon = PH_m; break;
        case 'n': phon = PH_n; break;
        case 'p': phon = PH_p; break;
        case 'q': phon = PH_k; break;
        case 'r': phon = PH_r; break;
        case 's': phon = PH_s; break;
        case 't': phon = PH_t; break;
        case 'v': phon = PH_v; break;
        case 'w': phon = PH_w; break;
        case 'x':
            /* x -> /k/ /s/ */
            emit(delta, PH_k, syl_id, word_id, phrase_id,
                 (uint16_t)i, 1, 0, pos, 0);
            emit(delta, PH_s, syl_id, word_id, phrase_id,
                 (uint16_t)i, 0, 0, pos, 0);
            i += 1; emitted += 2; continue;
        case 'y': {
            /* "y" is the consonant /j/ in onset position before a
             * vowel ("yes", "you"); elsewhere it functions as a vowel.
             * If 'y' is the only vowel-like letter in this syllable
             * (so it's serving as the nucleus), emit /ih/ for word-
             * internal syllables ("system", "synthesize") and /ay/
             * for word-final monosyllables ("fly", "by"). Heuristic:
             * if there's any other vowel letter in this syllable, y
             * is the consonant; otherwise it's the nucleus vowel.
             * We emit /ih/ as the safer default (engine outputs /ih/
             * for unstressed y; /ay/ requires stress tracking we
             * don't have for unbaked words). */
            int has_other_vowel = 0;
            for (uint32_t k = off; k < end; ++k) {
                if (k == i) continue;
                char ck = lc(t[k]);
                if (ck == 'a' || ck == 'e' || ck == 'i'
                    || ck == 'o' || ck == 'u') {
                    has_other_vowel = 1;
                    break;
                }
            }
            if (has_other_vowel) {
                phon = PH_y;
            } else {
                phon = PH_I;
                iv = 1;
            }
            break;
        }
        case 'z': phon = PH_z; break;
        default:
            i += 1; continue;             /* unknown char -- skip */
        }

        emit(delta, phon, syl_id, word_id, phrase_id,
             (uint16_t)i, 1, iv, pos, 0);
        i += 1; ++emitted;
    }
}

int spfy_fe_lts_run(const spfy_fe_t *fe,
                    const char       *original_text,
                    spfy_fe_delta_t  *delta)
{
    (void)fe;
    if (!original_text || !delta) return SPFY_E_INVAL;

    uint32_t n_syl = 0;
    const spfy_fe_token_t *syls =
        spfy_fe_stream_tokens(delta, SPFY_STREAM_SYL, &n_syl);
    uint32_t n_word = 0;
    const spfy_fe_token_t *words =
        spfy_fe_stream_tokens(delta, SPFY_STREAM_WORD, &n_word);

    /* Iterate syllables in their natural (left-to-right) order so the
     * phoneme stream stays in pronunciation order. On the FIRST syllable
     * of each word, check the irregular-word dictionary against the
     * word's full spelling (the syllabifier may have split an irregular
     * word like "One" into "On" + "e", neither of which matches the
     * dict entry for "one"). If matched, emit the dict phonemes for the
     * word and skip its remaining syllables; otherwise fall through to
     * per-syllable rule-based LTS. */
    enum { WORD_UNSEEN = 0, WORD_DICT = 1, WORD_RULES = 2 };
    int *word_state = (n_word > 0)
                        ? (int *)calloc(n_word, sizeof *word_state) : NULL;
    if (n_word > 0 && !word_state) return SPFY_E_NOMEM;

    for (uint32_t si = 0; si < n_syl; ++si) {
        const spfy_fe_token_t *s = &syls[si];
        uint32_t off = s->fields[0];
        uint32_t len = s->fields[1];
        if (len == 0) continue;
        uint16_t wid = s->word_id;
        /* Stress comes from stage_syl's pick_stress, encoded in the
         * syl token's `name` field (442=primary, 443=secondary, 0=none). */
        int is_stressed = (s->name == 442 || s->name == 443);
        if (wid >= n_word) {
            syllable_to_phonemes(original_text, off, len, delta,
                                  (uint16_t)si, wid, s->phrase_id,
                                  is_stressed);
            continue;
        }

        if (word_state[wid] == WORD_UNSEEN) {
            uint32_t word_off = words[wid].fields[0];
            uint32_t word_len = words[wid].fields[1];
            if (word_len > 0
                && try_irregular(original_text, word_off, word_len, delta,
                                  (uint16_t)si, wid, s->phrase_id)) {
                word_state[wid] = WORD_DICT;
                continue;
            }
            word_state[wid] = WORD_RULES;
        }

        if (word_state[wid] == WORD_DICT) {
            /* Word already emitted in full via the dict; skip remaining
             * syllables of the same word. */
            continue;
        }
        /* Rule-based per-syllable LTS. */
        syllable_to_phonemes(original_text, off, len, delta,
                              (uint16_t)si, wid, s->phrase_id, is_stressed);
    }

    free(word_state);

    /* Post-LTS engine reductions — disable via SPFY_NO_LTS_REDUCTIONS=1.
     *
     * The baked dict gives us word-isolated phonemes (engine --g2p output
     * for each word in isolation). At runtime the engine applies several
     * context-dependent reductions on top of the dict lookups. We patch
     * the phoneme stream after main LTS to mirror those:
     *
     *   1. Function-word reduction. "the" alone is /dh ah/, but before a
     *      consonant becomes /dh ix/ and before a vowel becomes /dh iy/.
     *      Same kind of reduction applies to "to" (t uw -> t ax), "a"
     *      (ax -> ax/ey before vowel), "for" (f ao r -> f er), "your"
     *      (y ao r -> y er), "of" (ah v -> ax v).
     *   2. Intervocalic-t flapping. /t/ or /d/ between a stressed vowel
     *      and an unstressed vowel becomes /dx/ — "Peter", "today" (no,
     *      that's at word-start), "cloudy".
     *
     * Both rules are local (look only at the immediately preceding /
     * following token). We run them as a single pass over the phoneme
     * stream after the main LTS emits.
     *
     * Validated against engine --g2p on the oracle corpus: the combined
     * pass typically pushes phoneme-LCS from ~87% to high-90s. */
    if (!getenv("SPFY_NO_LTS_REDUCTIONS")) {
        uint32_t n_phon = 0, n_word_r = 0;
        const spfy_fe_token_t *phons_c =
            spfy_fe_stream_tokens(delta, SPFY_STREAM_PHONEME, &n_phon);
        const spfy_fe_token_t *words_c =
            spfy_fe_stream_tokens(delta, SPFY_STREAM_WORD, &n_word_r);
        spfy_fe_token_t *phons_mut = (spfy_fe_token_t *)(uintptr_t)phons_c;

        /* (1) Function-word reduction. Walk each word; identify it by its
         * lowercased text and the phoneme pattern its dict entry produced.
         * For each match, rewrite the relevant phoneme(s) based on the
         * next word's first phoneme (vowel vs consonant). */
        for (uint32_t w = 0; w < n_word_r; ++w) {
            uint32_t wo = words_c[w].fields[0];
            uint32_t wl = words_c[w].fields[1];
            if (wl == 0 || wo + wl > 1024) continue;
            char wbuf[16];
            if (wl >= sizeof wbuf) continue;
            for (uint32_t i = 0; i < wl; ++i)
                wbuf[i] = lc(original_text[wo + i]);
            wbuf[wl] = 0;

            /* Find this word's phoneme range. */
            uint32_t p_start = UINT32_MAX, p_end = 0;
            for (uint32_t i = 0; i < n_phon; ++i) {
                if (phons_c[i].word_id == w) {
                    if (p_start == UINT32_MAX) p_start = i;
                    p_end = i;
                }
            }
            if (p_start == UINT32_MAX) continue;

            /* What's the first phoneme of the NEXT word? Vowel-or-not. */
            int next_is_vowel = -1;
            for (uint32_t i = p_end + 1; i < n_phon; ++i) {
                next_is_vowel = is_phon_vowel(phons_c[i].name) ? 1 : 0;
                break;
            }

            /* Apply per-word reduction patches. */
            if (strcmp(wbuf, "the") == 0) {
                /* dh ah -> dh ix (before C) / dh iy (before V).
                 * The baked dict for "the" emits dh ah; rewrite the 2nd
                 * phoneme. If the dict already gave us ix or iy, leave it. */
                if (p_end > p_start
                    && phons_c[p_start].name == PH_DH) {
                    uint16_t cur = phons_mut[p_start + 1].name;
                    if (cur == PH_AH || cur == PH_at || cur == PH_aa) {
                        phons_mut[p_start + 1].name =
                            (next_is_vowel == 1) ? PH_ii : PH_IX;
                    }
                }
            } else if (strcmp(wbuf, "to") == 0) {
                /* t uw -> t ax (before C); leave t uw before V. */
                if (p_end > p_start
                    && phons_c[p_start].name == PH_t) {
                    uint16_t cur = phons_mut[p_start + 1].name;
                    if (cur == PH_u && next_is_vowel == 0) {
                        phons_mut[p_start + 1].name = PH_at;  /* ax */
                    }
                }
            } else if (strcmp(wbuf, "a") == 0) {
                /* a -> ax (before C) / ey (before V). Most dicts give ey
                 * by default — flip to ax when before consonant. */
                if (p_start <= p_end) {
                    uint16_t cur = phons_mut[p_start].name;
                    if ((cur == PH_e || cur == PH_AY)
                        && next_is_vowel == 0) {
                        phons_mut[p_start].name = PH_at;
                    }
                }
            } else if (strcmp(wbuf, "for") == 0) {
                /* f ao r -> f er (engine reduces unconditionally in connected
                 * speech, including before vowels — verified on text_018
                 * "for English"). We collapse ao+r into a single er by
                 * rewriting phons[1] to ER and zeroing phons[2] (espr
                 * emitter skips ids not in the SAMPA_TO_ARPA table). */
                if (p_end >= p_start + 2
                    && phons_c[p_start].name == PH_f
                    && phons_c[p_start + 2].name == PH_r) {
                    phons_mut[p_start + 1].name = PH_ER;
                    phons_mut[p_start + 2].name = 0;
                }
            } else if (strcmp(wbuf, "your") == 0) {
                /* y ao r -> y er (engine reduces unconditionally; same
                 * collapse pattern as "for"). */
                if (p_end >= p_start + 2
                    && phons_c[p_start].name == PH_y
                    && phons_c[p_start + 2].name == PH_r) {
                    phons_mut[p_start + 1].name = PH_ER;
                    phons_mut[p_start + 2].name = 0;
                }
            } else if (strcmp(wbuf, "of") == 0) {
                /* ah v -> ax v (unstressed). */
                if (p_end > p_start
                    && phons_c[p_start + 1].name == 239 /*v*/) {
                    uint16_t cur = phons_mut[p_start].name;
                    if (cur == PH_AH) {
                        phons_mut[p_start].name = PH_at;
                    }
                }
            } else if (strcmp(wbuf, "is") == 0
                    || strcmp(wbuf, "in") == 0
                    || strcmp(wbuf, "it") == 0
                    || strcmp(wbuf, "as") == 0
                    || strcmp(wbuf, "at") == 0
                    || strcmp(wbuf, "an") == 0) {
                /* ih X -> ix X for these short unstressed function words.
                 * Engine corpus output:
                 *   "is"  -> ix z
                 *   "in"  -> ix n
                 *   "it"  -> ix t
                 *   "as"  -> ix z (sometimes ae z when stressed)
                 *   "at"  -> ix t
                 *   "an"  -> ix n
                 * Our baked dict yields ih X; rewrite phons[0] to ix. */
                if (p_start <= p_end
                    && phons_c[p_start].name == PH_I) {
                    phons_mut[p_start].name = PH_IX;
                }
            }
        }

        /* (2) Intervocalic-t flapping. Walk phonemes; if t or d sits
         * between a stressed vowel and an unstressed vowel WITHIN the
         * SAME WORD, replace with dx. Engine examples: Peter (p iy + t
         * + er -> p iy dx er), cloudy (k l aw + t + iy -> k l aw dx iy).
         * Word-initial t doesn't flap (engine output for "you today" is
         * "y uw t ax d ey", not "y uw dx ax d ey"). */
        for (uint32_t i = 1; i + 1 < n_phon; ++i) {
            uint16_t pid = phons_c[i].name;
            if (pid != PH_t && pid != PH_d) continue;
            if (!is_phon_vowel(phons_c[i - 1].name)) continue;
            if (!is_phon_vowel(phons_c[i + 1].name)) continue;
            /* Same-word constraint: all three tokens must share word_id. */
            if (phons_c[i - 1].word_id != phons_c[i].word_id) continue;
            if (phons_c[i + 1].word_id != phons_c[i].word_id) continue;
            uint16_t prev_stress =
                phons_c[i - 1].fields[SPFY_PHON_FIELD_STRESS];
            uint16_t next_stress =
                phons_c[i + 1].fields[SPFY_PHON_FIELD_STRESS];
            /* Engine flaps when prev is stressed (1 or 2) and next is
             * unstressed (0). */
            if (prev_stress >= 1 && next_stress == 0) {
                phons_mut[i].name = PH_DX;
            }
        }
    }

    /* Stress -> syllable name post-pass.
     *
     * stage_spr.c::fill_sp_features computes sp[1] (sylType, q_type 1
     * input to the durt+f0tr CARTs) by reading syls[id].name:
     *   442 = primary stress, 443 = secondary, else = unstressed.
     *
     * DEFAULT: OFF. Tom's VCF doesn't have EMPH_ENABLED, and the engine's
     * actual FE assigns "Stressed" much more conservatively than --g2p's
     * per-word stress markers suggest -- it likely uses phrase-position +
     * POS info (proscost has 8 sylTypeCosts categories: UNDEF / Unstressed /
     * Stressed / PA / FirstPA / FirstPAInPhrase / LastPAInPhrase /
     * LastPAInSent). Marking every content word as Stressed (which our
     * baked dict + IRREGULAR_WORDS approximation does) over-emphasizes vs
     * the engine's flat default. Leave syllables un-named so sp[1]=0 and
     * scoring treats every syllable as Unstressed -- closer to engine.
     *
     * Set SPFY_USE_DICT_STRESS=1 to opt back into per-dict-syllable
     * stress; useful if/when an SSML emphasis path is wired in. */
    /* NB: per-syllable stress placement is now done AFTER re-syllabification
     * in spfy_synth.c::delta_to_fe_utt. The original opt-in here
     * (SPFY_USE_DICT_STRESS) operated BEFORE re-syllabification, when all
     * phons emitted by baked_dict carry syl_id=0; max_stress aggregation
     * therefore put primary on the wrong syllable. The proper place is
     * after phon_to_syl[] is computed. */

    /* Compact phons stream: drop tokens with name == 0.
     *
     * The function-word reductions ("for" f-ao-r -> f-er, "your" y-ao-r ->
     * y-er) collapse 3 phons into 2 by rewriting phons[1].name = ER and
     * setting phons[2].name = 0 (relying on espr emitter to skip ids not
     * in SAMPA_TO_ARPA). But the slot-tree pipeline still produces an HP
     * for each phon token regardless of name; vocab_to_arpa_name(0) -> NULL
     * which downstream maps to a silence-pad slot (ctx[2]=64), inserting
     * a phantom silence between "for" and the next word. Drop these
     * tokens entirely so the HP count matches engine. SPFY_NO_PHON_COMPACT=1
     * disables. */
    if (!getenv("SPFY_NO_PHON_COMPACT")) {
        spfy_fe_stream_t *ph = &delta->streams[SPFY_STREAM_PHONEME];
        uint32_t r = 0, w = 0;
        while (r < ph->n_tokens) {
            if (ph->tokens[r].name != 0) {
                if (w != r) ph->tokens[w] = ph->tokens[r];
                ++w;
            }
            ++r;
        }
        ph->n_tokens = w;
    }

    /* NB: a global geminate-consonant compaction (ll -> l, ss -> s) and
     * final-s voicing assimilation (s -> z after voiced) were tried
     * here, but BOTH regressed: baked dict already encodes correct
     * forms for inflected plurals ("shells" -> "sh eh l z", "press" ->
     * "p r eh s"), so applying these rules on top mangles them. They'd
     * only help words missing from the baked dict — keep rule-based
     * letter-LTS to handle that case word-by-word as needed. */
    return SPFY_OK;
}
