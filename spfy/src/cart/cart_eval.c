/* CART tree traversal + question evaluation. */

#include "cart.h"

#include "../../include/spfy/spfy.h"

int spfy_ques_eval(const spfy_ques_t *q, uint32_t value)
{
    /* Linear scan, matching the engine (FUN_08e87c70 / 0x8e87c90 dispatcher).
     * Question value-sets are small (median ~5 entries, max ~50) so linear
     * scan is fine. */
    for (uint32_t i = 0; i < q->n_values; ++i) {
        if (spfy_ques_value(q, i) == value) return 1;
    }
    return 0;
}

int spfy_cart_traverse(const spfy_cart_t *c, uint32_t tree_idx,
                       spfy_cart_feat_fn feat, void *user,
                       float *out_mean, float *out_var)
{
    if (!c || !feat || !out_mean || !out_var) return SPFY_E_INVAL;
    if (tree_idx >= c->n_trees)               return SPFY_E_OOB;

    const spfy_cart_tree_t *t = &c->trees[tree_idx];
    if (t->n_nodes == 0) return SPFY_E_FORMAT;

    /* Bound traversal depth to catch malformed cycles. Real durt trees max
     * out at ~12 deep; 256 is generous. */
    uint32_t depth = 0;
    uint32_t idx   = 0;        /* always start at root */
    while (depth++ < 256u) {
        if (idx >= t->n_nodes) return SPFY_E_FORMAT;
        const spfy_cart_node_t *n = &t->nodes[idx];

        if (n->yes_child < 0) {
            /* Leaf */
            *out_mean = n->leaf_mean;
            *out_var  = n->leaf_var;
            return SPFY_OK;
        }
        /* Branch: evaluate the question */
        if (n->q_index >= c->n_ques) return SPFY_E_FORMAT;
        const spfy_ques_t *q = &c->ques[n->q_index];

        int32_t  feat_value = feat(q->type, user);
        int      yes        = spfy_ques_eval(q, (uint32_t)feat_value);

        idx = yes ? (uint32_t)n->yes_child : n->no_child;
    }
    return SPFY_E_FORMAT;     /* depth exceeded -- malformed */
}
