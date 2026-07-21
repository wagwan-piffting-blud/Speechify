#ifndef SPFY_VOICE_PHONE_ORDER_H
#define SPFY_VOICE_PHONE_ORDER_H

#include <stddef.h>
#include <stdint.h>

#include "voice.h"        /* spfy_vin_t */
#include "unit_table.h"   /* spfy_unit_table_t */

/* Phone-order reconciliation between the VIN's two phone vocabularies.
 *
 * A voice names its phones twice, and the two lists are NOT always in the
 * same order:
 *
 *   feat["name"]  the half-phone variant table -- "aa1","aa2","ae1","ae2",...
 *                 Stripping the trailing 1/2 gives the PHONE order. This is
 *                 the order hp_class is expressed in.
 *   ccos/labl     the label table used to index the ccos matrices. This is
 *                 the order unit_record.phone_center is expressed in.
 *
 * For Jill/Felix/Javier the two orders coincide (both alphabetical) and the
 * permutation is the identity. Tom's labl is NOT alphabetical -- it has
 * "ch dx d dh" and "el er en" -- giving a 3-cycle on d/dh/dx and a swap on
 * en/er. Paulina's Spanish labl diverges from feat in 28 of 31 positions.
 * Hardcoding Tom's swaps (the old `tom_swap_label`) is therefore wrong for
 * every other voice.
 *
 * With this mapping, the engine-truth hp_class of a unit is exactly
 *
 *     hp_class(uid) = labl_to_feat[unit[uid].phone_center] * 2 + (uid & 1)
 *
 * i.e. units are stored as consecutive (left-half, right-half) pairs, so
 * the half side is the parity of the unit id -- NOT the `is_first_half`
 * field, which means something else. Verified 2026-07-20 to be byte-exact
 * against the 169579-entry Frida-dumped tom_hpclass.bin, which this
 * derivation replaces for every voice.
 *
 * Note ccos/labl may carry one trailing EMPTY name (Tom: 47 labels for 46
 * phones), which is why n_labels can exceed n_phones. That empty slot maps
 * to SPFY_PHONE_NONE. */

#define SPFY_PHONE_NONE 0xFFu

typedef struct {
    uint32_t  n_phones;     /* distinct phones in feat["name"] */
    uint32_t  n_labels;     /* entries in ccos/labl (may be n_phones + 1) */
    /* labl index -> feat phone index; SPFY_PHONE_NONE if unmatched. */
    uint8_t  *labl_to_feat; /* heap, n_labels entries */
    /* feat phone index -> labl index; SPFY_PHONE_NONE if unmatched. */
    uint8_t  *feat_to_labl; /* heap, n_phones entries */
    /* Phone names in feat order. Owned, NUL-terminated copies: the VIN
     * stores them as counted, non-terminated slices, but consumers --
     * notably the FE's phone-symbol lookup -- want plain C strings. */
    char       **phone_names;      /* heap array, n_phones entries, owned */
    uint16_t    *phone_name_len;
} spfy_phone_order_t;

/* Build from the VIN's feat + ccos chunks. Returns SPFY_OK or SPFY_E_*. */
int  spfy_phone_order_build(const spfy_vin_t *vin, spfy_phone_order_t *out);
void spfy_phone_order_free(spfy_phone_order_t *p);

/* Derive the per-unit hp_class table (one byte per unit), replacing the
 * Frida-dumped hpclass.bin. Caller frees with spfy_phone_order_hpclass_free.
 * Requires a loaded unit table for the same VIN. */
int  spfy_phone_order_hpclass(const spfy_phone_order_t *po,
                              const spfy_unit_table_t *units,
                              uint8_t **out_data, uint32_t *out_n);
void spfy_phone_order_hpclass_free(uint8_t *data);

/* feat-order index of a phone by NAME, or SPFY_PHONE_NONE if absent.
 *
 * This is the engine's phone id -- the same numbering hp_class is built
 * from -- so it supersedes the compiled-in en-US
 * `data/en_us_engine_phone_ids.csv` table for every voice. Verified
 * 2026-07-20: that CSV is EXACTLY this order for Tom (45 entries, 0
 * mismatches), and feat additionally carries the one phone the CSV
 * lacks. */
uint8_t spfy_phone_order_index(const spfy_phone_order_t *po,
                               const char *name);

#endif
