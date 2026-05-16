/* spfy_voice_load / spfy_voice_free — voice loading lifted out of the
 * spfy_synth CLI's main() (formerly lines 673..867). Logic is unchanged;
 * the only change is that all the per-call locals now live in a
 * spfy_voice_t. The voicing-table build (engine-faithful per-HP-label
 * walk of the VIN "name" feat chunk) is preserved verbatim and is the
 * trickiest piece — keep [[project_voicing_gate_2026_05_12]] in mind
 * before touching it. */

#include "spfy_synth_lib.h"

#include "../fe/phoneset.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void spfy_synth_set_pitch_semitones(spfy_voice_t *v, float semitones)
{
    if (!v) return;
    if (semitones == 0.0f) {
        /* Exact 1.0 — pitch path becomes a true no-op. */
        v->pitch_scale = 1.0f;
        return;
    }
    /* Clamp to a sane octave around baseline; useful range is much
     * smaller (Tom's recorded corpus has roughly +-3 st of natural
     * variation). */
    if (semitones >  12.0f) semitones =  12.0f;
    if (semitones < -12.0f) semitones = -12.0f;
    v->pitch_scale = powf(2.0f, semitones / 12.0f);
}

/* Tom's corpus 1-99% f0_mid range probed via spfy_dump_f0:
 *   median = 118 Hz, p1 = 101 Hz (-2.69 st), p99 = 130 Hz (+1.68 st).
 * We use slightly tighter limits so selection stays within a healthy
 * pool density (not at the corpus tails). */
#define SPFY_PITCH_SEL_UP     1.5f
#define SPFY_PITCH_SEL_DOWN  -2.0f

void spfy_synth_split_pitch(float target_st,
                            float *out_selection_st,
                            float *out_psola_st)
{
    float sel = target_st;
    if (sel > SPFY_PITCH_SEL_UP)   sel = SPFY_PITCH_SEL_UP;
    if (sel < SPFY_PITCH_SEL_DOWN) sel = SPFY_PITCH_SEL_DOWN;
    if (out_selection_st) *out_selection_st = sel;
    if (out_psola_st)     *out_psola_st     = target_st - sel;
}

