/* Write the voice's .vcf and its sidecar XML. See vb_vcf.h for what this does
 * and does not claim about the numbers. */

#include "vb_vcf.h"

#include "vb_io.h"
#include "../common/log.h"
#include "../../include/spfy/spfy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The 2:1 nibble-expansion cipher, the same table the engine's loader uses
 * (voice/vcf_loader.c, confirmed against the DLL). Each plaintext byte becomes
 * two cipher bytes: high nibble then low. */
static const uint8_t ENC_TABLE[16] = {
    0xDD, 0xDC, 0xDF, 0xDE, 0xD9, 0xD8, 0xDB, 0xDA,
    0xD5, 0xD4, 0xAC, 0xAF, 0xAE, 0xA9, 0xA8, 0xAB,
};

static int write_file(const char *path, const uint8_t *d, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return SPFY_E_IO;
    size_t w = n ? fwrite(d, 1, n, f) : 0;
    fclose(f);
    return w == n ? SPFY_OK : SPFY_E_IO;
}

/* Replace the FIRST `<value> ... </value>` that follows
 * `name="tts.voiceCfg.<key>"`. Returns 1 if it fired.
 *
 * ⚠ Deliberately not a general XML edit. The file is a flat list of
 * param/value pairs written by one tool, and a real parser here would be more
 * code than the format has structure. */
static int set_param(spfy_vb_buf *b, const char *key, const char *value)
{
    char needle[256];
    int nn = snprintf(needle, sizeof needle,
                      "name=\"tts.voiceCfg.%s\"", key);
    if (nn <= 0 || (size_t)nn >= sizeof needle) return 0;

    char *hay = (char *)b->p;
    char *at = NULL;
    for (size_t i = 0; i + (size_t)nn <= b->n; ++i) {
        if (!memcmp(hay + i, needle, (size_t)nn)) { at = hay + i; break; }
    }
    if (!at) return 0;
    size_t off = (size_t)(at - hay);
    /* `<value>` then `</value>`, both after the key and before the next
     * `<param`, so a missing close cannot run into the following entry. */
    const char *vo = NULL, *vc = NULL, *stop = NULL;
    for (size_t i = off; i + 6 <= b->n; ++i) {
        if (!memcmp(hay + i, "<param", 6)) { stop = hay + i; break; }
    }
    for (size_t i = off; i + 7 <= b->n; ++i) {
        if (stop && hay + i >= stop) break;
        if (!vo && !memcmp(hay + i, "<value>", 7)) { vo = hay + i + 7; continue; }
        if (vo && i + 8 <= b->n && !memcmp(hay + i, "</value>", 8)) {
            vc = hay + i; break;
        }
    }
    if (!vo || !vc) return 0;

    size_t head = (size_t)(vo - hay);
    size_t tail = (size_t)(vc - hay);
    size_t vlen = strlen(value);
    spfy_vb_buf nb = {0};
    if (spfy_vb_buf_put(&nb, hay, head) != SPFY_OK) goto fail;
    if (spfy_vb_buf_put(&nb, " ", 1) != SPFY_OK) goto fail;
    if (spfy_vb_buf_put(&nb, value, vlen) != SPFY_OK) goto fail;
    if (spfy_vb_buf_put(&nb, " ", 1) != SPFY_OK) goto fail;
    if (spfy_vb_buf_put(&nb, hay + tail, b->n - tail) != SPFY_OK) goto fail;
    spfy_vb_buf_free(b);
    *b = nb;
    return 1;
fail:
    spfy_vb_buf_free(&nb);
    return 0;
}

int spfy_vb_vcf_write(const char *dir, const char *voice,
                      const spfy_vb_vcf_set *sets, size_t n_sets,
                      size_t *n_applied)
{
    if (!dir || !voice) return SPFY_E_INVAL;
    const char *xml = spfy_vb_vcf_plaintext();
    spfy_vb_buf b = {0};
    int rc = spfy_vb_buf_put(&b, xml, strlen(xml));
    if (rc != SPFY_OK) return rc;

    size_t applied = 0;
    for (size_t i = 0; i < n_sets; ++i) {
        if (set_param(&b, sets[i].key, sets[i].value)) {
            ++applied;
            continue;
        }
        spfy_log_err("vcf: no param named tts.voiceCfg.%s -- refusing, "
                     "because a typo that changed nothing would look exactly "
                     "like a weight that is inert", sets[i].key);
        spfy_vb_buf_free(&b);
        return SPFY_E_FORMAT;
    }
    if (n_applied) *n_applied = applied;

    size_t need = b.n * 2u;
    uint8_t *enc = (uint8_t *)malloc(need ? need : 1u);
    if (!enc) { spfy_vb_buf_free(&b); return SPFY_E_NOMEM; }
    for (size_t i = 0; i < b.n; ++i) {
        enc[i * 2u]      = ENC_TABLE[(b.p[i] >> 4) & 0xFu];
        enc[i * 2u + 1u] = ENC_TABLE[b.p[i] & 0xFu];
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.vcf", dir, voice);
    rc = write_file(path, enc, b.n * 2u);
    free(enc);
    spfy_vb_buf_free(&b);
    return rc;
}

/* Replace every occurrence of `tok` with `rep`. */
static int subst_all(spfy_vb_buf *b, const char *tok, const char *rep)
{
    size_t tn = strlen(tok), rn = strlen(rep);
    for (;;) {
        char *hay = (char *)b->p;
        size_t at = (size_t)-1;
        for (size_t i = 0; i + tn <= b->n; ++i)
            if (!memcmp(hay + i, tok, tn)) { at = i; break; }
        if (at == (size_t)-1) return SPFY_OK;
        spfy_vb_buf nb = {0};
        int rc = spfy_vb_buf_put(&nb, hay, at);
        if (rc == SPFY_OK) rc = spfy_vb_buf_put(&nb, rep, rn);
        if (rc == SPFY_OK)
            rc = spfy_vb_buf_put(&nb, hay + at + tn, b->n - at - tn);
        if (rc != SPFY_OK) { spfy_vb_buf_free(&nb); return rc; }
        spfy_vb_buf_free(b);
        *b = nb;
    }
}

int spfy_vb_sidecar_write(const char *dir, const char *voice,
                          int format, const char *language, int port)
{
    if (!dir || !voice) return SPFY_E_INVAL;
    const char *t = spfy_vb_sidecar_template();
    spfy_vb_buf b = {0};
    int rc = spfy_vb_buf_put(&b, t, strlen(t));
    if (rc != SPFY_OK) return rc;

    char fbuf[16], pbuf[16];
    snprintf(fbuf, sizeof fbuf, "%d", format);
    snprintf(pbuf, sizeof pbuf, "%d", port);
    if (rc == SPFY_OK) rc = subst_all(&b, "%%VOICE%%", voice);
    if (rc == SPFY_OK) rc = subst_all(&b, "%%FORMAT%%", fbuf);
    if (rc == SPFY_OK) rc = subst_all(&b, "%%LANG%%",
                                      language ? language : "en-US");
    if (rc == SPFY_OK) rc = subst_all(&b, "%%PORT%%", pbuf);
    if (rc != SPFY_OK) { spfy_vb_buf_free(&b); return rc; }

    char path[1024];
    snprintf(path, sizeof path, "%s/%s%d.xml", dir, voice, format);
    rc = write_file(path, b.p, b.n);
    spfy_vb_buf_free(&b);
    return rc;
}
