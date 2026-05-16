/* Stage 6 (alt): ESPR text emitter.
 *
 * Walks %word + %syl + %phoneme streams and produces a valid ESPR
 * document for the original Speechify engine.
 */

#include "stage_espr.h"
#include "stage_prosody.h"
#include "fe.h"
#include "stream.h"
#include "vocab.h"

#include <spfy/spfy.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* SAMPA-vocab-id -> ARPAbet 2-letter name. Captures the phoneme set
 * the engine accepts (verified via Frida hook on parsePhoneme; see
 * spfy/build/fe_phoneset.json for the full inventory).
 *
 * Vocab IDs come from spfy/build/fe_symbol_table.json. The 1-char
 * SAMPA names our LTS emits map to the ARPAbet equivalents as listed
 * in project_fe_f0_eloquence.md. Phonemes our LTS doesn't currently
 * produce (oy, ax, dx, ix, en, jh, etc) are added for completeness so
 * future LTS revisions can use them by symbol-vocab ID. */
typedef struct {
    uint16_t vocab_id;
    const char *arpabet;
} sampa_to_arpa_t;

static const sampa_to_arpa_t SAMPA_TO_ARPA[] = {
    /* Vowels */
    { 271, "ae" },   /* a -> ae (cat) */
    { 259, "aa" },   /* A -> aa (father) */
    { 258, "eh" },   /* E -> eh (set) */
    { 257, "ey" },   /* e -> ey (say) */
    { 255, "iy" },   /* i -> iy (see) */
    { 256, "ih" },   /* I -> ih (sit) */
    { 274, "ow" },   /* o -> ow (go) */
    { 278, "ao" },   /* O -> ao (thought) */
    { 272, "uw" },   /* u -> uw (boot) */
    { 273, "uh" },   /* U -> uh (put) */
    { 266, "ax" },   /* @ -> ax (schwa) */
    { 276, "ay" },   /* Y -> ay (boy/by) */
    { 277, "aw" },   /* W -> aw (now) */
    { 260, "ah" },   /* X -> ah (cup) */
    { 264, "er" },   /* R -> er (bird) */
    { 265, "ah" },   /* L (syllabic) -> ah */
    { 261, "ix" },   /* reduced /ɪ/ — engine emits for "the X" before C */
    /* Consonants */
    { 233, "dx" },   /* flap-t (intervocalic t/d) */
    { 263, "en" },   /* syllabic n */
    { 229, "b"  },
    { 230, "p"  },
    { 231, "d"  },
    { 232, "t"  },
    { 235, "k"  },
    { 236, "g"  },
    { 240, "f"  },
    { 239, "v"  },
    { 238, "th" },   /* T -> th */
    { 237, "dh" },   /* D -> dh */
    { 242, "s"  },
    { 241, "z"  },
    { 244, "sh" },   /* S -> sh */
    { 243, "zh" },   /* Z -> zh */
    { 246, "ch" },   /* C -> ch */
    { 245, "jh" },   /* J -> jh */
    { 247, "hh" },   /* h -> hh */
    { 248, "m"  },
    { 249, "n"  },
    { 250, "ng" },   /* G -> ng */
    { 251, "r"  },
    { 252, "l"  },
    { 253, "y"  },
    { 254, "w"  },
    { 0,    NULL }
};

static const char *arpabet_for_id(uint16_t vocab_id)
{
    for (int i = 0; SAMPA_TO_ARPA[i].arpabet; ++i) {
        if (SAMPA_TO_ARPA[i].vocab_id == vocab_id)
            return SAMPA_TO_ARPA[i].arpabet;
    }
    return NULL;
}

/* Buffered writer with overflow guard. */
typedef struct {
    char  *buf;
    size_t cap;
    size_t pos;
    int    overflowed;
} writer_t;

static void w_init(writer_t *w, char *buf, size_t cap)
{
    w->buf = buf; w->cap = cap; w->pos = 0; w->overflowed = 0;
    if (cap > 0) buf[0] = 0;
}

static void w_putn(writer_t *w, const char *s, size_t n)
{
    if (w->overflowed) return;
    if (w->pos + n + 1 >= w->cap) { w->overflowed = 1; return; }
    memcpy(w->buf + w->pos, s, n);
    w->pos += n;
    w->buf[w->pos] = 0;
}

static void w_puts(writer_t *w, const char *s)
{
    w_putn(w, s, strlen(s));
}

static void w_printf(writer_t *w, const char *fmt, ...)
{
    if (w->overflowed) return;
    va_list ap;
    va_start(ap, fmt);
    size_t avail = w->cap > w->pos ? w->cap - w->pos : 0;
    int wrote = vsnprintf(w->buf + w->pos, avail, fmt, ap);
    va_end(ap);
    if (wrote < 0 || (size_t)wrote >= avail) {
        w->overflowed = 1;
    } else {
        w->pos += (size_t)wrote;
    }
}

