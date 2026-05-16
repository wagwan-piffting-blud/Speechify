/* spfy_fe tables loader: 553 + 173 = 726 carved blobs. */

#include "tables.h"

#include <spfy/spfy.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int load_one(const char *dir, uint32_t idx, uint8_t **out, uint32_t *out_n)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/t%03u.bin", dir, idx);
    FILE *fp = fopen(path, "rb");
    if (!fp) return SPFY_E_IO;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0 || sz > (1 << 24)) {     /* 16 MB sanity cap */
        fclose(fp);
        return SPFY_E_FORMAT;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(fp); return SPFY_E_NOMEM; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf); fclose(fp); return SPFY_E_IO;
    }
    fclose(fp);
    *out   = buf;
    *out_n = (uint32_t)sz;
    return SPFY_OK;
}

int spfy_fe_tables_load(const char       *tables_a_dir,
                         const char       *tables_b_dir,
                         spfy_fe_tables_t *out)
{
    memset(out, 0, sizeof *out);
    int rc = SPFY_OK;

    /* Two-pass load: first compute total arena size, then alloc and
     * copy. Saves N-fragmented heap allocations. */
    uint32_t  total = 0;
    uint8_t **tmp_a = (uint8_t **)calloc(SPFY_FE_REG_A_N, sizeof *tmp_a);
    uint8_t **tmp_b = (uint8_t **)calloc(SPFY_FE_REG_B_N, sizeof *tmp_b);
    if (!tmp_a || !tmp_b) {
        rc = SPFY_E_NOMEM;
        goto fail;
    }

    for (uint32_t i = 0; i < SPFY_FE_REG_A_N; ++i) {
        uint32_t n = 0;
        rc = load_one(tables_a_dir, i, &tmp_a[i], &n);
        if (rc != SPFY_OK) goto fail;
        out->a[i].size = n;
        total += n;
    }
    for (uint32_t i = 0; i < SPFY_FE_REG_B_N; ++i) {
        uint32_t n = 0;
        rc = load_one(tables_b_dir, i, &tmp_b[i], &n);
        if (rc != SPFY_OK) goto fail;
        out->b[i].size = n;
        total += n;
    }

    /* Concatenate into a single arena. */
    out->arena_size = total;
    out->arena = (uint8_t *)malloc(total ? total : 1);
    if (!out->arena) { rc = SPFY_E_NOMEM; goto fail; }

    size_t off = 0;
    for (uint32_t i = 0; i < SPFY_FE_REG_A_N; ++i) {
        memcpy(out->arena + off, tmp_a[i], out->a[i].size);
        out->a[i].data = out->arena + off;
        off += out->a[i].size;
        free(tmp_a[i]); tmp_a[i] = NULL;
    }
    for (uint32_t i = 0; i < SPFY_FE_REG_B_N; ++i) {
        memcpy(out->arena + off, tmp_b[i], out->b[i].size);
        out->b[i].data = out->arena + off;
        off += out->b[i].size;
        free(tmp_b[i]); tmp_b[i] = NULL;
    }

    free(tmp_a); free(tmp_b);
    return SPFY_OK;

fail:
    if (tmp_a) for (uint32_t i = 0; i < SPFY_FE_REG_A_N; ++i) free(tmp_a[i]);
    if (tmp_b) for (uint32_t i = 0; i < SPFY_FE_REG_B_N; ++i) free(tmp_b[i]);
    free(tmp_a); free(tmp_b);
    spfy_fe_tables_free(out);
    return rc;
}

void spfy_fe_tables_free(spfy_fe_tables_t *t)
{
    if (!t) return;
    for (uint32_t i = 0; i < SPFY_FE_REG_A_N; ++i) free(t->a[i].record_offs);
    for (uint32_t i = 0; i < SPFY_FE_REG_B_N; ++i) free(t->b[i].record_offs);
    free(t->arena);
    memset(t, 0, sizeof *t);
}

uint32_t spfy_fe_table_index(spfy_fe_table_t *t)
{
    if (t->record_offs) return t->n_records;
    /* Records are NUL-terminated. Count records first, then alloc. */
    uint32_t n = 0;
    for (uint32_t i = 0; i < t->size; ++i) {
        if (t->data[i] == 0) ++n;
    }
    /* Trailing data without NUL still counts as a record. */
    if (t->size > 0 && t->data[t->size - 1] != 0) ++n;
    if (n == 0) {
        t->n_records = 0;
        return 0;
    }
    t->record_offs = (uint32_t *)malloc(n * sizeof *t->record_offs);
    if (!t->record_offs) return 0;
    uint32_t k = 0;
    uint32_t start = 0;
    int in_record = 0;
    for (uint32_t i = 0; i < t->size; ++i) {
        if (t->data[i] == 0) {
            if (in_record) {
                t->record_offs[k++] = start;
                in_record = 0;
            }
        } else {
            if (!in_record) {
                start = i;
                in_record = 1;
            }
        }
    }
    if (in_record && k < n) {
        t->record_offs[k++] = start;
    }
    t->n_records = k;
    return k;
}

uint32_t spfy_fe_table_walk(const spfy_fe_table_t *t,
                             spfy_fe_record_cb     cb,
                             void                 *user)
{
    uint32_t n = 0;
    uint32_t start = 0;
    int      in_record = 0;
    for (uint32_t i = 0; i < t->size; ++i) {
        if (t->data[i] == 0) {
            if (in_record) {
                if (cb && cb(t->data + start, i - start, n, user)) return n + 1;
                ++n;
                in_record = 0;
            }
        } else {
            if (!in_record) {
                start = i;
                in_record = 1;
            }
        }
    }
    if (in_record) {
        if (cb) cb(t->data + start, t->size - start, n, user);
        ++n;
    }
    return n;
}
