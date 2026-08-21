#include "vb_io.h"

#include "../common/log.h"
#include "../common/obfuscation.h"
#include "../../include/spfy/spfy.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

/* ====================================================================== */
/* Buffer                                                                  */

int spfy_vb_buf_reserve(spfy_vb_buf *b, size_t need)
{
    if (b->n + need <= b->cap) return SPFY_OK;
    size_t cap = b->cap ? b->cap : 1024u;
    while (cap < b->n + need) {
        if (cap > (size_t)-1 / 2u) return SPFY_E_NOMEM;
        cap *= 2u;
    }
    uint8_t *p = (uint8_t *)realloc(b->p, cap);
    if (!p) return SPFY_E_NOMEM;
    b->p = p;
    b->cap = cap;
    return SPFY_OK;
}

int spfy_vb_buf_put(spfy_vb_buf *b, const void *src, size_t n)
{
    if (!n) return SPFY_OK;
    int rc = spfy_vb_buf_reserve(b, n);
    if (rc != SPFY_OK) return rc;
    memcpy(b->p + b->n, src, n);
    b->n += n;
    return SPFY_OK;
}

int spfy_vb_buf_u8(spfy_vb_buf *b, uint8_t v)
{
    return spfy_vb_buf_put(b, &v, 1);
}

int spfy_vb_buf_u16(spfy_vb_buf *b, uint16_t v)
{
    uint8_t t[2] = { (uint8_t)(v & 0xFFu), (uint8_t)(v >> 8) };
    return spfy_vb_buf_put(b, t, 2);
}

int spfy_vb_buf_u32(spfy_vb_buf *b, uint32_t v)
{
    uint8_t t[4] = { (uint8_t)(v & 0xFFu), (uint8_t)((v >> 8) & 0xFFu),
                     (uint8_t)((v >> 16) & 0xFFu), (uint8_t)((v >> 24) & 0xFFu) };
    return spfy_vb_buf_put(b, t, 4);
}

int spfy_vb_buf_f32(spfy_vb_buf *b, float v)
{
    union { float f; uint32_t u; } cv;
    cv.f = v;
    return spfy_vb_buf_u32(b, cv.u);
}

int spfy_vb_buf_pstr(spfy_vb_buf *b, const char *s)
{
    size_t n = strlen(s);
    if (n > 0xFFFFu) n = 0xFFFFu;
    int rc = spfy_vb_buf_u16(b, (uint16_t)n);
    if (rc != SPFY_OK) return rc;
    return spfy_vb_buf_put(b, s, n);
}

void spfy_vb_buf_free(spfy_vb_buf *b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

/* ====================================================================== */
/* Files                                                                   */

int spfy_vb_read_bytes(const char *path, uint8_t **out, size_t *out_n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return SPFY_E_IO;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return SPFY_E_IO; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return SPFY_E_IO; }
    rewind(f);
    uint8_t *b = (uint8_t *)malloc((size_t)sz + 1u);
    if (!b) { fclose(f); return SPFY_E_NOMEM; }
    if (sz && fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b); fclose(f); return SPFY_E_IO;
    }
    fclose(f);
    b[sz] = 0;
    *out = b;
    *out_n = (size_t)sz;
    return SPFY_OK;
}

int spfy_vb_read_text(const char *path, char **out, size_t *out_n)
{
    uint8_t *b = NULL;
    size_t n = 0;
    int rc = spfy_vb_read_bytes(path, &b, &n);
    if (rc != SPFY_OK) return rc;
    *out = (char *)b;
    *out_n = n;
    return SPFY_OK;
}

int spfy_vb_file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int spfy_vb_list_stems(const char *dir, char ***out, size_t *out_n)
{
    char **v = NULL;
    size_t n = 0, cap = 0;

#ifdef _WIN32
    char pat[1024];
    snprintf(pat, sizeof pat, "%s\\*.wav", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { *out = NULL; *out_n = 0; return SPFY_OK; }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        size_t ln = strlen(fd.cFileName);
        if (ln < 5) continue;
        if (n == cap) {
            size_t nc = cap ? cap * 2u : 256u;
            char **nv = (char **)realloc(v, nc * sizeof *nv);
            if (!nv) { FindClose(h); goto oom; }
            v = nv; cap = nc;
        }
        char *s = (char *)malloc(ln - 3);
        if (!s) { FindClose(h); goto oom; }
        memcpy(s, fd.cFileName, ln - 4);
        s[ln - 4] = 0;
        v[n++] = s;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) { *out = NULL; *out_n = 0; return SPFY_OK; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t ln = strlen(e->d_name);
        if (ln < 5 || strcmp(e->d_name + ln - 4, ".wav") != 0) continue;
        if (n == cap) {
            size_t nc = cap ? cap * 2u : 256u;
            char **nv = (char **)realloc(v, nc * sizeof *nv);
            if (!nv) { closedir(d); goto oom; }
            v = nv; cap = nc;
        }
        char *s = (char *)malloc(ln - 3);
        if (!s) { closedir(d); goto oom; }
        memcpy(s, e->d_name, ln - 4);
        s[ln - 4] = 0;
        v[n++] = s;
    }
    closedir(d);
#endif

    if (n > 1) qsort(v, n, sizeof *v, cmp_str);
    *out = v;
    *out_n = n;
    return SPFY_OK;

oom:
    spfy_vb_free_stems(v, n);
    return SPFY_E_NOMEM;
}

