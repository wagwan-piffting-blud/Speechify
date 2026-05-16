/* spfy_dump_f0tr_ques — quick diagnostic dumping the f0tr CART's
 * question types and value-sets. Used to identify which q_types our
 * cart_feat callback needs to expose. */
#include "../../include/spfy/spfy.h"
#include "../cart/cart.h"
#include "../voice/vin.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <vin>\n", argv[0]); return 1; }
    spfy_vin_t vin = {0};
    spfy_cart_t f0tr = {0};
    if (spfy_vin_load(&vin, argv[1]) != SPFY_OK) {
        fprintf(stderr, "vin load failed\n"); return 2;
    }
    if (spfy_cart_load_f0tr(&vin, &f0tr) != SPFY_OK) {
        fprintf(stderr, "f0tr load failed\n"); return 3;
    }
    printf("f0tr: %u trees, %u questions\n", f0tr.n_trees, f0tr.n_ques);

    /* Histogram of q_types referenced by any branch node in any tree. */
    uint32_t qt_count[256] = {0};
    for (uint32_t t = 0; t < f0tr.n_trees; ++t) {
        const spfy_cart_tree_t *tr = &f0tr.trees[t];
        for (uint32_t i = 0; i < tr->n_nodes; ++i) {
            const spfy_cart_node_t *n = &tr->nodes[i];
            if (n->yes_child < 0) continue;     /* leaf */
            if (n->q_index >= f0tr.n_ques) continue;
            uint8_t qt = (uint8_t)f0tr.ques[n->q_index].type;
            qt_count[qt]++;
        }
    }
    printf("q_type usage across branch nodes:\n");
    for (int q = 0; q < 256; ++q)
        if (qt_count[q] > 0)
            printf("  q_type %d: %u branches\n", q, qt_count[q]);

    /* For each unique q_type, dump first ques's values to give a flavor. */
    for (int qt = 1; qt < 256; ++qt) {
        if (qt_count[qt] == 0) continue;
        for (uint32_t q = 0; q < f0tr.n_ques; ++q) {
            if (f0tr.ques[q].type != (uint32_t)qt) continue;
            printf("  q_type %d first instance (q_index=%u): n_values=%u  vals=[",
                   qt, q, f0tr.ques[q].n_values);
            for (uint32_t v = 0; v < f0tr.ques[q].n_values && v < 10; ++v) {
                printf("%s%u", v ? "," : "", f0tr.ques[q].values[v]);
            }
            if (f0tr.ques[q].n_values > 10) printf(",...");
            printf("]\n");
            break;
        }
    }
    return 0;
}
