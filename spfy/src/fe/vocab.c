/* spfy_fe vocab loader: 469-symbol vocabulary from JSON. */

#include "vocab.h"

#include <spfy/spfy.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Tiny JSON walker -- the vocab JSON has a well-known shape:
 *   [
 *     {"idx":0, "va":"0x...", "name":"GAP"},
 *     {"idx":1, "va":"0x...", "name":"a"},
 *     ...
 *   ]
 * We don't need full JSON parsing; just locate `"name":"..."` strings
 * by entry index. */
static int read_file(const char *path, char **out, size_t *out_n)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return SPFY_E_IO;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)sz + 1);
    if (!b) { fclose(fp); return SPFY_E_NOMEM; }
    if (fread(b, 1, (size_t)sz, fp) != (size_t)sz) {
        free(b); fclose(fp); return SPFY_E_IO;
    }
    b[sz] = 0;
    fclose(fp);
    *out   = b;
    *out_n = (size_t)sz;
    return SPFY_OK;
}

/* Find the next `"name":` substring after p, return ptr to first char of
 * the value (just past the colon). On `"name":null` returns ptr to "n". */
static const char *next_name_field(const char *p, const char *end)
{
    static const char needle[] = "\"name\":";
    size_t n = sizeof needle - 1;
    for (const char *q = p; q + n <= end; ++q) {
        if (memcmp(q, needle, n) == 0) return q + n;
    }
    return NULL;
}

int spfy_fe_vocab_load(const char *json_path, spfy_fe_vocab_t *out)
{
    char  *buf = NULL;
    size_t buf_n = 0;
    int    rc = read_file(json_path, &buf, &buf_n);
    if (rc != SPFY_OK) return rc;
    memset(out, 0, sizeof *out);

    const char *p   = buf;
    const char *end = buf + buf_n;
    uint32_t idx = 0;
    while (idx < SPFY_FE_VOCAB_N) {
        const char *v = next_name_field(p, end);
        if (!v) break;
        p = v;
        /* Skip whitespace. */
        while (p < end && (*p == ' ' || *p == '\t')) ++p;
        if (p >= end) break;

        spfy_fe_symbol_t *e = &out->entries[idx];
        if (*p == 'n') {
            /* "null" -- non-ASCII raw byte slot; we have no display
             * name. The actual byte is in the va field but for
             * Path-B we only need the printable name; raw_byte stays
             * 0 and lookups fall through to the slot index itself. */
            e->name = NULL;
            e->raw_byte = 0;
            p += 4;
        } else if (*p == '"') {
            const char *str_start = p + 1;
            const char *str_end = str_start;
            while (str_end < end && *str_end != '"') {
                if (*str_end == '\\' && str_end + 1 < end) ++str_end;
                ++str_end;
            }
            size_t len = (size_t)(str_end - str_start);
            char *copy = (char *)malloc(len + 1);
            if (!copy) { rc = SPFY_E_NOMEM; goto fail; }
            memcpy(copy, str_start, len);
            copy[len] = 0;
            /* Unescape \" and \\ inline. */
            char *r = copy, *w = copy;
            while (*r) {
                if (r[0] == '\\' && (r[1] == '"' || r[1] == '\\')) {
                    *w++ = r[1]; r += 2;
                } else {
                    *w++ = *r++;
                }
            }
            *w = 0;
            e->name = copy;
            e->raw_byte = 0;
            /* If single-char, treat as character ID. */
            p = str_end < end ? str_end + 1 : str_end;
        } else {
            /* Unexpected token; skip. */
            ++p;
        }
        ++idx;
    }
    out->n = idx;
    free(buf);
    if (idx != SPFY_FE_VOCAB_N) {
        spfy_fe_vocab_free(out);
        return SPFY_E_FORMAT;
    }

    /* Build the byte -> symbol-ID reverse table. Single-char ASCII
     * names are the primary populator; exact byte = name[0].
     * Two cases need extra care:
     *   - Multiple slots can map to the same byte (e.g., the apostrophe
     *     'X' appears in ASCII range and Latin-1 range). The earliest
     *     slot wins -- that's the "primary" representation the FE
     *     uses for input characters.
     *   - Slot 0 (GAP) is special; we treat it as "no input byte". */
    for (int i = 0; i < 256; ++i) out->byte_to_id[i] = 0xFFFFu;
    for (uint32_t i = 1; i < out->n; ++i) {
        const char *nm = out->entries[i].name;
        if (nm && nm[0] && nm[1] == 0) {
            uint8_t b = (uint8_t)nm[0];
            if (out->byte_to_id[b] == 0xFFFFu)
                out->byte_to_id[b] = (uint16_t)i;
        }
    }
    return SPFY_OK;

fail:
    free(buf);
    spfy_fe_vocab_free(out);
    return rc;
}

void spfy_fe_vocab_free(spfy_fe_vocab_t *v)
{
    if (!v) return;
    for (uint32_t i = 0; i < v->n; ++i) {
        free((char *)v->entries[i].name);
    }
    memset(v, 0, sizeof *v);
}

const char *spfy_fe_vocab_name(const spfy_fe_vocab_t *v, uint32_t id)
{
    if (id >= v->n) return NULL;
    return v->entries[id].name;
}

uint32_t spfy_fe_vocab_id(const spfy_fe_vocab_t *v, const char *name)
{
    if (!name) return 0xFFFFFFFFu;
    for (uint32_t i = 0; i < v->n; ++i) {
        const char *e = v->entries[i].name;
        if (e && strcmp(e, name) == 0) return i;
    }
    return 0xFFFFFFFFu;
}
