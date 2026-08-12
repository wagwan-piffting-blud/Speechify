
#include "pmarks.h"

#include "../common/file_io.h"
#include "../common/log.h"
#include "../../include/spfy/spfy.h"

#include <stdlib.h>
#include <string.h>

#define PM_HEADER_WORDS 3u
#define PM_MAX_UNITS    (1u << 22)

static uint32_t be_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static int16_t be_i16(const uint8_t *p)
{
    return (int16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

int spfy_pmarks_load(const char *stem, spfy_pmarks_t *out)
{
    if (!stem || !out) return SPFY_E_INVAL;
    memset(out, 0, sizeof *out);

    char path[1024];
    uint8_t *ibuf = NULL, *dbuf = NULL;
    size_t   in_ = 0, dn = 0;
    int rc;

    if (snprintf(path, sizeof path, "%s.pmindex", stem) >= (int)sizeof path)
        return SPFY_E_INVAL;
    rc = spfy_slurp_file(path, &ibuf, &in_);
    if (rc != SPFY_OK) return rc;

    if (snprintf(path, sizeof path, "%s.pmdata", stem) >= (int)sizeof path) {
        free(ibuf); return SPFY_E_INVAL;
    }
    rc = spfy_slurp_file(path, &dbuf, &dn);
    if (rc != SPFY_OK) { free(ibuf); return rc; }

    if (in_ < PM_HEADER_WORDS * 4u || ((in_ - PM_HEADER_WORDS * 4u) % 8u)) {
        spfy_log_err("pmarks: %s.pmindex size %zu is not 12 + 8*n", stem, in_);
        free(ibuf); free(dbuf); return SPFY_E_FORMAT;
    }
    uint32_t n_units = (uint32_t)((in_ - PM_HEADER_WORDS * 4u) / 8u);
    if (n_units == 0 || n_units > PM_MAX_UNITS) {
        free(ibuf); free(dbuf); return SPFY_E_FORMAT;
    }

    out->rate    = be_u32(ibuf);
    out->n_units = n_units;
    out->n_data  = dn / 2u;
    out->index   = (uint32_t *)malloc((size_t)n_units * 2u * sizeof *out->index);
    out->data    = (int16_t *)malloc(out->n_data * sizeof *out->data);
    if (!out->index || !out->data) {
        free(ibuf); free(dbuf); spfy_pmarks_free(out); return SPFY_E_NOMEM;
    }

    for (uint32_t k = 0; k < n_units; ++k) {
        const uint8_t *p = ibuf + (PM_HEADER_WORDS + 2u * k) * 4u;
        out->index[2u * k]      = be_u32(p);
        out->index[2u * k + 1u] = be_u32(p + 4);
    }
    for (size_t i = 0; i < out->n_data; ++i)
        out->data[i] = be_i16(dbuf + i * 2u);

    for (uint32_t k = 0; k < n_units; ++k) {
        uint32_t off = out->index[2u * k], cnt = out->index[2u * k + 1u];
        if ((size_t)off + cnt > out->n_data) {
            spfy_log_err("pmarks: unit %u span %u+%u exceeds pmdata (%zu)",
                         k, off, cnt, out->n_data);
            free(ibuf); free(dbuf); spfy_pmarks_free(out);
            return SPFY_E_FORMAT;
        }
    }

    free(ibuf);
    free(dbuf);
    return SPFY_OK;
}

void spfy_pmarks_free(spfy_pmarks_t *t)
{
    if (!t) return;
    free(t->index);
    free(t->data);
    memset(t, 0, sizeof *t);
}

int spfy_pmarks_get(const spfy_pmarks_t *t, uint32_t uid,
                    const int16_t **periods)
{
    if (!t || !t->index || uid >= t->n_units) return 0;
    uint32_t off = t->index[2u * uid];
    uint32_t cnt = t->index[2u * uid + 1u];
    if (cnt == 0) return 0;
    if (periods) *periods = t->data + off;
    return (int)cnt;
}
