/* BuildGraph + post-order slot allocation, plus LinkGraph predecessor
 * derivation. See build_graph.h for the algorithm overview and the
 * engine-side mapping. */

#include "build_graph.h"

#include "../../include/spfy/spfy.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void spfy_fe_utt_free(spfy_fe_utt_t *u)
{
    if (!u) return;
    free(u->word_shareds);
    if (u->word_names) {
        for (uint32_t i = 0; i < u->n_words; ++i) free(u->word_names[i]);
        free(u->word_names);
    }
    if (u->word_syls) {
        for (uint32_t i = 0; i < u->n_words; ++i) free(u->word_syls[i]);
        free(u->word_syls);
    }
    free(u->word_n_syls);
    if (u->syl_segs) {
        for (uint32_t i = 0; i < u->n_syls; ++i) free(u->syl_segs[i]);
        free(u->syl_segs);
    }
    free(u->syl_n_segs);
    free(u->syl_stress);
    free(u->syl_accent);
    memset(u, 0, sizeof *u);
}

void spfy_slot_tree_free(spfy_slot_tree_t *t)
{
    if (!t) return;
    if (t->slots) {
        for (uint32_t i = 0; i < t->n_slots; ++i) free(t->slots[i].child_idx);
        free(t->slots);
    }
    memset(t, 0, sizeof *t);
}

/* Internal "tree node" used during construction. We allocate a flat
 * pool sized to (1 phrase + n_words + n_syls + 2 * n_segs). After the
 * tree is built we run a post-order DFS to fill in `post_idx` and emit
 * the final slots[] array indexed by post-order. */
typedef struct tnode {
    spfy_slot_kind_t kind;
    uint32_t         fe_shared;
    uint32_t         halfphone_side;
    /* Children: flat indices into the pool. */
    uint32_t        *child_idx;
    uint32_t         n_children;
    uint32_t         children_cap;
    uint32_t         parent;          /* index into pool; UINT32_MAX = root */
    uint32_t         post_idx;        /* set during post-order pass */
} tnode_t;

static int tnode_add_child(tnode_t *parent, uint32_t child_pool_idx)
{
    if (parent->n_children == parent->children_cap) {
        uint32_t  nc = parent->children_cap ? parent->children_cap * 2u : 4u;
        uint32_t *na = realloc(parent->child_idx, nc * sizeof *na);
        if (!na) return -1;
        parent->child_idx = na;
        parent->children_cap = nc;
    }
    parent->child_idx[parent->n_children++] = child_pool_idx;
    return 0;
}

/* Recursive post-order traversal: assign post_idx in DFS-leaves-first
 * order. counter is shared across the whole tree. */
static void post_order_walk(tnode_t *pool, uint32_t cur, uint32_t *counter)
{
    tnode_t *n = &pool[cur];
    for (uint32_t i = 0; i < n->n_children; ++i) {
        post_order_walk(pool, n->child_idx[i], counter);
    }
    n->post_idx = (*counter)++;
}