int spfy_voice_load(const spfy_voice_paths_t *paths, spfy_voice_t *v)
{
    int rc;
    if (!paths || !v) return SPFY_E_INVAL;
    if (!paths->vin || !paths->vdb || !paths->vcf || !paths->hpclass
        || !paths->vocab || !paths->fe_tables_a || !paths->fe_tables_b)
        return SPFY_E_INVAL;

    memset(v, 0, sizeof *v);
    v->hpc_buckets = 256u;
    v->pitch_scale = 1.0f;

    if ((rc = spfy_vin_load(paths->vin, &v->vin))                   != SPFY_OK) goto fail;
    if ((rc = spfy_vdb_load(paths->vdb, &v->vdb))                   != SPFY_OK) goto fail;
    if ((rc = spfy_vdb_require_8k_mulaw(&v->vdb, paths->vdb))       != SPFY_OK) goto fail;
    if ((rc = spfy_vcf_load(paths->vcf, &v->vcf))                   != SPFY_OK) goto fail;
    if ((rc = spfy_unit_table_load(&v->vin, &v->units))             != SPFY_OK) goto fail;
    if ((rc = spfy_feat_table_load(&v->vin, &v->feat))              != SPFY_OK) goto fail;
    if ((rc = spfy_vdb_lookup_build(&v->vdb, &v->lookup))           != SPFY_OK) goto fail;
    if ((rc = spfy_ccos_load(&v->vin, &v->ccos))                    != SPFY_OK) goto fail;
    if ((rc = spfy_voice_maps_build(&v->ccos, &v->maps))            != SPFY_OK) goto fail;
    if ((rc = spfy_proscost_load(&v->vcf, v->pros))                 != SPFY_OK) goto fail;
    if ((rc = spfy_hash_load(&v->vin, &v->hash))                    != SPFY_OK) goto fail;
    if ((rc = spfy_prsl_load(&v->vin, &v->prsl))                    != SPFY_OK) goto fail;
    if ((rc = spfy_cart_load_durt(&v->vin, &v->durt_cart))          != SPFY_OK) goto fail;
    if ((rc = spfy_cart_load_f0tr(&v->vin, &v->f0tr_cart))          != SPFY_OK) goto fail;
    if ((rc = spfy_chunk_tables_load(&v->vin, &v->chunks))          != SPFY_OK) goto fail;
    if ((rc = spfy_anchor_hpclass_load(paths->hpclass,
                                       &v->hpc, &v->hpc_n))         != SPFY_OK) goto fail;

    /* Build per-HP-class candidate fallback index (lifted verbatim). */
    v->bucket_n   = (uint32_t *)calloc(v->hpc_buckets, sizeof *v->bucket_n);
    v->bucket_cap = (uint32_t *)calloc(v->hpc_buckets, sizeof *v->bucket_cap);
    v->bucket     = (uint32_t **)calloc(v->hpc_buckets, sizeof *v->bucket);
    if (!v->bucket_n || !v->bucket_cap || !v->bucket) {
        rc = SPFY_E_NOMEM; goto fail;
    }
    for (uint32_t u = 1; u < v->units.n_units; ++u) {
        spfy_unit_record_t r;
        if (spfy_unit_record_get(&v->units, u, &r) != SPFY_OK) continue;
        uint32_t hp = (uint32_t)r.phone_center * 2u
                    + (uint32_t)(r.is_first_half ? 0u : 1u);
        if (hp >= v->hpc_buckets) continue;
        if (v->bucket_n[hp] >= v->bucket_cap[hp]) {
            uint32_t cap = v->bucket_cap[hp] ? v->bucket_cap[hp] * 2u : 32u;
            uint32_t *p  = (uint32_t *)realloc(v->bucket[hp],
                                               cap * sizeof *p);
            if (!p) { rc = SPFY_E_NOMEM; goto fail; }
            v->bucket[hp] = p;
            v->bucket_cap[hp] = cap;
        }
        v->bucket[hp][v->bucket_n[hp]++] = u;
    }

    v->av.units = &v->units;
    v->av.ccos  = &v->ccos;
    v->av.maps  = &v->maps;
    v->av.proscost = v->pros;
    v->av.hpclass_table = v->hpc;
    v->av.hpclass_n     = v->hpc_n;
    spfy_anchor_voice_set_default_weights(&v->av);

    /* FE host: opens vocab + fe_tables, sets per-voice VCF. */
    if ((rc = spfy_fe_open(paths->vocab, paths->fe_tables_a,
                            paths->fe_tables_b, &v->fe))            != SPFY_OK) goto fail;
    if ((rc = spfy_fe_set_voice_vcf(v->fe, paths->vcf))             != SPFY_OK) goto fail;

    /* Engine-faithful voicing table — see [[project_voicing_gate_2026_05_12]]
     * and the in-line comment in spfy_synth.c for the rationale (the
     * "name" feat chunk in the VIN is alphabetical and the canonical
     * source of HP-label positional indices). Lifted verbatim. */
    {
        const spfy_phoneset_t *ps = spfy_fe_phoneset(v->fe);
        if (ps && ps->n_phones > 0 && v->vin.feat && v->vin.feat_n > 0) {
            const uint8_t *p   = v->vin.feat;
            const uint8_t *end = v->vin.feat + v->vin.feat_n;
            const uint8_t *name_entries = NULL;
            uint32_t       name_count   = 0;
            while (p + 2 <= end) {
                uint16_t klen = (uint16_t)(p[0] | (p[1] << 8));
                p += 2;
                if (p + klen + 4 > end) break;
                int is_name = (klen == 4) && (memcmp(p, "name", 4) == 0);
                p += klen;
                uint32_t cnt = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                             | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
                p += 4;
                if (is_name) {
                    name_entries = p;
                    name_count   = cnt;
                    break;
                }
                for (uint32_t i = 0; i < cnt && p + 2 <= end; ++i) {
                    uint16_t nlen = (uint16_t)(p[0] | (p[1] << 8));
                    p += 2;
                    if (p + nlen + 4 > end) { p = end; break; }
                    p += nlen + 4;
                }
            }
            if (name_entries && name_count > 0) {
                v->voicing_buf = (uint32_t *)calloc(name_count,
                                                    sizeof *v->voicing_buf);
                if (!v->voicing_buf) { rc = SPFY_E_NOMEM; goto fail; }
                const uint8_t *q = name_entries;
                for (uint32_t pos = 0; pos < name_count && q + 2 <= end; ++pos) {
                    uint16_t nlen = (uint16_t)(q[0] | (q[1] << 8));
                    q += 2;
                    if (q + nlen + 4 > end) break;
                    const char *nm = (const char *)q;
                    q += nlen;
                    q += 4;     /* skip stored_id */
                    if (nlen < 2) continue;
                    char base[SPFY_PHONESET_NAME_MAX + 1];
                    uint16_t blen = (uint16_t)(nlen - 1);
                    if (blen >= sizeof base) blen = (uint16_t)(sizeof base - 1);
                    memcpy(base, nm, blen);
                    base[blen] = '\0';
                    uint8_t pid = spfy_phoneset_lookup(ps, base);
                    if (pid != 0xff && pid < ps->n_phones) {
                        v->voicing_buf[pos] =
                            ps->entries[pid].is_voiced ? 1u : 0u;
                    }
                }
                v->av.voicing   = v->voicing_buf;
                v->av.voicing_n = name_count;
            } else {
                uint32_t vn = ps->n_phones * 2u;
                v->voicing_buf = (uint32_t *)calloc(vn,
                                                    sizeof *v->voicing_buf);
                if (!v->voicing_buf) { rc = SPFY_E_NOMEM; goto fail; }
                for (uint32_t i = 0; i < ps->n_phones; ++i) {
                    uint32_t vv = ps->entries[i].is_voiced ? 1u : 0u;
                    v->voicing_buf[i * 2u]     = vv;
                    v->voicing_buf[i * 2u + 1] = vv;
                }
                v->av.voicing   = v->voicing_buf;
                v->av.voicing_n = vn;
            }
        }
    }

    return SPFY_OK;

fail:
    spfy_voice_free(v);
    return rc;
}

void spfy_voice_free(spfy_voice_t *v)
{
    if (!v) return;
    if (v->bucket) {
        for (uint32_t i = 0; i < v->hpc_buckets; ++i) free(v->bucket[i]);
        free(v->bucket);
    }
    free(v->bucket_n);
    free(v->bucket_cap);
    free(v->voicing_buf);
    spfy_cart_free(&v->durt_cart);
    spfy_cart_free(&v->f0tr_cart);
    spfy_chunk_tables_free(&v->chunks);
    if (v->fe) spfy_fe_close(v->fe);
    spfy_anchor_hpclass_free(v->hpc);
    spfy_prsl_free(&v->prsl);
    spfy_hash_free(&v->hash);
    spfy_proscost_free(v->pros);
    spfy_voice_maps_free(&v->maps);
    spfy_ccos_free(&v->ccos);
    spfy_vdb_lookup_free(&v->lookup);
    spfy_feat_table_free(&v->feat);
    spfy_vcf_free(&v->vcf);
    spfy_vdb_free(&v->vdb);
    spfy_vin_free(&v->vin);
    memset(v, 0, sizeof *v);
}
