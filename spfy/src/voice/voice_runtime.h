#ifndef SPFY_VOICE_RUNTIME_H
#define SPFY_VOICE_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "voice.h"
#include "ccos.h"

/* Runtime-derived voice maps used by the cost components.
 *
 * Per reveng/README_TECHNICAL.md "ccos Loader functions":
 *   voice+0x604[hp_id] = label_idx                  (always, 0..N-1)
 *   voice+0x608[hp_id] = label_idx                  if name ended in '1' (LEFT)
 *                      = label_idx + num_labels     otherwise            (RIGHT)
 *
 * The engine builds these by looking the half-phone's NAME up in the labl
 * list. The half-phone-name source IS in the VIN -- it is the feat["name"]
 * table ("aa1","aa2",...) -- so the mapping is fully derivable per voice;
 * see phone_order.h. The identity shortcut that used to live here is
 * correct only for Jill/Felix/Javier, whose labl order already matches
 * feat order. Tom needs a 3-cycle on d/dh/dx plus an en/er swap, and
 * Paulina permutes 28 of its 31 labels.
 *
 * Public surface: a flat phone_id -> label_id table plus a flat
 * (phone_center * 2 + side) -> hp_class table. Both 8-bit indices
 * since values fit in [0, 2*N-1] = [0, 93] for Tom. */

typedef struct {
    uint8_t  *L;              /* heap, n_labels entries; phone_id -> label_id */
    uint32_t  n_labels;
    uint8_t  *hp_class;       /* heap, 2*n_labels entries; (phone*2 + (1-ifh)) -> hp_class */
    uint32_t  n_hp_entries;   /* = 2*n_labels */
} spfy_voice_maps_t;

/* Build L[] and hp_class[] using the voice's real phone/label name tables.
 * This is the correct entry point for any voice. */
int  spfy_voice_maps_build_from_vin(const spfy_vin_t *vin,
                                    const spfy_ccos_t *ccos,
                                    spfy_voice_maps_t *out);

/* Legacy: assumes phone_center == label_idx. Correct only for voices whose
 * ccos label order matches feat["name"] order. Prefer the _from_vin form. */
int  spfy_voice_maps_build(const spfy_ccos_t *ccos, spfy_voice_maps_t *out);
void spfy_voice_maps_free(spfy_voice_maps_t *m);

#endif
