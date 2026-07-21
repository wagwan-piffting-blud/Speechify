/* Voice runtime maps: L[] (phone -> label) and hp_class[] tables.
 *
 * These mirror the engine's create_context_label_mapping (FUN_08e83ce0),
 * which walks the voice's half-phone names (feat["name"]), strips the
 * trailing '1'/'2', looks the base name up in the ccos label list, and
 * writes:
 *
 *     voice+0x604[hp_id] = label_idx                        (always)
 *     voice+0x608[hp_id] = label_idx                        if name ends '1'
 *     voice+0x608[hp_id] = label_idx + n_labels             otherwise
 *
 * Both source lists live in the VIN, so this is derivable per voice with no
 * Frida capture -- which supersedes the old Tom-only path here that assumed
 * phone_center == label_idx and patched in Tom's five-phone permutation by
 * hand. That assumption holds for Jill/Felix/Javier but is badly wrong for
 * Tom (a 3-cycle on d/dh/dx plus an en/er swap) and catastrophically wrong
 * for Paulina (28 of 31 labels permuted).
 *
 * See phone_order.c for the name-matching itself. */

#include "voice_runtime.h"
#include "phone_order.h"

#include "../common/log.h"
#include "../../include/spfy/spfy.h"

#include <stdlib.h>
#include <string.h>

int spfy_voice_maps_build_from_vin(const spfy_vin_t *vin,
                                   const spfy_ccos_t *ccos,
                                   spfy_voice_maps_t *out)
{
    if (!vin || !ccos || !out) return SPFY_E_INVAL;
    if (ccos->n_labels == 0 || ccos->n_hp_classes == 0) return SPFY_E_FORMAT;
    memset(out, 0, sizeof *out);

    spfy_phone_order_t po;
    int rc = spfy_phone_order_build(vin, &po);
    if (rc != SPFY_OK) return rc;

    out->n_labels     = ccos->n_labels;
    out->n_hp_entries = 2u * ccos->n_labels;

    out->L        = (uint8_t *)malloc(out->n_labels);
    out->hp_class = (uint8_t *)malloc(out->n_hp_entries);
    if (!out->L || !out->hp_class) {
        spfy_phone_order_free(&po);
        spfy_voice_maps_free(out);
        return SPFY_E_NOMEM;
    }

    /* L[] maps the unit record's phone_ctx[] entries into ccos label
     * space. Those bytes come from the same vocabulary as phone_center,
     * i.e. they are ALREADY labl indices -- so L is the identity for every
     * voice, not just Tom. (The engine's own +0x604 is indexed by
     * half-phone id instead; our callers only ever use the phone-level
     * form.) */
    for (uint32_t i = 0; i < out->n_labels; ++i) out->L[i] = (uint8_t)i;

    /* hp_class[] is indexed by a half-phone class (phone * 2 + side) in
     * FEAT order -- that is what slice ctx[] and the derived hpclass table
     * carry -- and yields the ccos forest index, which is in LABL order.
     * So the permutation applied here is feat -> labl, the inverse of the
     * one phone_order exposes for hp_class derivation. Side 0 = LEFT
     * (label_idx), side 1 = RIGHT (label_idx + n_labels), matching the
     * engine's +0x608 layout. */
    for (uint32_t ph = 0; ph < out->n_labels; ++ph) {
        uint8_t lab = (ph < po.n_phones) ? po.feat_to_labl[ph]
                                         : SPFY_PHONE_NONE;
        if (lab == SPFY_PHONE_NONE) lab = (uint8_t)ph;   /* unnamed label */
        out->hp_class[ph * 2 + 0] = lab;
        out->hp_class[ph * 2 + 1] = (uint8_t)(lab + out->n_labels);
    }

    spfy_phone_order_free(&po);
    return SPFY_OK;
}

int spfy_voice_maps_build(const spfy_ccos_t *ccos, spfy_voice_maps_t *out)
{
    /* Legacy entry point: no VIN, so the phone/label permutation cannot be
     * recovered and identity is assumed. Correct only for voices whose
     * ccos label order already matches feat["name"] order (Jill, Felix,
     * Javier). Callers with a VIN in hand should use
     * spfy_voice_maps_build_from_vin instead. */
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

    for (uint32_t i = 0; i < out->n_labels; ++i) out->L[i] = (uint8_t)i;
    for (uint32_t pc = 0; pc < out->n_labels; ++pc) {
        out->hp_class[pc * 2 + 0] = (uint8_t)pc;                    /* LEFT  */
        out->hp_class[pc * 2 + 1] = (uint8_t)(pc + out->n_labels);  /* RIGHT */
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
