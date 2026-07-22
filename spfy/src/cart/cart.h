#ifndef SPFY_CART_INTERNAL_H
#define SPFY_CART_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "../common/le.h"

/* CART tree evaluator (durt + f0tr).
 *
 * On disk, both chunks share the format:
 *
 *   <chunk> {
 *     trhd {
 *       labl  : { u32 n_labels; n_labels * { u16 len; char[len] name } }
 *       ques  : { u32 n_ques;
 *                 n_ques * { u8 q_type;
 *                            u32 n_values;
 *                            u32[n_values] values } }
 *     }
 *     tree[]: { u32 n_nodes;
 *               n_nodes * { branch=16B | leaf=20B } }
 *   }
 *
 * f0tr: exactly 1 tree (single global F0 model, ~109 nodes).
 * durt: exactly 47 trees (one per phone label).
 *
 * Branch node (16B):  u32 idx, s32 yes_child, u32 no_child, u32 q_index
 * Leaf   node (20B):  u32 idx, s32 -1,        u32 -1,       f32 mean, f32 var
 *
 * See reveng/README_TECHNICAL.md sections '`ques` format' and
 * '`tree` wire layout' for the full spec.
 */

/* `values` points into the VIN buffer at an arbitrary chunk offset, so it
 * is held as raw bytes and read through spfy_ques_value(). See common/le.h. */
typedef struct {
    uint32_t       type;        /* 1..9; see q_type table in README_TECHNICAL */
    uint32_t       n_values;
    const uint8_t *values;      /* LE u32 array, points into VIN buffer */
} spfy_ques_t;

static inline uint32_t spfy_ques_value(const spfy_ques_t *q, uint32_t i)
{
    return spfy_le_u32(q->values + (size_t)i * 4u);
}

typedef struct {
    /* For branches: q_index >= 0, yes_child >= 0, no_child >= 0
     * For leaves:   q_index == 0xFFFFFFFF, yes_child == -1
     *               leaf_mean and leaf_var are populated. */
    uint32_t   q_index;
    int32_t    yes_child;
    uint32_t   no_child;
    float      leaf_mean;
    float      leaf_var;
} spfy_cart_node_t;

typedef struct {
    uint32_t           n_nodes;
    spfy_cart_node_t  *nodes;     /* heap, owned */
} spfy_cart_tree_t;

typedef struct {
    /* Decoded trhd. */
    uint32_t      n_labels;
    /* Each label is a small string; we don't keep them as C strings
     * here, callers map by index (the engine likewise uses indices). */

    uint32_t      n_ques;
    spfy_ques_t  *ques;          /* heap, owned (values[] still alias VIN) */

    uint32_t            n_trees;
    spfy_cart_tree_t   *trees;   /* heap, owned */
} spfy_cart_t;

#include "../voice/voice.h"   /* pulls spfy_vin_t */

/* Parse the named chunk (FOURCC 'f0tr' or 'durt') into out.
 * out->ques->values aliases VIN buffer; do not free vin while using out. */
int  spfy_cart_load_f0tr(const spfy_vin_t *vin, spfy_cart_t *out);
int  spfy_cart_load_durt(const spfy_vin_t *vin, spfy_cart_t *out);
void spfy_cart_free(spfy_cart_t *c);

/* Question evaluation: returns 1 if value is in the question's value-set,
 * 0 otherwise. Linear scan, matching the engine. */
int  spfy_ques_eval(const spfy_ques_t *q, uint32_t value);

/* Traverse tree[tree_idx]. The feature callback receives a q_type and
 * returns the feature value to test. Returns SPFY_OK and writes the leaf
 * (mean, var) on success, or SPFY_E_* on malformed tree.
 *
 * NB: callback is invoked once per branch node visited; same q_type may be
 * asked multiple times within one traversal -- callbacks must be
 * deterministic for that q_type. */
typedef int32_t (*spfy_cart_feat_fn)(uint32_t q_type, void *user);

int  spfy_cart_traverse(const spfy_cart_t *c, uint32_t tree_idx,
                        spfy_cart_feat_fn feat, void *user,
                        float *out_mean, float *out_var);

#endif
