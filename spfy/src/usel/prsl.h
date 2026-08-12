#ifndef SPFY_USEL_PRSL_H
#define SPFY_USEL_PRSL_H

#include <stddef.h>
#include <stdint.h>

#include "../common/le.h"
#include "../voice/voice.h"

/* PRSL preselection cache. */

/* `candidates` aliases the VIN buffer at an arbitrary chunk offset, so it
 * is held as raw bytes and read through spfy_prsl_cand() rather than as a
 * `const uint32_t *`. */
typedef struct {
    uint32_t       context_key;
    uint32_t       n_candidates;
    const uint8_t *candidates;
} spfy_prsl_group_t;

static inline uint32_t spfy_prsl_cand(const uint8_t *cands, uint32_t i)
{
    return spfy_le_u32(cands + (size_t)i * 4u);
}

typedef struct {
    spfy_prsl_group_t *groups;
    uint32_t           n_groups;
} spfy_prsl_t;

int  spfy_prsl_load  (const spfy_vin_t *vin, spfy_prsl_t *out);
void spfy_prsl_free  (spfy_prsl_t *p);

static inline uint32_t spfy_prsl_context_key(uint32_t left_hp,
                                             uint32_t center_hp,
                                             uint32_t right_hp)
{
    return left_hp * 10000u + center_hp * 100u + right_hp;
}

/* Lookup. */
int  spfy_prsl_lookup(const spfy_prsl_t *p, uint32_t context_key,
                      const uint8_t **cands, uint32_t *n_cands);

#endif
