#ifndef SPFY_FE_PHONESET_H
#define SPFY_FE_PHONESET_H

#include <stddef.h>
#include <stdint.h>

#include "../voice/voice.h"

/* Voice phoneset: ARPAbet name <-> phone_id. */

#define SPFY_PHONESET_MAX        128
#define SPFY_PHONESET_NAME_MAX   8

typedef struct {
    char     name[SPFY_PHONESET_NAME_MAX];
    uint8_t  phone_id;
    uint8_t  is_voiced;
    uint8_t  is_vowel;
    uint8_t  pad;
} spfy_phone_entry_t;

typedef struct {
    spfy_phone_entry_t entries[SPFY_PHONESET_MAX];
    uint32_t           n_phones;
    uint8_t            silence_phone_id;
} spfy_phoneset_t;

/* Parse `tts.voiceCfg.phones` from already-decrypted VCF XML and populate
 * `out`. */
int  spfy_phoneset_load_from_vcf(const spfy_vcf_t *vcf,
                                  spfy_phoneset_t  *out);

void spfy_phoneset_free(spfy_phoneset_t *ps);

uint8_t spfy_phoneset_lookup(const spfy_phoneset_t *ps, const char *name);

const char *spfy_phoneset_name_of(const spfy_phoneset_t *ps, uint8_t phone_id);

#endif
