#ifndef SPFY_FE_FE_H
#define SPFY_FE_FE_H

#include <stddef.h>
#include <stdint.h>

#include "vocab.h"
#include "tables.h"
#include "stream.h"
#include "prosody.h"
#include "phoneset.h"

/* Front-End (FE) module: text -> per-slot scoring inputs. */

typedef struct spfy_fe_s spfy_fe_t;

/* Initialise the FE from on-disk resources. */
int  spfy_fe_open(const char *vocab_json,
                  const char *tables_a_dir,
                  const char *tables_b_dir,
                  spfy_fe_t **out);

/* Same, but selects which embedded SWIttsFe-<lang>.dll image to host.
 * `lang` is a VCF `tts.voiceCfg.language` tag ("en-US", "fr-CA",
 * "es-MX"); NULL or an unbuilt language falls back to the first embedded
 * image with a warning. Which languages are available is a build-time
 * choice -- see SPFY_FE_LANGS in src/fe_host/CMakeLists.txt. */
int  spfy_fe_open_lang(const char *lang,
                       const char *vocab_json,
                       const char *tables_a_dir,
                       const char *tables_b_dir,
                       spfy_fe_t **out);

/* Load voice-specific phoneset from a VCF file. */
int  spfy_fe_set_voice_vcf(spfy_fe_t  *fe,
                            const char *vcf_path);

/* Supply the voice's phone-symbol -> engine-phone-id table, in the VIN's
 * feat["name"] order. */
int  spfy_fe_set_phone_names(spfy_fe_t   *fe,
                             char *const *names,
                             uint32_t     n);

/* Enable the FE's ESPR output mode using this voice's config. */
int  spfy_fe_set_espr_config(spfy_fe_t  *fe,
                             const char *name,
                             const char *gender,
                             const char *phoneset,
                             const char *version);

void spfy_fe_close(spfy_fe_t *fe);

/* Per-slot FE output. */
typedef struct {
    /* Halfphone-class context. */
    int32_t  ctx[5];

    /* Five SP feature row indices used by the per-HP InnerScorer. */
    uint32_t sp[5];

    /* Whether this slot is voiced (drives F0-cost gating downstream). */
    int      is_voiced;

    /* CART target prosody. */
    float    durt_mean, durt_var;
    int      durt_valid;
    float    f0tr_mean, f0tr_var;
    int      f0tr_valid;

    /* Prosody hints attached to this slot (after parser propagation). */
    uint8_t  emphasis_level;
    int8_t   pitch_offset_st;
    int8_t   rate_offset_pct;
} spfy_fe_slot_t;

typedef struct {
    spfy_fe_slot_t *slots;
    uint32_t        n_slots;
    /* Original prosody-hint bundle the caller passed. */
    const spfy_prosody_hints_t *hints;
} spfy_fe_utterance_t;

/* Convert plain text (or SSML, or pre-phonemized SPR) to a per-slot scoring
 * schema. */
int  spfy_fe_synth_text(spfy_fe_t                  *fe,
                        const char                 *text,
                        const spfy_prosody_hints_t *hints,
                        spfy_fe_utterance_t       **out_utt);

/* Bypass the FE DLL entirely: feed a pre-built tagged-output string (same
 * syntax the FE emits naturally - see fe_parse.h / fe_parse.c) directly
 * through the parser + slot-builder. */
int  spfy_fe_synth_tagged(spfy_fe_t                  *fe,
                          const char                 *tagged,
                          const spfy_prosody_hints_t *hints,
                          spfy_fe_utterance_t       **out_utt);

/* Phonemize plain text and return the FE's RAW tagged-output string in
 * `out` (NUL-terminated, truncated to out_n), stopping before the parse +
 * slot-build that spfy_fe_synth_text does. */
int  spfy_fe_text_to_tagged(spfy_fe_t  *fe,
                            const char *text,
                            char       *out,
                            size_t      out_n);

void spfy_fe_utterance_free(spfy_fe_utterance_t *u);


const spfy_fe_vocab_t  *spfy_fe_vocab   (const spfy_fe_t *fe);
const spfy_fe_tables_t *spfy_fe_tables  (const spfy_fe_t *fe);
const spfy_phoneset_t  *spfy_fe_phoneset(const spfy_fe_t *fe);

/* Run only the text-norm stage. */
int  spfy_fe_textnorm_only(const spfy_fe_t            *fe,
                            const char                 *text,
                            const spfy_prosody_hints_t *hints,
                            spfy_fe_delta_t            *delta);


void spfy_fe_print_stats(const spfy_fe_t *fe);


/* Opaque pointer to the hosted FE's last-parsed tagged output (type
 * `fe_parsed_t` from spfy/src/fe_host/fe_parse.h). */
const void *spfy_fe_get_parsed(const spfy_fe_t *fe);

#endif