void spfy_vb_free_stems(char **v, size_t n)
{
    for (size_t i = 0; i < n; ++i) free(v[i]);
    free(v);
}

/* ====================================================================== */
/* RIFF                                                                    */

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static int riff_push(spfy_vb_riff *r, const spfy_vb_chunk *c)
{
    if (r->n_ch == r->cap_ch) {
        size_t nc = r->cap_ch ? r->cap_ch * 2u : 32u;
        spfy_vb_chunk *nv = (spfy_vb_chunk *)realloc(r->ch, nc * sizeof *nv);
        if (!nv) return SPFY_E_NOMEM;
        r->ch = nv;
        r->cap_ch = nc;
    }
    r->ch[r->n_ch++] = *c;
    return SPFY_OK;
}

int spfy_vb_riff_load(const char *path, spfy_vb_riff *r)
{
    memset(r, 0, sizeof *r);
    uint8_t *raw = NULL;
    size_t n = 0;
    int rc = spfy_vb_read_bytes(path, &raw, &n);
    if (rc != SPFY_OK) return rc;
    for (size_t i = 0; i < n; ++i) raw[i] ^= SPFY_OBFUSCATION_BYTE;
    if (n < 12 || memcmp(raw, "RIFF", 4) != 0) {
        free(raw);
        spfy_log_err("vb_io: %s is not RIFF after decode", path);
        return SPFY_E_FORMAT;
    }
    r->raw = raw;
    r->raw_n = n;
    r->declared = rd_u32(raw + 4);
    memcpy(r->form, raw + 8, 4);
    r->form[4] = 0;

    /* ⚠ Parse against the FILE length, not the declared size: tom.vin's
     * declared value is 8 short of its real length and trusting it drops the
     * last chunk. The declared value is still carried, for writing. */
    size_t p = 12;
    while (p + 8 <= n) {
        spfy_vb_chunk c;
        memset(&c, 0, sizeof c);
        memcpy(c.id, raw + p, 4);
        c.id[4] = 0;
        uint32_t sz = rd_u32(raw + p + 4);
        if (p + 8 + (size_t)sz > n) break;
        c.data = raw + p + 8;
        c.n = sz;
        if (sz & 1u) {
            if (p + 8 + sz < n) { c.pad = raw[p + 8 + sz]; c.has_pad = 1; }
        }
        rc = riff_push(r, &c);
        if (rc != SPFY_OK) { spfy_vb_riff_free(r); return rc; }
        p += 8u + sz + (sz & 1u);
    }
    return SPFY_OK;
}

int spfy_vb_riff_new(spfy_vb_riff *r, const char *form)
{
    if (!r || !form) return SPFY_E_INVAL;
    memset(r, 0, sizeof *r);
    memcpy(r->form, form, 4);
    r->form[4] = 0;
    /* Recomputed by _save; a fresh container has nothing to preserve. */
    r->declared = 4;
    return SPFY_OK;
}

int spfy_vb_riff_put(spfy_vb_riff *r, const char *id, uint8_t *data, size_t n)
{
    if (!r || !id) return SPFY_E_INVAL;
    if (spfy_vb_riff_get(r, id)) return SPFY_E_FORMAT;
    spfy_vb_chunk c;
    memset(&c, 0, sizeof c);
    memcpy(c.id, id, 4);
    c.id[4] = 0;
    c.data = data;
    c.n = n;
    c.owned = data ? 1 : 0;
    return riff_push(r, &c);
}

const spfy_vb_chunk *spfy_vb_riff_get(const spfy_vb_riff *r, const char *id)
{
    for (size_t i = 0; i < r->n_ch; ++i)
        if (memcmp(r->ch[i].id, id, 4) == 0) return &r->ch[i];
    return NULL;
}

int spfy_vb_riff_set(spfy_vb_riff *r, const char *id, uint8_t *data, size_t n)
{
    for (size_t i = 0; i < r->n_ch; ++i) {
        if (memcmp(r->ch[i].id, id, 4) != 0) continue;
        if (r->ch[i].owned) free(r->ch[i].data);
        r->ch[i].data  = data;
        r->ch[i].n     = n;
        r->ch[i].owned = 1;
        return SPFY_OK;
    }
    return SPFY_E_FORMAT;
}

