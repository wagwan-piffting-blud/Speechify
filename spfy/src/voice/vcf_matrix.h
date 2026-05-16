#ifndef SPFY_VOICE_VCF_MATRIX_H
#define SPFY_VOICE_VCF_MATRIX_H

#include <stddef.h>
#include <stdint.h>

#include "voice.h"   /* spfy_vcf_t */

/* VCF proscost matrix loader.
 *
 * The proscost matrices are stored in tom.vcf as nested namedValue
 * structures, one <param> per matrix row:
 *
 *   <param name="tts.voiceCfg.proscost.<Matrix>.<RowName>">
 *     <namedValue name="<Col0>"> 100 </namedValue>
 *     <namedValue name="<Col1>"> 0   </namedValue>
 *     ...
 *   </param>
 *
 * Both row and column orderings are CANONICAL (every row has the same
 * column-name list in the same order); the engine's enum maps name -> 1-
 * based index. Our loader stores matrices as 0-indexed flat row-major
 * f32 arrays (heap-allocated) and exposes the canonical name list so the
 * caller can do name -> index lookups when needed.
 *
 * Matrix sizes per Tom (verified empirically + per README_TECHNICAL "SP"
 * section):
 *   sylInPhraseCosts    10 x 10
 *   sylTypeCosts         9 x 9
 *   wordInPhraseCosts    7 x 7
 *   sylInWordCosts       7 x 7
 *   phoneInSylCosts      7 x 7  (degenerate for Tom; col idx hardcoded 6)
 */

typedef struct {
    float    *data;          /* heap, row-major, owned */
    uint32_t  n_rows;
    uint32_t  n_cols;
    char    **row_names;     /* heap, n_rows entries, each owned */
    char    **col_names;     /* heap, n_cols entries, each owned */
} spfy_proscost_matrix_t;

typedef enum {
    SPFY_PROSCOST_SYL_IN_PHRASE = 0,
    SPFY_PROSCOST_SYL_TYPE      = 1,
    SPFY_PROSCOST_WORD_IN_PHRASE= 2,
    SPFY_PROSCOST_SYL_IN_WORD   = 3,
    SPFY_PROSCOST_PHONE_IN_SYL  = 4,
    SPFY_PROSCOST_N             = 5,
} spfy_proscost_kind_t;

/* Load all 5 proscost matrices from a VCF. Out parameter is filled with
 * heap-allocated matrices; free via spfy_proscost_free(). Missing matrices
 * are returned with data=NULL and n_rows/n_cols=0 (e.g. phoneInSylCosts is
 * absent in some Tom builds). */
int  spfy_proscost_load(const spfy_vcf_t *vcf,
                        spfy_proscost_matrix_t out[SPFY_PROSCOST_N]);
void spfy_proscost_free(spfy_proscost_matrix_t mats[SPFY_PROSCOST_N]);

/* Lookup a column index by name; returns 0..n_cols-1 on hit, or -1. */
int  spfy_proscost_col_idx(const spfy_proscost_matrix_t *m, const char *name);

#endif
