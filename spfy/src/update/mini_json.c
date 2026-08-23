#include "mini_json.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *s;
    mj_node    *n;
    int         cap;
    int         count;
    int         err;        /* -2 once the pool is exhausted */
} P;

static int p_alloc(P *p, mj_type t)
{
    int i;
    if (p->count >= p->cap) { p->err = -2; return -1; }
    i = p->count++;
    memset(&p->n[i], 0, sizeof p->n[i]);
    p->n[i].type  = t;
    p->n[i].first = -1;
    p->n[i].next  = -1;
    return i;
}

static void skip_ws(P *p)
{
    while (*p->s == ' ' || *p->s == '\t' || *p->s == '\n' || *p->s == '\r')
        p->s++;
}

/* Advance past a string literal, leaving `body`/`body_n` on its contents.
 * Escapes are validated only far enough to find the closing quote. */
static int scan_string(P *p, const char **body, int *body_n)
{
    const char *start;
    if (*p->s != '"') return -1;
    p->s++;
    start = p->s;
    while (*p->s && *p->s != '"') {
        if (*p->s == '\\') {
            if (!p->s[1]) return -1;
            p->s += 2;
        } else {
            p->s++;
        }
    }
    if (*p->s != '"') return -1;
    *body   = start;
    *body_n = (int)(p->s - start);
    p->s++;
    return 0;
}

static int parse_value(P *p);

static int parse_object(P *p)
{
    int obj = p_alloc(p, MJ_OBJ);
    int last = -1;
    if (obj < 0) return -1;
    p->s++;                                  /* '{' */
    skip_ws(p);
    if (*p->s == '}') { p->s++; return obj; }

    for (;;) {
        const char *k;
        int k_n, child;

        skip_ws(p);
        if (scan_string(p, &k, &k_n) != 0) return -1;
        skip_ws(p);
        if (*p->s != ':') return -1;
        p->s++;
        child = parse_value(p);
        if (child < 0) return -1;
        p->n[child].key   = k;
        p->n[child].key_n = k_n;
        if (last < 0) p->n[obj].first = child;
        else          p->n[last].next = child;
        last = child;

        skip_ws(p);
        if (*p->s == ',') { p->s++; continue; }
        if (*p->s == '}') { p->s++; return obj; }
        return -1;
    }
}

static int parse_array(P *p)
{
    int arr = p_alloc(p, MJ_ARR);
    int last = -1;
    if (arr < 0) return -1;
    p->s++;                                  /* '[' */
    skip_ws(p);
    if (*p->s == ']') { p->s++; return arr; }

    for (;;) {
        int child = parse_value(p);
        if (child < 0) return -1;
        if (last < 0) p->n[arr].first = child;
        else          p->n[last].next = child;
        last = child;

        skip_ws(p);
        if (*p->s == ',') { p->s++; continue; }
        if (*p->s == ']') { p->s++; return arr; }
        return -1;
    }
}

static int parse_value(P *p)
{
    skip_ws(p);
    switch (*p->s) {
    case '{': return parse_object(p);
    case '[': return parse_array(p);
    case '"': {
        const char *b;
        int b_n;
        int i;
        if (scan_string(p, &b, &b_n) != 0) return -1;
        i = p_alloc(p, MJ_STR);
        if (i < 0) return -1;
        p->n[i].raw   = b;
        p->n[i].raw_n = b_n;
        return i;
    }
    case 't':
        if (strncmp(p->s, "true", 4) != 0) return -1;
        p->s += 4;
        { int i = p_alloc(p, MJ_BOOL); if (i >= 0) p->n[i].bval = 1; return i; }
    case 'f':
        if (strncmp(p->s, "false", 5) != 0) return -1;
        p->s += 5;
        { int i = p_alloc(p, MJ_BOOL); if (i >= 0) p->n[i].bval = 0; return i; }
    case 'n':
        if (strncmp(p->s, "null", 4) != 0) return -1;
        p->s += 4;
        return p_alloc(p, MJ_NULL);
    default: {
        char *endp = NULL;
        double v;
        int i;
        v = strtod(p->s, &endp);
        if (!endp || endp == p->s) return -1;
        i = p_alloc(p, MJ_NUM);
        if (i < 0) return -1;
        p->n[i].raw   = p->s;
        p->n[i].raw_n = (int)(endp - p->s);
        p->n[i].num   = v;
        p->s = endp;
        return i;
    }
    }
}

int mj_parse(const char *src, mj_node *nodes, int cap, mj_doc *doc)
{
    P p;
    int root;

    if (!src || !nodes || cap <= 0 || !doc) return -1;
    p.s = src; p.n = nodes; p.cap = cap; p.count = 0; p.err = 0;

    root = parse_value(&p);
    if (root < 0) return p.err ? p.err : -1;
    if (root != 0) return -1;               /* root must be the first node */

    doc->n     = nodes;
    doc->cap   = cap;
    doc->count = p.count;
    return 0;
}