static size_t riff_total(const spfy_vb_riff *r)
{
    size_t t = 12;
    for (size_t i = 0; i < r->n_ch; ++i)
        t += 8u + r->ch[i].n + (r->ch[i].n & 1u);
    return t;
}

int spfy_vb_riff_save(spfy_vb_riff *r, const char *path)
{
    /* Python does this in two passes -- serialise, set declared to
     * len - 8, serialise again -- so the declared value follows the NEW
     * length rather than the template's. Same arithmetic here. */
    size_t total = riff_total(r);
    r->declared = (uint32_t)(total - 8u);

    FILE *f = fopen(path, "wb");
    if (!f) return SPFY_E_IO;

    uint8_t hdr[12];
    memcpy(hdr, "RIFF", 4);
    wr_u32(hdr + 4, r->declared);
    memcpy(hdr + 8, r->form, 4);
    for (int i = 0; i < 12; ++i) hdr[i] ^= SPFY_OBFUSCATION_BYTE;
    if (fwrite(hdr, 1, 12, f) != 12) { fclose(f); return SPFY_E_IO; }

    /* Encode in blocks so a 140 MB data chunk is not duplicated in memory. */
    enum { BLK = 1 << 16 };
    uint8_t *tmp = (uint8_t *)malloc(BLK);
    if (!tmp) { fclose(f); return SPFY_E_NOMEM; }

    for (size_t i = 0; i < r->n_ch; ++i) {
        uint8_t h[8];
        memcpy(h, r->ch[i].id, 4);
        wr_u32(h + 4, (uint32_t)r->ch[i].n);
        for (int k = 0; k < 8; ++k) h[k] ^= SPFY_OBFUSCATION_BYTE;
        if (fwrite(h, 1, 8, f) != 8) goto io_err;

        size_t left = r->ch[i].n, off = 0;
        while (left) {
            size_t take = left < BLK ? left : (size_t)BLK;
            for (size_t k = 0; k < take; ++k)
                tmp[k] = r->ch[i].data[off + k] ^ SPFY_OBFUSCATION_BYTE;
            if (fwrite(tmp, 1, take, f) != take) goto io_err;
            off += take;
            left -= take;
        }
        if (r->ch[i].n & 1u) {
            /* join_chunks writes the carried pad byte, or 0x00 when the
             * template chunk was even and ours is odd. */
            uint8_t pb = (uint8_t)((r->ch[i].has_pad ? r->ch[i].pad : 0x00u)
                                   ^ SPFY_OBFUSCATION_BYTE);
            if (fwrite(&pb, 1, 1, f) != 1) goto io_err;
        }
    }
    free(tmp);
    if (fclose(f) != 0) return SPFY_E_IO;
    return SPFY_OK;

io_err:
    free(tmp);
    fclose(f);
    return SPFY_E_IO;
}

void spfy_vb_riff_free(spfy_vb_riff *r)
{
    if (!r) return;
    for (size_t i = 0; i < r->n_ch; ++i)
        if (r->ch[i].owned) free(r->ch[i].data);
    free(r->ch);
    free(r->raw);
    memset(r, 0, sizeof *r);
}

int spfy_vb_subchunk(const uint8_t *body, size_t n, size_t *pos,
                     char id_out[5], const uint8_t **data, size_t *data_n)
{
    size_t p = *pos;
    if (p + 8 > n) return 0;
    memcpy(id_out, body + p, 4);
    id_out[4] = 0;
    uint32_t sz = rd_u32(body + p + 4);
    if (p + 8 + (size_t)sz > n) return 0;
    *data = body + p + 8;
    *data_n = sz;
    *pos = p + 8u + sz + (sz & 1u);
    return 1;
}

/* ====================================================================== */
/* WAV                                                                     */

