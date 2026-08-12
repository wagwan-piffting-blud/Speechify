#ifndef SPFY_VOICE_INTERNAL_H
#define SPFY_VOICE_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

/* Internal voice representation. */

typedef struct {
    uint8_t  *bytes;
    size_t    n_bytes;
    const uint8_t *vers;   size_t vers_n;
    const uint8_t *cnts;   size_t cnts_n;
    const uint8_t *feat;   size_t feat_n;
    const uint8_t *mean;   size_t mean_n;
    const uint8_t *hist;   size_t hist_n;
    const uint8_t *unit;   size_t unit_n;
    const uint8_t *prsl;   size_t prsl_n;
    const uint8_t *ccos;   size_t ccos_n;
    const uint8_t *ckls;   size_t ckls_n;
    const uint8_t *cklx;   size_t cklx_n;
    const uint8_t *f0tr;   size_t f0tr_n;
    const uint8_t *durt;   size_t durt_n;
    const uint8_t *hash;   size_t hash_n;
    size_t        hash_n_rows;
    size_t        hash_n_cells;
} spfy_vin_t;

typedef struct {
    uint8_t  *bytes;
    size_t    n_bytes;
    const uint8_t *fmt;   size_t fmt_n;
    const uint8_t *indx;  size_t indx_n;
    const uint8_t *data;  size_t data_n;
    struct spfy_indx_entry *indx_entries;
    size_t                  n_indx_entries;
    uint32_t  sample_rate;
    /* wFormatTag decides the storage, and it is the ONLY field that does. */
    uint16_t  fmt_tag;
    uint32_t  bytes_per_sample;
} spfy_vdb_t;

typedef struct spfy_vcf_kv {
    char            *key;
    char            *value;
    struct spfy_vcf_kv *next;
} spfy_vcf_kv_t;

typedef struct {
    uint8_t        *xml_bytes;
    size_t          xml_n;
    spfy_vcf_kv_t  *params;
} spfy_vcf_t;

struct spfy_voice {
    spfy_vin_t  vin;
    spfy_vdb_t  vdb;
    spfy_vcf_t  vcf;
};

struct spfy_indx_entry {
    uint32_t  data_offset;
    const char *name;
    uint16_t  name_len;
};

/* Stage-1 loaders: read file, decrypt, populate the per-chunk pointer
 * table. */
int spfy_vin_load(const char *path, spfy_vin_t *out);
int spfy_vdb_load(const char *path, spfy_vdb_t *out);
int spfy_vcf_load(const char *path, spfy_vcf_t *out);

void spfy_vin_free(spfy_vin_t *v);
void spfy_vdb_free(spfy_vdb_t *v);
void spfy_vcf_free(spfy_vcf_t *v);

/* VCF param lookup by BARE name -- "tts.voiceCfg." is prepended. */
const char *spfy_vcf_str(const spfy_vcf_t *vcf, const char *name);
float       spfy_vcf_f32(const spfy_vcf_t *vcf, const char *name, float dflt);
/* Same, but tries `name` then `alias`. */
float       spfy_vcf_f32_alias(const spfy_vcf_t *vcf, const char *name,
                               const char *alias, float dflt);

/* Refuse a VDB whose storage format the decode path cannot handle. */
int spfy_vdb_require_supported(const spfy_vdb_t *vdb, const char *path);

/* Decode `n_samples` starting at sample index `sample_off` within the
 * record at byte offset `rec_off`, into `dst`. */
size_t spfy_vdb_decode(const spfy_vdb_t *vdb, size_t rec_off,
                       size_t sample_off, size_t n_samples, int16_t *dst);

#endif
