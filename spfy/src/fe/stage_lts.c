/* Stage 4: Letter-to-phoneme rules (basic English LTS). */

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

/* Phoneme symbol IDs in the 469-vocabulary. */
enum {
    PH_b = 229, PH_p = 230, PH_d = 231, PH_t = 232,
    PH_k = 235, PH_g = 236,
    PH_TH = 238,
    PH_DH = 237,
    PH_v = 239, PH_f = 240, PH_z = 241, PH_s = 242,
    PH_ZH = 243, PH_SH = 244,
    PH_JH = 245,
    PH_CH = 246,
    PH_h = 247,
    PH_m = 248, PH_n = 249, PH_NG = 250,
    PH_r = 251, PH_l = 252,
    PH_y = 253, PH_w = 254,
    PH_ii = 255,
    PH_I  = 256,
    PH_e  = 257,
    PH_E  = 258,
    PH_A  = 259,
    PH_aa = 271,
    PH_u  = 272,
    PH_U  = 273,
    PH_o  = 274,
    PH_O  = 278,
    PH_at = 266,
    PH_AH = 260,
    PH_AY = 276,
    PH_AW = 277,
    PH_IX = 261,
    PH_ER = 264,
    PH_DX = 233,
};

/* ARPAbet vowel set: vocab IDs for aa, ae, ah, ao, aw, ax, ay, eh, er, ey,
 * ih, ix, iy, ow, oy, uh, uw plus the syllabic en/el. */
static int is_phon_vowel(uint16_t pid)
{
    switch (pid) {
        case PH_A:
        case PH_aa:
        case PH_AH:
        case PH_O:
        case PH_AW:
        case PH_at:
        case PH_AY:
        case PH_E:
        case PH_ER:
        case PH_e:
        case PH_I:
        case PH_IX:
        case PH_ii:
        case PH_o:
        case 275:
        case PH_U:
        case PH_u:
            return 1;
        default:
            return 0;
    }
}

/* ARPAbet voiced phoneme set. */
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
 * carried from the baked dictionary). */
#define SPFY_PHON_FIELD_STRESS 4

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

static int dg(const char *t, uint32_t i, uint32_t end,
               const char *lit)
{
    if (i + 2 > end) return 0;
    return lc(t[i]) == lit[0] && lc(t[i + 1]) == lit[1];
}

/* Pick vowel phoneme. */
static uint16_t pick_vowel(char letter, int is_open, int has_e,
                            int is_stressed)
{
    char v = lc(letter);
    /* Unstressed open 'e' reduces to /ix/ ("synthe-sizing" syl "the" -> /th
     * ix/, not /th iy/). */
    if (!is_stressed && (has_e || is_open) && v == 'e') {
        return PH_IX;
    }
    if (has_e) {
        switch (v) {
        case 'a': return PH_e;
        case 'e': return PH_ii;
        case 'i': return PH_AY;
        case 'o': return PH_o;
        case 'u': return PH_u;
        }
    }
    if (is_open) {
        switch (v) {
        case 'a': return PH_e;
        case 'e': return PH_ii;
        case 'i': return PH_AY;
        case 'o': return PH_o;
        case 'u': return PH_u;
        }
    }
    /* Closed syllable. */
    switch (v) {
    case 'a': return PH_aa;
    case 'e': return PH_E;
    case 'i': return PH_I;
    case 'o': return PH_A;         /* "stop"=/stɑp/ - engine uses ARPAbet "aa" not "ao" for American /ɑ/. */
    case 'u': return PH_U;
    }
    return PH_at;
}

static int syl_eq(const char *t, uint32_t off, uint32_t len, const char *lit)
{
    size_t n = strlen(lit);
    if (len != n) return 0;
    for (size_t i = 0; i < n; ++i) {
        if (lc(t[off + i]) != lit[i]) return 0;
    }
    return 1;
}

/* High-frequency irregular-word dictionary. */
typedef struct {
    const char *spelling;
    uint16_t    phons[16];
} irreg_word_t;

