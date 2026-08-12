/* Stage 3: Syllabification + lexical stress. */

#include "stage_syl.h"
#include "baked_dict.h"
#include "fe.h"
#include "stream.h"

#include <spfy/spfy.h>

#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SYL_PER_WORD 16

/* Vowel + syllabic-consonant SAMPA vocab IDs (from
 * spfy/build/fe_symbol_table.json + stage_espr.c::SAMPA_TO_ARPA). */
static int is_vowel_vocab(uint16_t id)
{
    switch (id) {
    case 255: case 256: case 257: case 258: case 259:
    case 260: case 261: case 263: case 264: case 265:
    case 266: case 271: case 272: case 273: case 274:
    case 276: case 277: case 278:
        return 1;
    default:
        return 0;
    }
}

static int is_vowel(char c, int allow_y)
{
    char lc = (char)tolower((unsigned char)c);
    if (lc == 'a' || lc == 'e' || lc == 'i' || lc == 'o' || lc == 'u') {
        return 1;
    }
    return (allow_y && lc == 'y');
}

static int ends_with_ci(const char *t, uint32_t off, uint32_t len,
                         const char *lit, uint32_t n)
{
    if (n > len) return 0;
    for (uint32_t i = 0; i < n; ++i) {
        char c = (char)tolower((unsigned char)t[off + len - n + i]);
        if (c != lit[i]) return 0;
    }
    return 1;
}

typedef struct {
    uint32_t nucleus_start;
    uint32_t nucleus_end;
    uint32_t syl_start;
    uint32_t syl_end;
} syl_t;

/* Word-level silent-e detection. */
static uint32_t silent_e_trim(const char *t, uint32_t off, uint32_t len)
{
    if (len < 3) return len;
    if ((char)tolower((unsigned char)t[off + len - 1]) != 'e') return len;
    if (is_vowel(t[off + len - 2], 1)) return len;
    /* And there must be at least one earlier vowel in the word (else 'e' is
     * the only vowel and must surface, e.g. */
    for (uint32_t i = 0; i + 1 < len - 1; ++i) {
        if (is_vowel(t[off + i], i > 0)) {
            return len - 1;
        }
    }
    return len;
}

/* Find vowel-cluster nuclei in a word [off..off+len). */
static int find_nuclei(const char *t, uint32_t off, uint32_t len,
                        syl_t *out, int max)
{
    uint32_t scan_len = silent_e_trim(t, off, len);
    int n = 0;
    int in_nuc = 0;
    uint32_t nuc_start = 0;
    for (uint32_t i = 0; i < scan_len && n < max; ++i) {
        int allow_y = (i > 0);
        int v = is_vowel(t[off + i], allow_y);
        if (v && !in_nuc) {
            in_nuc = 1;
            nuc_start = i;
        } else if (!v && in_nuc) {
            out[n].nucleus_start = nuc_start;
            out[n].nucleus_end   = i;
            ++n;
            in_nuc = 0;
        }
    }
    if (in_nuc && n < max) {
        out[n].nucleus_start = nuc_start;
        out[n].nucleus_end   = scan_len;
        ++n;
    }
    return n;
}

/* Two-letter consonant digraphs that act as a single onset unit. */
static int is_consonant_digraph(char a, char b)
{
    a = (char)tolower((unsigned char)a);
    b = (char)tolower((unsigned char)b);
    if (b != 'h' && !(a == 'n' && b == 'g') && !(a == 'c' && b == 'k'))
        return 0;
    if (a == 's' && b == 'h') return 1;
    if (a == 'c' && b == 'h') return 1;
    if (a == 't' && b == 'h') return 1;
    if (a == 'p' && b == 'h') return 1;
    if (a == 'w' && b == 'h') return 1;
    if (a == 'n' && b == 'g') return 1;
    if (a == 'c' && b == 'k') return 1;
    return 0;
}

/* Two-letter onset clusters licensed in English (max-onset principle):
 * stop+liquid, s+stop, s+nasal, s+fricative, fricative+liquid, etc. */
static int is_onset_cluster(char a, char b)
{
    a = (char)tolower((unsigned char)a);
    b = (char)tolower((unsigned char)b);
    if ((a=='p' || a=='b' || a=='t' || a=='d' || a=='k' || a=='c' || a=='g')
        && (b=='l' || b=='r')) {
        if ((a=='t' || a=='d') && b=='l') return 0;
        return 1;
    }
    if ((a=='f' || a=='v') && (b=='l' || b=='r')) return 1;
    /* s + voiceless stop / nasal / liquid / fricative: sp, st, sk/sc, sm,
     * sn, sl, sw */
    if (a=='s' && (b=='p' || b=='t' || b=='k' || b=='c'
                   || b=='m' || b=='n' || b=='l' || b=='w')) return 1;
    if (a=='t' && b=='r') return 1;
    return 0;
}

