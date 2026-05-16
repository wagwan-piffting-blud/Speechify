#include "vdb_lookup.h"

#include "../../include/spfy/spfy.h"

#include <stdlib.h>
#include <string.h>

static int compare_indx_entries(const void *a, const void *b, void *ctx)
{
    const spfy_vdb_t *vdb = (const spfy_vdb_t *)ctx;
    uint32_t ai = *(const uint32_t *)a;
    uint32_t bi = *(const uint32_t *)b;
    const struct spfy_indx_entry *ea = &vdb->indx_entries[ai];
    const struct spfy_indx_entry *eb = &vdb->indx_entries[bi];

    size_t n = ea->name_len < eb->name_len ? ea->name_len : eb->name_len;
    int c = memcmp(ea->name, eb->name, n);
    if (c) return c;
    if (ea->name_len < eb->name_len) return -1;
    if (ea->name_len > eb->name_len) return  1;
    return 0;
}

/* qsort_r is non-portable; use a thread-local trick: stash ctx in a
 * file-static and use plain qsort. Single-threaded loader makes this
 * safe in our context. */
static const spfy_vdb_t *g_sort_ctx_vdb;
static int compare_indx_entries_qsort(const void *a, const void *b)
{
    return compare_indx_entries(a, b, (void *)g_sort_ctx_vdb);
}

int spfy_vdb_lookup_build(const spfy_vdb_t *vdb, spfy_vdb_lookup_t *out)
{
    if (!vdb || !out) return SPFY_E_INVAL;
    memset(out, 0, sizeof *out);
    out->vdb = vdb;
    if (vdb->n_indx_entries == 0) return SPFY_OK;

    out->order = (uint32_t *)malloc(vdb->n_indx_entries * sizeof *out->order);
    if (!out->order) return SPFY_E_NOMEM;
    for (uint32_t i = 0; i < vdb->n_indx_entries; ++i) out->order[i] = i;
    out->n_entries = vdb->n_indx_entries;

    g_sort_ctx_vdb = vdb;
    qsort(out->order, out->n_entries, sizeof *out->order,
          compare_indx_entries_qsort);
    g_sort_ctx_vdb = NULL;
    return SPFY_OK;
}

void spfy_vdb_lookup_free(spfy_vdb_lookup_t *l)
{
    if (!l) return;
    free(l->order);
    memset(l, 0, sizeof *l);
}

int spfy_vdb_lookup_by_name(const spfy_vdb_lookup_t *l,
                            const char *name, size_t name_len,
                            uint32_t *out_offset, uint32_t *out_size)
{
    if (!l || !name || !out_offset || !out_size) return SPFY_E_INVAL;
    if (!l->vdb || !l->order || l->n_entries == 0) return SPFY_E_INVAL;

    uint32_t lo = 0, hi = l->n_entries;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        const struct spfy_indx_entry *e =
            &l->vdb->indx_entries[l->order[mid]];
        size_t n = name_len < e->name_len ? name_len : e->name_len;
        int c = memcmp(name, e->name, n);
        if (c == 0) {
            if (name_len < e->name_len) c = -1;
            else if (name_len > e->name_len) c = 1;
        }
        if (c == 0) {
            /* Hit. Compute size as offset_of(next_entry_in_FILE_order) -
             * this entry's offset. The indx sentinel last entry holds
             * data_chunk_total_size as offset. */
            uint32_t my_idx = l->order[mid];
            uint32_t off    = e->data_offset;
            uint32_t size   = 0;
            if (my_idx + 1 < l->vdb->n_indx_entries) {
                size = l->vdb->indx_entries[my_idx + 1].data_offset - off;
            } else {
                size = (uint32_t)l->vdb->data_n - off;
            }
            *out_offset = off;
            *out_size   = size;
            return SPFY_OK;
        }
        if (c < 0) hi = mid;
        else       lo = mid + 1;
    }
    *out_offset = 0;
    *out_size   = 0;
    return SPFY_E_OOB;
}
