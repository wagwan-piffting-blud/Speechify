#ifndef SPFY_CART_INTERNAL_H
#define SPFY_CART_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "../common/le.h"

/* CART tree evaluator (durt + f0tr). */

/* `values` points into the VIN buffer at an arbitrary chunk offset, so it
 * is held as raw bytes and read through spfy_ques_value(). */
typedef struct {
    uint32_t       type;
    uint32_t       n_values;
    const uint8_t *values;
} spfy_ques_t;

static inline uint32_t spfy_ques_value(const spfy_ques_t *q, uint32_t i)
{
    return spfy_le_u32(q->values + (size_t)i * 4u);
}

typedef struct {
    /* For branches: q_index >= 0, yes_child >= 0, no_child >= 0 For leaves:
     * q_index == 0xFFFFFFFF, yes_child == -1 leaf_mean and leaf_var are
     * populated. */
    uint32_t   q_index;
    int32_t    yes_child;
    uint32_t   no_child;
    float      leaf_mean;
    float      leaf_var;
} spfy_cart_node_t;

typedef struct {
    uint32_t           n_nodes;
    spfy_cart_node_t  *nodes;
} spfy_cart_tree_t;

typedef struct {
    uint32_t      n_labels;
    /* Each label is a small string; we don't keep them as C strings here,
     * callers map by index (the engine likewise uses indices). */

    uint32_t      n_ques;
    spfy_ques_t  *ques;

    uint32_t            n_trees;
    spfy_cart_tree_t   *trees;
} spfy_cart_t;

#include "../voice/voice.h"

/* Parse the named chunk (FOURCC 'f0tr' or 'durt') into out. */
int  spfy_cart_load_f0tr(const spfy_vin_t *vin, spfy_cart_t *out);
int  spfy_cart_load_durt(const spfy_vin_t *vin, spfy_cart_t *out);
void spfy_cart_free(spfy_cart_t *c);

/* Question evaluation: returns 1 if value is in the question's value-set, 0
 * otherwise. */
int  spfy_ques_eval(const spfy_ques_t *q, uint32_t value);

/* Traverse tree[tree_idx]. */
typedef int32_t (*spfy_cart_feat_fn)(uint32_t q_type, void *user);

int  spfy_cart_traverse(const spfy_cart_t *c, uint32_t tree_idx,
                        spfy_cart_feat_fn feat, void *user,
                        float *out_mean, float *out_var);

#endif
