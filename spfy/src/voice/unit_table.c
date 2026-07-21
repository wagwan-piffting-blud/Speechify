#include "unit_table.h"

#include "../common/riff.h"
#include "../common/log.h"
#include "../../include/spfy/spfy.h"

#include <string.h>

#define FCC_VERS SPFY_FOURCC('v','e','r','s')
#define FCC_DATA SPFY_FOURCC('d','a','t','a')

#define OFF_ABSENT 0xFFu

/* Per-version record geometry. Bytes +0x00..+0x0F are common to all
 * versions; only the tail differs. See unit_table.h for the derivation. */
typedef struct {
    uint32_t version;
    uint8_t  rec_size;
    uint8_t  phone_in_syl;
    uint8_t  f0_start;      /* f0_end/f0_mid/f0_context follow at +1/+2/+3 */
    uint8_t  phone_center;
    uint8_t  is_first_half;
    uint8_t  phone_ctx;     /* first of 4 */
    uint8_t  flag_b;
    uint8_t  context_cost;
} unit_layout_t;

/* Every record is a 12-byte fixed prefix (+0x00..+0x0B) followed by a
 * variable block whose size the engine computes as
 *
 *     11  + (version >= 100007 ? 1 : 0)      // phone_in_syl present
 *         + (version == 100006 ||
 *            version == 100008 ? 5 : 0)      // phone_ctx[4] + flag_b present
 *         + (version >= 100005 ? 1 : 0)      // context_cost present
 *
 * giving strides 23/24/29/25/30 for 100004/5/6/7/8. Transcribed from the
 * decompiled loader `load_chunky_index` (FUN_08e857a0); see
 * spfy/usel-decomp/reports/tier3/08e857a0_FUN_08e857a0.md.
 *
 * NB: README_TECHNICAL.md's "Mapping for version 100007+" claims the F0
 * fields take on shifted MEANINGS (that in-memory +0x0F becomes f0_end).
 * That is wrong. The loader writes the new phone_in_syl byte to in-memory
 * +0x0E and then advances its read pointer by one, so every later field
 * keeps its semantics and merely moves one byte later on disk. Confirmed
 * both by the decompile and empirically: Jill's disk +0x11 carries
 * f0_start's ~25% zero-rate signature, not f0_end's. */
static const unit_layout_t UNIT_LAYOUTS[] = {
    /* No voice we ship uses 100004 or 100007; both are transcribed from the
     * loader for completeness and are untested against real data. */
    { 100004u, 23u, OFF_ABSENT, 0x10u, 0x14u, 0x15u, OFF_ABSENT,
      OFF_ABSENT, OFF_ABSENT },
    /* Paulina. No phone_ctx[4] and no flag_b on disk; the loader forces
     * flag_b = 1 for every unit. +0x16 is a small-integer field (1..12)
     * that the loader skips outright. */
    { 100005u, 24u, OFF_ABSENT, 0x10u, 0x14u, 0x15u, OFF_ABSENT,
      OFF_ABSENT, 0x17u },
    /* Tom, Felix, Javier, aimara, aicraig. +0x16 is a per-voice constant
     * (Tom 3, Jill 3, Javier 4, Felix 5) that the loader skips. */
    { 100006u, 29u, OFF_ABSENT, 0x10u, 0x14u, 0x15u, 0x17u, 0x1Bu, 0x1Cu },
    { 100007u, 25u, 0x10u,      0x11u, 0x15u, 0x16u, OFF_ABSENT,
      OFF_ABSENT, 0x18u },
    /* Jill. One byte inserted at +0x10 (phoneInSylCosts column); every
     * field from f0_start onward shifts by +1. */
    { 100008u, 30u, 0x10u,      0x11u, 0x15u, 0x16u, 0x18u, 0x1Cu, 0x1Du },
};

/* v100006 voices have no on-disk phoneInSyl column. 6 == "SyllUnknown",
 * matching the constant the engine passes for these voices. Inert in
 * practice: their VCFs ship no phoneInSylCosts matrix and their
 * PHONE_IN_SYL_MISMATCH_COST weight is 0. */
