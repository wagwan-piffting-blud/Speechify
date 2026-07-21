#ifndef SPFY_VOICE_UNIT_TABLE_H
#define SPFY_VOICE_UNIT_TABLE_H

#include <stddef.h>
#include <stdint.h>

#include "voice.h"   /* spfy_vin_t */

/* Decoded per-unit record. Holds the raw fields the engine reads from each
 * on-disk row. Three record versions are known, differing only in the tail:
 *
 *   v100005  24 bytes  (Paulina)         no phone_ctx[], no flag_b
 *   v100006  29 bytes  (Tom/Felix/Javier/aimara/aicraig)
 *   v100008  30 bytes  (Jill)            adds sp_phone_in_syl at +0x10
 *
 * Bytes +0x00..+0x0F are identical in all three. v100008 inserts ONE byte
 * at +0x10 (the phoneInSylCosts column index -- the 5th proscost matrix,
 * which v100006 voices have no column source for) and shifts every
 * subsequent field by +1. Verified 2026-07-20 by column-distribution
 * alignment across all five shipped voices; see reveng/README_TECHNICAL.md
 * "unit record versions".
 *
 * Field semantics + cost-component mapping per
 * reveng/README_TECHNICAL.md "unit" section and
 * reveng/DLL_ANALYSIS.md "Duration scoring" / "F0 scoring".
 *
 * The engine repacks these into 24-byte in-memory records and looks them
 * up by unit_id (= array index). The on-disk pad byte (+0x16 in v100006)
 * and the unit_id field itself are dropped (unit_id is implicit in the
 * array index). */

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
    /* phoneInSylCosts col. Present on disk only in v100008 (at +0x10);
     * synthesised as 6 ("SyllUnknown") for older versions, which is inert
     * because those voices ship no phoneInSylCosts matrix and their
     * PHONE_IN_SYL_MISMATCH_COST weight is 0. Column vocabulary:
     *   0 UNDEF  1 WordInitial  2 SyllInitial  3 SyllMedial
     *   4 SyllFinal  5 WordFinal  6 SyllUnknown */
    uint8_t   sp_phone_in_syl;
    /* pitch fields. f0_start is stored_f0 for the F0-cost. */
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
    uint32_t              rec_size;    /* 24 / 29 / 30 by version */
    /* Lazy decode: we read the on-disk bytes directly via VIN's mmap'd
     * buffer rather than copying. The decoder is bit-cheap. */
    const uint8_t        *data;        /* points into VIN buffer */
    size_t                data_n;
    /* Byte offsets of the version-dependent tail fields. 0xFF = the field
     * is absent in this version and the decoder substitutes a default. */
    uint8_t               off_phone_in_syl;
    uint8_t               off_f0_start;
    uint8_t               off_phone_center;
    uint8_t               off_is_first_half;
    uint8_t               off_phone_ctx;    /* first of 4 */
    uint8_t               off_flag_b;
    uint8_t               off_context_cost;
} spfy_unit_table_t;

/* Parse the 'unit' chunk into a table view. The VIN buffer must outlive
 * the table. Returns SPFY_OK or SPFY_E_*. */
int  spfy_unit_table_load(const spfy_vin_t *vin, spfy_unit_table_t *out);

/* Decode one record by unit_id (= array index). Returns SPFY_OK or
 * SPFY_E_OOB. */
int  spfy_unit_record_get(const spfy_unit_table_t *t, uint32_t uid,
                          spfy_unit_record_t *out);

#endif