/* Emit the normalized text of a word (lowercase, ASCII letters only). */
static void emit_norm_text(writer_t *w, const char *text,
                            uint16_t off, uint16_t len)
{
    char tmp[128];
    size_t n = (len < sizeof tmp - 1) ? len : sizeof tmp - 1;
    for (size_t i = 0; i < n; ++i) {
        char c = text[off + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        tmp[i] = c;
    }
    tmp[n] = 0;
    w_puts(w, tmp);
}

/* Emit a single word's ESPR record: <NormText [phonemes]>
 *
 * For the first iteration we omit PhraseType and GramCat. The
 * minimal `<text [phonemes]>` form is what we test first. If the
 * engine's parser rejects, we'll add stubs in iteration 2. */
static void emit_word(writer_t *w, const spfy_fe_token_t *word,
                       const char *text,
                       const spfy_fe_token_t *syls, uint32_t n_syls,
                       const spfy_fe_token_t *phons, uint32_t n_phons)
{
    w_puts(w, "<");
    emit_norm_text(w, text, word->fields[0], word->fields[1]);
    w_puts(w, " ");

    /* Optional VolumeRate from prosody hints (rate/volume). */
    int rate_pct = (int16_t)word->fields[SPFY_PROSODY_FIELD_RATE_PCT];
    int pitch_st = (int16_t)word->fields[SPFY_PROSODY_FIELD_PITCH_ST];
    if (rate_pct != 0) {
        /* `(rN)` -- rate adjustment percent. Engine multiplies. */
        int rate_value = 100 + rate_pct;
        if (rate_value < 10) rate_value = 10;
        w_printf(w, "(r%d) ", rate_value);
    }
    (void)pitch_st;        /* pitch is per-phoneme via F0 in [...] */

    /* SylPhones: walk syllables of this word, emitting their phonemes
     * grouped by syllable boundary markers. */
    w_puts(w, "[");
    int first_syl = 1;
    for (uint16_t si = 0; si < n_syls; ++si) {
        if (syls[si].word_id != word->word_id) continue;
        if (!first_syl) w_puts(w, " ");
        first_syl = 0;

        uint16_t syl_pos = syls[si].fields[2];
        uint16_t syl_total = syls[si].fields[3];
        /* Syllable marker: .<index>.  We use 1-based stress (1 = primary,
         * 0 = none) -- engine pattern is `.1,...;...` with the int as
         * stress level by some conventions. */
        int stress_level = (syls[si].name == 442) ? 1 : 0;   /* 442=str */
        w_printf(w, ".%d ", stress_level);
        (void)syl_pos; (void)syl_total;

        /* Emit each phoneme in this syllable. */
        for (uint32_t pi = 0; pi < n_phons; ++pi) {
            if (phons[pi].syl_id != si) continue;
            const char *arpa = arpabet_for_id(phons[pi].name);
            if (!arpa) continue;
            /* Phoneme parameter `p` defaults to 1.0 (relative duration).
             * Future: scale by emphasis_level or rate hints. */
            float p = 1.0f;
            uint16_t emph = phons[pi].fields[SPFY_PROSODY_FIELD_EMPHASIS];
            if (emph >= 3)      p = 1.20f;     /* strong: stretch 20% */
            else if (emph == 2) p = 1.10f;
            w_printf(w, "%s(p%.2f) ", arpa, p);
        }
    }
    w_puts(w, "]>");
}

int spfy_fe_espr_emit(const spfy_fe_t *fe,
                      const char       *original_text,
                      spfy_fe_delta_t  *delta,
                      char             *buf,
                      size_t            buf_n,
                      size_t           *out_len)
{
    (void)fe;
    if (!original_text || !delta || !buf || buf_n == 0)
        return SPFY_E_INVAL;

    writer_t w;
    w_init(&w, buf, buf_n);

    uint32_t n_word = 0, n_syl = 0, n_phon = 0;
    const spfy_fe_token_t *xw  = spfy_fe_stream_tokens(delta, SPFY_STREAM_WORD,    &n_word);
    const spfy_fe_token_t *xs  = spfy_fe_stream_tokens(delta, SPFY_STREAM_SYL,     &n_syl);
    const spfy_fe_token_t *xph = spfy_fe_stream_tokens(delta, SPFY_STREAM_PHONEME, &n_phon);

    /* ESPR document opener. */
    w_puts(&w, "\\!SWIespr0 %% { ");

    /* Walk words ordered by phrase, emitting phrase boundaries. */
    uint16_t cur_phrase = (n_word > 0) ? xw[0].phrase_id : 0;
    int wrote_phrase_open = 0;
    for (uint32_t i = 0; i < n_word; ++i) {
        if (i > 0 && xw[i].phrase_id != cur_phrase) {
            /* End current phrase, start new one with a short pause. */
            w_puts(&w, " pau(p0.20) ");
            cur_phrase = xw[i].phrase_id;
        }
        if (!wrote_phrase_open) {
            w_puts(&w, "# ");
            wrote_phrase_open = 1;
        } else if (i > 0) {
            w_puts(&w, " ");
        }
        emit_word(&w, &xw[i], original_text, xs, n_syl, xph, n_phon);
    }

    /* Document closer. */
    w_puts(&w, " } %% \\!SWIespr1");

    if (w.overflowed) return SPFY_E_OOB;
    if (out_len) *out_len = w.pos;
    return SPFY_OK;
}
