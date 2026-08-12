#ifndef SPFY_VOICE_RUNTIME_H
#define SPFY_VOICE_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "voice.h"
#include "ccos.h"

/* Runtime-derived voice maps used by the cost components. */

typedef struct {
    uint8_t  *L;
    uint32_t  n_labels;
    uint8_t  *hp_class;
    uint32_t  n_hp_entries;
} spfy_voice_maps_t;

/* Build L[] and hp_class[] using the voice's real phone/label name tables. */
int  spfy_voice_maps_build_from_vin(const spfy_vin_t *vin,
                                    const spfy_ccos_t *ccos,
                                    spfy_voice_maps_t *out);

/* Legacy: assumes phone_center == label_idx. */
int  spfy_voice_maps_build(const spfy_ccos_t *ccos, spfy_voice_maps_t *out);
void spfy_voice_maps_free(spfy_voice_maps_t *m);

#endif