static const irreg_word_t IRREGULAR_WORDS[] = {
    { "i",       { PH_AY, 0 } },
    { "a",       { PH_e,  0 } },
    { "o",       { PH_o,  0 } },
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
    { "the",     { PH_d,  PH_IX,          0 } },
    { "of",      { PH_at, PH_v,           0 } },
    { "hello",   { PH_h,  PH_E, PH_l, PH_o, 0 } },
    { "world",   { PH_w,  PH_ER, PH_l, PH_d, 0 } },
    { "her",     { PH_h,  PH_ER,          0 } },
    { "fox",     { PH_f,  PH_aa, PH_k, PH_s, 0 } },
    { "dog",     { PH_d,  PH_A, PH_g,     0 } },
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
    { "is",      { PH_IX, PH_z,           0 } },
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
    { "stop",    { PH_s,  PH_t, PH_A, PH_p, 0 } },
    { "hi",      { PH_h,  PH_AY,          0 } },
    { "aye",     { PH_AY,                 0 } },
    { "why",     { PH_w,  PH_AY,          0 } },
    { "mmm",     { PH_E, PH_m, PH_E, PH_m, PH_E, PH_m, 0 } },
    { NULL, { 0 } },
};

/* Look up a word in the dictionary; emit its phonemes and return 1 on
 * match, 0 otherwise. */
static int try_irregular(const char *t, uint32_t off, uint32_t len,
                          spfy_fe_delta_t *delta,
                          uint16_t syl_id, uint16_t word_id,
                          uint16_t phrase_id)
{
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

        /* Compound-word morpheme decomposition. */
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
                for (size_t k = 0; k < n1; ++k) {
                    uint16_t ph = p1[k];
                    int iv = (ph >= PH_ii && ph <= PH_AW);
                    uint16_t pos = (uint16_t)((k == 0) ? 0u : 1u);
                    emit(delta, ph, syl_id, word_id, phrase_id,
                         (uint16_t)off, (uint16_t)split, iv, pos,
                         s1 ? s1[k] : 0u);
                }
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

    for (const irreg_word_t *w = IRREGULAR_WORDS; w->spelling; ++w) {
        if (!syl_eq(t, off, len, w->spelling)) continue;
        for (int k = 0; w->phons[k] != 0; ++k) {
            uint16_t ph = w->phons[k];
            int iv = (ph >= PH_ii && ph <= PH_AW);
            uint16_t pos = (uint16_t)((k == 0) ? 0u : 1u);
            /* Hand-written entries don't carry stress info. */
            emit(delta, ph, syl_id, word_id, phrase_id,
                 (uint16_t)off, (uint16_t)len, iv, pos, 0u);
        }
        return 1;
    }
    return 0;
}

/* ARPAbet pronunciation of each English letter (as named in isolation). */
static const uint16_t LETTER_NAMES[26][8] = {
     { PH_e, 0 },
     { PH_b, PH_ii, 0 },
     { PH_s, PH_ii, 0 },
     { PH_d, PH_ii, 0 },
     { PH_ii, 0 },
     { PH_E, PH_f, 0 },
     { PH_JH, PH_ii, 0 },
     { PH_e, PH_CH, 0 },
     { PH_AY, 0 },
     { PH_JH, PH_e, 0 },
     { PH_k, PH_e, 0 },
     { PH_E, PH_l, 0 },
     { PH_E, PH_m, 0 },
     { PH_E, PH_n, 0 },
     { PH_o, 0 },
     { PH_p, PH_ii, 0 },
     { PH_k, PH_y, PH_u, 0 },
     { PH_A, PH_r, 0 },
     { PH_E, PH_s, 0 },
     { PH_t, PH_ii, 0 },
     { PH_y, PH_u, 0 },
     { PH_v, PH_ii, 0 },
     { PH_d, PH_AH, PH_b, PH_at, PH_l, PH_y, PH_u, 0 },
     { PH_E, PH_k, PH_s, 0 },
     { PH_w, PH_AY, 0 },
     { PH_z, PH_ii, 0 },
};

