/* Stage 2: Morphological analysis.
 *
 * Decomposes each word in the %word stream into prefix + root + suffix
 * morphemes via a small hand-curated affix list. Emits to %morph.
 */

#include "stage_morph.h"
#include "fe.h"
#include "stream.h"
#include "vocab.h"

#include <spfy/spfy.h>

#include <ctype.h>
#include <stddef.h>
#include <string.h>

/* Common English prefixes -- ORDERED LONGEST FIRST so longest match
 * wins with a simple linear scan. Each entry: prefix string, length. */
static const struct { const char *p; uint8_t n; } PREFIXES[] = {
    {"super", 5}, {"under", 5}, {"trans", 5},
    {"over",  4}, {"anti",  4}, {"semi",  4}, {"para", 4},
    {"fore",  4}, {"sub",   3}, {"non",   3}, {"out",  3},
    {"pre",   3}, {"mis",   3}, {"dis",   3}, {"con",  3},
    {"com",   3}, {"un",    2}, {"re",    2}, {"in",   2},
    {"im",    2}, {"en",    2}, {"em",    2}, {"de",   2},
    {"ex",    2},
    {NULL,    0}
};

/* Common English suffixes (longest first). */
static const struct { const char *s; uint8_t n; } SUFFIXES[] = {
    {"ation",  5}, {"ition", 5},
    {"ment",   4}, {"ness",  4}, {"tion",  4}, {"sion", 4},
    {"able",   4}, {"ible",  4}, {"less",  4}, {"ship", 4},
    {"hood",   4}, {"like",  4}, {"some",  4}, {"ward", 4},
    {"ize",    3}, {"ise",   3}, {"ate",   3}, {"ify",  3},
    {"fy",     2}, {"ity",   3}, {"ous",   3}, {"ful",  3},
    {"ish",    3}, {"ing",   3}, {"est",   3}, {"ery",  3},
    {"er",     2}, {"ed",    2}, {"al",    2}, {"ic",   2},
    {"ly",     2}, {"en",    2}, {"or",    2},
    {"y",      1}, {"s",     1},
    {NULL,     0}
};

#define MIN_ROOT_LEN 3

/* Lower-case a single ASCII byte. */
static char lc(char c) { return (char)tolower((unsigned char)c); }

/* True if input[off..off+n) matches lit (case-insensitive ASCII). */
static int match_ci(const char *input, uint32_t off, uint32_t avail,
                     const char *lit, uint8_t n)
{
    if ((uint32_t)n > avail) return 0;
    for (uint8_t i = 0; i < n; ++i) {
        if (lc(input[off + i]) != lit[i]) return 0;
    }
    return 1;
}

/* True if input[off+n..end) ends with suffix lit (case-insensitive). */
static int match_suffix_ci(const char *input, uint32_t off, uint32_t len,
                            const char *lit, uint8_t n)
{
    if ((uint32_t)n > len) return 0;
    return match_ci(input, off + len - n, n, lit, n);
}

int spfy_fe_morph_run(const spfy_fe_t *fe,
                      const char       *original_text,
                      spfy_fe_delta_t  *delta)
{
    if (!fe || !original_text || !delta) return SPFY_E_INVAL;

    uint32_t n_word = 0;
    const spfy_fe_token_t *words =
        spfy_fe_stream_tokens(delta, SPFY_STREAM_WORD, &n_word);

    for (uint32_t wi = 0; wi < n_word; ++wi) {
        const spfy_fe_token_t *w = &words[wi];
        uint32_t off = w->fields[0];
        uint32_t len = w->fields[1];
        if (len == 0) continue;

        /* Default: whole word is root, no affixes. */
        uint32_t pre_len = 0;
        uint32_t suf_len = 0;

        /* Look for a prefix. */
        if (len >= MIN_ROOT_LEN + 2) {
            for (int i = 0; PREFIXES[i].p != NULL; ++i) {
                uint8_t pn = PREFIXES[i].n;
                if (len < (uint32_t)pn + MIN_ROOT_LEN) continue;
                if (match_ci(original_text, off, len, PREFIXES[i].p, pn)) {
                    pre_len = pn;
                    break;
                }
            }
        }

        /* Look for a suffix in the part AFTER any prefix. */
        uint32_t root_off = off + pre_len;
        uint32_t root_len = len - pre_len;
        if (root_len >= MIN_ROOT_LEN + 1) {
            for (int i = 0; SUFFIXES[i].s != NULL; ++i) {
                uint8_t sn = SUFFIXES[i].n;
                if (root_len < (uint32_t)sn + MIN_ROOT_LEN) continue;
                if (match_suffix_ci(original_text, root_off, root_len,
                                     SUFFIXES[i].s, sn)) {
                    suf_len = sn;
                    break;
                }
            }
        }
        root_len -= suf_len;

        /* Push prefix morpheme if any. */
        if (pre_len > 0) {
            spfy_fe_token_t mt = {0};
            mt.name      = SPFY_MORPH_PRE;
            mt.word_id   = w->word_id;
            mt.phrase_id = w->phrase_id;
            mt.fields[0] = (uint16_t)off;
            mt.fields[1] = (uint16_t)pre_len;
            spfy_fe_stream_push(delta, SPFY_STREAM_MORPH, mt);
        }
        /* Root morpheme. */
        {
            spfy_fe_token_t mt = {0};
            mt.name      = SPFY_MORPH_ROOT;
            mt.word_id   = w->word_id;
            mt.phrase_id = w->phrase_id;
            mt.fields[0] = (uint16_t)root_off;
            mt.fields[1] = (uint16_t)root_len;
            spfy_fe_stream_push(delta, SPFY_STREAM_MORPH, mt);
        }
        /* Suffix morpheme if any. */
        if (suf_len > 0) {
            spfy_fe_token_t mt = {0};
            mt.name      = SPFY_MORPH_SUF;
            mt.word_id   = w->word_id;
            mt.phrase_id = w->phrase_id;
            mt.fields[0] = (uint16_t)(root_off + root_len);
            mt.fields[1] = (uint16_t)suf_len;
            spfy_fe_stream_push(delta, SPFY_STREAM_MORPH, mt);
        }
    }
    return SPFY_OK;
}
