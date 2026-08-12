#ifndef SPFY_VOICE_VCF_MATRIX_H
#define SPFY_VOICE_VCF_MATRIX_H

#include <stddef.h>
#include <stdint.h>

#include "voice.h"

/* VCF proscost matrix loader. */

typedef struct {
    float    *data;
    uint32_t  n_rows;
    uint32_t  n_cols;
    char    **row_names;
    char    **col_names;
} spfy_proscost_matrix_t;

typedef enum {
    SPFY_PROSCOST_SYL_IN_PHRASE = 0,
    SPFY_PROSCOST_SYL_TYPE      = 1,
    SPFY_PROSCOST_WORD_IN_PHRASE= 2,
    SPFY_PROSCOST_SYL_IN_WORD   = 3,
    SPFY_PROSCOST_PHONE_IN_SYL  = 4,
    SPFY_PROSCOST_N             = 5,
} spfy_proscost_kind_t;

/* Load all 5 proscost matrices from a VCF. */
int  spfy_proscost_load(const spfy_vcf_t *vcf,
                        spfy_proscost_matrix_t out[SPFY_PROSCOST_N]);
void spfy_proscost_free(spfy_proscost_matrix_t mats[SPFY_PROSCOST_N]);

int  spfy_proscost_col_idx(const spfy_proscost_matrix_t *m, const char *name);

#endif
