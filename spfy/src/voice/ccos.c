/* ccos chunk loader: parse upper triangles, apply per-slot scaling, expand
 * into full symmetric (n_labels x n_labels) matrices in memory. */

#include "ccos.h"

#include "../common/riff.h"
#include "../common/log.h"
#include "../../include/spfy/spfy.h"

#include <stdlib.h>
#include <string.h>

#define FCC_LABL SPFY_FOURCC('l','a','b','l')
#define FCC_DATA SPFY_FOURCC('d','a','t','a')

/* Per-slot scaling factors (FUN_08e83f60). Side LEFT = hp_class < n_labels,
 * RIGHT = hp_class >= n_labels. */
static const float SLOT_SCALE_LEFT [SPFY_CCOS_N_SLOTS] = { 0.2f, 1.0f, 0.5f, 0.1f };
static const float SLOT_SCALE_RIGHT[SPFY_CCOS_N_SLOTS] = { 0.1f, 0.5f, 1.0f, 0.2f };

static uint32_t le_u32(const uint8_t *p)
{
    return (uint32_t)p[0]        | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static float le_f32(const uint8_t *p)
{
    union { uint32_t u; float f; } v;
    v.u = le_u32(p);
    return v.f;
}

static int parse_labl(const uint8_t *data, size_t n, uint32_t *out_n_labels)
{
    if (n < 4) return SPFY_E_FORMAT;
    *out_n_labels = le_u32(data);
    if (*out_n_labels == 0 || *out_n_labels > 256u) return SPFY_E_FORMAT;
    return SPFY_OK;
}

static int parse_data(const uint8_t *data, size_t n,
                      uint32_t n_labels, spfy_ccos_t *out)
{
    /* Each (hp_class, slot) on disk:
     *     u32 hp_class_validator
     *     u32 slot_validator
     *     f32[N*(N-1)/2] upper_triangle    (j < i, row-major i=1..N-1)
     */
    const size_t triangle_n  = (size_t)n_labels * (n_labels - 1) / 2;
    const size_t per_slot_b  = 8 + triangle_n * 4;
    const size_t n_hp_classes = 2u * (size_t)n_labels;
    const size_t expected_n   = n_hp_classes * SPFY_CCOS_N_SLOTS * per_slot_b;
    if (n != expected_n) {
        spfy_log_err("ccos: data size %zu != expected %zu", n, expected_n);
        return SPFY_E_FORMAT;
    }

    /* Allocate full matrices: n_hp_classes * SPFY_CCOS_N_SLOTS * (N*N) f32. */
    const size_t matrix_floats = (size_t)n_labels * n_labels;
    const size_t total_floats  =
        n_hp_classes * SPFY_CCOS_N_SLOTS * matrix_floats;
    out->tables = (float *)calloc(total_floats, sizeof *out->tables);
    if (!out->tables) return SPFY_E_NOMEM;
    out->n_labels     = n_labels;
    out->n_hp_classes = (uint32_t)n_hp_classes;

    const uint8_t *p = data;
    for (uint32_t hp = 0; hp < n_hp_classes; ++hp) {
        const float *scale_row = (hp < n_labels) ? SLOT_SCALE_LEFT
                                                 : SLOT_SCALE_RIGHT;
        for (uint32_t slot = 0; slot < SPFY_CCOS_N_SLOTS; ++slot) {
            uint32_t v_hp   = le_u32(p);     p += 4;
            uint32_t v_slot = le_u32(p);     p += 4;
            if (v_hp != hp || v_slot != slot) {
                spfy_log_err("ccos: validator mismatch at hp=%u slot=%u "
                             "(got hp=%u slot=%u)",
                             hp, slot, v_hp, v_slot);
                return SPFY_E_FORMAT;
            }

            float       *mat   = out->tables +
                                 (size_t)(hp * SPFY_CCOS_N_SLOTS + slot) *
                                 matrix_floats;
            const float  scale = scale_row[slot];

            /* Read N*(N-1)/2 upper-triangle entries, scale, mirror. */
            for (uint32_t i = 1; i < n_labels; ++i) {
                for (uint32_t j = 0; j < i; ++j) {
                    float raw    = le_f32(p);                    p += 4;
                    float scaled = (raw + 0.1f) * scale;
                    mat[i * n_labels + j] = scaled;
                    mat[j * n_labels + i] = scaled;
                }
            }
            /* Diagonal is already 0 from calloc. */
        }
    }
    return SPFY_OK;
}

int spfy_ccos_load(const spfy_vin_t *vin, spfy_ccos_t *out)
{
    if (!vin || !out) return SPFY_E_INVAL;
    if (!vin->ccos || vin->ccos_n == 0) return SPFY_E_FORMAT;
    memset(out, 0, sizeof *out);

    spfy_riff_iter it;
    spfy_riff_iter_init(&it, vin->ccos, vin->ccos_n);
    spfy_chunk c;
    int rc;
    uint32_t n_labels = 0;
    const uint8_t *data_ptr = NULL;
    size_t         data_n   = 0;

    while ((rc = spfy_riff_iter_next(&it, &c)) == 1) {
        if (c.fourcc == FCC_LABL) {
            rc = parse_labl(c.data, c.size, &n_labels);
            if (rc != SPFY_OK) return rc;
        } else if (c.fourcc == FCC_DATA) {
            data_ptr = c.data;
            data_n   = c.size;
        }
    }
    if (rc < 0)         return SPFY_E_FORMAT;
    if (n_labels == 0)  return SPFY_E_FORMAT;
    if (data_ptr == NULL) return SPFY_E_FORMAT;

    return parse_data(data_ptr, data_n, n_labels, out);
}

void spfy_ccos_free(spfy_ccos_t *c)
{
    if (!c) return;
    free(c->tables);
    memset(c, 0, sizeof *c);
}

const float *spfy_ccos_table(const spfy_ccos_t *c,
                             uint32_t hp_class, uint32_t slot)
{
    if (!c || !c->tables) return NULL;
    if (hp_class >= c->n_hp_classes) return NULL;
    if (slot >= SPFY_CCOS_N_SLOTS)   return NULL;
    return c->tables + (size_t)(hp_class * SPFY_CCOS_N_SLOTS + slot) *
                       (size_t)c->n_labels * c->n_labels;
}

float spfy_ccos_cell(const spfy_ccos_t *c,
                     uint32_t hp_class, uint32_t slot,
                     uint32_t i, uint32_t j)
{
    const float *t = spfy_ccos_table(c, hp_class, slot);
    if (!t) return 0.0f;
    if (i >= c->n_labels || j >= c->n_labels) return 0.0f;
    return t[i * c->n_labels + j];
}
