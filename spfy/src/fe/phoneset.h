#ifndef SPFY_FE_PHONESET_H
#define SPFY_FE_PHONESET_H

#include <stddef.h>
#include <stdint.h>

#include "../voice/voice.h"

/* Voice phoneset: ARPAbet name <-> phone_id.
 *
 * Each Speechify voice declares its own phoneset in the VCF as
 * `tts.voiceCfg.phones` -- an ORDERED list of <namedValue> entries
 * where the position IS the phone_id (Tom: 0..45, 46 phonemes).
 *
 * The existing spfy_vcf_load doesn't capture <namedValue> children
 * (it only records flat <param><value>), but it DOES keep the
 * decrypted XML at vcf->xml_bytes. We re-scan that.
 *
 * Tom's list (46 entries):
 *   0=aa, 1=ae, 2=ah, 3=ao, 4=aw, 5=ax, 6=ay, 7=b, 8=ch, 9=dx,
 *  10=d, 11=dh, 12=eh, 13=el, 14=er, 15=en, 16=ey, 17=f, 18=g,
 *  19=hh, 20=ih, 21=ix, 22=iy, 23=jh, 24=k, 25=l, 26=m, 27=n,
 *  28=ng, 29=ow, 30=oy, 31=p, 32=pau, 33=r, 34=s, 35=sh, 36=t,
 *  37=th, 38=uh, 39=uw, 40=v, 41=w, 42=xx, 43=y, 44=z, 45=zh
 *
 * Per-phoneme features (parsed from the namedValue body) include
 * `voiced` and `vowel` markers. These directly drive the
 * `is_voiced` flag in spfy_fe_slot_t and the F0-cost gating in our
 * existing C scoring stack.
 */

#define SPFY_PHONESET_MAX        128
#define SPFY_PHONESET_NAME_MAX   8

typedef struct {
    char     name[SPFY_PHONESET_NAME_MAX];   /* NUL-terminated */
    uint8_t  phone_id;                       /* position == ID */
    uint8_t  is_voiced;
    uint8_t  is_vowel;
    uint8_t  pad;
} spfy_phone_entry_t;

typedef struct {
    spfy_phone_entry_t entries[SPFY_PHONESET_MAX];
    uint32_t           n_phones;
    uint8_t            silence_phone_id;     /* position of "pau" */
} spfy_phoneset_t;

/* Parse `tts.voiceCfg.phones` from already-decrypted VCF XML and
 * populate `out`. Returns SPFY_OK or SPFY_E_*. */
int  spfy_phoneset_load_from_vcf(const spfy_vcf_t *vcf,
                                  spfy_phoneset_t  *out);

void spfy_phoneset_free(spfy_phoneset_t *ps);

/* Lookup name -> phone_id. Returns 0xFF if not found. */
uint8_t spfy_phoneset_lookup(const spfy_phoneset_t *ps, const char *name);

/* Lookup phone_id -> name. Returns NULL if id is OOR. */
const char *spfy_phoneset_name_of(const spfy_phoneset_t *ps, uint8_t phone_id);

#endif
