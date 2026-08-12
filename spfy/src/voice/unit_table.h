#ifndef SPFY_VOICE_UNIT_TABLE_H
#define SPFY_VOICE_UNIT_TABLE_H

#include <stddef.h>
#include <stdint.h>

#include "voice.h"

/* Decoded per-unit record. */

typedef struct {
    uint16_t  file_idx;
    uint16_t  local_pos;
    /* +0x0A dur_like: this unit's duration (ms). */
    uint16_t  dur_like;
    /* +0x0C..+0x0F: SP-cost feature bytes (column indices into the 5
     * proscost matrices). */
    uint8_t   sp_syl_in_phrase;
    uint8_t   sp_syl_type;
    uint8_t   sp_word_in_phrase;
    uint8_t   sp_syl_in_word;
    /* phoneInSylCosts col. */
    uint8_t   sp_phone_in_syl;
    uint8_t   f0_start;
    uint8_t   f0_end;
    uint8_t   f0_mid;
    uint8_t   f0_context;
    uint8_t   phone_center;
    uint8_t   is_first_half;
    uint8_t   phone_ctx[4];
    uint8_t   flag_b;
    uint8_t   context_cost;
} spfy_unit_record_t;

typedef struct {
    uint32_t              version;
    uint32_t              n_units;
    uint32_t              rec_size;
    /* Lazy decode: we read the on-disk bytes directly via VIN's mmap'd
     * buffer rather than copying. */
    const uint8_t        *data;
    size_t                data_n;
    /* Byte offsets of the version-dependent tail fields. */
    uint8_t               off_phone_in_syl;
    uint8_t               off_f0_start;
    uint8_t               off_phone_center;
    uint8_t               off_is_first_half;
    uint8_t               off_phone_ctx;
    uint8_t               off_flag_b;
    uint8_t               off_context_cost;
} spfy_unit_table_t;

/* Parse the 'unit' chunk into a table view. */
int  spfy_unit_table_load(const spfy_vin_t *vin, spfy_unit_table_t *out);

/* Decode one record by unit_id (= array index). */
int  spfy_unit_record_get(const spfy_unit_table_t *t, uint32_t uid,
                          spfy_unit_record_t *out);

#endif