#define PHONE_IN_SYL_DEFAULT 6u

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

    /* The 'unit' chunk wraps {vers, data}. Version must be read before the
     * data size can be validated, so stash the data chunk and resolve the
     * layout afterwards -- chunk order within 'unit' is not guaranteed. */
    spfy_riff_iter it;
    spfy_riff_iter_init(&it, vin->unit, vin->unit_n);
    spfy_chunk c;
    int rc;
    const uint8_t *data_ptr = NULL;
    uint32_t       data_n   = 0;
    while ((rc = spfy_riff_iter_next(&it, &c)) == 1) {
        if (c.fourcc == FCC_VERS) {
            if (c.size < 4) return SPFY_E_FORMAT;
            out->version = le_u32(c.data);
        } else if (c.fourcc == FCC_DATA) {
            data_ptr = c.data;
            data_n   = c.size;
        }
    }
    if (rc < 0)            return SPFY_E_FORMAT;
    if (out->version == 0) return SPFY_E_FORMAT;
    if (data_ptr == NULL)  return SPFY_E_FORMAT;

    const unit_layout_t *L = NULL;
    for (size_t i = 0; i < sizeof UNIT_LAYOUTS / sizeof *UNIT_LAYOUTS; ++i) {
        if (UNIT_LAYOUTS[i].version == out->version) { L = &UNIT_LAYOUTS[i]; break; }
    }
    if (!L) {
        spfy_log_err("unit_table: unsupported version %u "
                     "(known: 100005, 100006, 100008)", out->version);
        return SPFY_E_NOTSUP;
    }
    if (data_n % L->rec_size != 0) {
        spfy_log_err("unit/data: %u bytes not divisible by %u (version %u)",
                     data_n, L->rec_size, out->version);
        return SPFY_E_FORMAT;
    }

    out->data     = data_ptr;
    out->data_n   = data_n;
    out->rec_size = L->rec_size;
    out->n_units  = data_n / L->rec_size;

    out->off_phone_in_syl  = L->phone_in_syl;
    out->off_f0_start      = L->f0_start;
    out->off_phone_center  = L->phone_center;
    out->off_is_first_half = L->is_first_half;
    out->off_phone_ctx     = L->phone_ctx;
    out->off_flag_b        = L->flag_b;
    out->off_context_cost  = L->context_cost;
    return SPFY_OK;
}

int spfy_unit_record_get(const spfy_unit_table_t *t, uint32_t uid,
                         spfy_unit_record_t *out)
{
    if (!t || !out)             return SPFY_E_INVAL;
    if (uid >= t->n_units)      return SPFY_E_OOB;

    const uint8_t *p = t->data + (size_t)uid * t->rec_size;
    /* The on-disk +0x00 unit_id is verified against uid as a sanity check
     * since it should equal the array index per README_TECHNICAL. */
    uint32_t stored_uid = le_u32(p + 0x00);
    if (stored_uid != uid) {
        spfy_log_err("unit_table[%u]: on-disk unit_id=%u (mismatch)",
                     uid, stored_uid);
        return SPFY_E_FORMAT;
    }

    /* +0x00..+0x0F are identical across all record versions. */
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

    /* Version-dependent tail. */
    out->sp_phone_in_syl = (t->off_phone_in_syl == OFF_ABSENT)
                         ? (uint8_t)PHONE_IN_SYL_DEFAULT
                         : p[t->off_phone_in_syl];

    const uint8_t *f0 = p + t->off_f0_start;
    out->f0_start       = f0[0];
    out->f0_end         = f0[1];
    out->f0_mid         = f0[2];
    out->f0_context     = f0[3];

    out->phone_center   = p[t->off_phone_center];
    out->is_first_half  = p[t->off_is_first_half];
    /* The byte between is_first_half and phone_ctx[] is a per-voice constant
     * (Tom 3, Javier 4, Felix 5; Jill 3) -- not exposed. The engine only
     * tests it for != 0 in FUN_08e89530, so the gate is degenerate: it
     * always passes, meaning the syl-advance gate is effectively "not first
     * iter && local_34 < last_hp". The advance walks through
     * syl_idx_per_hp[] until the value changes from the prior local_10.
     * Our code previously had an `is_first_half != 0` gate that caused
     * under-advancing for multi-syllable spans. */

    if (t->off_phone_ctx == OFF_ABSENT) {
        /* v100005 stores no phone context. 255 is the engine's "no context"
         * sentinel, which the S-cost treats as a free match. */
        out->phone_ctx[0] = out->phone_ctx[1] = 255u;
        out->phone_ctx[2] = out->phone_ctx[3] = 255u;
    } else {
        const uint8_t *cx = p + t->off_phone_ctx;
        out->phone_ctx[0] = cx[0];
        out->phone_ctx[1] = cx[1];
        out->phone_ctx[2] = cx[2];
        out->phone_ctx[3] = cx[3];
    }
    /* When flag_b is absent the loader stores a literal 1 for every unit
     * (`*(undefined *)(psVar17 + 0xb) = 1` in load_chunky_index), NOT 0 --
     * so on v100005 the "same recording, consecutive uid" zero-join
     * shortcut loses its discriminating power rather than never firing. */
    out->flag_b       = (t->off_flag_b == OFF_ABSENT)
                      ? 1u : p[t->off_flag_b];
    out->context_cost = (t->off_context_cost == OFF_ABSENT)
                      ? 0u : p[t->off_context_cost];
    return SPFY_OK;
}
