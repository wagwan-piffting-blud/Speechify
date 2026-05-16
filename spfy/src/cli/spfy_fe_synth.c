/* spfy_fe_synth -- run the full Path-B FE pipeline on text/SSML input,
 * dump the per-slot output for inspection.
 *
 * This is the M8 capstone for the FE. It exercises every stage of the
 * pipeline (M1..M7) and shows what would be fed to USel.
 *
 * NOTE on actual WAV output: producing engine-compatible audio requires
 * the Tom-voice-specific SAMPA-symbol -> phone-ID mapping, which is a
 * separate RE task (the voice's phone table). The current
 * stage_spr.c uses an approximate placeholder mapping. Once the
 * voice-side mapping is extracted, this CLI can be extended to feed
 * the FE slots into spfy_synth_replay's scoring + Viterbi + WSOLA
 * pipeline to produce WAV directly. For now, it dumps a JSON-shaped
 * report of each pipeline stage so you can verify the FE end-to-end.
 *
 *   spfy_fe_synth <vocab.json> <tables_a_dir> <tables_b_dir> [--ssml] "<text>"
 */

#include "../fe/fe.h"
#include "../fe/stage_morph.h"
#include "../fe/stage_syl.h"
#include "../fe/stage_lts.h"
#include "../fe/stage_prosody.h"
#include "../fe/stage_spr.h"
#include "../fe/stage_espr.h"

#include <spfy/spfy.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s <vocab.json> <tables_a_dir> <tables_b_dir> "
        "[--ssml] [--espr] [--vcf <path>] \"<text>\"\n"
        "  --ssml: input is SSML (parse <emphasis>/<prosody>/<break>)\n"
        "  --espr: emit ESPR text only (for SWIttsSpeakEx); skip JSON dump\n"
        "  --vcf <p>: load voice phoneset from VCF (DLL-free phone_id mapping)\n",
        argv0);
}

