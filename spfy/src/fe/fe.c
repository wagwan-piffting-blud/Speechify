/* spfy_fe top-level orchestrator (Path B skeleton).
 *
 * Stages currently land here as TODO stubs that will be filled in over
 * the next milestones:
 *
 *   F2.1  Text normalisation (numbers, abbreviations, units, TLDs)
 *   F2.2  Tokenisation + word-boundary detection
 *   F2.3  Morphological analysis (using rootdict / worddict)
 *   F2.4  Lexical stress prediction (using stress tables)
 *   F2.5  Letter-to-phoneme rules (using registry-B LTS tables)
 *   F2.6  Syllabification + intonation
 *   F2.7  Halfphone slot generation (per-slot ctx[5] + sp[5])
 *   F2.8  Prosody-hint propagation -> emphasis_level / pitch / rate
 *   F2.9  SPR escape-code emission for engine downstream prosody
 *
 * For now the public API loads resources, validates them, and returns
 * an empty utterance. This lets us wire spfy_synth_replay against the
 * FE iteratively as stages come online.
 */

#include "fe.h"
#include "stage_textnorm.h"

#include <spfy/spfy.h>

#include <stdlib.h>
#include <string.h>

struct spfy_fe_s {
    spfy_fe_vocab_t  vocab;
    spfy_fe_tables_t tables;
    spfy_vcf_t       voice_vcf;            /* zeroed = no voice loaded */
    spfy_phoneset_t  phoneset;             /* n_phones=0 = no phoneset */
    int              voice_loaded;
};

int spfy_fe_open(const char *vocab_json,
                  const char *tables_a_dir,
                  const char *tables_b_dir,
                  spfy_fe_t **out)
{
    if (!vocab_json || !tables_a_dir || !tables_b_dir || !out) {
        return SPFY_E_INVAL;
    }
    spfy_fe_t *fe = (spfy_fe_t *)calloc(1, sizeof *fe);
    if (!fe) return SPFY_E_NOMEM;

    int rc = spfy_fe_vocab_load(vocab_json, &fe->vocab);
    if (rc != SPFY_OK) goto fail;
    rc = spfy_fe_tables_load(tables_a_dir, tables_b_dir, &fe->tables);
    if (rc != SPFY_OK) goto fail;

    *out = fe;
    return SPFY_OK;

fail:
    spfy_fe_close(fe);
    return rc;
}

void spfy_fe_close(spfy_fe_t *fe)
{
    if (!fe) return;
    spfy_fe_vocab_free(&fe->vocab);
    spfy_fe_tables_free(&fe->tables);
    if (fe->voice_loaded) {
        spfy_vcf_free(&fe->voice_vcf);
        spfy_phoneset_free(&fe->phoneset);
    }
    free(fe);
}

int spfy_fe_set_voice_vcf(spfy_fe_t *fe, const char *vcf_path)
{
    if (!fe || !vcf_path) return SPFY_E_INVAL;
    if (fe->voice_loaded) {
        spfy_vcf_free(&fe->voice_vcf);
        spfy_phoneset_free(&fe->phoneset);
        fe->voice_loaded = 0;
    }
    int rc = spfy_vcf_load(vcf_path, &fe->voice_vcf);
    if (rc != SPFY_OK) return rc;
    rc = spfy_phoneset_load_from_vcf(&fe->voice_vcf, &fe->phoneset);
    if (rc != SPFY_OK) {
        spfy_vcf_free(&fe->voice_vcf);
        return rc;
    }
    fe->voice_loaded = 1;
    return SPFY_OK;
}

int spfy_fe_synth_text(spfy_fe_t                  *fe,
                        const char                 *text,
                        const spfy_prosody_hints_t *hints,
                        spfy_fe_utterance_t       **out_utt)
{
    (void)fe; (void)text; (void)hints;       /* stages land progressively */
    if (!out_utt) return SPFY_E_INVAL;

    spfy_fe_utterance_t *u = (spfy_fe_utterance_t *)
        calloc(1, sizeof *u);
    if (!u) return SPFY_E_NOMEM;
    u->hints = hints;       /* borrowed */
    *out_utt = u;
    return SPFY_OK;
}

/* Internal accessors (private to the fe library; consumed by stages
 * that need vocab/tables but shouldn't see the full struct). */
const spfy_fe_vocab_t *spfy_fe_vocab(const spfy_fe_t *fe)
{
    return fe ? &fe->vocab : NULL;
}

const spfy_fe_tables_t *spfy_fe_tables(const spfy_fe_t *fe)
{
    return fe ? &fe->tables : NULL;
}

const spfy_phoneset_t *spfy_fe_phoneset(const spfy_fe_t *fe)
{
    if (!fe || !fe->voice_loaded) return NULL;
    return &fe->phoneset;
}

int spfy_fe_textnorm_only(const spfy_fe_t            *fe,
                           const char                 *text,
                           const spfy_prosody_hints_t *hints,
                           spfy_fe_delta_t            *delta)
{
    if (!fe || !text || !delta) return SPFY_E_INVAL;
    spfy_fe_delta_init(delta);
    return spfy_fe_textnorm_run(fe, text, hints, delta);
}

void spfy_fe_utterance_free(spfy_fe_utterance_t *u)
{
    if (!u) return;
    free(u->slots);
    free(u);
}

void spfy_fe_print_stats(const spfy_fe_t *fe)
{
    if (!fe) return;
    fprintf(stdout, "spfy_fe stats:\n");
    fprintf(stdout, "  vocab entries: %u (expect %u)\n",
            fe->vocab.n, (unsigned)SPFY_FE_VOCAB_N);
    /* Spot-check a few canonical IDs. */
    const char *spot[] = {
        spfy_fe_vocab_name(&fe->vocab, 0),
        spfy_fe_vocab_name(&fe->vocab, 1),
        spfy_fe_vocab_name(&fe->vocab, 26),
        spfy_fe_vocab_name(&fe->vocab, SPFY_SYM_WORDDICT),
        spfy_fe_vocab_name(&fe->vocab, SPFY_SYM_HUGEDICT),
    };
    fprintf(stdout, "  spot[0]=%s  [1]=%s  [26]=%s  [worddict=%u]=%s  [hugedict=%u]=%s\n",
            spot[0] ? spot[0] : "(null)",
            spot[1] ? spot[1] : "(null)",
            spot[2] ? spot[2] : "(null)",
            (unsigned)SPFY_SYM_WORDDICT, spot[3] ? spot[3] : "(null)",
            (unsigned)SPFY_SYM_HUGEDICT, spot[4] ? spot[4] : "(null)");
    fprintf(stdout, "  registry-A: %u tables (%zu bytes total via arena)\n",
            (unsigned)SPFY_FE_REG_A_N, (size_t)fe->tables.arena_size);
    fprintf(stdout, "  registry-B: %u tables\n", (unsigned)SPFY_FE_REG_B_N);

    /* Show sizes of a few interesting tables for sanity. */
    fprintf(stdout, "  reg-A t000 (TLDs):              size=%u\n",
            fe->tables.a[0].size);
    fprintf(stdout, "  reg-A t001 (units of measure):  size=%u\n",
            fe->tables.a[1].size);
    fprintf(stdout, "  reg-A t229 (multi-word phrases):size=%u\n",
            fe->tables.a[229].size);
    fprintf(stdout, "  reg-A t292 (proper names):      size=%u\n",
            fe->tables.a[292].size);
    fprintf(stdout, "  reg-B t000:                     size=%u\n",
            fe->tables.b[0].size);
}