/* Convert one syllable's source letters to phonemes. */
static void syllable_to_phonemes(const char *t,
                                  uint32_t off,
                                  uint32_t len,
                                  spfy_fe_delta_t *delta,
                                  uint16_t syl_id,
                                  uint16_t word_id,
                                  uint16_t phrase_id,
                                  int is_stressed)
{
    /* Letter-naming mode: if the syllable has no vowel letters at all, the
     * engine spells each letter using its alphabet name ("Sssss" -> "eh s
     * eh s eh s eh s eh s"; "Brrr" -> "b iy aa r aa r aa r"). */
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
     * spfy_fe_lts_run (when word_state == WORD_UNSEEN). */
    uint32_t end = off + len;

    /* Detect "magic e": ends in 'e' AND the previous syllable letter was a
     * consonant AND there's a vowel before that consonant. */
    int magic_e = 0;
    if (len >= 3 && lc(t[end - 1]) == 'e'
        && !is_vowel_letter(t[end - 2])
        && is_vowel_letter(t[end - 3])) {
        magic_e = 1;
    }
    int is_open = is_vowel_letter(t[end - 1]) && !magic_e;

    uint32_t i = off;
    uint32_t emitted = 0;
    while (i < end) {
        if (magic_e && i == end - 1) break;

        uint16_t pos = (emitted == 0) ? 0 : 1;
        char     c   = t[i];
        char     l   = lc(c);

        /* Four-letter and three-letter combinations FIRST (longest match wins). */
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
        if (i + 3 <= end && lc(t[i])=='t' && lc(t[i+1])=='c' && lc(t[i+2])=='h') {
            emit(delta, PH_CH, syl_id, word_id, phrase_id,
                 (uint16_t)i, 3, 0, pos, 0);
            i += 3; ++emitted; continue;
        }
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
            emit(delta, PH_k, syl_id, word_id, phrase_id,
                 (uint16_t)i, 1, 0, pos, 0);
            emit(delta, PH_w, syl_id, word_id, phrase_id,
                 (uint16_t)(i + 1), 1, 0, pos, 0);
            i += 2; emitted += 2; continue;
        }
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
            /* "ow" is /o/ ("show", "low") OR /aw/ ("now", "how"). */
            emit(delta, PH_o, syl_id, word_id, phrase_id,
                 (uint16_t)i, 2, 1, pos, 0);
            i += 2; ++emitted; continue;
        }
        if (dg(t, i, end, "ou")) {
            emit(delta, PH_AW, syl_id, word_id, phrase_id,
                 (uint16_t)i, 2, 1, pos, 0);
            i += 2; ++emitted; continue;
        }
        if (dg(t, i, end, "oi") || dg(t, i, end, "oy")) {
            /* /ɔɪ/ - no clean SAMPA mapping in our table; emit as /O/
             * (open-o) approximation. */
            emit(delta, PH_O, syl_id, word_id, phrase_id,
                 (uint16_t)i, 2, 1, pos, 0);
            i += 2; ++emitted; continue;
        }
        if (i == off && (dg(t, i, end, "kn") || dg(t, i, end, "gn") ||
                          dg(t, i, end, "wr"))) {
            i += 1;
            continue;
        }

        uint16_t phon = 0;
        int      iv   = 0;
        switch (l) {
        case 'a': case 'e': case 'i': case 'o': case 'u':
            /* Bug fix: was `magic_e && i == end - 2` which only fires on
             * the CONSONANT position (never a vowel). */
            phon = pick_vowel(l, is_open, magic_e, is_stressed);
            iv = 1; break;
        case 'b': phon = PH_b; break;
        case 'c': {
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
            emit(delta, PH_k, syl_id, word_id, phrase_id,
                 (uint16_t)i, 1, 0, pos, 0);
            emit(delta, PH_s, syl_id, word_id, phrase_id,
                 (uint16_t)i, 0, 0, pos, 0);
            i += 1; emitted += 2; continue;
        case 'y': {
            /* "y" is the consonant /j/ in onset position before a vowel
             * ("yes", "you"); elsewhere it functions as a vowel. */
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
            i += 1; continue;
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
     * phoneme stream stays in pronunciation order. */
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
        /* Stress comes from stage_syl's pick_stress, encoded in the syl
         * token's `name` field (442=primary, 443=secondary, 0=none). */
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
        syllable_to_phonemes(original_text, off, len, delta,
                              (uint16_t)si, wid, s->phrase_id, is_stressed);
    }

    free(word_state);

    /* Post-LTS engine reductions - disable via SPFY_NO_LTS_REDUCTIONS=1. */
    if (!getenv("SPFY_NO_LTS_REDUCTIONS")) {
        uint32_t n_phon = 0, n_word_r = 0;
        const spfy_fe_token_t *phons_c =
            spfy_fe_stream_tokens(delta, SPFY_STREAM_PHONEME, &n_phon);
        const spfy_fe_token_t *words_c =
            spfy_fe_stream_tokens(delta, SPFY_STREAM_WORD, &n_word_r);
        spfy_fe_token_t *phons_mut = (spfy_fe_token_t *)(uintptr_t)phons_c;

        /* (1) Function-word reduction. */
        for (uint32_t w = 0; w < n_word_r; ++w) {
            uint32_t wo = words_c[w].fields[0];
            uint32_t wl = words_c[w].fields[1];
            if (wl == 0 || wo + wl > 1024) continue;
            char wbuf[16];
            if (wl >= sizeof wbuf) continue;
            for (uint32_t i = 0; i < wl; ++i)
                wbuf[i] = lc(original_text[wo + i]);
            wbuf[wl] = 0;

            uint32_t p_start = UINT32_MAX, p_end = 0;
            for (uint32_t i = 0; i < n_phon; ++i) {
                if (phons_c[i].word_id == w) {
                    if (p_start == UINT32_MAX) p_start = i;
                    p_end = i;
                }
            }
            if (p_start == UINT32_MAX) continue;

            int next_is_vowel = -1;
            for (uint32_t i = p_end + 1; i < n_phon; ++i) {
                next_is_vowel = is_phon_vowel(phons_c[i].name) ? 1 : 0;
                break;
            }

            if (strcmp(wbuf, "the") == 0) {
                /* dh ah -> dh ix (before C) / dh iy (before V). */
                if (p_end > p_start
                    && phons_c[p_start].name == PH_DH) {
                    uint16_t cur = phons_mut[p_start + 1].name;
                    if (cur == PH_AH || cur == PH_at || cur == PH_aa) {
                        phons_mut[p_start + 1].name =
                            (next_is_vowel == 1) ? PH_ii : PH_IX;
                    }
                }
            } else if (strcmp(wbuf, "to") == 0) {
                if (p_end > p_start
                    && phons_c[p_start].name == PH_t) {
                    uint16_t cur = phons_mut[p_start + 1].name;
                    if (cur == PH_u && next_is_vowel == 0) {
                        phons_mut[p_start + 1].name = PH_at;
                    }
                }
            } else if (strcmp(wbuf, "a") == 0) {
                /* a -> ax (before C) / ey (before V). */
                if (p_start <= p_end) {
                    uint16_t cur = phons_mut[p_start].name;
                    if ((cur == PH_e || cur == PH_AY)
                        && next_is_vowel == 0) {
                        phons_mut[p_start].name = PH_at;
                    }
                }
            } else if (strcmp(wbuf, "for") == 0) {
                /* f ao r -> f er (engine reduces unconditionally in
                 * connected speech, including before vowels - verified on
                 * text_018 "for English"). */
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
                if (p_end > p_start
                    && phons_c[p_start + 1].name == 239 ) {
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
                /* ih X -> ix X for these short unstressed function words. */
                if (p_start <= p_end
                    && phons_c[p_start].name == PH_I) {
                    phons_mut[p_start].name = PH_IX;
                }
            }
        }

        /* (2) Intervocalic-t flapping. */
        for (uint32_t i = 1; i + 1 < n_phon; ++i) {
            uint16_t pid = phons_c[i].name;
            if (pid != PH_t && pid != PH_d) continue;
            if (!is_phon_vowel(phons_c[i - 1].name)) continue;
            if (!is_phon_vowel(phons_c[i + 1].name)) continue;
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

    /* Stress -> syllable name post-pass. */
    /* NB: per-syllable stress placement is now done AFTER
     * re-syllabification in spfy_synth.c::delta_to_fe_utt. */

    /* Compact phons stream: drop tokens with name == 0. */
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
     * final-s voicing assimilation (s -> z after voiced) were tried here,
     * but BOTH regressed: baked dict already encodes correct forms for
     * inflected plurals... */
    return SPFY_OK;
}