static void assign_boundaries(const char *t, uint32_t off,
                              syl_t *s, int n, uint32_t word_len)
{
    if (n == 0) return;
    s[0].syl_start = 0;
    for (int i = 0; i < n - 1; ++i) {
        uint32_t between_lo = s[i].nucleus_end;
        uint32_t between_hi = s[i + 1].nucleus_start;
        uint32_t cluster    = between_hi - between_lo;
        uint32_t split;
        if (cluster <= 1) {
            split = between_lo;
        } else if (cluster == 2) {
            /* CVC.CV vs CV.CCV. */
            char a = t[off + between_lo];
            char b = t[off + between_lo + 1];
            if (is_consonant_digraph(a, b) || is_onset_cluster(a, b)) {
                split = between_lo;
            } else {
                split = between_lo + 1;
            }
        } else {
            /* 3+ cluster: place first consonant in coda, the maximal
             * licensed onset to syl 2. */
            char b1 = t[off + between_hi - 2];
            char b2 = t[off + between_hi - 1];
            if (is_consonant_digraph(b1, b2) || is_onset_cluster(b1, b2)) {
                split = between_hi - 2;
            } else {
                split = between_hi - 1;
            }
        }
        s[i].syl_end       = split;
        s[i + 1].syl_start = split;
    }
    s[n - 1].syl_end = word_len;
}

/* Pick stress placement. */
static int pick_stress(const char *t, uint32_t off, uint32_t len,
                        int n_syls)
{
    if (n_syls <= 1) return 0;

    if (ends_with_ci(t, off, len, "tion", 4) ||
        ends_with_ci(t, off, len, "sion", 4) ||
        ends_with_ci(t, off, len, "ical", 4) ||
        ends_with_ci(t, off, len, "ity",  3) ||
        ends_with_ci(t, off, len, "ic",   2)) {
        return n_syls >= 2 ? n_syls - 2 : 0;
    }
    if (ends_with_ci(t, off, len, "ate", 3) && n_syls >= 3) {
        return n_syls - 3;
    }
    if ((ends_with_ci(t, off, len, "ize", 3) ||
         ends_with_ci(t, off, len, "ify", 3)) && n_syls >= 3) {
        return n_syls - 3;
    }
    return 0;
}

int spfy_fe_syl_run(const spfy_fe_t *fe,
                    const char       *original_text,
                    spfy_fe_delta_t  *delta)
{
    (void)fe;
    if (!original_text || !delta) return SPFY_E_INVAL;

    uint32_t n_word = 0;
    const spfy_fe_token_t *words =
        spfy_fe_stream_tokens(delta, SPFY_STREAM_WORD, &n_word);

    syl_t syls[MAX_SYL_PER_WORD];
    for (uint32_t wi = 0; wi < n_word; ++wi) {
        const spfy_fe_token_t *w = &words[wi];
        uint32_t off = w->fields[0];
        uint32_t len = w->fields[1];
        if (len == 0) continue;

        int n = find_nuclei(original_text, off, len,
                             syls, MAX_SYL_PER_WORD);

        /* Phoneme-derived syllable count via baked dict. */
        if (n > 0 && len < 64 && !getenv("SPFY_NO_DICT_SYL_CAP")) {
            char lcbuf[64];
            for (uint32_t i = 0; i < len; ++i) {
                char c = original_text[off + i];
                lcbuf[i] = (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c;
            }
            lcbuf[len] = 0;
            const uint16_t *dict_phons = NULL;
            size_t dict_n = 0;
            if (spfy_fe_baked_dict_lookup(lcbuf, len, &dict_phons,
                                          NULL, &dict_n) && dict_phons) {
                int nv = 0;
                for (size_t i = 0; i < dict_n; ++i) {
                    if (is_vowel_vocab(dict_phons[i])) ++nv;
                }
                if (nv > 0 && nv < n) n = nv;
            }
        }

        if (n == 0) {
            /* Word has no vowels (e.g., "tt") -- treat the whole word as
             * one zero-stress syllable. */
            spfy_fe_token_t st = {0};
            st.name      = SPFY_STRESS_NONE;
            st.word_id   = w->word_id;
            st.phrase_id = w->phrase_id;
            st.fields[0] = (uint16_t)off;
            st.fields[1] = (uint16_t)len;
            st.fields[2] = 0;
            st.fields[3] = 1;
            spfy_fe_stream_push(delta, SPFY_STREAM_SYL, st);
            continue;
        }
        assign_boundaries(original_text, off, syls, n, len);
        /* Pick one stressed syllable per word. */
        int stress_pos = -1;
        if (!getenv("SPFY_NO_PICK_STRESS")) {
            stress_pos = pick_stress(original_text, off, len, n);
        }
        for (int i = 0; i < n; ++i) {
            spfy_fe_token_t st = {0};
            st.name = (i == stress_pos)
                      ? (uint16_t)SPFY_STRESS_PRIMARY
                      : (uint16_t)SPFY_STRESS_NONE;
            st.word_id   = w->word_id;
            st.phrase_id = w->phrase_id;
            st.fields[0] = (uint16_t)(off + syls[i].syl_start);
            st.fields[1] = (uint16_t)(syls[i].syl_end - syls[i].syl_start);
            st.fields[2] = (uint16_t)i;
            st.fields[3] = (uint16_t)n;
            spfy_fe_stream_push(delta, SPFY_STREAM_SYL, st);
        }
    }
    return SPFY_OK;
}