int spfy_build_graph(const spfy_fe_utt_t *in, spfy_slot_tree_t *out)
{
    if (!in || !out) return SPFY_E_INVAL;
    memset(out, 0, sizeof *out);

    /* Sanity: word_n_syls and syl_n_segs sizes match. */
    uint32_t expected_n_syls = 0;
    for (uint32_t i = 0; i < in->n_words; ++i)
        expected_n_syls += in->word_n_syls ? in->word_n_syls[i] : 0;
    if (expected_n_syls != in->n_syls) return SPFY_E_FORMAT;

    uint32_t expected_n_segs = 0;
    for (uint32_t g = 0; g < in->n_syls; ++g)
        expected_n_segs += in->syl_n_segs ? in->syl_n_segs[g] : 0;
    if (expected_n_segs != in->n_segs) return SPFY_E_FORMAT;

    /* Allocate pool of tnodes. */
    uint32_t pool_cap = 1u + in->n_words + in->n_syls + 2u * in->n_segs;
    if (pool_cap == 0) return SPFY_E_INVAL;
    tnode_t *pool = (tnode_t *)calloc(pool_cap, sizeof *pool);
    if (!pool) return SPFY_E_NOMEM;
    uint32_t pool_n = 0;

    /* 1. Phrase root. */
    uint32_t phrase = pool_n++;
    pool[phrase].kind   = SPFY_SK_PHRASE;
    pool[phrase].parent = UINT32_MAX;

    /* 2. Words (in FE order). */
    uint32_t syl_global = 0;
    for (uint32_t w = 0; w < in->n_words; ++w) {
        uint32_t word = pool_n++;
        pool[word].kind      = SPFY_SK_WORD;
        pool[word].parent    = phrase;
        pool[word].fe_shared = in->word_shareds[w];
        if (tnode_add_child(&pool[phrase], word) != 0) goto oom;

        uint32_t n_syls_in_word = in->word_n_syls[w];
        for (uint32_t s = 0; s < n_syls_in_word; ++s, ++syl_global) {
            uint32_t syl = pool_n++;
            pool[syl].kind      = SPFY_SK_SYLLABLE;
            pool[syl].parent    = word;
            pool[syl].fe_shared = in->word_syls[w][s];
            if (tnode_add_child(&pool[word], syl) != 0) goto oom;

            /* Segments under this syllable. */
            uint32_t n_segs_in_syl = in->syl_n_segs[syl_global];
            for (uint32_t g = 0; g < n_segs_in_syl; ++g) {
                uint32_t seg_shared = in->syl_segs[syl_global][g];
                /* Halfphone left. */
                uint32_t hpL = pool_n++;
                pool[hpL].kind = SPFY_SK_HALFPHONE;
                pool[hpL].parent = syl;
                pool[hpL].fe_shared = seg_shared;
                pool[hpL].halfphone_side = 0;
                if (tnode_add_child(&pool[syl], hpL) != 0) goto oom;
                /* Halfphone right. */
                uint32_t hpR = pool_n++;
                pool[hpR].kind = SPFY_SK_HALFPHONE;
                pool[hpR].parent = syl;
                pool[hpR].fe_shared = seg_shared;
                pool[hpR].halfphone_side = 1;
                if (tnode_add_child(&pool[syl], hpR) != 0) goto oom;
            }
        }
    }
    if (pool_n != pool_cap) {
        /* Shouldn't happen if the FE input is consistent. */
        goto badf;
    }

    /* 3. Post-order traversal: assign post_idx contiguously. */
    uint32_t counter = 0;
    post_order_walk(pool, phrase, &counter);
    if (counter != pool_n) goto badf;

    /* 4. Emit final slots[] array indexed by post_idx. */
    spfy_slot_node_t *slots = (spfy_slot_node_t *)
        calloc(pool_n, sizeof *slots);
    if (!slots) goto oom;
    for (uint32_t i = 0; i < pool_n; ++i) {
        const tnode_t *n = &pool[i];
        spfy_slot_node_t *s = &slots[n->post_idx];
        s->kind           = n->kind;
        s->post_idx       = n->post_idx;
        s->parent_idx     = (n->parent == UINT32_MAX)
                            ? UINT32_MAX
                            : pool[n->parent].post_idx;
        s->fe_shared      = n->fe_shared;
        s->halfphone_side = n->halfphone_side;
        s->n_children     = n->n_children;
        if (n->n_children > 0) {
            s->child_idx = (uint32_t *)malloc(n->n_children *
                                              sizeof *s->child_idx);
            if (!s->child_idx) {
                /* Roll back the entire slots array. */
                for (uint32_t j = 0; j <= i; ++j) {
                    free(slots[j].child_idx);
                }
                free(slots);
                goto oom;
            }
            for (uint32_t c = 0; c < n->n_children; ++c) {
                s->child_idx[c] = pool[n->child_idx[c]].post_idx;
            }
        }
    }

    /* Free internal pool. */
    for (uint32_t i = 0; i < pool_n; ++i) free(pool[i].child_idx);
    free(pool);

    out->slots       = slots;
    out->n_slots     = pool_n;
    out->n_phrase    = 1;
    out->n_word      = in->n_words;
    out->n_syl       = in->n_syls;
    out->n_halfphone = 2u * in->n_segs;
    return SPFY_OK;

    /* fall-through to error labels not used here */

oom:
    for (uint32_t i = 0; i < pool_n; ++i) free(pool[i].child_idx);
    free(pool);
    return SPFY_E_NOMEM;
badf:
    for (uint32_t i = 0; i < pool_n; ++i) free(pool[i].child_idx);
    free(pool);
    return SPFY_E_FORMAT;
}

