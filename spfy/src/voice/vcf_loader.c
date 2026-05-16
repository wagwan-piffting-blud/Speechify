/* VCF loader: nibble-expansion cipher (2:1) + minimal XML scan.
 *
 * Cipher (confirmed via reveng/vcf_edit.py + DLL disassembly):
 *   Each plaintext byte -> 2 cipher bytes: high nibble then low nibble,
 *   each nibble mapped through a 16-entry substitution table.
 *
 *   nibble:  0    1    2    3    4    5    6    7
 *           0xDD 0xDC 0xDF 0xDE 0xD9 0xD8 0xDB 0xDA
 *   nibble:  8    9    A    B    C    D    E    F
 *           0xD5 0xD4 0xAC 0xAF 0xAE 0xA9 0xA8 0xAB
 *
 * Plaintext is ISO-8859-1 XML. Schema is flat:
 *   <SWIttsConfig version="1.0.0">
 *     <lang name="en-US">
 *       <param name="key.dotted.path">
 *         <value>val</value>
 *       </param>
 *       ...
 *     </lang>
 *   </SWIttsConfig>
 *
 * For M0a we use a hand-rolled scanner (no libexpat dep) that finds every
 * <param name="X"> ... <value>Y</value> ... </param> triple. This matches
 * the same behaviour as reveng/vcf_edit.py. M5+ may swap to libexpat for
 * stricter parsing if VCF schemas grow. */

#include "voice.h"

#include "../common/file_io.h"
#include "../common/log.h"
#include "../../include/spfy/spfy.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* nibble -> cipher byte (file order: high nibble first, then low nibble) */
static const uint8_t ENC_TABLE[16] = {
    0xDD, 0xDC, 0xDF, 0xDE, 0xD9, 0xD8, 0xDB, 0xDA,
    0xD5, 0xD4, 0xAC, 0xAF, 0xAE, 0xA9, 0xA8, 0xAB,
};

/* Inverse: cipher byte -> nibble (0..15), or 0xFF for invalid. */
static uint8_t s_dec_table[256];
static int     s_dec_table_init = 0;

static void init_dec_table(void)
{
    if (s_dec_table_init) return;
    for (int i = 0; i < 256; ++i) s_dec_table[i] = 0xFF;
    for (int i = 0; i < 16;  ++i) s_dec_table[ENC_TABLE[i]] = (uint8_t)i;
    s_dec_table_init = 1;
}

static int vcf_decrypt(const uint8_t *src, size_t n_src,
                       uint8_t **out, size_t *out_n)
{
    if (n_src % 2u != 0u) {
        spfy_log_err("vcf: odd file size %zu (cipher requires even)", n_src);
        return SPFY_E_FORMAT;
    }
    init_dec_table();

    size_t n = n_src / 2u;
    uint8_t *plain = (uint8_t *)malloc(n + 1);   /* +1 for NUL convenience */
    if (!plain) return SPFY_E_NOMEM;

    for (size_t i = 0; i < n; ++i) {
        uint8_t hi = s_dec_table[src[2*i  ]];
        uint8_t lo = s_dec_table[src[2*i+1]];
        if (hi > 15 || lo > 15) {
            free(plain);
            spfy_log_err("vcf: invalid cipher byte at offset %zu", 2*i);
            return SPFY_E_FORMAT;
        }
        plain[i] = (uint8_t)((hi << 4) | lo);
    }
    plain[n] = '\0';
    *out   = plain;
    *out_n = n;
    return SPFY_OK;
}

/* Substring search bounded to [haystack, haystack+n). */
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

/* Trim leading/trailing ASCII whitespace in-place; returns new length. */
static size_t trim_ascii(char *s)
{
    size_t n = strlen(s);
    size_t i = 0;
    while (i < n && (unsigned char)s[i] <= ' ') ++i;
    if (i > 0) memmove(s, s + i, n - i + 1), n -= i;
    while (n > 0 && (unsigned char)s[n-1] <= ' ') s[--n] = '\0';
    return n;
}

static int vcf_scan_params(const char *xml, size_t xml_n, spfy_vcf_t *out)
{
    /* Walks <param name="KEY"> ... <value>VAL</value> ... </param>.
     * Tolerant of attribute whitespace and ordering. */
    const char *p   = xml;
    const char *end = xml + xml_n;

    spfy_vcf_kv_t **tail = &out->params;

    while (p < end) {
        const char *tag = bounded_str(p, (size_t)(end - p), "<param");
        if (!tag) break;

        /* find "name=" attribute */
        const char *gt = bounded_str(tag, (size_t)(end - tag), ">");
        if (!gt) break;
        const char *name_attr = bounded_str(tag, (size_t)(gt - tag), "name=");
        if (!name_attr) { p = gt + 1; continue; }

        /* opening quote */
        const char *q1 = name_attr + 5;
        while (q1 < gt && *q1 != '"' && *q1 != '\'') ++q1;
        if (q1 >= gt) { p = gt + 1; continue; }
        char quote = *q1++;
        const char *q2 = q1;
        while (q2 < gt && *q2 != quote) ++q2;
        if (q2 >= gt) { p = gt + 1; continue; }
        char *key = xstrdup_n(q1, (size_t)(q2 - q1));
        if (!key) return SPFY_E_NOMEM;

        /* find <value> ... </value> within this <param>...</param> */
        const char *param_end = bounded_str(gt, (size_t)(end - gt),
                                            "</param>");
        if (!param_end) { free(key); break; }
        const char *vstart = bounded_str(gt, (size_t)(param_end - gt),
                                         "<value>");
        if (!vstart) { free(key); p = param_end + 8; continue; }
        vstart += 7;   /* past "<value>" */
        const char *vend = bounded_str(vstart, (size_t)(param_end - vstart),
                                       "</value>");
        if (!vend) { free(key); p = param_end + 8; continue; }

        char *val = xstrdup_n(vstart, (size_t)(vend - vstart));
        if (!val) { free(key); return SPFY_E_NOMEM; }
        trim_ascii(val);

        spfy_vcf_kv_t *kv = (spfy_vcf_kv_t *)malloc(sizeof *kv);
        if (!kv) { free(key); free(val); return SPFY_E_NOMEM; }
        kv->key = key;
        kv->value = val;
        kv->next = NULL;
        *tail = kv; tail = &kv->next;

        p = param_end + 8;   /* past "</param>" */
    }
    return SPFY_OK;
}

int spfy_vcf_load(const char *path, spfy_vcf_t *out)
{
    if (!path || !out) return SPFY_E_INVAL;
    memset(out, 0, sizeof *out);

    uint8_t *cipher = NULL;
    size_t   n_c    = 0;
    int rc = spfy_slurp_file(path, &cipher, &n_c);
    if (rc != SPFY_OK) return rc;

    uint8_t *plain = NULL;
    size_t   n_p   = 0;
    rc = vcf_decrypt(cipher, n_c, &plain, &n_p);
    free(cipher);
    if (rc != SPFY_OK) return rc;

    out->xml_bytes = plain;
    out->xml_n     = n_p;

    rc = vcf_scan_params((const char *)plain, n_p, out);
    if (rc != SPFY_OK) { spfy_vcf_free(out); return rc; }

    return SPFY_OK;
}
