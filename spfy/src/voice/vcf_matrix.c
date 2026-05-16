/* VCF proscost matrix loader.
 *
 * Scans the decrypted VCF XML for <param name="tts.voiceCfg.proscost.MATRIX.ROW">
 * blocks, extracts <namedValue name="COL"> FLOAT </namedValue> children,
 * stacks rows in encounter order, and builds a row-major f32 matrix with
 * accompanying name arrays.
 *
 * The XML schema is small and rigid; we use the same hand-rolled scanner
 * approach as vcf_loader.c (no libexpat dep). */

#include "vcf_matrix.h"
#include "voice.h"

#include "../common/log.h"
#include "../../include/spfy/spfy.h"

#include <stdlib.h>
#include <string.h>

/* IMPORTANT: matrices 2 and 3 are stored in the engine's voice memory
 * at voice+0x3ac (sylInWordCosts data) and voice+0x470 (wordInPhraseCosts
 * data) respectively -- the OPPOSITE of the VCF logical order. The
 * engine cost formula at FUN_08e88de0 reads:
 *   term 2: matrix-at-0x3ac[target_sp[2]][cand_byte+0xc = sp_word_in_phrase]
 *   term 3: matrix-at-0x470[target_sp[3]][cand_byte+0xd = sp_syl_in_word]
 * meaning the engine pairs the sp_word_in_phrase byte with the sylInWord
 * matrix and vice versa. Confirmed empirically via Frida read of
 * voice+0x3ac/0x470 (M3.4l, 2026-05-05). To make our storage indices
 * match the engine's term order, we load wordInPhraseCosts at array
 * index 3 and sylInWordCosts at index 2. */
static const char *KIND_NAME[SPFY_PROSCOST_N] = {
    "sylInPhraseCosts",
    "sylTypeCosts",
    "sylInWordCosts",      /* index 2 in engine order */
    "wordInPhraseCosts",   /* index 3 in engine order */
    "phoneInSylCosts",
};

static const char *bounded_str(const char *hay, size_t n, const char *needle)
{
    size_t needle_n = strlen(needle);
    if (needle_n == 0 || needle_n > n) return NULL;
    for (size_t i = 0; i + needle_n <= n; ++i) {
        if (memcmp(hay + i, needle, needle_n) == 0) return hay + i;
    }
    return NULL;
}