int spfy_vb_wav_read(const char *path, spfy_vb_wav *out)
{
    memset(out, 0, sizeof *out);
    uint8_t *raw = NULL;
    size_t n = 0;
    int rc = spfy_vb_read_bytes(path, &raw, &n);
    if (rc != SPFY_OK) return rc;
    if (n < 12 || memcmp(raw, "RIFF", 4) || memcmp(raw + 8, "WAVE", 4)) {
        free(raw);
        return SPFY_E_FORMAT;
    }
    const uint8_t *fmt = NULL, *data = NULL;
    size_t fmt_n = 0, data_n = 0;
    size_t p = 12;
    while (p + 8 <= n) {
        uint32_t sz = rd_u32(raw + p + 4);
        if (p + 8 + (size_t)sz > n) break;
        if (!memcmp(raw + p, "fmt ", 4)) { fmt = raw + p + 8; fmt_n = sz; }
        else if (!memcmp(raw + p, "data", 4)) { data = raw + p + 8; data_n = sz; }
        p += 8u + sz + (sz & 1u);
    }
    if (!fmt || fmt_n < 16 || !data) { free(raw); return SPFY_E_FORMAT; }

    out->channels = (int)((uint32_t)fmt[2] | ((uint32_t)fmt[3] << 8));
    out->rate     = (int)rd_u32(fmt + 4);
    out->width    = (int)((uint32_t)fmt[14] | ((uint32_t)fmt[15] << 8)) / 8;
    if (out->channels != 1 || out->width != 2) { free(raw); return SPFY_E_FORMAT; }

    out->n_samples = data_n / 2u;
    size_t pcm_bytes = out->n_samples * sizeof *out->pcm;
    out->pcm = (int16_t *)malloc(pcm_bytes ? pcm_bytes : 1u);
    if (!out->pcm) { free(raw); return SPFY_E_NOMEM; }
    for (size_t i = 0; i < out->n_samples; ++i)
        out->pcm[i] = (int16_t)((uint16_t)data[i * 2] |
                                ((uint16_t)data[i * 2 + 1] << 8));
    free(raw);
    return SPFY_OK;
}

void spfy_vb_wav_free(spfy_vb_wav *w)
{
    free(w->pcm);
    memset(w, 0, sizeof *w);
}

/* CPython Modules/audioop.c st_14linear2ulaw, reached through
 * lin2ulaw(sample >> 2). Transcribed rather than reinvented: the shift, the
 * CLIP-after-shift and the BIAS>>2 are all load-bearing for byte equality. */
#define ULAW_CLIP 8159
#define ULAW_BIAS 0x84

uint8_t spfy_vb_ulaw_encode(int16_t pcm)
{
    static const int16_t seg_uend[8] = {
        0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF
    };
    int16_t v = (int16_t)(pcm >> 2);       /* arithmetic, as GETRAWSAMPLE>>2 */
    int16_t mask;
    if (v < 0) { v = (int16_t)-v; mask = 0x7F; }
    else       { mask = (int16_t)0xFF; }
    if (v > ULAW_CLIP) v = ULAW_CLIP;
    v = (int16_t)(v + (ULAW_BIAS >> 2));

    int seg = 8;
    for (int i = 0; i < 8; ++i) {
        if (v <= seg_uend[i]) { seg = i; break; }
    }
    if (seg >= 8) return (uint8_t)(0x7F ^ mask);
    uint8_t uval = (uint8_t)((seg << 4) | ((v >> (seg + 1)) & 0xF));
    return (uint8_t)(uval ^ mask);
}

void spfy_vb_ulaw_encode_block(const int16_t *pcm, size_t n, uint8_t *out)
{
    for (size_t i = 0; i < n; ++i) out[i] = spfy_vb_ulaw_encode(pcm[i]);
}

/* ====================================================================== */
/* TextGrid                                                                */

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (unsigned char)*p <= ' ') ++p;
    return p;
}

/* Mirrors the Python regex `xmin = ([\d.]+)`: digits and dots only, no sign
 * and no exponent. Anything else fails the interval and the scan moves on. */
static const char *num_at(const char *p, const char *end, double *out)
{
    const char *s = p;
    while (p < end && ((*p >= '0' && *p <= '9') || *p == '.')) ++p;
    if (p == s) return NULL;
    char tmp[64];
    size_t ln = (size_t)(p - s);
    if (ln >= sizeof tmp) return NULL;
    memcpy(tmp, s, ln);
    tmp[ln] = 0;
    *out = atof(tmp);
    return p;
}

static const char *lit_at(const char *p, const char *end, const char *lit)
{
    size_t ln = strlen(lit);
    if ((size_t)(end - p) < ln) return NULL;
    if (memcmp(p, lit, ln) != 0) return NULL;
    return p + ln;
}

