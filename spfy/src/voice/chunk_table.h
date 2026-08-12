#ifndef SPFY_VOICE_CHUNK_TABLE_H
#define SPFY_VOICE_CHUNK_TABLE_H

#include <stddef.h>
#include <stdint.h>

#include "voice.h"

/* ckls + cklx chunk-table loader for PostScoringAdj (Phase B4.4). */

typedef struct {
    char    **keys;
    uint32_t  n_keys;
    /* For each key, a posting list: postings_offset[i] -> first posting in
     * flat postings[] array postings_offset[i+1]-> end (exclusive). */
    uint32_t *postings_offset;
    uint32_t *postings;
    uint32_t  n_postings;
} spfy_cklx_group_t;

typedef struct {
    uint32_t *span_start;
    uint32_t *span_end;
    char    **token_text;
    uint32_t  n_postings;
} spfy_ckls_group_t;

typedef struct {
    spfy_cklx_group_t cklx[2];
    spfy_ckls_group_t ckls[2];
} spfy_chunk_tables_t;

#define SPFY_CHUNK_GROUP_WORD 0u
#define SPFY_CHUNK_GROUP_SYL  1u

int  spfy_chunk_tables_load(const spfy_vin_t *vin,
                            spfy_chunk_tables_t *out);
void spfy_chunk_tables_free(spfy_chunk_tables_t *t);

/* Lookup by key (binary search). */
int  spfy_cklx_lookup(const spfy_cklx_group_t *g,
                      const char *key,
                      const uint32_t **out_postings,
                      uint32_t *out_count);

#endif
