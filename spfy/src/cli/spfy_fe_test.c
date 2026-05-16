/* spfy_fe_test -- sanity-test the FE skeleton.
 *
 * Loads vocabulary + 726 tables, prints stats, and runs a hand-decoded
 * verification of two known tables (t000 = TLDs, t001 = units of
 * measure) so we can be sure the on-disk format + symbol vocabulary
 * are wired up correctly before we start writing actual stages.
 */

#include "../fe/fe.h"
#include "../fe/stage_morph.h"
#include "../fe/stage_syl.h"
#include "../fe/stage_lts.h"
#include "../fe/stage_prosody.h"
#include "../fe/stage_spr.h"

#include <spfy/spfy.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char    buf[256];
    size_t  len;
    const spfy_fe_vocab_t *vocab;
} record_decoder_t;

static int record_cb(const uint8_t *bytes, uint32_t n, uint32_t idx, void *user)
{
    record_decoder_t *d = (record_decoder_t *)user;
    d->len = 0;
    for (uint32_t i = 0; i < n && d->len + 8 < sizeof d->buf; ++i) {
        const char *s = spfy_fe_vocab_name(d->vocab, bytes[i]);
        if (s) {
            size_t sl = strlen(s);
            if (d->len + sl + 1 >= sizeof d->buf) break;
            memcpy(d->buf + d->len, s, sl);
            d->len += sl;
        } else {
            int wrote = snprintf(d->buf + d->len,
                                  sizeof d->buf - d->len,
                                  "<%u>", (unsigned)bytes[i]);
            if (wrote > 0) d->len += (size_t)wrote;
        }
    }
    d->buf[d->len] = 0;
    /* Print first 12 records, then summarise. */
    if (idx < 12) {
        fprintf(stdout, "    [%2u] %s\n", idx, d->buf);
    } else if (idx == 12) {
        fprintf(stdout, "    ... (more)\n");
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr,
            "usage: %s <vocab.json> <tables_a_dir> <tables_b_dir>\n",
            argv[0]);
        return 2;
    }
    spfy_fe_t *fe = NULL;
    int rc = spfy_fe_open(argv[1], argv[2], argv[3], &fe);
    if (rc != SPFY_OK) {
        fprintf(stderr, "spfy_fe_open failed: %s\n", spfy_strerror(rc));
        return 1;
    }
    spfy_fe_print_stats(fe);

    /* Hand-verify t000 (TLDs) and t001 (unit-of-measure words). */
    spfy_fe_t *raw = fe;
    extern int spfy_fe_test_walk_table(spfy_fe_t *fe, int reg, uint32_t idx);
    (void)raw;

    /* Walk t000 (TLD recognition table). */
    fprintf(stdout, "\n=== reg-A t000 (TLD recognition) ===\n");
    record_decoder_t d = {0};
    /* Hack: re-fetch the vocabulary via a tiny accessor; easier is to
     * just open the vocab again. For first iteration we know the
     * fe_t internals enough to wire this through fe.c -- for now
     * we'll just re-load the vocab here. */
    spfy_fe_vocab_t v = {0};
    rc = spfy_fe_vocab_load(argv[1], &v);
    if (rc != SPFY_OK) {
        fprintf(stderr, "vocab reload failed\n");
        spfy_fe_close(fe);
        return 1;
    }
    d.vocab = &v;

    spfy_fe_tables_t t = {0};
    rc = spfy_fe_tables_load(argv[2], argv[3], &t);
    if (rc != SPFY_OK) {
        fprintf(stderr, "tables reload failed\n");
        spfy_fe_vocab_free(&v);
        spfy_fe_close(fe);
        return 1;
    }

    spfy_fe_table_walk(&t.a[0], record_cb, &d);
    fprintf(stdout, "\n=== reg-A t001 (unit-of-measure words) ===\n");
    spfy_fe_table_walk(&t.a[1], record_cb, &d);
    fprintf(stdout, "\n=== reg-A t229 (multi-word phrases) ===\n");
    spfy_fe_table_walk(&t.a[229], record_cb, &d);
    fprintf(stdout, "\n=== reg-A t292 (proper names) ===\n");
    spfy_fe_table_walk(&t.a[292], record_cb, &d);

    spfy_fe_tables_free(&t);
    spfy_fe_vocab_free(&v);

    /* Stage 1 sanity: run text-norm on a short phrase + prosody hint. */
    fprintf(stdout, "\n=== Stage 1 (text-norm) sanity ===\n");
    const char *sample = "Re-running the testing process is unkindness.";
    spfy_prosody_hints_t hints;
    spfy_prosody_hints_init(&hints);
    spfy_prosody_emphasize(&hints, 25, 32, SPFY_EMPH_STRONG);  /* "process" */

    spfy_fe_delta_t delta;
    rc = spfy_fe_textnorm_only(fe, sample, &hints, &delta);
    if (rc != SPFY_OK) {
        fprintf(stderr, "text-norm failed: %s\n", spfy_strerror(rc));
        spfy_prosody_hints_free(&hints);
        spfy_fe_close(fe);
        return 1;
    }
    fprintf(stdout, "input: %s\n", sample);
    uint32_t n_text = 0, n_word = 0, n_phrase = 0, n_token = 0;
    const spfy_fe_token_t *xt =
        spfy_fe_stream_tokens(&delta, SPFY_STREAM_TEXT, &n_text);
    const spfy_fe_token_t *xw =
        spfy_fe_stream_tokens(&delta, SPFY_STREAM_WORD, &n_word);
    const spfy_fe_token_t *xp =
        spfy_fe_stream_tokens(&delta, SPFY_STREAM_PHRASE, &n_phrase);
    const spfy_fe_token_t *xk =
        spfy_fe_stream_tokens(&delta, SPFY_STREAM_TOKEN, &n_token);
    fprintf(stdout, "  %%text=%u tokens, %%word=%u, %%phrase=%u, %%token=%u\n",
            n_text, n_word, n_phrase, n_token);
    fprintf(stdout, "  per-word spans:\n");
    for (uint32_t i = 0; i < n_word; ++i) {
        uint16_t off = xw[i].fields[0];
        uint16_t len = xw[i].fields[1];
        char buf[64];
        size_t copy = len < sizeof buf - 1 ? len : sizeof buf - 1;
        memcpy(buf, sample + off, copy);
        buf[copy] = 0;
        fprintf(stdout, "    [%u] phrase=%u  word=\"%s\"\n",
                xw[i].word_id, xw[i].phrase_id, buf);
    }
    fprintf(stdout, "  per-text emphasis tags (non-zero):\n");
    for (uint32_t i = 0; i < n_text; ++i) {
        uint16_t emph = xt[i].fields[3];
        if (emph) {
            fprintf(stdout, "    byte=%u (\"%c\") emphasis=%u\n",
                    xt[i].fields[2], (char)sample[xt[i].fields[2]],
                    (unsigned)emph);
        }
    }
    fprintf(stdout, "  phrase delimiters: ");
    for (uint32_t i = 0; i < n_phrase; ++i) {
        const char *nm = spfy_fe_vocab_name(spfy_fe_vocab(fe), xp[i].name);
        fprintf(stdout, "[%s%s] ", nm ? nm : "?",
                xp[i].fields[0] ? "*" : "");
    }
    fprintf(stdout, "\n");
    (void)xk;

    /* Stage 2: morphological analysis. */
    fprintf(stdout, "\n=== Stage 2 (morph) sanity ===\n");
    rc = spfy_fe_morph_run(fe, sample, &delta);
    if (rc != SPFY_OK) {
        fprintf(stderr, "morph failed: %s\n", spfy_strerror(rc));
        spfy_fe_delta_free(&delta);
        spfy_prosody_hints_free(&hints);
        spfy_fe_close(fe);
        return 1;
    }
    uint32_t n_morph = 0;
    const spfy_fe_token_t *xm =
        spfy_fe_stream_tokens(&delta, SPFY_STREAM_MORPH, &n_morph);
    fprintf(stdout, "  %%morph=%u tokens\n", n_morph);
    for (uint32_t i = 0; i < n_morph; ++i) {
        const char *kind = spfy_fe_vocab_name(spfy_fe_vocab(fe), xm[i].name);
        uint16_t off = xm[i].fields[0];
        uint16_t len = xm[i].fields[1];
        char buf[64];
        size_t copy = len < sizeof buf - 1 ? len : sizeof buf - 1;
        memcpy(buf, sample + off, copy);
        buf[copy] = 0;
        fprintf(stdout, "    word=%u  kind=%-5s  text=\"%s\"\n",
                xm[i].word_id, kind ? kind : "?", buf);
    }

    /* Stage 3: syllabification + lexical stress. */
    fprintf(stdout, "\n=== Stage 3 (syl + stress) sanity ===\n");
    rc = spfy_fe_syl_run(fe, sample, &delta);
    if (rc != SPFY_OK) {
        fprintf(stderr, "syl failed: %s\n", spfy_strerror(rc));
        spfy_fe_delta_free(&delta);
        spfy_prosody_hints_free(&hints);
        spfy_fe_close(fe);
        return 1;
    }
    uint32_t n_syl = 0;
    const spfy_fe_token_t *xs =
        spfy_fe_stream_tokens(&delta, SPFY_STREAM_SYL, &n_syl);
    fprintf(stdout, "  %%syl=%u tokens\n", n_syl);
    for (uint32_t i = 0; i < n_syl; ++i) {
        const char *stress = spfy_fe_vocab_name(spfy_fe_vocab(fe),
                                                  xs[i].name);
        uint16_t off = xs[i].fields[0];
        uint16_t len = xs[i].fields[1];
        uint16_t pos = xs[i].fields[2];
        uint16_t tot = xs[i].fields[3];
        char buf[32];
        size_t copy = len < sizeof buf - 1 ? len : sizeof buf - 1;
        memcpy(buf, sample + off, copy);
        buf[copy] = 0;
        fprintf(stdout, "    word=%u  syl=%u/%u  text=\"%s\"  stress=%s\n",
                xs[i].word_id, pos + 1, tot, buf,
                stress ? stress : "?");
    }

    /* Stage 4: LTS letter-to-phoneme. */
    fprintf(stdout, "\n=== Stage 4 (LTS) sanity ===\n");
    rc = spfy_fe_lts_run(fe, sample, &delta);
    if (rc != SPFY_OK) {
        fprintf(stderr, "lts failed: %s\n", spfy_strerror(rc));
        spfy_fe_delta_free(&delta);
        spfy_prosody_hints_free(&hints);
        spfy_fe_close(fe);
        return 1;
    }
    uint32_t n_phon = 0;
    const spfy_fe_token_t *xph =
        spfy_fe_stream_tokens(&delta, SPFY_STREAM_PHONEME, &n_phon);
    fprintf(stdout, "  %%phoneme=%u tokens\n", n_phon);
    /* Group phonemes by word for readability. */
    uint16_t cur_word = 0xFFFFu;
    fprintf(stdout, "  ");
    for (uint32_t i = 0; i < n_phon; ++i) {
        if (xph[i].word_id != cur_word) {
            fprintf(stdout, "  ");
            cur_word = xph[i].word_id;
        }
        const char *pn = spfy_fe_vocab_name(spfy_fe_vocab(fe), xph[i].name);
        fprintf(stdout, "%s ", pn ? pn : "?");
    }
    fprintf(stdout, "\n");

    /* Stage 5: Propagate prosody hints up the stream stack. */
    fprintf(stdout, "\n=== Stage 5 (prosody propagation) sanity ===\n");
    rc = spfy_fe_prosody_run(fe, &delta);
    if (rc != SPFY_OK) {
        fprintf(stderr, "prosody-prop failed: %s\n", spfy_strerror(rc));
        spfy_fe_delta_free(&delta);
        spfy_prosody_hints_free(&hints);
        spfy_fe_close(fe);
        return 1;
    }
    /* Show per-word emphasis after propagation. */
    uint32_t n_word2 = 0;
    const spfy_fe_token_t *xw2 =
        spfy_fe_stream_tokens(&delta, SPFY_STREAM_WORD, &n_word2);
    fprintf(stdout, "  per-word emphasis after propagation:\n");
    for (uint32_t i = 0; i < n_word2; ++i) {
        uint16_t emph = xw2[i].fields[5];
        if (emph == 0) continue;
        uint16_t off = xw2[i].fields[0];
        uint16_t len = xw2[i].fields[1];
        char buf[32];
        size_t copy = len < sizeof buf - 1 ? len : sizeof buf - 1;
        memcpy(buf, sample + off, copy);
        buf[copy] = 0;
        fprintf(stdout, "    word=%u  text=\"%s\"  emphasis=%u\n",
                xw2[i].word_id, buf, (unsigned)emph);
    }

    /* Stage 6: SPR formatter -> per-slot ctx[5]/sp[5]. */
    fprintf(stdout, "\n=== Stage 6 (SPR formatter) sanity ===\n");
    spfy_fe_utterance_t utt = {0};
    rc = spfy_fe_spr_run(fe, &delta, &utt);
    if (rc != SPFY_OK) {
        fprintf(stderr, "spr-fmt failed: %s\n", spfy_strerror(rc));
        spfy_fe_delta_free(&delta);
        spfy_prosody_hints_free(&hints);
        spfy_fe_close(fe);
        return 1;
    }
    fprintf(stdout, "  produced %u halfphone slots\n", utt.n_slots);
    fprintf(stdout, "  first 12 slots:\n");
    for (uint32_t i = 0; i < utt.n_slots && i < 12; ++i) {
        const spfy_fe_slot_t *s = &utt.slots[i];
        fprintf(stdout,
            "    [%2u] ctx=[%2d %2d %2d %2d %2d]  sp=[%u %u %u %u %u]  "
            "voiced=%d  emph=%u  pitch=%d  rate=%d\n",
            i, s->ctx[0], s->ctx[1], s->ctx[2], s->ctx[3], s->ctx[4],
            s->sp[0], s->sp[1], s->sp[2], s->sp[3], s->sp[4],
            s->is_voiced, (unsigned)s->emphasis_level,
            (int)s->pitch_offset_st, (int)s->rate_offset_pct);
    }
    free(utt.slots);

    /* Stage 7: SSML parser sanity. */
    fprintf(stdout, "\n=== Stage 7 (SSML parser) sanity ===\n");
    const char *ssml =
        "Hello <emphasis level=\"strong\">world</emphasis>. "
        "Slowly, with <prosody pitch=\"+5st\" rate=\"-20%\">high pitch"
        "</prosody> and a <break time=\"500ms\"/> pause.";
    char *plain = NULL;
    spfy_prosody_hints_t shints;
    rc = spfy_prosody_parse_ssml(ssml, &plain, &shints);
    if (rc != SPFY_OK) {
        fprintf(stderr, "ssml-parse failed: %s\n", spfy_strerror(rc));
    } else {
        fprintf(stdout, "  ssml-stripped plain: \"%s\"\n", plain);
        fprintf(stdout, "  hints extracted (%u):\n", shints.n_hints);
        for (uint32_t i = 0; i < shints.n_hints; ++i) {
            const spfy_prosody_hint_t *h = &shints.hints[i];
            const char *kind = "?";
            char vbuf[64] = {0};
            switch (h->kind) {
            case SPFY_HINT_EMPHASIS:
                kind = "emphasis";
                snprintf(vbuf, sizeof vbuf, "level=%u",
                         (unsigned)h->v.emphasis); break;
            case SPFY_HINT_PITCH:
                kind = "pitch";
                snprintf(vbuf, sizeof vbuf, "%+d semitones",
                         (int)h->v.pitch_st); break;
            case SPFY_HINT_RATE:
                kind = "rate";
                snprintf(vbuf, sizeof vbuf, "%+d%%",
                         (int)h->v.rate_pct); break;
            case SPFY_HINT_BREAK:
                kind = "break";
                snprintf(vbuf, sizeof vbuf, "%u ms", h->v.break_ms); break;
            default: break;
            }
            fprintf(stdout, "    [%u] %-9s bytes=[%u..%u)  %s\n",
                    i, kind, h->byte_start, h->byte_end, vbuf);
        }
        free(plain);
        spfy_prosody_hints_free(&shints);
    }

    spfy_fe_delta_free(&delta);
    spfy_prosody_hints_free(&hints);

    spfy_fe_close(fe);
    return 0;
}