int spfy_vb_textgrid_phones(const char *path, spfy_vb_interval **out,
                            size_t *out_n)
{
    *out = NULL;
    *out_n = 0;
    char *txt = NULL;
    size_t n = 0;
    int rc = spfy_vb_read_text(path, &txt, &n);
    if (rc != SPFY_OK) return rc;

    const char *cut = strstr(txt, "name = \"phones\"");
    if (!cut) { free(txt); return SPFY_OK; }

    const char *end = txt + n;
    spfy_vb_interval *v = NULL;
    size_t cnt = 0, cap = 0;

    const char *p = cut;
    while ((p = strstr(p, "intervals [")) != NULL) {
        const char *q = p + 11;
        const char *save = p + 1;
        while (q < end && *q >= '0' && *q <= '9') ++q;
        if (!(q = lit_at(q, end, "]:")))            { p = save; continue; }
        q = skip_ws(q, end);
        if (!(q = lit_at(q, end, "xmin = ")))       { p = save; continue; }
        double xmin, xmax;
        if (!(q = num_at(q, end, &xmin)))           { p = save; continue; }
        q = skip_ws(q, end);
        if (!(q = lit_at(q, end, "xmax = ")))       { p = save; continue; }
        if (!(q = num_at(q, end, &xmax)))           { p = save; continue; }
        q = skip_ws(q, end);
        if (!(q = lit_at(q, end, "text = \"")))     { p = save; continue; }
        const char *close = memchr(q, '"', (size_t)(end - q));
        if (!close)                                 { p = save; continue; }

        if (cnt == cap) {
            size_t nc = cap ? cap * 2u : 256u;
            spfy_vb_interval *nv =
                (spfy_vb_interval *)realloc(v, nc * sizeof *nv);
            if (!nv) { free(v); free(txt); return SPFY_E_NOMEM; }
            v = nv; cap = nc;
        }
        v[cnt].start = xmin;
        v[cnt].end   = xmax;
        size_t ln = (size_t)(close - q);
        if (ln >= sizeof v[cnt].label) ln = sizeof v[cnt].label - 1u;
        memcpy(v[cnt].label, q, ln);
        v[cnt].label[ln] = 0;
        ++cnt;
        p = close + 1;
    }
    free(txt);
    *out = v;
    *out_n = cnt;
    return SPFY_OK;
}

/* ====================================================================== */
/* FE tagged output                                                        */

/* `spfy_dump_voice --phones` order, and the authority on what is a phone.
 * Anything outside it is not a unit. */
static const char *const FE_PHONES[] = {
    "aa","ae","ah","ao","aw","ax","ay","b","ch","d","dh","dx","eh","el","en",
    "er","ey","f","g","hh","ih","ix","iy","jh","k","l","m","n","ng","ow","oy",
    "p","pau","r","s","sh","t","th","uh","uw","v","w","xx","y","z","zh"
};
enum { FE_N_PHONES = (int)(sizeof FE_PHONES / sizeof FE_PHONES[0]) };

static int fe_is_phone(const char *s, size_t n)
{
    for (int i = 0; i < FE_N_PHONES; ++i) {
        if (strlen(FE_PHONES[i]) == n && !memcmp(FE_PHONES[i], s, n)) return 1;
    }
    return 0;
}

static int fe_is_non_speech(const char *s, size_t n)
{
    return (n == 3 && !memcmp(s, "pau", 3)) || (n == 2 && !memcmp(s, "xx", 2));
}

/* `^([a-z]+)\(p[^)]*\)$` over a whitespace-delimited token. */
static int fe_symbol(const char *tok, size_t n, const char **name, size_t *nn)
{
    size_t i = 0;
    while (i < n && tok[i] >= 'a' && tok[i] <= 'z') ++i;
    if (i == 0) return 0;
    if (i + 2 > n || tok[i] != '(' || tok[i + 1] != 'p') return 0;
    size_t j = i + 2;
    while (j < n && tok[j] != ')') ++j;
    if (j >= n || j != n - 1) return 0;     /* the ')' must END the token */
    *name = tok;
    *nn = i;
    return 1;
}

int spfy_vb_fe_phones(const char *text, spfy_vb_phones *out)
{
    memset(out, 0, sizeof *out);
    size_t cap = 0;
    const char *p = text;
    while (*p) {
        while (*p && (unsigned char)*p <= ' ') ++p;
        if (!*p) break;
        const char *tok = p;
        while (*p && (unsigned char)*p > ' ') ++p;
        size_t tn = (size_t)(p - tok);

        const char *nm;
        size_t nn;
        if (!fe_symbol(tok, tn, &nm, &nn)) continue;
        if (!fe_is_phone(nm, nn)) continue;
        /* keep_pau=True keeps `pau`; `xx` only ever produced a BREAK, and
         * vb_build1 filters every BREAK out, so it contributes nothing. */
        if (fe_is_non_speech(nm, nn) && !(nn == 3 && !memcmp(nm, "pau", 3)))
            continue;

        if (out->n == cap) {
            size_t nc = cap ? cap * 2u : 512u;
            char (*nv)[8] = (char (*)[8])realloc(out->ph, nc * sizeof *nv);
            if (!nv) { spfy_vb_phones_free(out); return SPFY_E_NOMEM; }
            out->ph = nv; cap = nc;
        }
        size_t k = nn < 7 ? nn : 7;
        memcpy(out->ph[out->n], nm, k);
        out->ph[out->n][k] = 0;
        ++out->n;
    }
    return SPFY_OK;
}

