/* PRSL preselection cache loader + lookup. */

#include "prsl.h"

#include "../common/log.h"
#include "../../include/spfy/spfy.h"

#include <stdlib.h>
#include <string.h>

static uint32_t le_u32(const uint8_t *p)
{
    return (uint32_t)p[0]        | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int spfy_prsl_load(const spfy_vin_t *vin, spfy_prsl_t *out)
{
    if (!vin || !out) return SPFY_E_INVAL;
    if (!vin->prsl || vin->prsl_n == 0) return SPFY_E_FORMAT;
    memset(out, 0, sizeof *out);

    const uint8_t *p   = vin->prsl;
    const uint8_t *end = vin->prsl + vin->prsl_n;
    if ((size_t)(end - p) < 4) return SPFY_E_FORMAT;

    uint32_t group_count = le_u32(p); p += 4;
    if (group_count == 0) return SPFY_OK;
    /* Defensive cap: 10M groups is well above any documented voice. */
    if (group_count > 10u * 1024u * 1024u) return SPFY_E_FORMAT;

    spfy_prsl_group_t *groups =
        (spfy_prsl_group_t *)calloc(group_count, sizeof *groups);
    if (!groups) return SPFY_E_NOMEM;

    uint32_t prev_key = 0;
    int      check_monotonic = 1;
    for (uint32_t g = 0; g < group_count; ++g) {
        if ((size_t)(end - p) < 8) {
            free(groups); return SPFY_E_FORMAT;
        }
        uint32_t n = le_u32(p);     p += 4;
        if (n < 1) {
            free(groups);
            spfy_log_err("prsl: group %u has n=0", g);
            return SPFY_E_FORMAT;
        }
        uint32_t key = le_u32(p);   p += 4;

        /* (n - 1) candidate_ids follow */
        uint32_t n_cand = n - 1u;
        if ((size_t)(end - p) < (size_t)n_cand * 4u) {
            free(groups); return SPFY_E_FORMAT;
        }
        groups[g].context_key  = key;
        groups[g].n_candidates = n_cand;
        groups[g].candidates   = (const uint32_t *)p;
        p += n_cand * 4u;

        if (g > 0 && key <= prev_key) check_monotonic = 0;
        prev_key = key;
    }

    if (!check_monotonic) {
        spfy_log_warn("prsl: context_keys are not strictly monotonic; "
                      "binary search may misbehave");
    }
    out->groups   = groups;
    out->n_groups = group_count;
    return SPFY_OK;
}

void spfy_prsl_free(spfy_prsl_t *p)
{
    if (!p) return;
    free(p->groups);
    memset(p, 0, sizeof *p);
}

int spfy_prsl_lookup(const spfy_prsl_t *p, uint32_t context_key,
                     const uint32_t **cands, uint32_t *n_cands)
{
    if (!p || !cands || !n_cands) return SPFY_E_INVAL;
    *cands = NULL; *n_cands = 0;
    if (p->n_groups == 0) return SPFY_E_OOB;

    /* Binary search on monotonically-increasing context_key. */
    uint32_t lo = 0, hi = p->n_groups;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        uint32_t k = p->groups[mid].context_key;
        if      (k == context_key) {
            *cands   = p->groups[mid].candidates;
            *n_cands = p->groups[mid].n_candidates;
            return SPFY_OK;
        }
        else if (k <  context_key) lo = mid + 1;
        else                       hi = mid;
    }
    return SPFY_E_OOB;
}
