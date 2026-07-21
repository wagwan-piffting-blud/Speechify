/* ckls + cklx loader. Format per reveng/README_TECHNICAL.md "cklx" /
 * "ckls" sections; Python parser at c:/tmp/cklx_ckls_parse.py validated
 * cross-checks. */

#include "chunk_table.h"

#include "../common/log.h"
#include "../../include/spfy/spfy.h"

#include <stdlib.h>
#include <string.h>

static uint16_t le_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le_u32(const uint8_t *p)
{
    return (uint32_t)p[0]        | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Parse cklx chunk:
 *   u32 group_count        (= 2)
 *   for each group:
 *     u16 name_len; char[name_len] group_name
 *     u32 entry_count
 *     for each entry:
 *       u16 key_len; char[key_len] key
 *       u32 posting_count
 *       u32[posting_count] posting_ids
 */
static int parse_cklx(const uint8_t *data, size_t n, spfy_chunk_tables_t *out)
{
    const uint8_t *p   = data;
    const uint8_t *end = data + n;
    if ((size_t)(end - p) < 4) return SPFY_E_FORMAT;

    uint32_t n_groups = le_u32(p); p += 4;
    if (n_groups != 2) return SPFY_E_FORMAT;

    for (uint32_t gi = 0; gi < 2; ++gi) {
        if ((size_t)(end - p) < 2) return SPFY_E_FORMAT;
        uint16_t nm_len = le_u16(p); p += 2;
        if ((size_t)(end - p) < nm_len + 4u) return SPFY_E_FORMAT;
        const char *gname = (const char *)p;
        p += nm_len;
        uint32_t group_idx;
        if (nm_len == 6 && memcmp(gname, "_WORD_", 6) == 0)
            group_idx = SPFY_CHUNK_GROUP_WORD;
        else if (nm_len == 5 && memcmp(gname, "_SYL_", 5) == 0)
            group_idx = SPFY_CHUNK_GROUP_SYL;
        else
            return SPFY_E_FORMAT;

        uint32_t n_entries = le_u32(p); p += 4;
        spfy_cklx_group_t *g = &out->cklx[group_idx];
        g->n_keys = n_entries;
        /* An empty group is legitimate -- Felix (fr-CA) ships no word
         * chunks at all. calloc(0) may return NULL, which would otherwise
         * be misread as OOM. */
        if (n_entries == 0) {
            g->postings_offset = (uint32_t *)calloc(1,
                                                sizeof *g->postings_offset);
            if (!g->postings_offset) return SPFY_E_NOMEM;
            continue;
        }
        g->keys            = (char **)calloc(n_entries, sizeof *g->keys);
        g->postings_offset = (uint32_t *)calloc(n_entries + 1,
                                                sizeof *g->postings_offset);
        if (!g->keys || !g->postings_offset) return SPFY_E_NOMEM;

        /* Two-pass: first count total postings, then fill. */
        const uint8_t *p_save = p;
        uint32_t total = 0;
        for (uint32_t i = 0; i < n_entries; ++i) {
            if ((size_t)(end - p) < 2) return SPFY_E_FORMAT;
            uint16_t klen = le_u16(p); p += 2;
            if ((size_t)(end - p) < klen + 4u) return SPFY_E_FORMAT;
            p += klen;
            uint32_t npost = le_u32(p); p += 4;
            if ((size_t)(end - p) < npost * 4u) return SPFY_E_FORMAT;
            p += npost * 4u;
            total += npost;
        }
        g->postings   = (uint32_t *)calloc(total, sizeof *g->postings);
        g->n_postings = total;
        if (total > 0 && !g->postings) return SPFY_E_NOMEM;

        p = p_save;
        uint32_t posting_off = 0;
        for (uint32_t i = 0; i < n_entries; ++i) {
            uint16_t klen = le_u16(p); p += 2;
            char *k = (char *)malloc((size_t)klen + 1);
            if (!k) return SPFY_E_NOMEM;
            memcpy(k, p, klen); k[klen] = 0;
            g->keys[i] = k;
            p += klen;
            uint32_t npost = le_u32(p); p += 4;
            g->postings_offset[i] = posting_off;
            for (uint32_t j = 0; j < npost; ++j) {
                g->postings[posting_off + j] = le_u32(p + j * 4);
            }
            posting_off += npost;
            p += npost * 4u;
        }
        g->postings_offset[n_entries] = posting_off;
    }
    return SPFY_OK;
}

/* Parse ckls chunk:
 *   u32 group_count        (= 2)
 *   for each group:
 *     u16 name_len; char[name_len] group_name
 *     u32 token_count
 *     u32 unk0                     -- ONLY when token_count > 0
 *     for each (token, filename) record pair (token_count of each):
 *       token: u16 len; char[len] text; u32 ss; u32 se
 *       filename: u16 len; char[len] fname; u32 file_id
 *         (final filename has no trailing u32)
 *
 * The conditional unk0 was found 2026-07-20 on Felix (fr-CA), whose
 * _WORD_ group is genuinely EMPTY -- he ships syllable chunks but no word
 * chunks. Reading unk0 unconditionally consumed the next group's name
 * record (the bytes decode as u16 len=5 + "_S", i.e. 0x535F0005) and the
 * parse then ran off the end of the chunk. With the field made
 * conditional, all five shipped voices consume their ckls payload
 * exactly: 394375 / 355308 / 816044 / 1013449 / 5234800 bytes. */
static int parse_ckls(const uint8_t *data, size_t n,
                      spfy_chunk_tables_t *out)
{
    const uint8_t *p   = data;
    const uint8_t *end = data + n;
    if ((size_t)(end - p) < 4) return SPFY_E_FORMAT;
    uint32_t n_groups = le_u32(p); p += 4;
    if (n_groups != 2) return SPFY_E_FORMAT;

    for (uint32_t gi = 0; gi < 2; ++gi) {
        if ((size_t)(end - p) < 2) return SPFY_E_FORMAT;
        uint16_t nm_len = le_u16(p); p += 2;
        if ((size_t)(end - p) < nm_len + 8u) return SPFY_E_FORMAT;
        const char *gname = (const char *)p;
        p += nm_len;
        uint32_t group_idx;
        if (nm_len == 6 && memcmp(gname, "_WORD_", 6) == 0)
            group_idx = SPFY_CHUNK_GROUP_WORD;
        else if (nm_len == 5 && memcmp(gname, "_SYL_", 5) == 0)
            group_idx = SPFY_CHUNK_GROUP_SYL;
        else
            return SPFY_E_FORMAT;

        uint32_t token_count = le_u32(p); p += 4;
        if (token_count > 0) {
            if ((size_t)(end - p) < 4) return SPFY_E_FORMAT;
            p += 4;   /* unk0 -- absent entirely when the group is empty */
        }
        spfy_ckls_group_t *g = &out->ckls[group_idx];
        g->n_postings = token_count;
        if (token_count == 0) continue;   /* nothing to allocate or read */
        g->span_start = (uint32_t *)calloc(token_count, sizeof *g->span_start);
        g->span_end   = (uint32_t *)calloc(token_count, sizeof *g->span_end);
        g->token_text = (char    **)calloc(token_count, sizeof *g->token_text);
        if (!g->span_start || !g->span_end || !g->token_text)
            return SPFY_E_NOMEM;

        for (uint32_t k = 0; k < token_count; ++k) {
            /* Token record */
            if ((size_t)(end - p) < 2) return SPFY_E_FORMAT;
            uint16_t tlen = le_u16(p); p += 2;
            if ((size_t)(end - p) < tlen + 8u) return SPFY_E_FORMAT;
            char *tt = (char *)malloc((size_t)tlen + 1);
            if (!tt) return SPFY_E_NOMEM;
            memcpy(tt, p, tlen); tt[tlen] = 0;
            g->token_text[k] = tt;
            p += tlen;
            g->span_start[k] = le_u32(p); p += 4;
            g->span_end[k]   = le_u32(p); p += 4;
            /* Filename record */
            if ((size_t)(end - p) < 2) return SPFY_E_FORMAT;
            uint16_t flen = le_u16(p); p += 2;
            if ((size_t)(end - p) < flen) return SPFY_E_FORMAT;
            p += flen;
            if (k + 1 < token_count) {
                if ((size_t)(end - p) < 4) return SPFY_E_FORMAT;
                p += 4;   /* file_id */
            }
        }
    }
    return SPFY_OK;
}

int spfy_chunk_tables_load(const spfy_vin_t *vin,
                           spfy_chunk_tables_t *out)
{
    if (!vin || !out) return SPFY_E_INVAL;
    if (!vin->cklx || !vin->ckls) return SPFY_E_FORMAT;
    memset(out, 0, sizeof *out);

    int rc = parse_cklx(vin->cklx, vin->cklx_n, out);
    if (rc != SPFY_OK) { spfy_chunk_tables_free(out); return rc; }
    rc = parse_ckls(vin->ckls, vin->ckls_n, out);
    if (rc != SPFY_OK) { spfy_chunk_tables_free(out); return rc; }

    /* Cross-check: cklx posting count == ckls token count. */
    for (uint32_t gi = 0; gi < 2; ++gi) {
        if (out->cklx[gi].n_postings != out->ckls[gi].n_postings) {
            spfy_log_err("chunk_tables: group %u posting mismatch "
                         "cklx=%u ckls=%u", gi,
                         out->cklx[gi].n_postings,
                         out->ckls[gi].n_postings);
            spfy_chunk_tables_free(out);
            return SPFY_E_FORMAT;
        }
    }
    return SPFY_OK;
}

void spfy_chunk_tables_free(spfy_chunk_tables_t *t)
{
    if (!t) return;
    for (uint32_t gi = 0; gi < 2; ++gi) {
        spfy_cklx_group_t *cx = &t->cklx[gi];
        if (cx->keys) {
            for (uint32_t i = 0; i < cx->n_keys; ++i) free(cx->keys[i]);
            free(cx->keys);
        }
        free(cx->postings_offset);
        free(cx->postings);

        spfy_ckls_group_t *cs = &t->ckls[gi];
        free(cs->span_start);
        free(cs->span_end);
        if (cs->token_text) {
            for (uint32_t i = 0; i < cs->n_postings; ++i)
                free(cs->token_text[i]);
            free(cs->token_text);
        }
    }
    memset(t, 0, sizeof *t);
}

int spfy_cklx_lookup(const spfy_cklx_group_t *g,
                     const char *key,
                     const uint32_t **out_postings,
                     uint32_t *out_count)
{
    if (!g || !key) return 0;
    /* Binary search; cklx keys are sorted ASCII per the docs. */
    int32_t lo = 0, hi = (int32_t)g->n_keys - 1;
    while (lo <= hi) {
        int32_t mid = (lo + hi) / 2;
        int cmp = strcmp(key, g->keys[mid]);
        if (cmp == 0) {
            uint32_t off  = g->postings_offset[mid];
            uint32_t off1 = g->postings_offset[mid + 1];
            if (out_postings) *out_postings = g->postings + off;
            if (out_count)    *out_count    = off1 - off;
            return 1;
        }
        if (cmp < 0) hi = mid - 1;
        else         lo = mid + 1;
    }
    return 0;
}
