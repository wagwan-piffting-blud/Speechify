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
 * For Tom, phone_center (0..45) aligns with the ccos label set, so the
 * mapping is essentially identity:
 *     L[phone_center]      = phone_center
 *     hp_class(phone, is_first_half) = phone + (is_first_half ? 0 : N)
 *
 * For voices where phone_center and label_idx don't align, the engine
 * looks up the halfphone's NAME in the labl list. We don't yet have a
 * halfphone-name source from the VIN -- the engine's `voice+0x28`
 * num_halfphones array is populated from a chunk we haven't decoded.
 * Until then, this loader is Tom-specific (identity L[], straightforward
 * hp_class[]).
 *
 * Public surface: a flat phone_id -> label_id table plus a flat
 * (phone_center * 2 + is_first_half) -> hp_class table. Both 8-bit indices
 * since values fit in [0, 2*N-1] = [0, 93] for Tom. */

typedef struct {
    uint8_t  *L;              /* heap, n_labels entries; phone_id -> label_id */
    uint32_t  n_labels;
    uint8_t  *hp_class;       /* heap, 2*n_labels entries; (phone*2 + (1-ifh)) -> hp_class */
    uint32_t  n_hp_entries;   /* = 2*n_labels */
} spfy_voice_maps_t;

/* Build the L[] and hp_class[] tables from a loaded ccos. Tom-specific
 * (assumes phone_center aligns with label index). Returns SPFY_E_NOTSUP if
 * the voice's halfphone naming isn't recoverable from chunks alone. */
int  spfy_voice_maps_build(const spfy_ccos_t *ccos, spfy_voice_maps_t *out);
void spfy_voice_maps_free(spfy_voice_maps_t *m);

#endif
