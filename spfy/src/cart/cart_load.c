/* CART loader: parses f0tr / durt chunks into spfy_cart_t. */

#include "cart.h"

#include "../common/riff.h"
#include "../common/log.h"
#include "../../include/spfy/spfy.h"

#include <stdlib.h>
#include <string.h>

#define FCC_TRHD SPFY_FOURCC('t','r','h','d')
#define FCC_LABL SPFY_FOURCC('l','a','b','l')
#define FCC_QUES SPFY_FOURCC('q','u','e','s')
#define FCC_TREE SPFY_FOURCC('t','r','e','e')

static uint32_t le_u32(const uint8_t *p)
{
    return (uint32_t)p[0]        | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static float le_f32(const uint8_t *p)
{
    union { uint32_t u; float f; } v;
    v.u = le_u32(p);
    return v.f;
}

static int parse_trhd(const uint8_t *data, size_t n, spfy_cart_t *c)
{
    spfy_riff_iter it;
    spfy_riff_iter_init(&it, data, n);
    spfy_chunk sc;
    int rc;
    while ((rc = spfy_riff_iter_next(&it, &sc)) == 1) {
        if (sc.fourcc == FCC_LABL) {
            if (sc.size < 4) return SPFY_E_FORMAT;
            c->n_labels = le_u32(sc.data);
            /* Strings follow but we don't need them keyed by name --
             * the engine and our CART evaluator both use phone-label
             * indices, not names. */
        } else if (sc.fourcc == FCC_QUES) {
            if (sc.size < 4) return SPFY_E_FORMAT;
            const uint8_t *p   = sc.data;
            const uint8_t *end = sc.data + sc.size;
            uint32_t n_q = le_u32(p); p += 4;
            spfy_ques_t *qs = (spfy_ques_t *)calloc(n_q, sizeof *qs);
            if (!qs) return SPFY_E_NOMEM;
            for (uint32_t i = 0; i < n_q; ++i) {
                if ((size_t)(end - p) < 1 + 4) { free(qs); return SPFY_E_FORMAT; }
                qs[i].type = (uint32_t)*p++;
                qs[i].n_values = le_u32(p); p += 4;
                if ((size_t)(end - p) < (size_t)qs[i].n_values * 4) {
                    free(qs); return SPFY_E_FORMAT;
                }
                qs[i].values = (const uint32_t *)p;
                p += qs[i].n_values * 4;
            }
            c->n_ques = n_q;
            c->ques   = qs;
        }
    }
    if (rc < 0) return SPFY_E_FORMAT;
    return SPFY_OK;
}

static int parse_one_tree(const uint8_t *data, size_t n,
                          spfy_cart_tree_t *t)
{
    if (n < 4) return SPFY_E_FORMAT;
    uint32_t n_nodes = le_u32(data);
    if (n_nodes == 0) {
        t->n_nodes = 0; t->nodes = NULL;
        return SPFY_OK;
    }
    /* Defensive cap: 100k nodes is more than any documented tree. */
    if (n_nodes > 100000u) return SPFY_E_FORMAT;

    spfy_cart_node_t *nodes = (spfy_cart_node_t *)calloc(n_nodes, sizeof *nodes);
    if (!nodes) return SPFY_E_NOMEM;

    const uint8_t *p   = data + 4;
    const uint8_t *end = data + n;
    for (uint32_t i = 0; i < n_nodes; ++i) {
        if ((size_t)(end - p) < 12) { free(nodes); return SPFY_E_FORMAT; }
        /* Common 12B prefix:
         *   u32  node_index   (dead, sequential)
         *   s32  yes_child    (>= 0: branch, child index; < 0: leaf marker)
         *   u32  no_child     (branch: child index; leaf: 0xFFFFFFFF sentinel) */
        uint32_t  dead       = le_u32(p);     (void)dead;        p += 4;
        int32_t   yes_child  = (int32_t)le_u32(p);                p += 4;
        uint32_t  no_child   = le_u32(p);                          p += 4;
        nodes[i].yes_child = yes_child;
        nodes[i].no_child  = no_child;
        if (yes_child >= 0) {
            /* Branch: 4 more bytes for q_index -> 16B total. */
            if ((size_t)(end - p) < 4) { free(nodes); return SPFY_E_FORMAT; }
            nodes[i].q_index = le_u32(p);                          p += 4;
            nodes[i].leaf_mean = 0.0f;
            nodes[i].leaf_var  = 0.0f;
        } else {
            /* Leaf: 8 more bytes for mean (f32) + variance (f32) -> 20B total. */
            if ((size_t)(end - p) < 8) { free(nodes); return SPFY_E_FORMAT; }
            nodes[i].q_index   = 0xFFFFFFFFu;
            nodes[i].leaf_mean = le_f32(p);                        p += 4;
            nodes[i].leaf_var  = le_f32(p);                        p += 4;
        }
    }
    t->n_nodes = n_nodes;
    t->nodes   = nodes;
    return SPFY_OK;
}

static int load_chunk(const uint8_t *chunk_data, size_t chunk_n,
                      uint32_t expected_n_trees, spfy_cart_t *out)
{
    memset(out, 0, sizeof *out);

    /* The chunk body is a sequence of nested sub-chunks: one trhd
     * followed by one or more tree chunks. */
    spfy_riff_iter it;
    spfy_riff_iter_init(&it, chunk_data, chunk_n);
    spfy_chunk sc;
    int rc;

    /* First pass: count tree subchunks, parse trhd. */
    uint32_t n_trees = 0;
    int saw_trhd = 0;
    while ((rc = spfy_riff_iter_next(&it, &sc)) == 1) {
        if (sc.fourcc == FCC_TRHD) {
            int prc = parse_trhd(sc.data, sc.size, out);
            if (prc != SPFY_OK) { spfy_cart_free(out); return prc; }
            saw_trhd = 1;
        } else if (sc.fourcc == FCC_TREE) {
            n_trees++;
        }
    }
    if (rc < 0)    { spfy_cart_free(out); return SPFY_E_FORMAT; }
    if (!saw_trhd) { spfy_cart_free(out); return SPFY_E_FORMAT; }
    if (expected_n_trees != 0 && n_trees != expected_n_trees) {
        spfy_log_err("cart: expected %u tree subchunks, found %u",
                     expected_n_trees, n_trees);
        spfy_cart_free(out);
        return SPFY_E_FORMAT;
    }

    out->n_trees = n_trees;
    out->trees   = (spfy_cart_tree_t *)calloc(n_trees, sizeof *out->trees);
    if (!out->trees) { spfy_cart_free(out); return SPFY_E_NOMEM; }

    /* Second pass: parse trees. */
    spfy_riff_iter_init(&it, chunk_data, chunk_n);
    uint32_t ti = 0;
    while (spfy_riff_iter_next(&it, &sc) == 1) {
        if (sc.fourcc != FCC_TREE) continue;
        int prc = parse_one_tree(sc.data, sc.size, &out->trees[ti++]);
        if (prc != SPFY_OK) { spfy_cart_free(out); return prc; }
    }
    return SPFY_OK;
}

int spfy_cart_load_f0tr(const spfy_vin_t *vin, spfy_cart_t *out)
{
    if (!vin || !out) return SPFY_E_INVAL;
    if (!vin->f0tr || vin->f0tr_n == 0) return SPFY_E_FORMAT;
    return load_chunk(vin->f0tr, vin->f0tr_n, 1u, out);
}

int spfy_cart_load_durt(const spfy_vin_t *vin, spfy_cart_t *out)
{
    if (!vin || !out) return SPFY_E_INVAL;
    if (!vin->durt || vin->durt_n == 0) return SPFY_E_FORMAT;
    return load_chunk(vin->durt, vin->durt_n, 47u, out);
}

void spfy_cart_free(spfy_cart_t *c)
{
    if (!c) return;
    if (c->trees) {
        for (uint32_t i = 0; i < c->n_trees; ++i) free(c->trees[i].nodes);
        free(c->trees);
    }
    free(c->ques);
    memset(c, 0, sizeof *c);
}