void spfy_vb_phones_free(spfy_vb_phones *p)
{
    free(p->ph);
    p->ph = NULL;
    p->n = 0;
}

static int spans_push(spfy_vb_spans *s, const char *text, size_t tn,
                      uint32_t first, uint32_t last)
{
    if (s->n == s->cap) {
        size_t nc = s->cap ? s->cap * 2u : 128u;
        spfy_vb_span *nv = (spfy_vb_span *)realloc(s->v, nc * sizeof *nv);
        if (!nv) return SPFY_E_NOMEM;
        s->v = nv; s->cap = nc;
    }
    char *t = (char *)malloc(tn + 1u);
    if (!t) return SPFY_E_NOMEM;
    memcpy(t, text, tn);
    t[tn] = 0;
    s->v[s->n].text  = t;
    s->v[s->n].first = first;
    s->v[s->n].last  = last;
    ++s->n;
    return SPFY_OK;
}

void spfy_vb_spans_free(spfy_vb_spans *s)
{
    for (size_t i = 0; i < s->n; ++i) free(s->v[i].text);
    free(s->v);
    s->v = NULL;
    s->n = s->cap = 0;
}

/* `\(\s*(?:(?:\?d|\d+)\s*,\s*\d+)?\s*\)` -- the offset pair after a word. */
static const char *offpair(const char *p, const char *end)
{
    if (p >= end || *p != '(') return NULL;
    ++p;
    p = skip_ws(p, end);
    if (p < end && *p != ')') {
        if (p + 1 < end && p[0] == '?' && p[1] == 'd') p += 2;
        else {
            const char *s = p;
            while (p < end && *p >= '0' && *p <= '9') ++p;
            if (p == s) return NULL;
        }
        p = skip_ws(p, end);
        if (p >= end || *p != ',') return NULL;
        ++p;
        p = skip_ws(p, end);
        const char *s = p;
        while (p < end && *p >= '0' && *p <= '9') ++p;
        if (p == s) return NULL;
        p = skip_ws(p, end);
    }
    if (p >= end || *p != ')') return NULL;
    return p + 1;
}

int spfy_vb_fe_spans(const char *text, spfy_vb_spans *words,
                     spfy_vb_spans *syls)
{
    memset(words, 0, sizeof *words);
    memset(syls, 0, sizeof *syls);

    const char *end = text + strlen(text);
    uint32_t idx = 0;
    const char *w_text = NULL;
    size_t w_tn = 0;
    int have_w = 0, have_w_start = 0;
    uint32_t w_start = 0;

    int s_open = 0;
    uint32_t s_start = 0;
    /* The syllable key is its phones joined with '_'. */
    char s_key[512];
    size_t s_key_n = 0;

    int rc = SPFY_OK;
    const char *p = text;

#define CLOSE_SYL(endi)                                                       \
    do {                                                                      \
        if (s_open && s_key_n) {                                              \
            rc = spans_push(syls, s_key, s_key_n, s_start, (endi));           \
            if (rc != SPFY_OK) goto fail;                                     \
        }                                                                     \
    } while (0)

    while (p < end) {
        /* Alternation, in the regex's own order: word-start, word-end,
         * syllable head, phone. finditer takes the leftmost match, so the
         * scan tries each alternative at the current position and otherwise
         * advances one byte. */
        if (*p == '<') {
            const char *q = p + 1;
            const char *ws = q;
            while (q < end && *q != '(' && *q != '<' && *q != '>' &&
                   (unsigned char)*q > ' ') ++q;
            if (q > ws) {
                const char *after = skip_ws(q, end);
                const char *op = offpair(after, end);
                if (op) {
                    w_text = ws;
                    w_tn = (size_t)(q - ws);
                    have_w = 1;
                    have_w_start = 0;
                    p = op;
                    continue;
                }
            }
            ++p;
            continue;
        }
        if (*p == ']') {
            const char *q = skip_ws(p + 1, end);
            if (q < end && *q == '>') {
                CLOSE_SYL(idx - 1u);
                if (have_w && have_w_start) {
                    rc = spans_push(words, w_text, w_tn, w_start, idx - 1u);
                    if (rc != SPFY_OK) goto fail;
                }
                have_w = have_w_start = 0;
                s_open = 0;
                s_key_n = 0;
                p = q + 1;
                continue;
            }
            ++p;
            continue;
        }
        if (*p == '.' && p + 1 < end && p[1] >= '0' && p[1] <= '9') {
            CLOSE_SYL(idx - 1u);
            s_open = 1;
            s_start = idx;
            s_key_n = 0;
            p += 2;
            continue;
        }
        if (*p >= 'a' && *p <= 'z') {
            const char *q = p;
            while (q < end && *q >= 'a' && *q <= 'z') ++q;
            size_t nn = (size_t)(q - p);
            if (q + 1 < end && q[0] == '(' && q[1] == 'p') {
                const char *r = q + 2;
                while (r < end && *r != ')') ++r;
                if (r < end) {
                    if (fe_is_phone(p, nn)) {
                        if (fe_is_non_speech(p, nn)) {
                            ++idx;      /* pau occupies a unit-table slot */
                        } else {
                            if (have_w && !have_w_start) {
                                w_start = idx;
                                have_w_start = 1;
                            }
                            if (s_open) {
                                if (s_key_n) s_key[s_key_n++] = '_';
                                if (s_key_n + nn < sizeof s_key) {
                                    memcpy(s_key + s_key_n, p, nn);
                                    s_key_n += nn;
                                } else {
                                    s_key_n = sizeof s_key - 1u;
                                }
                            }
                            ++idx;
                        }
                    }
                    p = r + 1;
                    continue;
                }
            }
            p = q;                       /* letters that are not a phone tag */
            continue;
        }
        ++p;
    }
#undef CLOSE_SYL
    return SPFY_OK;

fail:
    spfy_vb_spans_free(words);
    spfy_vb_spans_free(syls);
    return rc;
}

