#ifndef SPFY_VOICE_UNIT_TABLE_H
#define SPFY_VOICE_UNIT_TABLE_H

#include <stddef.h>
#include <stdint.h>

#include "voice.h"   /* spfy_vin_t */

/* Decoded per-unit record. Holds the 17 raw fields the engine reads from
 * each 29-byte on-disk row (unit version 100006, used by Tom + Mara).
 *
 * Field semantics + cost-component mapping per
 * reveng/README_TECHNICAL.md "unit" section and
 * reveng/DLL_ANALYSIS.md "Duration scoring" / "F0 scoring".
 *
 * The engine repacks these into 24-byte in-memory records and looks them
 * up by unit_id (= array index). The on-disk +0x16 constant=3 byte and the
 * unit_id field itself are dropped (unit_id is implicit in array index). */

typedef struct {
    /* +0x04 file_idx: index into VIN feat[] for the recording name. */
    uint16_t  file_idx;
    /* +0x06 local_pos: byte offset / 8 = ms within recording. */
    uint16_t  local_pos;
    /* +0x0A dur_like: this unit's duration (ms). Used by D-cost as
     *   stored_dur in `score = |stddev * (stored_dur - durt_pred)|^2`. */
    uint16_t  dur_like;
    /* +0x0C..+0x0F: SP-cost feature bytes (column indices into the 5
     * proscost matrices). Names per the empirically-verified SP analysis
     * in README_TECHNICAL.md "SP (position-mismatch) matrices" section,
     * NOT the older "unit" section labels which had +0x0C and +0x0D
     * swapped. */
    uint8_t   sp_syl_in_phrase;   /* +0x0C: sylInPhraseCosts col */
    uint8_t   sp_syl_type;        /* +0x0D: sylTypeCosts col */
    uint8_t   sp_word_in_phrase;  /* +0x0E: wordInPhraseCosts col */
    uint8_t   sp_syl_in_word;     /* +0x0F: sylInWordCosts col */
    /* +0x10..+0x13: pitch fields. f0_start used as stored_f0 by F0-cost
     *   (v100006); f0_end is used in v100007+. */
    uint8_t   f0_start;
    uint8_t   f0_end;
    uint8_t   f0_mid;
    uint8_t   f0_context;
    /* +0x14..+0x15: phone identity / boundary side. */
    uint8_t   phone_center;
    uint8_t   is_first_half;
    /* +0x17..+0x1A: 4-deep phone context, sentinel 255 = none. */
    uint8_t   phone_ctx[4];
    /* +0x1B..+0x1C: prosodic flags. */
    uint8_t   flag_b;
    uint8_t   context_cost;
} spfy_unit_record_t;

typedef struct {
    uint32_t              version;     /* from unit/vers chunk */
    uint32_t              n_units;
    /* Lazy decode: we read the on-disk bytes directly via VIN's mmap'd
     * buffer rather than copying. The decoder is bit-cheap. */
    const uint8_t        *data;        /* points into VIN buffer */
    size_t                data_n;
} spfy_unit_table_t;

/* Parse the 'unit' chunk into a table view. The VIN buffer must outlive
 * the table. Returns SPFY_OK or SPFY_E_*. */
int  spfy_unit_table_load(const spfy_vin_t *vin, spfy_unit_table_t *out);

/* Decode one record by unit_id (= array index). Returns SPFY_OK or
 * SPFY_E_OOB. */
int  spfy_unit_record_get(const spfy_unit_table_t *t, uint32_t uid,
                          spfy_unit_record_t *out);

#endif
