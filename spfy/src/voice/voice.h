#ifndef SPFY_VOICE_INTERNAL_H
#define SPFY_VOICE_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

/* Internal voice representation. Populated by vin_loader.c, vdb_loader.c,
 * vcf_loader.c. The public spfy_voice handle in <spfy/spfy.h> aliases this. */

typedef struct {
    uint8_t  *bytes;          /* decrypted RIFF (heap-allocated, owned) */
    size_t    n_bytes;
    /* Per-chunk pointers (point INTO bytes; not owned). NULL if absent. */
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
    /* Hash header parsed: */
    size_t        hash_n_rows;
    size_t        hash_n_cells;
} spfy_vin_t;

typedef struct {
    uint8_t  *bytes;          /* decrypted VDB RIFF (heap, owned) */
    size_t    n_bytes;
    const uint8_t *fmt;   size_t fmt_n;
    const uint8_t *indx;  size_t indx_n;
    const uint8_t *data;  size_t data_n;
    /* Parsed indx: array of {offset_into_data, name_len, name_ptr_into_indx}. */
    struct spfy_indx_entry *indx_entries;
    size_t                  n_indx_entries;
    uint32_t  sample_rate;    /* 8000 from fmt header */
} spfy_vdb_t;

typedef struct spfy_vcf_kv {
    char            *key;     /* dotted, e.g. "tts.usel.weights.JOIN_COST_WEIGHT" */
    char            *value;   /* always stored as string */
    struct spfy_vcf_kv *next;
} spfy_vcf_kv_t;

typedef struct {
    uint8_t        *xml_bytes;     /* decrypted XML (heap, owned) */
    size_t          xml_n;
    spfy_vcf_kv_t  *params;        /* linked list of resolved params */
} spfy_vcf_t;

struct spfy_voice {
    spfy_vin_t  vin;
    spfy_vdb_t  vdb;
    spfy_vcf_t  vcf;
};

struct spfy_indx_entry {
    uint32_t  data_offset;
    const char *name;     /* points into vdb.bytes; NOT NUL-terminated */
    uint16_t  name_len;
};

/* Stage-1 loaders: read file, decrypt, populate the per-chunk pointer table.
 * Each returns SPFY_OK or SPFY_E_*. */
int spfy_vin_load(const char *path, spfy_vin_t *out);
int spfy_vdb_load(const char *path, spfy_vdb_t *out);
int spfy_vcf_load(const char *path, spfy_vcf_t *out);

void spfy_vin_free(spfy_vin_t *v);
void spfy_vdb_free(spfy_vdb_t *v);
void spfy_vcf_free(spfy_vcf_t *v);

/* VCF param lookup by BARE name -- "tts.voiceCfg." is prepended. Returns
 * NULL / dflt when the param is absent, which is meaningful: the engine
 * falls back to its own built-in default in exactly that case. */
const char *spfy_vcf_str(const spfy_vcf_t *vcf, const char *name);
float       spfy_vcf_f32(const spfy_vcf_t *vcf, const char *name, float dflt);
/* Same, but tries `name` then `alias`. Needed because Jill spells
 * SYL_IN_WORD_MISMATCH_COST where every other voice spells SYLL_IN_WORD_. */
float       spfy_vcf_f32_alias(const spfy_vcf_t *vcf, const char *name,
                               const char *alias, float dflt);

/* Refuse a VDB whose storage format does not match the spfy decode
 * path (8 kHz, 1 byte/sample µ-law). Returns SPFY_OK if the VDB is
 * compatible, SPFY_E_FORMAT otherwise (and logs a clear message).
 * Callers should invoke this immediately after spfy_vdb_load. */
int spfy_vdb_require_8k_mulaw(const spfy_vdb_t *vdb, const char *path);

#endif