static char *xstrdup_n(const char *s, size_t n)
{
    char *r = (char *)malloc(n + 1);
    if (!r) return NULL;
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

static void free_string_array(char **arr, uint32_t n)
{
    if (!arr) return;
    for (uint32_t i = 0; i < n; ++i) free(arr[i]);
    free(arr);
}

/* Parse one <param>...</param> block: extract row name from name="...ROW",
 * collect <namedValue name="COL"> VALUE </namedValue> children. */
typedef struct {
    char    *row_name;
    char   **col_names;       /* nv_count entries */
    float   *col_values;      /* nv_count entries */
    uint32_t nv_count;
} parsed_row_t;

static void free_parsed_row(parsed_row_t *r)
{
    free(r->row_name);
    free_string_array(r->col_names, r->nv_count);
    free(r->col_values);
    memset(r, 0, sizeof *r);
}

static int parse_one_row(const char *param_open,
                         const char *param_close,
                         const char *kind_prefix, /* e.g. "sylInPhraseCosts." */
                         parsed_row_t *out)
{
    memset(out, 0, sizeof *out);

    /* Extract row name: name="...kind_prefix.ROW" -> row name */
    const char *name_attr = bounded_str(param_open,
                                        (size_t)(param_close - param_open),
                                        "name=\"");
    if (!name_attr) return SPFY_E_FORMAT;
    name_attr += 6;
    const char *prefix_pos = bounded_str(name_attr,
                                         (size_t)(param_close - name_attr),
                                         kind_prefix);
    if (!prefix_pos) return SPFY_E_FORMAT;
    const char *row_start = prefix_pos + strlen(kind_prefix);
    const char *row_end = row_start;
    while (row_end < param_close && *row_end != '"') ++row_end;
    if (row_end >= param_close) return SPFY_E_FORMAT;
    out->row_name = xstrdup_n(row_start, (size_t)(row_end - row_start));
    if (!out->row_name) return SPFY_E_NOMEM;

    /* Walk <namedValue> entries. Cap at 64 to bound the allocation. */
    char    *names[64];
    float    vals[64];
    uint32_t n = 0;

    const char *p = row_end;
    while (p < param_close && n < 64) {
        const char *nv = bounded_str(p, (size_t)(param_close - p),
                                     "<namedValue ");
        if (!nv) break;
        const char *nameq = bounded_str(nv, (size_t)(param_close - nv),
                                        "name=\"");
        if (!nameq) break;
        nameq += 6;
        const char *nameq_end = nameq;
        while (nameq_end < param_close && *nameq_end != '"') ++nameq_end;
        if (nameq_end >= param_close) break;

        /* Find '>' then float, then '</namedValue>'. */
        const char *gt = bounded_str(nameq_end,
                                     (size_t)(param_close - nameq_end), ">");
        if (!gt) break;
        const char *close = bounded_str(gt + 1,
                                        (size_t)(param_close - gt - 1),
                                        "</namedValue>");
        if (!close) break;

        char *cn = xstrdup_n(nameq, (size_t)(nameq_end - nameq));
        if (!cn) { free_parsed_row(out);
                   for (uint32_t i = 0; i < n; ++i) free(names[i]);
                   return SPFY_E_NOMEM; }

        /* Parse float between gt+1 and close. */
        char buf[32];
        size_t num_n = (size_t)(close - (gt + 1));
        if (num_n >= sizeof buf) num_n = sizeof buf - 1;
        memcpy(buf, gt + 1, num_n);
        buf[num_n] = '\0';
        char *endp = NULL;
        float v = (float)strtod(buf, &endp);
        names[n] = cn;
        vals[n]  = v;
        ++n;
        p = close + 13;        /* past "</namedValue>" */
    }

    if (n == 0) { free_parsed_row(out); return SPFY_E_FORMAT; }
    out->col_names = (char **)calloc(n, sizeof *out->col_names);
    out->col_values = (float *)calloc(n, sizeof *out->col_values);
    if (!out->col_names || !out->col_values) {
        for (uint32_t i = 0; i < n; ++i) free(names[i]);
        free_parsed_row(out); return SPFY_E_NOMEM;
    }
    for (uint32_t i = 0; i < n; ++i) {
        out->col_names[i]  = names[i];
        out->col_values[i] = vals[i];
    }
    out->nv_count = n;
    return SPFY_OK;
}

static int load_one_kind(const char *xml, size_t xml_n,
                         const char *kind, spfy_proscost_matrix_t *out)
{
    memset(out, 0, sizeof *out);

    /* Build the search prefix: "tts.voiceCfg.proscost.<kind>." */
    char prefix[128];
    int rc = snprintf(prefix, sizeof prefix,
                      "tts.voiceCfg.proscost.%s.", kind);
    if (rc < 0 || (size_t)rc >= sizeof prefix) return SPFY_E_FORMAT;

    /* Pass 1: discover all <param> blocks for this matrix. We hold up to
     * 64 rows. */
    parsed_row_t rows[64];
    uint32_t     n_rows = 0;
    const char  *p   = xml;
    const char  *end = xml + xml_n;

    while (p < end && n_rows < 64) {
        const char *param_open = bounded_str(p, (size_t)(end - p), "<param ");
        if (!param_open) break;
        const char *param_close = bounded_str(param_open,
                                              (size_t)(end - param_open),
                                              "</param>");
        if (!param_close) break;
        param_close += 8;     /* include "</param>" */

        /* Match this <param> against our prefix. */
        const char *prefix_pos = bounded_str(param_open,
                                             (size_t)(param_close - param_open),
                                             prefix);
        if (prefix_pos) {
            int prc = parse_one_row(param_open, param_close, prefix,
                                    &rows[n_rows]);
            if (prc != SPFY_OK) {
                /* Roll back any rows already parsed. */
                for (uint32_t i = 0; i < n_rows; ++i) free_parsed_row(&rows[i]);
                return prc;
            }
            ++n_rows;
        }
        p = param_close;
    }

    if (n_rows == 0) {
        /* Matrix not present (e.g. phoneInSylCosts may be missing in Tom). */
        return SPFY_OK;
    }

    /* Pass 2: validate column-name consistency across rows -- they must all
     * have the same nv_count and the same names in the same order. */
    uint32_t n_cols = rows[0].nv_count;
    for (uint32_t i = 1; i < n_rows; ++i) {
        if (rows[i].nv_count != n_cols) {
            spfy_log_err("vcf_matrix(%s): row '%s' has %u cols, expected %u",
                         kind, rows[i].row_name, rows[i].nv_count, n_cols);
            for (uint32_t j = 0; j < n_rows; ++j) free_parsed_row(&rows[j]);
            return SPFY_E_FORMAT;
        }
        for (uint32_t k = 0; k < n_cols; ++k) {
            if (strcmp(rows[i].col_names[k], rows[0].col_names[k]) != 0) {
                spfy_log_err("vcf_matrix(%s): column %u name mismatch "
                             "(row '%s' has '%s', expected '%s')",
                             kind, k, rows[i].row_name,
                             rows[i].col_names[k], rows[0].col_names[k]);
                for (uint32_t j = 0; j < n_rows; ++j) free_parsed_row(&rows[j]);
                return SPFY_E_FORMAT;
            }
        }
    }

    /* Build output: flat row-major float array + name arrays. */
    out->data = (float *)calloc((size_t)n_rows * n_cols, sizeof *out->data);
    out->row_names = (char **)calloc(n_rows, sizeof *out->row_names);
    out->col_names = (char **)calloc(n_cols, sizeof *out->col_names);
    if (!out->data || !out->row_names || !out->col_names) {
        for (uint32_t j = 0; j < n_rows; ++j) free_parsed_row(&rows[j]);
        free(out->data); free(out->row_names); free(out->col_names);
        memset(out, 0, sizeof *out);
        return SPFY_E_NOMEM;
    }
    out->n_rows = n_rows;
    out->n_cols = n_cols;

    /* Place each row at the index whose col_name matches the row_name.
     * The engine indexes the matrix as [row_label_idx][col_label_idx]
     * with row_label and col_label drawn from the SAME label vocabulary
     * (e.g. "PhrInitial" maps to label index 1 both as a target row and
     * a candidate column). VCF rows arrive in encounter order which
     * does NOT match the col-label order, so we need to remap.
     *
     * Rows whose name doesn't appear in col_names (e.g. "ContextUnknown",
     * "FirstPA") get appended at the END (row indices >= n_cols). The
     * engine's lookup of those rows still works because the actual usage
     * is always row=col_label_idx. The engine never asks for row indices
     * past n_cols-1 in normal scoring. */
    uint8_t placed[64] = {0};
    for (uint32_t i = 0; i < n_rows; ++i) {
        int placed_at = -1;
        for (uint32_t k = 0; k < n_cols; ++k) {
            if (strcmp(rows[i].row_name, rows[0].col_names[k]) == 0 &&
                !placed[k]) {
                placed_at = (int)k;
                placed[k] = 1;
                break;
            }
        }
        if (placed_at < 0) {
            /* Append at end. */
            for (uint32_t k = n_cols; k < n_rows; ++k) {
                if (!placed[k]) {
                    placed_at = (int)k;
                    placed[k] = 1;
                    break;
                }
            }
        }
        if (placed_at < 0) placed_at = (int)i;     /* fallback */
        out->row_names[placed_at] = rows[i].row_name;
        rows[i].row_name = NULL;
        for (uint32_t k = 0; k < n_cols; ++k) {
            out->data[(size_t)placed_at * n_cols + k] = rows[i].col_values[k];
        }
    }
    /* Adopt col_names from row 0; free others. */
    for (uint32_t k = 0; k < n_cols; ++k) {
        out->col_names[k] = rows[0].col_names[k];
        rows[0].col_names[k] = NULL;
    }
    for (uint32_t i = 0; i < n_rows; ++i) free_parsed_row(&rows[i]);
    return SPFY_OK;
}

int spfy_proscost_load(const spfy_vcf_t *vcf,
                       spfy_proscost_matrix_t out[SPFY_PROSCOST_N])
{
    if (!vcf || !out) return SPFY_E_INVAL;
    if (!vcf->xml_bytes || vcf->xml_n == 0) return SPFY_E_FORMAT;

    memset(out, 0, sizeof *out * SPFY_PROSCOST_N);

    for (int k = 0; k < SPFY_PROSCOST_N; ++k) {
        int rc = load_one_kind((const char *)vcf->xml_bytes, vcf->xml_n,
                               KIND_NAME[k], &out[k]);
        if (rc != SPFY_OK) {
            spfy_proscost_free(out);
            return rc;
        }
    }
    return SPFY_OK;
}

void spfy_proscost_free(spfy_proscost_matrix_t mats[SPFY_PROSCOST_N])
{
    if (!mats) return;
    for (int k = 0; k < SPFY_PROSCOST_N; ++k) {
        free(mats[k].data);
        free_string_array(mats[k].row_names, mats[k].n_rows);
        free_string_array(mats[k].col_names, mats[k].n_cols);
        memset(&mats[k], 0, sizeof mats[k]);
    }
}

int spfy_proscost_col_idx(const spfy_proscost_matrix_t *m, const char *name)
{
    if (!m || !name || !m->col_names) return -1;
    for (uint32_t i = 0; i < m->n_cols; ++i) {
        if (strcmp(m->col_names[i], name) == 0) return (int)i;
    }
    return -1;
}
