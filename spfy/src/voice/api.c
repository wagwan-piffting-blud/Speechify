/* Public API surface for voice loading + introspection. */

#include "voice.h"
#include "unit_table.h"
#include "../../include/spfy/spfy.h"
#include "../../include/spfy/spfy_voice.h"

#include <stdlib.h>
#include <string.h>

int spfy_voice_open(const char *vin_path,
                    const char *vdb_path,
                    const char *vcf_path,
                    spfy_voice **out)
{
    if (!vin_path || !vdb_path || !vcf_path || !out) return SPFY_E_INVAL;

    spfy_voice *v = calloc(1, sizeof *v);
    if (!v) return SPFY_E_NOMEM;

    int rc = spfy_vin_load(vin_path, &v->vin);
    if (rc != SPFY_OK) goto fail;
    rc = spfy_vdb_load(vdb_path, &v->vdb);
    if (rc != SPFY_OK) goto fail;
    rc = spfy_vcf_load(vcf_path, &v->vcf);
    if (rc != SPFY_OK) goto fail;

    *out = v;
    return SPFY_OK;

fail:
    spfy_voice_close(v);
    return rc;
}

void spfy_voice_close(spfy_voice *v)
{
    if (!v) return;
    spfy_vin_free(&v->vin);
    spfy_vdb_free(&v->vdb);
    spfy_vcf_free(&v->vcf);
    free(v);
}

size_t spfy_voice_n_units(const spfy_voice *v)
{
    /* Record size is version-dependent (24/29/30), and unit_n covers the
     * whole chunk including its {vers,data} sub-chunk headers -- so this
     * has to go through the real parser rather than a plain division. */
    if (!v || !v->vin.unit) return 0;
    spfy_unit_table_t t;
    if (spfy_unit_table_load(&v->vin, &t) != SPFY_OK) return 0;
    return t.n_units;
}

size_t spfy_voice_n_recordings(const spfy_voice *v)
{
    if (!v) return 0;
    return v->vdb.n_indx_entries;
}

uint32_t spfy_voice_sample_rate(const spfy_voice *v)
{
    if (!v) return 0;
    return v->vdb.sample_rate;
}

size_t spfy_voice_hash_n_rows(const spfy_voice *v)
{
    return v ? v->vin.hash_n_rows : 0;
}

size_t spfy_voice_hash_n_cells(const spfy_voice *v)
{
    return v ? v->vin.hash_n_cells : 0;
}

int spfy_voice_vcf_get_str(const spfy_voice *v, const char *key,
                           const char **out)
{
    if (!v || !key || !out) return SPFY_E_INVAL;
    for (const spfy_vcf_kv_t *kv = v->vcf.params; kv; kv = kv->next) {
        if (strcmp(kv->key, key) == 0) { *out = kv->value; return SPFY_OK; }
    }
    return SPFY_E_INVAL;
}

int spfy_voice_vcf_get_f64(const spfy_voice *v, const char *key, double *out)
{
    const char *s = NULL;
    int rc = spfy_voice_vcf_get_str(v, key, &s);
    if (rc != SPFY_OK) return rc;
    char *end = NULL;
    double d = strtod(s, &end);
    if (end == s) return SPFY_E_FORMAT;
    *out = d;
    return SPFY_OK;
}

const char *spfy_strerror(int code)
{
    switch (code) {
    case SPFY_OK:        return "ok";
    case SPFY_E_INVAL:   return "invalid argument";
    case SPFY_E_IO:      return "io error";
    case SPFY_E_FORMAT:  return "format error";
    case SPFY_E_NOMEM:   return "out of memory";
    case SPFY_E_NOTSUP:  return "not supported";
    case SPFY_E_OOB:     return "out of bounds";
    default:             return "unknown error";
    }
}

const char *spfy_version(void)
{
    return "spfy 0.0.1";
}
