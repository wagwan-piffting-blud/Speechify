#ifndef SPFY_USEL_PRSL_H
#define SPFY_USEL_PRSL_H

#include <stddef.h>
#include <stdint.h>

#include "../common/le.h"
#include "../voice/voice.h"   /* spfy_vin_t */

/* PRSL preselection cache.
 *
 * On disk (per reveng/README_TECHNICAL.md "prsl" section):
 *   u32 group_count
 *   group_count * { u32 n; u32 context_key; u32[n-1] candidate_ids }
 *
 * Tom: 76676 groups, 1089111 total candidates (~14.2 avg).
 *
 * context_key = left_hp * 10000 + center_hp * 100 + right_hp
 * (See spfy_prsl_context_key().) Halfphone indices are even=left-boundary
 * half, odd=right-boundary half; 92 = silence/boundary marker.
 *
 * Keys are strictly monotonically increasing in file order, so we lookup
 * via O(log n) binary search rather than building a hash table. */

/* `candidates` aliases the VIN buffer at an arbitrary chunk offset, so it
 * is held as raw bytes and read through spfy_prsl_cand() rather than as a
 * `const uint32_t *`. See common/le.h. */
typedef struct {
    uint32_t       context_key;
    uint32_t       n_candidates;
    const uint8_t *candidates;      /* aliases VIN buffer; not owned; u32[] */
} spfy_prsl_group_t;

/* Read candidate i out of a pool returned by spfy_prsl_lookup(). */
static inline uint32_t spfy_prsl_cand(const uint8_t *cands, uint32_t i)
{
    return spfy_le_u32(cands + (size_t)i * 4u);
}

typedef struct {
    spfy_prsl_group_t *groups;      /* heap, owned; sorted by context_key */
    uint32_t           n_groups;
} spfy_prsl_t;

int  spfy_prsl_load  (const spfy_vin_t *vin, spfy_prsl_t *out);
void spfy_prsl_free  (spfy_prsl_t *p);

/* Build the triphone context key. */
static inline uint32_t spfy_prsl_context_key(uint32_t left_hp,
                                             uint32_t center_hp,
                                             uint32_t right_hp)
{
    return left_hp * 10000u + center_hp * 100u + right_hp;
}

/* Lookup. Returns SPFY_OK and writes *cands / *n_cands on hit;
 * SPFY_E_INVAL on bad args; SPFY_E_OOB on miss (and writes NULL/0). */
int  spfy_prsl_lookup(const spfy_prsl_t *p, uint32_t context_key,
                      const uint8_t **cands, uint32_t *n_cands);

#endif
