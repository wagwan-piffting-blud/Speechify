#ifndef SPFY_VOICE_PHONE_ORDER_H
#define SPFY_VOICE_PHONE_ORDER_H

#include <stddef.h>
#include <stdint.h>

#include "voice.h"
#include "unit_table.h"

/* Phone-order reconciliation between the VIN's two phone vocabularies. */

#define SPFY_PHONE_NONE 0xFFu

typedef struct {
    uint32_t  n_phones;
    uint32_t  n_labels;
    uint8_t  *labl_to_feat;
    uint8_t  *feat_to_labl;
    /* Phone names in feat order. */
    char       **phone_names;
    uint16_t    *phone_name_len;
} spfy_phone_order_t;

int  spfy_phone_order_build(const spfy_vin_t *vin, spfy_phone_order_t *out);
void spfy_phone_order_free(spfy_phone_order_t *p);

/* Derive the per-unit hp_class table (one byte per unit), replacing the
 * Frida-dumped hpclass.bin. */
int  spfy_phone_order_hpclass(const spfy_phone_order_t *po,
                              const spfy_unit_table_t *units,
                              uint8_t **out_data, uint32_t *out_n);
void spfy_phone_order_hpclass_free(uint8_t *data);

/* feat-order index of a phone by NAME, or SPFY_PHONE_NONE if absent. */
uint8_t spfy_phone_order_index(const spfy_phone_order_t *po,
                               const char *name);

#endif