/* ====================================================================== */
/* .sp  --  [{"ctx":[..],"sp":[..]}, ...]                                  */

static const char *json_ints(const char *p, const char *end,
                             int32_t *out, uint8_t *n_out, int max)
{
    while (p < end && *p != '[') ++p;
    if (p >= end) return NULL;
    ++p;
    int n = 0;
    while (p < end && *p != ']') {
        while (p < end && (*p == ' ' || *p == ',')) ++p;
        if (p < end && *p == ']') break;
        int neg = 0;
        if (p < end && *p == '-') { neg = 1; ++p; }
        if (p >= end || *p < '0' || *p > '9') break;
        long v = 0;
        while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); ++p; }
        if (n < max) out[n++] = (int32_t)(neg ? -v : v);
        while (p < end && *p == ' ') ++p;
    }
    if (p < end && *p == ']') ++p;
    *n_out = (uint8_t)n;
    return p;
}

int spfy_vb_sp_read(const char *path, spfy_vb_slot **out, size_t *out_n)
{
    *out = NULL;
    *out_n = 0;
    char *txt = NULL;
    size_t n = 0;
    if (spfy_vb_read_text(path, &txt, &n) != SPFY_OK) return SPFY_E_IO;

    const char *p = txt, *end = txt + n;
    spfy_vb_slot *v = NULL;
    size_t cnt = 0, cap = 0;

    while (p < end) {
        while (p < end && *p != '{') ++p;
        if (p >= end) break;
        const char *obj_end = p;
        while (obj_end < end && *obj_end != '}') ++obj_end;

        spfy_vb_slot s;
        memset(&s, 0, sizeof s);
        const char *c = NULL, *sp = NULL;
        for (const char *q = p; q < obj_end - 5; ++q) {
            if (!c  && !memcmp(q, "\"ctx\"", 5)) c = q + 5;
            if (!sp && !memcmp(q, "\"sp\"", 4))  sp = q + 4;
        }
        if (c)  json_ints(c,  obj_end, s.ctx, &s.n_ctx, 8);
        if (sp) json_ints(sp, obj_end, s.sp,  &s.n_sp,  8);

        if (cnt == cap) {
            size_t nc = cap ? cap * 2u : 512u;
            spfy_vb_slot *nv = (spfy_vb_slot *)realloc(v, nc * sizeof *nv);
            if (!nv) { free(v); free(txt); return SPFY_E_NOMEM; }
            v = nv; cap = nc;
        }
        v[cnt++] = s;
        p = (obj_end < end) ? obj_end + 1 : end;
    }
    free(txt);
    *out = v;
    *out_n = cnt;
    return SPFY_OK;
}

/* ====================================================================== */
/* .seg                                                                    */

