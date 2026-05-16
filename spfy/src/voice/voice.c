#include "voice.h"

#include <stdlib.h>

void spfy_vin_free(spfy_vin_t *v)
{
    if (!v) return;
    free(v->bytes);
    v->bytes = NULL; v->n_bytes = 0;
}

void spfy_vdb_free(spfy_vdb_t *v)
{
    if (!v) return;
    free(v->bytes);
    free(v->indx_entries);
    v->bytes = NULL; v->n_bytes = 0;
    v->indx_entries = NULL; v->n_indx_entries = 0;
}

void spfy_vcf_free(spfy_vcf_t *v)
{
    if (!v) return;
    spfy_vcf_kv_t *kv = v->params;
    while (kv) {
        spfy_vcf_kv_t *next = kv->next;
        free(kv->key);
        free(kv->value);
        free(kv);
        kv = next;
    }
    free(v->xml_bytes);
    v->xml_bytes = NULL; v->xml_n = 0;
    v->params = NULL;
}
