/* feat chunk loader.
 *
 * The feat chunk is a DICTIONARY of (key -> list of values), NOT a flat
 * array. Format per reveng/README_TECHNICAL.md "feat" section:
 *
 *   repeated until EOF:
 *     u16 key_len; char[key_len] key_name
 *     u32 entry_count
 *     repeated entry_count times:
 *       u16 name_len; char[name_len] name
 *       u32 index                  // stored_id (NOT positional)
 *
 * Tom has 16 keys total. The interesting ones are:
 *   "name"     -> 92 phone variants (aa1, aa2, ..., zh2)
 *   "filename" -> 8118 recording names (date_001..weather7_082)
 *
 * unit.file_idx is the `stored_id` of the recording (NOT the positional
 * index in the feat list). The first and last filename entries happen
 * to have positional == stored_id (date_001 stored_id 0, weather7_082
 * stored_id 8117) so naive positional lookup passes boundary tests, but
 * middle entries diverge by up to ~100. Confirmed empirically:
 *   positional[3869]: ('news27_076', stored_id 3969)
 *   stored_id 3869 -> 'news26_067'
 * The engine uses stored_id, so we sort entries by stored_id at load
 * time. Then entries[stored_id] is the canonical lookup. */

#include "feat_table.h"

#include "../common/log.h"
#include "../../include/spfy/spfy.h"

#include <stdlib.h>
#include <string.h>

static uint16_t le_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t le_u32(const uint8_t *p)
{
    return (uint32_t)p[0]        | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int spfy_feat_table_load(const spfy_vin_t *vin, spfy_feat_table_t *out)
{
    if (!vin || !out) return SPFY_E_INVAL;
    if (!vin->feat || vin->feat_n == 0) return SPFY_E_FORMAT;
    memset(out, 0, sizeof *out);

    const uint8_t *p   = vin->feat;
    const uint8_t *end = vin->feat + vin->feat_n;

    /* Walk keys until we find "filename". Skip other keys, returning
     * SPFY_E_FORMAT if a key block is malformed. */
    while (p < end) {
        if ((size_t)(end - p) < 2) return SPFY_E_FORMAT;
        uint16_t klen = le_u16(p); p += 2;
        if ((size_t)(end - p) < (size_t)klen + 4) return SPFY_E_FORMAT;
        const char *key      = (const char *)p;
        uint16_t    key_len  = klen;
        p += klen;
        uint32_t entry_count = le_u32(p); p += 4;

        int is_filename = (key_len == 8) && (memcmp(key, "filename", 8) == 0);

        if (is_filename) {
            /* Read entries in file order, then sort by stored_id so
             * entries[stored_id] is the canonical lookup. */
            spfy_feat_entry_t *arr = (spfy_feat_entry_t *)
                calloc(entry_count, sizeof *arr);
            if (!arr) return SPFY_E_NOMEM;
            for (uint32_t i = 0; i < entry_count; ++i) {
                if ((size_t)(end - p) < 2) {
                    free(arr); return SPFY_E_FORMAT;
                }
                uint16_t nlen = le_u16(p); p += 2;
                if ((size_t)(end - p) < (size_t)nlen + 4) {
                    free(arr); return SPFY_E_FORMAT;
                }
                arr[i].name      = (const char *)p;
                arr[i].name_len  = nlen;
                p += nlen;
                arr[i].stored_id = le_u32(p); p += 4;
            }

            /* Sort by stored_id ascending (insertion sort kept simple;
             * 8118 entries is fast either way). qsort_r isn't portable;
             * a small in-place sort is fine here. */
            for (uint32_t i = 1; i < entry_count; ++i) {
                spfy_feat_entry_t sort_key = arr[i];
                uint32_t j = i;
                while (j > 0 && arr[j - 1].stored_id > sort_key.stored_id) {
                    arr[j] = arr[j - 1];
                    --j;
                }
                arr[j] = sort_key;
            }
            /* Validate density: stored_ids should be 0..entry_count-1. If
             * not, the array still works for dense ranges but boundary
             * stored_ids fail. Warn but proceed. */
            for (uint32_t i = 0; i < entry_count; ++i) {
                if (arr[i].stored_id != i) {
                    spfy_log_warn(
                        "feat.filename: stored_ids not dense -- "
                        "entries[%u].stored_id=%u (expected %u)",
                        i, arr[i].stored_id, i);
                    break;
                }
            }
            out->entries   = arr;
            out->n_entries = entry_count;
            return SPFY_OK;
        }

        /* Skip this key's entries. */
        for (uint32_t i = 0; i < entry_count; ++i) {
            if ((size_t)(end - p) < 2) return SPFY_E_FORMAT;
            uint16_t nlen = le_u16(p); p += 2;
            if ((size_t)(end - p) < (size_t)nlen + 4) return SPFY_E_FORMAT;
            p += nlen + 4u;
        }
    }
    spfy_log_err("feat: 'filename' key not found in chunk");
    return SPFY_E_FORMAT;
}

void spfy_feat_table_free(spfy_feat_table_t *t)
{
    if (!t) return;
    free(t->entries);
    memset(t, 0, sizeof *t);
}