int mj_obj_get(const mj_doc *d, int obj, const char *key)
{
    int i;
    size_t n;
    if (!d || obj < 0 || obj >= d->count || d->n[obj].type != MJ_OBJ) return -1;
    n = strlen(key);
    for (i = d->n[obj].first; i >= 0; i = d->n[i].next) {
        if (d->n[i].key && (size_t)d->n[i].key_n == n &&
            memcmp(d->n[i].key, key, n) == 0)
            return i;
    }
    return -1;
}

int mj_arr_first(const mj_doc *d, int arr)
{
    if (!d || arr < 0 || arr >= d->count) return -1;
    if (d->n[arr].type != MJ_ARR && d->n[arr].type != MJ_OBJ) return -1;
    return d->n[arr].first;
}

int mj_next(const mj_doc *d, int idx)
{
    if (!d || idx < 0 || idx >= d->count) return -1;
    return d->n[idx].next;
}

/* One \uXXXX escape -> UTF-8. Consumes a low surrogate when it follows a
 * high one. `p` points just past the 'u'; returns the new position. */
static const char *utf16_escape(const char *p, const char *end,
                                char *out, size_t out_n, size_t *w)
{
    unsigned long cp = 0;
    int i;

    if (end - p < 4) return end;
    for (i = 0; i < 4; i++) {
        char c = p[i];
        cp <<= 4;
        if      (c >= '0' && c <= '9') cp |= (unsigned long)(c - '0');
        else if (c >= 'a' && c <= 'f') cp |= (unsigned long)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') cp |= (unsigned long)(c - 'A' + 10);
        else return end;
    }
    p += 4;

    if (cp >= 0xD800u && cp <= 0xDBFFu) {
        if (end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
            unsigned long lo = 0;
            for (i = 0; i < 4; i++) {
                char c = p[2 + i];
                lo <<= 4;
                if      (c >= '0' && c <= '9') lo |= (unsigned long)(c - '0');
                else if (c >= 'a' && c <= 'f') lo |= (unsigned long)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') lo |= (unsigned long)(c - 'A' + 10);
                else { lo = 0; break; }
            }
            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                p += 6;
            } else {
                cp = 0xFFFDu;
            }
        } else {
            cp = 0xFFFDu;
        }
    } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
        cp = 0xFFFDu;
    }

    /* Encode, but only if the whole sequence fits -- a truncated multi-byte
     * character would be worse than dropping it. */
    if (cp < 0x80u) {
        if (*w + 1 < out_n) out[(*w)++] = (char)cp;
    } else if (cp < 0x800u) {
        if (*w + 2 < out_n) {
            out[(*w)++] = (char)(0xC0u | (cp >> 6));
            out[(*w)++] = (char)(0x80u | (cp & 0x3Fu));
        }
    } else if (cp < 0x10000u) {
        if (*w + 3 < out_n) {
            out[(*w)++] = (char)(0xE0u | (cp >> 12));
            out[(*w)++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            out[(*w)++] = (char)(0x80u | (cp & 0x3Fu));
        }
    } else {
        if (*w + 4 < out_n) {
            out[(*w)++] = (char)(0xF0u | (cp >> 18));
            out[(*w)++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
            out[(*w)++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            out[(*w)++] = (char)(0x80u | (cp & 0x3Fu));
        }
    }
    return p;
}

size_t mj_str(const mj_doc *d, int idx, char *out, size_t out_n)
{
    const char *p, *end;
    size_t w = 0;

    if (!out || out_n == 0) return 0;
    out[0] = '\0';
    if (!d || idx < 0 || idx >= d->count || d->n[idx].type != MJ_STR) return 0;

    p   = d->n[idx].raw;
    end = p + d->n[idx].raw_n;
    while (p < end && w + 1 < out_n) {
        if (*p != '\\') { out[w++] = *p++; continue; }
        p++;
        if (p >= end) break;
        switch (*p) {
        case 'n': out[w++] = '\n'; p++; break;
        case 't': out[w++] = '\t'; p++; break;
        case 'r': out[w++] = '\r'; p++; break;
        case 'b': out[w++] = '\b'; p++; break;
        case 'f': out[w++] = '\f'; p++; break;
        case 'u': p = utf16_escape(p + 1, end, out, out_n, &w); break;
        default:  out[w++] = *p++; break;    /* \" \\ \/ and anything else */
        }
    }
    out[w] = '\0';
    return w;
}

double mj_num(const mj_doc *d, int idx, double dflt)
{
    if (!d || idx < 0 || idx >= d->count) return dflt;
    if (d->n[idx].type == MJ_NUM)  return d->n[idx].num;
    if (d->n[idx].type == MJ_BOOL) return d->n[idx].bval ? 1.0 : 0.0;
    return dflt;
}

int mj_bool(const mj_doc *d, int idx, int dflt)
{
    if (!d || idx < 0 || idx >= d->count) return dflt;
    if (d->n[idx].type == MJ_BOOL) return d->n[idx].bval;
    if (d->n[idx].type == MJ_NUM)  return d->n[idx].num != 0.0;
    return dflt;
}
