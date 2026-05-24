/*
 * spfy/src/fe_host/fe_stub.c — C Native-flavored spfy_fe_t shim.
 *
 * Same opaque-handle surface as fe_host.c, but with the SWIttsFe-en-US.dll
 * hosting machinery stripped out. ARM CPUs can't decode 32-bit x86 PE, so
 * on ARM/other devices we drive the pipeline through the in-house pure-C FE
 * (src/fe_internal/) instead.
 *
 * Concretely:
 *   - spfy_fe_open() allocates the opaque struct (no DLL, no PE loader).
 *   - spfy_fe_set_voice_vcf() loads the phoneset from the voice VCF.
 *   - spfy_fe_synth_text(text) routes text through
 *       spfy_fe_internal_text_to_tagged → spfy_fe_synth_tagged.
 *   - spfy_fe_synth_tagged(tagged) parses tagged input → spfy_fe_slot_t[]
 *       via fe_parse + apply_phoneme_refinement + fe_parsed_to_full_slots.
 *   - spfy_fe_get_parsed() exposes the parsed fe_parsed_t to spfy_synth.c
 *       so its multi-utterance loop has the per-word / per-phoneme view.
 */

#include "fe.h"
#include "fe_parse.h"
#include "phoneset.h"
#include "../voice/voice.h"
#include "../fe_internal/fe_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct spfy_fe_s {
    spfy_phoneset_t phoneset;
    int             phoneset_loaded;
    fe_parsed_t     last_parsed;
    int             last_parsed_valid;
} native_fe_t;

int spfy_fe_open(const char *vocab_json,
                 const char *tables_a_dir,
                 const char *tables_b_dir,
                 spfy_fe_t **out) {
    (void)vocab_json; (void)tables_a_dir; (void)tables_b_dir;
    if (!out) return -1;
    native_fe_t *fe = (native_fe_t *)calloc(1, sizeof *fe);
    if (!fe) return -1;
    *out = (spfy_fe_t *)fe;
    return 0;
}

void spfy_fe_close(spfy_fe_t *opaque) {
    if (!opaque) return;
    native_fe_t *fe = (native_fe_t *)opaque;
    if (fe->last_parsed_valid) fe_parsed_free(&fe->last_parsed);
    free(fe);
}

int spfy_fe_set_voice_vcf(spfy_fe_t *opaque, const char *vcf_path) {
    if (!opaque || !vcf_path) return -1;
    native_fe_t *fe = (native_fe_t *)opaque;
    spfy_vcf_t vcf;
    int rc = spfy_vcf_load(vcf_path, &vcf);
    if (rc != 0) return rc;
    memset(&fe->phoneset, 0, sizeof fe->phoneset);
    rc = spfy_phoneset_load_from_vcf(&vcf, &fe->phoneset);
    spfy_vcf_free(&vcf);
    if (rc != 0) return rc;
    fe->phoneset_loaded = 1;
    return 0;
}

static int parse_into_slots(native_fe_t *fe,
                            const char *tagged,
                            spfy_fe_utterance_t *u) {
    if (fe->last_parsed_valid) {
        fe_parsed_free(&fe->last_parsed);
        fe->last_parsed_valid = 0;
    }
    if (fe_parse_tagged_output(tagged, &fe->last_parsed) != 0) {
        u->slots = NULL; u->n_slots = 0;
        return -1;
    }
    fe->last_parsed_valid = 1;
    const spfy_phoneset_t *ps = fe->phoneset_loaded ? &fe->phoneset : NULL;
    if (ps) {
        spfy_fe_slot_t *slots = NULL;
        uint32_t n_slots = 0;
        int rc = fe_parsed_to_full_slots(&fe->last_parsed, ps,
                                         &slots, &n_slots);
        if (rc != 0) return rc;
        u->slots = slots; u->n_slots = n_slots;
    } else {
        int n = fe_parsed_count_phonemes(&fe->last_parsed);
        if (n > 0) {
            u->slots = (spfy_fe_slot_t *)calloc((size_t)n, sizeof *u->slots);
            if (!u->slots) return -1;
            fe_parsed_flatten_to_slots(&fe->last_parsed, u->slots, n);
            u->n_slots = (uint32_t)n;
        }
    }
    return 0;
}

int spfy_fe_synth_tagged(spfy_fe_t *opaque,
                         const char *tagged,
                         const spfy_prosody_hints_t *hints,
                         spfy_fe_utterance_t **out_utt) {
    if (!opaque || !tagged || !out_utt) return -1;
    native_fe_t *fe = (native_fe_t *)opaque;
    *out_utt = NULL;
    spfy_fe_utterance_t *u = (spfy_fe_utterance_t *)calloc(1, sizeof *u);
    if (!u) return -3;
    u->hints = hints;
    if (parse_into_slots(fe, tagged, u) != 0) { free(u); return -1; }
    *out_utt = u;
    return 0;
}

int spfy_fe_synth_text(spfy_fe_t *opaque,
                       const char *text,
                       const spfy_prosody_hints_t *hints,
                       spfy_fe_utterance_t **out_utt) {
    if (!opaque || !text || !out_utt) return -1;
    static char tagged_buf[65536];
    int n = spfy_fe_internal_text_to_tagged(text, tagged_buf, sizeof tagged_buf);
    if (n < 0) return -1;
    return spfy_fe_synth_tagged(opaque, tagged_buf, hints, out_utt);
}

void spfy_fe_utterance_free(spfy_fe_utterance_t *u) {
    if (!u) return;
    free(u->slots);
    free(u);
}

const void *spfy_fe_get_parsed(const spfy_fe_t *opaque) {
    if (!opaque) return NULL;
    const native_fe_t *fe = (const native_fe_t *)opaque;
    return fe->last_parsed_valid ? (const void *)&fe->last_parsed : NULL;
}

const spfy_phoneset_t *spfy_fe_phoneset(const spfy_fe_t *opaque) {
    if (!opaque) return NULL;
    const native_fe_t *fe = (const native_fe_t *)opaque;
    return fe->phoneset_loaded ? &fe->phoneset : NULL;
}

const spfy_fe_vocab_t  *spfy_fe_vocab (const spfy_fe_t *fe) { (void)fe; return NULL; }
const spfy_fe_tables_t *spfy_fe_tables(const spfy_fe_t *fe) { (void)fe; return NULL; }

int spfy_fe_textnorm_only(const spfy_fe_t *fe, const char *text,
                          const spfy_prosody_hints_t *hints,
                          spfy_fe_delta_t *delta) {
    (void)fe; (void)text; (void)hints; (void)delta;
    return -1;
}

void spfy_fe_print_stats(const spfy_fe_t *fe) { (void)fe; }