/* --------------------------------------------------------------------- */
/* LinkGraph                                                              */
/* --------------------------------------------------------------------- */

void spfy_slot_preds_table_free(spfy_slot_preds_table_t *t)
{
    if (!t) return;
    if (t->per_slot) {
        for (uint32_t i = 0; i < t->n_slots; ++i) free(t->per_slot[i].preds);
        free(t->per_slot);
    }
    memset(t, 0, sizeof *t);
}

/* Find the previous sibling of `slot` in its parent's child list, or
 * UINT32_MAX if none. */
static uint32_t prev_sibling(const spfy_slot_tree_t *t, uint32_t slot)
{
    uint32_t parent = t->slots[slot].parent_idx;
    if (parent == UINT32_MAX) return UINT32_MAX;
    const spfy_slot_node_t *p = &t->slots[parent];
    for (uint32_t i = 0; i < p->n_children; ++i) {
        if (p->child_idx[i] == slot) {
            return (i == 0) ? UINT32_MAX : p->child_idx[i - 1];
        }
    }
    return UINT32_MAX;
}

/* Build the exit chain: [P, P.last_child, P.last_child.last_child, ...]
 * outer-first, ending at a leaf. Caller-allocated dynamic array. */
static int build_exit_chain(const spfy_slot_tree_t *t, uint32_t root,
                            uint32_t **out_chain, uint32_t *out_n)
{
    uint32_t cap = 8, cnt = 0;
    uint32_t *arr = (uint32_t *)malloc(cap * sizeof *arr);
    if (!arr) return -1;
    uint32_t cur = root;
    for (;;) {
        if (cnt == cap) {
            cap *= 2;
            uint32_t *na = realloc(arr, cap * sizeof *arr);
            if (!na) { free(arr); return -1; }
            arr = na;
        }
        arr[cnt++] = cur;
        const spfy_slot_node_t *n = &t->slots[cur];
        if (n->n_children == 0) break;
        cur = n->child_idx[n->n_children - 1];   /* last child */
    }
    *out_chain = arr;
    *out_n     = cnt;
    return 0;
}

int spfy_link_graph(const spfy_slot_tree_t *tree,
                    spfy_slot_preds_table_t *out)
{
    if (!tree || !out) return SPFY_E_INVAL;
    memset(out, 0, sizeof *out);
    out->n_slots = tree->n_slots;
    out->per_slot = (spfy_slot_preds_t *)
        calloc(tree->n_slots, sizeof *out->per_slot);
    if (!out->per_slot) return SPFY_E_NOMEM;

    for (uint32_t s = 0; s < tree->n_slots; ++s) {
        spfy_slot_preds_t *r = &out->per_slot[s];
        r->preds = NULL;
        r->n_preds = 0;

        if (tree->slots[s].parent_idx == UINT32_MAX) {
            /* Root: no predecessors. */
            continue;
        }

        /* Walk up looking for first ancestor with a left sibling. */
        uint32_t cur = s;
        uint32_t left = UINT32_MAX;
        while (cur != UINT32_MAX) {
            uint32_t ls = prev_sibling(tree, cur);
            if (ls != UINT32_MAX) { left = ls; break; }
            cur = tree->slots[cur].parent_idx;
        }
        if (left == UINT32_MAX) {
            /* No left sibling anywhere up the chain -- this is the
             * leftmost-leaf path; no predecessors. */
            continue;
        }

        /* Exit chain of left sibling subtree. */
        uint32_t *chain = NULL;
        uint32_t  chain_n = 0;
        if (build_exit_chain(tree, left, &chain, &chain_n) != 0) {
            spfy_slot_preds_table_free(out);
            return SPFY_E_NOMEM;
        }
        r->preds   = chain;
        r->n_preds = chain_n;
    }
    return SPFY_OK;
}
