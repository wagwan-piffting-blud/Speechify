#ifndef SPFY_VOICE_CCOS_H
#define SPFY_VOICE_CCOS_H

#include <stddef.h>
#include <stdint.h>

#include "voice.h"   /* spfy_vin_t */

/* ccos: precomputed phone-context distance tables used by the S cost.
 *
 * On disk (per reveng/README_TECHNICAL.md `ccos` section):
 *   labl  : { u32 n_labels; n_labels * { u16 len; char[len] name } }
 *   data  : (2 * n_labels) hp-classes
 *           * 4 slots (pp2, pp1, pn1, pn2)
 *           * { u32 hp_class_validator; u32 slot_validator;
 *               f32[N*(N-1)/2] upper_triangle }   N = n_labels
 *
 * Tom: 47 labels -> 94 hp-classes -> 376 tables, 1,628,832 bytes total.
 *
 * Runtime build (FUN_08e83f60):
 *   table[i][j] = table[j][i] = (raw + 0.1) * slot_scale
 *   table[i][i] = 0
 * with slot_scale depending on side (LEFT = hp_class < n_labels) and slot:
 *
 *   side    slot 0   slot 1   slot 2   slot 3
 *   LEFT    0.2      1.0      0.5      0.1
 *   RIGHT   0.1      0.5      1.0      0.2
 *
 * Storage: full (n_labels x n_labels) f32 matrix per (hp_class, slot).
 * Diagonals are 0; off-diagonals are symmetric. Memory: ~3.3 MB for Tom.
 */

#define SPFY_CCOS_N_SLOTS 4

typedef struct {
    /* Flat row-major float buffer of size n_labels^2 per (hp_class, slot).
     * Indexed as data[hp_class * SPFY_CCOS_N_SLOTS + slot] which is itself
     * a length-(n_labels^2) array. */
    float    *tables;           /* heap, owned */
    uint32_t  n_labels;         /* 47 for Tom */
    uint32_t  n_hp_classes;     /* 2 * n_labels = 94 for Tom */
} spfy_ccos_t;

int  spfy_ccos_load(const spfy_vin_t *vin, spfy_ccos_t *out);
void spfy_ccos_free(spfy_ccos_t *c);

/* Direct accessor: returns a pointer to the (n_labels x n_labels) matrix
 * for the given (hp_class, slot). NULL on out-of-bounds. */
const float *spfy_ccos_table(const spfy_ccos_t *c,
                             uint32_t hp_class, uint32_t slot);

/* Single-cell accessor with bounds check. Returns 0 on OOB. */
float spfy_ccos_cell(const spfy_ccos_t *c,
                     uint32_t hp_class, uint32_t slot,
                     uint32_t i, uint32_t j);

#endif