int spfy_vb_seg_read(const char *path, spfy_vb_segent **out, size_t *out_n,
                     size_t *n_bad)
{
    *out = NULL;
    *out_n = 0;
    *n_bad = 0;
    char *txt = NULL;
    size_t n = 0;
    if (spfy_vb_read_text(path, &txt, &n) != SPFY_OK) return SPFY_E_IO;

    /* rows[(phone, slot)] = (s, e, requested, unit_phone) */
    typedef struct { int ph, slot, s, e, rq, uf; } row_t;
    row_t *rows = NULL;
    size_t nr = 0, cap = 0;
    int max_ph = -1;

    char *line = txt, *end = txt + n;
    while (line < end) {
        char *nl = memchr(line, '\n', (size_t)(end - line));
        char *stop = nl ? nl : end;
        if (stop > line && stop[-1] == '\r') --stop;
        if (stop > line && *line != '#') {
            int f[8], nf = 0;
            char *q = line;
            while (q < stop && nf < 8) {
                while (q < stop && (*q == ' ' || *q == '\t')) ++q;
                if (q >= stop) break;
                int neg = 0;
                if (*q == '-') { neg = 1; ++q; }
                if (q >= stop || *q < '0' || *q > '9') break;
                long v = 0;
                while (q < stop && *q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); ++q; }
                f[nf++] = (int)(neg ? -v : v);
            }
            if (nf >= 6) {
                if (nr == cap) {
                    size_t nc = cap ? cap * 2u : 1024u;
                    row_t *nv = (row_t *)realloc(rows, nc * sizeof *nv);
                    if (!nv) { free(rows); free(txt); return SPFY_E_NOMEM; }
                    rows = nv; cap = nc;
                }
                rows[nr].ph = f[0]; rows[nr].slot = f[1];
                rows[nr].s = f[2];  rows[nr].e = f[3];
                rows[nr].rq = f[4]; rows[nr].uf = f[5];
                if (f[0] > max_ph) max_ph = f[0];
                ++nr;
            }
        }
        line = nl ? nl + 1 : end;
    }
    free(txt);
    if (max_ph < 0) { free(rows); return SPFY_OK; }

    /* Slot numbers restart each phrase, so the global index is the phrase's
     * running slot offset plus its own slot. Positional, never compacted --
     * a dropped phone must leave a HOLE or every later context shifts. */
    int *maxslot = (int *)calloc((size_t)max_ph + 1u, sizeof *maxslot);
    if (!maxslot) { free(rows); return SPFY_E_NOMEM; }
    for (size_t i = 0; i <= (size_t)max_ph; ++i) maxslot[i] = -1;
    for (size_t i = 0; i < nr; ++i)
        if (rows[i].slot > maxslot[rows[i].ph]) maxslot[rows[i].ph] = rows[i].slot;
    int *offset = (int *)calloc((size_t)max_ph + 1u, sizeof *offset);
    if (!offset) { free(maxslot); free(rows); return SPFY_E_NOMEM; }
    int base = 0;
    for (int ph = 0; ph <= max_ph; ++ph) {
        if (maxslot[ph] < 0) { offset[ph] = base; continue; }
        offset[ph] = base;
        base += maxslot[ph] + 1;
    }
    size_t n_ent = (size_t)base / 2u;
    spfy_vb_segent *ent = (spfy_vb_segent *)calloc(n_ent ? n_ent : 1u, sizeof *ent);
    if (!ent) { free(offset); free(maxslot); free(rows); return SPFY_E_NOMEM; }
    for (size_t i = 0; i < n_ent; ++i) ent[i].phone = -1;

    /* Index by GLOBAL slot so the right half is an O(1) lookup. The Python
     * dict does the same thing; scanning for the partner turned a 3,000-slot
     * recording into nine million comparisons. */
    const row_t **by_slot = (const row_t **)calloc((size_t)base ? (size_t)base : 1u,
                                                   sizeof *by_slot);
    if (!by_slot) { free(ent); free(offset); free(maxslot); free(rows); return SPFY_E_NOMEM; }
    for (size_t i = 0; i < nr; ++i) {
        long g = offset[rows[i].ph] + rows[i].slot;
        if (g >= 0 && g < base) by_slot[g] = &rows[i];
    }

    size_t bad = 0;
    for (size_t i = 0; i < nr; ++i) {
        if (rows[i].slot & 1) continue;              /* right half taken with left */
        long gs = offset[rows[i].ph] + rows[i].slot;
        size_t g = (size_t)gs / 2u;
        if (g >= n_ent) continue;
        if (rows[i].rq >= 0) ent[g].phone = rows[i].rq;
        const row_t *nxt = (gs + 1 < base) ? by_slot[gs + 1] : NULL;
        if (!nxt) { ++bad; continue; }
        if (rows[i].rq < 0 || rows[i].rq != nxt->rq ||
            rows[i].uf != rows[i].rq || nxt->uf != rows[i].rq ||
            nxt->s < rows[i].e || nxt->e <= rows[i].s) { ++bad; continue; }
        ent[g].lo  = rows[i].s / 8;
        ent[g].mid = rows[i].e / 8;
        ent[g].hi  = nxt->e / 8;
        ent[g].ok  = 1;
    }
    free(by_slot);
    free(offset);
    free(maxslot);
    free(rows);
    *out = ent;
    *out_n = n_ent;
    *n_bad = bad;
    return SPFY_OK;
}
