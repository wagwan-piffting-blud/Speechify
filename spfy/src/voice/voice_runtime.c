/* Voice runtime maps: L[] (phone -> label) and hp_class[] tables.
 *
 * Tom-specific path: phone_center aligns with the ccos label set, so L is
 * identity and hp_class is `phone + (is_first_half ? 0 : N)`. Other voices
 * may need a halfphone name list (not yet decoded from any chunk we know).
 */

#include "voice_runtime.h"

#include "../common/log.h"
#include "../../include/spfy/spfy.h"

#include <stdlib.h>
#include <string.h>

int spfy_voice_maps_build(const spfy_ccos_t *ccos, spfy_voice_maps_t *out)
{
    if (!ccos || !out) return SPFY_E_INVAL;
    if (ccos->n_labels == 0 || ccos->n_hp_classes == 0) return SPFY_E_FORMAT;
    memset(out, 0, sizeof *out);

    out->n_labels     = ccos->n_labels;
    out->n_hp_entries = 2u * ccos->n_labels;

    out->L = (uint8_t *)malloc(out->n_labels);
    out->hp_class = (uint8_t *)malloc(out->n_hp_entries);
    if (!out->L || !out->hp_class) {
        spfy_voice_maps_free(out);
        return SPFY_E_NOMEM;
    }

    /* Identity L[] for Tom (phone_center == label_idx). */
    for (uint32_t i = 0; i < out->n_labels; ++i) out->L[i] = (uint8_t)i;

    /* hp_class table indexed by (phone_center * 2 + (1 - is_first_half))
     * -- so even index = LEFT half (label_idx), odd index = RIGHT half
     * (label_idx + n_labels). */
    for (uint32_t pc = 0; pc < out->n_labels; ++pc) {
        out->hp_class[pc * 2 + 0] = (uint8_t)pc;                    /* LEFT  */
        out->hp_class[pc * 2 + 1] = (uint8_t)(pc + out->n_labels);  /* RIGHT */
    }
    /* Tom-specific phone-pair swaps in the engine's voice+0x608 hp_class
     * remap table (captured 2026-05-05 via Frida). The engine swaps
     * phones in two cycles: 9 -> 10 -> 11 -> 9, and 14 <-> 15.
     * Apply the same swaps to our hp_class[] so its semantics match the
     * engine when used directly with slice.ctx[2] indices. */
    if (out->n_labels == 47) {
        /* 9 → 10, 10 → 11, 11 → 9 (3-cycle). */
        out->hp_class[18] = 10;
        out->hp_class[19] = 10 + (uint8_t)out->n_labels;
        out->hp_class[20] = 11;
        out->hp_class[21] = 11 + (uint8_t)out->n_labels;
        out->hp_class[22] = 9;
        out->hp_class[23] = 9  + (uint8_t)out->n_labels;
        /* 14 ↔ 15. */
        out->hp_class[28] = 15;
        out->hp_class[29] = 15 + (uint8_t)out->n_labels;
        out->hp_class[30] = 14;
        out->hp_class[31] = 14 + (uint8_t)out->n_labels;
    }
    return SPFY_OK;
}

void spfy_voice_maps_free(spfy_voice_maps_t *m)
{
    if (!m) return;
    free(m->L);
    free(m->hp_class);
    memset(m, 0, sizeof *m);
}
