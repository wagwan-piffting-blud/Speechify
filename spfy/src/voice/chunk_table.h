#ifndef SPFY_VOICE_CHUNK_TABLE_H
#define SPFY_VOICE_CHUNK_TABLE_H

#include <stddef.h>
#include <stdint.h>

#include "voice.h"  /* spfy_vin_t */

/* ckls + cklx chunk-table loader for PostScoringAdj (Phase B4.4).
 *
 * cklx: inverted index `key_text -> [posting_id, ...]` per group
 *       (groups: "_WORD_", "_SYL_").
 * ckls: per-posting metadata `posting_id -> (span_start, span_end,
 *       token_text)`.
 *
 * For Tom: cklx._WORD_ has 1075 keys / 5108 postings; cklx._SYL_ has
 * 1350 keys / 7918 postings.
 */

typedef struct {
    /* sorted ASCII keys (heap, owned). */
    char    **keys;
    uint32_t  n_keys;
    /* For each key, a posting list:
     *   postings_offset[i]  -> first posting in flat postings[] array
     *   postings_offset[i+1]-> end (exclusive). One past last posting.
     */
    uint32_t *postings_offset;     /* length n_keys + 1 */
    uint32_t *postings;            /* flat array of posting IDs */
    uint32_t  n_postings;          /* total postings (= postings_offset[n_keys]) */
} spfy_cklx_group_t;

typedef struct {
    /* Per-posting (span_start, span_end). Indexed by posting_id. */
    uint32_t *span_start;
    uint32_t *span_end;
    /* Per-posting token text. Heap, owned. */
    char    **token_text;
    uint32_t  n_postings;
} spfy_ckls_group_t;

typedef struct {
    /* Two groups: index 0 = _WORD_, 1 = _SYL_. */
    spfy_cklx_group_t cklx[2];
    spfy_ckls_group_t ckls[2];
} spfy_chunk_tables_t;

#define SPFY_CHUNK_GROUP_WORD 0u
#define SPFY_CHUNK_GROUP_SYL  1u

int  spfy_chunk_tables_load(const spfy_vin_t *vin,
                            spfy_chunk_tables_t *out);
void spfy_chunk_tables_free(spfy_chunk_tables_t *t);

/* Lookup by key (binary search). Returns posting count + writes
 * postings array offset into *out_first / *out_count. Returns 0 on
 * miss. */
int  spfy_cklx_lookup(const spfy_cklx_group_t *g,
                      const char *key,
                      const uint32_t **out_postings,
                      uint32_t *out_count);

#endif
