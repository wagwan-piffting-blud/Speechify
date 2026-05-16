#include "unit_table.h"

#include "../common/riff.h"
#include "../common/log.h"
#include "../../include/spfy/spfy.h"

#include <string.h>

#define FCC_VERS SPFY_FOURCC('v','e','r','s')
#define FCC_DATA SPFY_FOURCC('d','a','t','a')

#define UNIT_RECORD_SIZE 29u

static uint16_t le_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le_u32(const uint8_t *p)
{
    return (uint32_t)p[0]        | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int spfy_unit_table_load(const spfy_vin_t *vin, spfy_unit_table_t *out)
{
    if (!vin || !out) return SPFY_E_INVAL;
    if (!vin->unit || vin->unit_n == 0) return SPFY_E_FORMAT;
    memset(out, 0, sizeof *out);

    /* The 'unit' chunk wraps {vers, data}. */
    spfy_riff_iter it;
    spfy_riff_iter_init(&it, vin->unit, vin->unit_n);
    spfy_chunk c;
    int rc;
    while ((rc = spfy_riff_iter_next(&it, &c)) == 1) {
        if (c.fourcc == FCC_VERS) {
            if (c.size < 4) return SPFY_E_FORMAT;
            out->version = le_u32(c.data);
        } else if (c.fourcc == FCC_DATA) {
            if (c.size % UNIT_RECORD_SIZE != 0) {
                spfy_log_err("unit/data: %u bytes not divisible by %u",
                             c.size, UNIT_RECORD_SIZE);
                return SPFY_E_FORMAT;
            }
            out->data    = c.data;
            out->data_n  = c.size;
            out->n_units = c.size / UNIT_RECORD_SIZE;
        }
    }
    if (rc < 0)            return SPFY_E_FORMAT;
    if (out->version == 0) return SPFY_E_FORMAT;
    if (out->data == NULL) return SPFY_E_FORMAT;

    /* Currently only 100006 (Tom/Mara) is wired up; v100007+ has a shifted
     * f0_start mapping (see README_TECHNICAL "Mapping for version 100007+").
     * Reject other versions early so callers don't get silently-wrong
     * cost values. */
    if (out->version != 100006u) {
        spfy_log_err("unit_table: unsupported version %u (only 100006 wired)",
                     out->version);
        return SPFY_E_NOTSUP;
    }
    return SPFY_OK;
}

int spfy_unit_record_get(const spfy_unit_table_t *t, uint32_t uid,
                         spfy_unit_record_t *out)
{
    if (!t || !out)             return SPFY_E_INVAL;
    if (uid >= t->n_units)      return SPFY_E_OOB;

    const uint8_t *p = t->data + (size_t)uid * UNIT_RECORD_SIZE;
    /* The on-disk +0x00 unit_id is verified against uid as a sanity check
     * since it should equal the array index per README_TECHNICAL. */
    uint32_t stored_uid = le_u32(p + 0x00);
    if (stored_uid != uid) {
        spfy_log_err("unit_table[%u]: on-disk unit_id=%u (mismatch)",
                     uid, stored_uid);
        return SPFY_E_FORMAT;
    }

    out->file_idx       = le_u16(p + 0x04);
    out->local_pos      = le_u16(p + 0x06);
    /* +0x08 always 0 -- not exposed */
    out->dur_like       = le_u16(p + 0x0A);
    /* +0x0C..+0x0F: SP feature bytes. Order CORRECTED 2026-04-19 via
     * empirical distribution sweep -- see README_TECHNICAL.md "SP" section. */
    out->sp_syl_in_phrase  = p[0x0C];
    out->sp_syl_type       = p[0x0D];
    out->sp_word_in_phrase = p[0x0E];
    out->sp_syl_in_word    = p[0x0F];
    out->f0_start       =          p[0x10];
    out->f0_end         =          p[0x11];
    out->f0_mid         =          p[0x12];
    out->f0_context     =          p[0x13];
    out->phone_center   =          p[0x14];
    out->is_first_half  =          p[0x15];
    /* +0x16 constant=3 -- not exposed (confirmed 2026-05-14: every unit in
     * Tom corpus has this byte=3, so engine's byte!=0 check in FUN_08e89530
     * is degenerate: it always passes, meaning engine's syl-advance gate is
     * effectively "not first iter && local_34 < last_hp". The advance walks
     * through syl_idx_per_hp[] until the value changes from the prior
     * local_10. Our code previously had an `is_first_half != 0` gate that
     * caused under-advancing for multi-syllable spans. */
    out->phone_ctx[0]   =          p[0x17];
    out->phone_ctx[1]   =          p[0x18];
    out->phone_ctx[2]   =          p[0x19];
    out->phone_ctx[3]   =          p[0x1A];
    out->flag_b         =          p[0x1B];
    out->context_cost   =          p[0x1C];
    return SPFY_OK;
}