int main(int argc, char **argv)
{
    if (argc < 5) { print_usage(argv[0]); return 2; }
    const char *vocab_p   = argv[1];
    const char *tables_a  = argv[2];
    const char *tables_b  = argv[3];
    int         ssml_mode = 0;
    int         espr_only = 0;
    const char *vcf_path  = NULL;
    const char *text      = NULL;
    int         ai        = 4;
    while (ai < argc - 1) {
        if (strcmp(argv[ai], "--ssml") == 0) { ssml_mode = 1; ai++; continue; }
        if (strcmp(argv[ai], "--espr") == 0) { espr_only = 1; ai++; continue; }
        if (strcmp(argv[ai], "--vcf")  == 0 && ai + 1 < argc) {
            vcf_path = argv[ai + 1]; ai += 2; continue;
        }
        break;
    }
    if (ai >= argc) { print_usage(argv[0]); return 2; }
    text = argv[ai];

    spfy_fe_t *fe = NULL;
    int rc = spfy_fe_open(vocab_p, tables_a, tables_b, &fe);
    if (rc != SPFY_OK) {
        fprintf(stderr, "spfy_fe_open failed: %s\n", spfy_strerror(rc));
        return 1;
    }
    if (vcf_path) {
        rc = spfy_fe_set_voice_vcf(fe, vcf_path);
        if (rc != SPFY_OK) {
            fprintf(stderr, "load voice VCF %s failed: %s\n",
                    vcf_path, spfy_strerror(rc));
            spfy_fe_close(fe);
            return 1;
        }
        const spfy_phoneset_t *ps = spfy_fe_phoneset(fe);
        fprintf(stderr, "loaded voice phoneset: %u phonemes (silence pid=%u)\n",
                ps->n_phones, (unsigned)ps->silence_phone_id);
    }

    /* Step 1: optionally parse SSML to (plain text, hints). */
    char *plain = NULL;
    spfy_prosody_hints_t hints;
    if (ssml_mode) {
        rc = spfy_prosody_parse_ssml(text, &plain, &hints);
        if (rc != SPFY_OK) {
            fprintf(stderr, "ssml parse failed: %s\n", spfy_strerror(rc));
            spfy_fe_close(fe);
            return 1;
        }
        text = plain;
    } else {
        spfy_prosody_hints_init(&hints);
    }

    /* Step 2: run all FE stages. */
    spfy_fe_delta_t delta;
    rc = spfy_fe_textnorm_only(fe, text, &hints, &delta);
    if (rc == SPFY_OK) rc = spfy_fe_morph_run   (fe, text, &delta);
    if (rc == SPFY_OK) rc = spfy_fe_syl_run     (fe, text, &delta);
    if (rc == SPFY_OK) rc = spfy_fe_lts_run     (fe, text, &delta);
    if (rc == SPFY_OK) rc = spfy_fe_prosody_run (fe, &delta);
    if (rc != SPFY_OK) {
        fprintf(stderr, "FE pipeline failed: %s\n", spfy_strerror(rc));
        goto fail;
    }

    /* Step 3a: ESPR text emission (Path B integration with engine). */
    if (espr_only) {
        char espr_buf[8192];
        size_t espr_n = 0;
        rc = spfy_fe_espr_emit(fe, text, &delta, espr_buf,
                                sizeof espr_buf, &espr_n);
        if (rc != SPFY_OK) {
            fprintf(stderr, "ESPR emit failed: %s\n", spfy_strerror(rc));
            goto fail_delta;
        }
        fwrite(espr_buf, 1, espr_n, stdout);
        fputc('\n', stdout);
        rc = SPFY_OK;
        goto fail_delta;        /* normal path; cleanup */
    }

    /* Step 3b: build the per-slot utterance struct. */
    spfy_fe_utterance_t utt = {0};
    rc = spfy_fe_spr_run(fe, &delta, &utt);
    if (rc != SPFY_OK) {
        fprintf(stderr, "SPR formatter failed: %s\n", spfy_strerror(rc));
        goto fail_delta;
    }

    /* Step 4: report. */
    fprintf(stdout, "{\n");
    fprintf(stdout, "  \"input\": \"%s\",\n", text);
    fprintf(stdout, "  \"ssml\": %s,\n", ssml_mode ? "true" : "false");
    fprintf(stdout, "  \"hints\": [");
    for (uint32_t i = 0; i < hints.n_hints; ++i) {
        fprintf(stdout, "%s\n    {\"kind\":%u,\"start\":%u,\"end\":%u}",
                i ? "," : "",
                (unsigned)hints.hints[i].kind,
                hints.hints[i].byte_start, hints.hints[i].byte_end);
    }
    fprintf(stdout, "%s],\n", hints.n_hints ? "\n  " : "");

    /* Per-stream counts. */
    uint32_t n_text = 0, n_word = 0, n_syl = 0, n_phon = 0, n_phrase = 0;
    spfy_fe_stream_tokens(&delta, SPFY_STREAM_TEXT,    &n_text);
    spfy_fe_stream_tokens(&delta, SPFY_STREAM_WORD,    &n_word);
    spfy_fe_stream_tokens(&delta, SPFY_STREAM_SYL,     &n_syl);
    spfy_fe_stream_tokens(&delta, SPFY_STREAM_PHONEME, &n_phon);
    spfy_fe_stream_tokens(&delta, SPFY_STREAM_PHRASE,  &n_phrase);
    fprintf(stdout, "  \"streams\": {\n"
                    "    \"text\": %u, \"word\": %u, \"syl\": %u,\n"
                    "    \"phoneme\": %u, \"phrase\": %u\n"
                    "  },\n",
            n_text, n_word, n_syl, n_phon, n_phrase);

    /* Phoneme summary by word. */
    fprintf(stdout, "  \"phonemes_by_word\": [\n");
    const spfy_fe_token_t *xph =
        spfy_fe_stream_tokens(&delta, SPFY_STREAM_PHONEME, &n_phon);
    const spfy_fe_token_t *xw =
        spfy_fe_stream_tokens(&delta, SPFY_STREAM_WORD, &n_word);
    for (uint32_t wi = 0; wi < n_word; ++wi) {
        uint16_t off = xw[wi].fields[0];
        uint16_t len = xw[wi].fields[1];
        char wbuf[64];
        size_t cp = len < sizeof wbuf - 1 ? len : sizeof wbuf - 1;
        memcpy(wbuf, text + off, cp); wbuf[cp] = 0;
        fprintf(stdout, "    {\"word\":\"%s\",\"phones\":\"", wbuf);
        for (uint32_t i = 0; i < n_phon; ++i) {
            if (xph[i].word_id != wi) continue;
            const char *pn = spfy_fe_vocab_name(spfy_fe_vocab(fe),
                                                  xph[i].name);
            fprintf(stdout, "%s ", pn ? pn : "?");
        }
        fprintf(stdout, "\",\"emphasis\":%u}%s\n",
                (unsigned)xw[wi].fields[5],
                wi + 1 < n_word ? "," : "");
    }
    fprintf(stdout, "  ],\n");

    /* Slot summary. */
    fprintf(stdout, "  \"slots\": {\"count\": %u, \"first\": [\n",
            utt.n_slots);
    uint32_t show = utt.n_slots;   /* dump every slot for full-coverage diff */
    for (uint32_t i = 0; i < show; ++i) {
        const spfy_fe_slot_t *s = &utt.slots[i];
        fprintf(stdout,
            "    {\"i\":%u,\"ctx\":[%d,%d,%d,%d,%d],"
            "\"sp\":[%u,%u,%u,%u,%u],\"voiced\":%d,"
            "\"emph\":%u,\"pitch_st\":%d,\"rate_pct\":%d}%s\n",
            i, s->ctx[0], s->ctx[1], s->ctx[2], s->ctx[3], s->ctx[4],
            s->sp[0], s->sp[1], s->sp[2], s->sp[3], s->sp[4],
            s->is_voiced, (unsigned)s->emphasis_level,
            (int)s->pitch_offset_st, (int)s->rate_offset_pct,
            i + 1 < show ? "," : "");
    }
    fprintf(stdout, "  ]}\n");
    fprintf(stdout, "}\n");

    free(utt.slots);
fail_delta:
    spfy_fe_delta_free(&delta);
fail:
    spfy_prosody_hints_free(&hints);
    free(plain);
    spfy_fe_close(fe);
    return rc == SPFY_OK ? 0 : 1;
}
