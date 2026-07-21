/* Phone-order reconciliation between feat["name"] and ccos/labl.
 *
 * See phone_order.h for the format notes and the hp_class derivation this
 * enables. Both source lists live in the VIN, so nothing here needs a
 * Frida capture. */

#include "phone_order.h"

#include "../common/riff.h"
#include "../common/log.h"
#include "../../include/spfy/spfy.h"

#include <stdlib.h>
#include <string.h>

#define FCC_LABL SPFY_FOURCC('l','a','b','l')

static uint16_t le_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t le_u32(const uint8_t *p)
{
    return (uint32_t)p[0]        | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* A borrowed, non-NUL-terminated string slice into the VIN buffer. */
typedef struct { const char *s; uint16_t n; } slice_t;

static int slice_eq(slice_t a, slice_t b)
{
    return a.n == b.n && memcmp(a.s, b.s, a.n) == 0;
}

/* ---------------------------------------------------------------- */
/* feat["name"] -> ordered phone list                                */
/* ---------------------------------------------------------------- */

/* The feat chunk is a dictionary of (key -> list of (name, id)); see
 * feat_table.c. We want the "name" key, whose entries are half-phone
 * variants "aa1","aa2",... in half-phone id order. Stripping the trailing
 * digit and de-duplicating consecutively yields the phone order. */
static int load_feat_phones(const spfy_vin_t *vin,
                            slice_t **out_names, uint32_t *out_n)
{
    const uint8_t *p   = vin->feat;
    const uint8_t *end = vin->feat + vin->feat_n;

    while (p < end) {
        if ((size_t)(end - p) < 2) return SPFY_E_FORMAT;
        uint16_t klen = le_u16(p); p += 2;
        if ((size_t)(end - p) < (size_t)klen + 4) return SPFY_E_FORMAT;
        const char *key = (const char *)p;
        p += klen;
        uint32_t entry_count = le_u32(p); p += 4;

        if (klen == 4 && memcmp(key, "name", 4) == 0) {
            slice_t *phones = (slice_t *)calloc(entry_count ? entry_count : 1,
                                                sizeof *phones);
            if (!phones) return SPFY_E_NOMEM;
            uint32_t n_ph = 0;
            for (uint32_t i = 0; i < entry_count; ++i) {
                if ((size_t)(end - p) < 2) { free(phones); return SPFY_E_FORMAT; }
                uint16_t nlen = le_u16(p); p += 2;
                if ((size_t)(end - p) < (size_t)nlen + 4) {
                    free(phones); return SPFY_E_FORMAT;
                }
                slice_t nm = { (const char *)p, nlen };
                p += nlen + 4u;   /* skip the u32 stored_id */

                /* Strip the half-phone suffix digit. */
                if (nm.n >= 2 && (nm.s[nm.n - 1] == '1' || nm.s[nm.n - 1] == '2'))
                    nm.n = (uint16_t)(nm.n - 1);
                else
                    continue;     /* not a half-phone variant; ignore */

                if (n_ph == 0 || !slice_eq(phones[n_ph - 1], nm))
                    phones[n_ph++] = nm;
            }
            if (n_ph == 0) { free(phones); return SPFY_E_FORMAT; }
            *out_names = phones;
            *out_n     = n_ph;
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
    spfy_log_err("feat: 'name' key not found in chunk");
    return SPFY_E_FORMAT;
}

/* ---------------------------------------------------------------- */
/* ccos/labl -> ordered label list                                   */
/* ---------------------------------------------------------------- */

/* labl body: u32 count, then count * { u16 len, char[len] }. */
static int load_labl(const spfy_vin_t *vin, slice_t **out_names, uint32_t *out_n)
{
    if (!vin->ccos || vin->ccos_n == 0) return SPFY_E_FORMAT;

    spfy_riff_iter it;
    spfy_riff_iter_init(&it, vin->ccos, vin->ccos_n);
    spfy_chunk c;
    int rc;
    while ((rc = spfy_riff_iter_next(&it, &c)) == 1) {
        if (c.fourcc != FCC_LABL) continue;
        if (c.size < 4) return SPFY_E_FORMAT;
        uint32_t count = le_u32(c.data);
        if (count == 0 || count > 256u) return SPFY_E_FORMAT;

        slice_t *names = (slice_t *)calloc(count, sizeof *names);
        if (!names) return SPFY_E_NOMEM;
        const uint8_t *p   = c.data + 4;
        const uint8_t *end = c.data + c.size;
        for (uint32_t i = 0; i < count; ++i) {
            if ((size_t)(end - p) < 2) { free(names); return SPFY_E_FORMAT; }
            uint16_t nlen = le_u16(p); p += 2;
            if ((size_t)(end - p) < nlen) { free(names); return SPFY_E_FORMAT; }
            names[i].s = (const char *)p;
            names[i].n = nlen;
            p += nlen;
        }
        *out_names = names;
        *out_n     = count;
        return SPFY_OK;
    }
    if (rc < 0) return SPFY_E_FORMAT;
    spfy_log_err("ccos: 'labl' sub-chunk not found");
    return SPFY_E_FORMAT;
}

/* ---------------------------------------------------------------- */

int spfy_phone_order_build(const spfy_vin_t *vin, spfy_phone_order_t *out)
{
    if (!vin || !out) return SPFY_E_INVAL;
    if (!vin->feat || vin->feat_n == 0) return SPFY_E_FORMAT;
    memset(out, 0, sizeof *out);

    slice_t *phones = NULL, *labels = NULL;
    uint32_t n_ph = 0, n_lab = 0;
    int rc = load_feat_phones(vin, &phones, &n_ph);
    if (rc != SPFY_OK) return rc;
    rc = load_labl(vin, &labels, &n_lab);
    if (rc != SPFY_OK) { free(phones); return rc; }

    if (n_ph > 255u || n_lab > 255u) {
        free(phones); free(labels);
        spfy_log_err("phone_order: too many phones/labels (%u/%u)", n_ph, n_lab);
        return SPFY_E_FORMAT;
    }

    out->labl_to_feat   = (uint8_t *)malloc(n_lab);
    out->feat_to_labl   = (uint8_t *)malloc(n_ph);
    out->phone_names    = (char **)calloc(n_ph, sizeof *out->phone_names);
    out->phone_name_len = (uint16_t *)malloc(n_ph * sizeof *out->phone_name_len);
    if (!out->labl_to_feat || !out->feat_to_labl ||
        !out->phone_names  || !out->phone_name_len) {
        free(phones); free(labels);
        spfy_phone_order_free(out);
        return SPFY_E_NOMEM;
    }
    out->n_phones = n_ph;
    out->n_labels = n_lab;

    memset(out->labl_to_feat, SPFY_PHONE_NONE, n_lab);
    memset(out->feat_to_labl, SPFY_PHONE_NONE, n_ph);
    for (uint32_t i = 0; i < n_ph; ++i) {
        char *copy = (char *)malloc((size_t)phones[i].n + 1u);
        if (!copy) {
            free(phones); free(labels);
            spfy_phone_order_free(out);
            return SPFY_E_NOMEM;
        }
        memcpy(copy, phones[i].s, phones[i].n);
        copy[phones[i].n] = '\0';
        out->phone_names[i]    = copy;
        out->phone_name_len[i] = phones[i].n;
    }

    uint32_t unmatched = 0;
    for (uint32_t li = 0; li < n_lab; ++li) {
        if (labels[li].n == 0) continue;     /* trailing empty label */
        for (uint32_t pi = 0; pi < n_ph; ++pi) {
            if (slice_eq(labels[li], phones[pi])) {
                out->labl_to_feat[li] = (uint8_t)pi;
                out->feat_to_labl[pi] = (uint8_t)li;
                break;
            }
        }
        if (out->labl_to_feat[li] == SPFY_PHONE_NONE) {
            ++unmatched;
            spfy_log_warn("phone_order: labl[%u] '%.*s' has no feat phone",
                          li, (int)labels[li].n, labels[li].s);
        }
    }
    free(phones);
    free(labels);

    if (unmatched > 0) {
        spfy_log_err("phone_order: %u label(s) unmatched -- hp_class would "
                     "be wrong", unmatched);
        spfy_phone_order_free(out);
        return SPFY_E_FORMAT;
    }
    return SPFY_OK;
}

void spfy_phone_order_free(spfy_phone_order_t *p)
{
    if (!p) return;
    free(p->labl_to_feat);
    free(p->feat_to_labl);
    if (p->phone_names) {
        for (uint32_t i = 0; i < p->n_phones; ++i) free(p->phone_names[i]);
        free(p->phone_names);
    }
    free(p->phone_name_len);
    memset(p, 0, sizeof *p);
}

uint8_t spfy_phone_order_index(const spfy_phone_order_t *po, const char *name)
{
    if (!po || !po->phone_names || !name || !*name) return SPFY_PHONE_NONE;
    for (uint32_t i = 0; i < po->n_phones; ++i) {
        if (po->phone_names[i] && strcmp(po->phone_names[i], name) == 0)
            return (uint8_t)i;
    }
    return SPFY_PHONE_NONE;
}

int spfy_phone_order_hpclass(const spfy_phone_order_t *po,
                             const spfy_unit_table_t *units,
                             uint8_t **out_data, uint32_t *out_n)
{
    if (!po || !units || !out_data || !out_n) return SPFY_E_INVAL;
    if (!units->data || units->n_units == 0)  return SPFY_E_FORMAT;

    uint8_t *tbl = (uint8_t *)malloc(units->n_units);
    if (!tbl) return SPFY_E_NOMEM;

    const uint8_t *p = units->data + units->off_phone_center;
    for (uint32_t uid = 0; uid < units->n_units; ++uid) {
        uint8_t labl = *p;
        p += units->rec_size;
        if (labl >= po->n_labels) {
            spfy_log_err("hpclass: unit %u phone_center=%u >= n_labels=%u",
                         uid, labl, po->n_labels);
            free(tbl);
            return SPFY_E_FORMAT;
        }
        uint8_t phone = po->labl_to_feat[labl];
        if (phone == SPFY_PHONE_NONE) {
            spfy_log_err("hpclass: unit %u maps to an unnamed label %u",
                         uid, labl);
            free(tbl);
            return SPFY_E_FORMAT;
        }
        /* Units are stored as consecutive (left, right) half-phone pairs,
         * so the side bit is the unit id's parity. */
        tbl[uid] = (uint8_t)(phone * 2u + (uid & 1u));
    }
    *out_data = tbl;
    *out_n    = units->n_units;
    return SPFY_OK;
}

void spfy_phone_order_hpclass_free(uint8_t *data)
{
    free(data);
}
