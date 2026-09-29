/* VDB loader. */

#include "voice.h"

#include "../common/file_io.h"
#include "../common/obfuscation.h"
#include "../common/riff.h"
#include "../common/log.h"

#include <stdio.h>
#include "../../include/spfy/spfy.h"

#include <stdlib.h>
#include <string.h>

#define FOURCC_RIFF SPFY_FOURCC('R','I','F','F')
#define FOURCC_WAVE SPFY_FOURCC('W','A','V','E')
#define FOURCC_LIST SPFY_FOURCC('L','I','S','T')
#define FCC_FMT     SPFY_FOURCC('f','m','t',' ')
#define FCC_INDX    SPFY_FOURCC('i','n','d','x')
#define FCC_DATA    SPFY_FOURCC('d','a','t','a')

static uint32_t le_u32(const uint8_t *p)
{
    return (uint32_t)p[0]        | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t le_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int parse_indx(spfy_vdb_t *v)
{
    const uint8_t *p   = v->indx;
    const uint8_t *end = v->indx + v->indx_n;
    if ((size_t)(end - p) < 4) return SPFY_E_FORMAT;

    uint32_t count = le_u32(p); p += 4;
    if (count == 0) {
        v->indx_entries   = NULL;
        v->n_indx_entries = 0;
        return SPFY_OK;
    }

    /* Sanity cap: indx with > 1M entries would mean ~6 MB of name overhead
     * and is implausible for the format. */
    if (count > 1024u * 1024u) return SPFY_E_FORMAT;

    struct spfy_indx_entry *arr = (struct spfy_indx_entry *)
        calloc(count, sizeof *arr);
    if (!arr) return SPFY_E_NOMEM;

    for (uint32_t i = 0; i < count; ++i) {
        if ((size_t)(end - p) < 6) { free(arr); return SPFY_E_FORMAT; }
        arr[i].data_offset = le_u32(p); p += 4;
        arr[i].name_len    = le_u16(p); p += 2;
        if ((size_t)(end - p) < arr[i].name_len) {
            free(arr); return SPFY_E_FORMAT;
        }
        arr[i].name = (const char *)p;
        p += arr[i].name_len;
    }

    v->indx_entries   = arr;
    v->n_indx_entries = count;
    return SPFY_OK;
}

/* The single top-level 'data' chunk's payload, walked exactly as
 * spfy_riff_iter_next would walk the de-obfuscated file. Anything unusual
 * (no data chunk, two of them, a malformed header) returns [n, n) so the
 * caller de-obfuscates the whole file as before. */
static void vdb_find_data_body(const uint8_t *buf, size_t n, size_t *d0, size_t *d1)
{
    const uint32_t k = 0x01010101u * (uint32_t)SPFY_OBFUSCATION_BYTE;
    *d0 = *d1 = n;
    if (n < 12) return;
    uint32_t riff_size = le_u32(buf + 4) ^ k;
    if (riff_size < 4 || (size_t)riff_size + 8 > n) return;
    size_t cur = 12, end = 12 + (size_t)riff_size - 4;
    size_t b0 = n, b1 = n;
    int found = 0;
    while (cur < end) {
        if (end - cur < 8) return;
        uint32_t fcc  = le_u32(buf + cur) ^ k;
        uint32_t size = le_u32(buf + cur + 4) ^ k;
        size_t payload = cur + 8;
        if (size > end - payload) return;
        if (fcc == FCC_DATA) {
            if (found++) return;
            b0 = payload;
            b1 = payload + size;
        }
        cur = payload + (size_t)size + ((size & 1u) ? 1u : 0u);
    }
    if (found == 1) { *d0 = b0; *d1 = b1; }
}

int spfy_vdb_load(const char *path, spfy_vdb_t *out)
{
    if (!path || !out) return SPFY_E_INVAL;
    memset(out, 0, sizeof *out);

    uint8_t *buf = NULL;
    size_t   n   = 0;
    int mapped = 0;
    int rc = spfy_map_file(path, &buf, &n, &mapped);
    if (rc != SPFY_OK) return rc;
    /* The audio stays obfuscated and mapped: a request touches a few
     * hundred KB of a ~60 MB file. spfy_vdb_decode applies data_xor. */
    size_t d0, d1;
    vdb_find_data_body(buf, n, &d0, &d1);
    spfy_unobfuscate_ce(buf, d0);
    spfy_unobfuscate_ce(buf + d1, n - d1);
    out->data_xor = (d0 < d1) ? (uint8_t)SPFY_OBFUSCATION_BYTE : 0u;
    out->bytes    = buf;
    out->n_bytes  = n;
    out->mapped   = mapped;

    if (n < 12) { spfy_vdb_free(out); return SPFY_E_FORMAT; }
    if (le_u32(buf) != FOURCC_RIFF || le_u32(buf + 8) != FOURCC_WAVE) {
        spfy_log_err("vdb: not a RIFF/WAVE file");
        spfy_vdb_free(out); return SPFY_E_FORMAT;
    }
    uint32_t riff_size = le_u32(buf + 4);
    if ((size_t)riff_size + 8 > n) {
        spfy_log_err("vdb: RIFF size %u overruns file (%zu bytes)",
                     riff_size, n);
        spfy_vdb_free(out); return SPFY_E_FORMAT;
    }

    spfy_riff_iter it;
    spfy_riff_iter_init(&it, buf + 12, (size_t)riff_size - 4);
    spfy_chunk c;
    int ir;
    while ((ir = spfy_riff_iter_next(&it, &c)) == 1) {
        switch (c.fourcc) {
        case FCC_FMT:  out->fmt  = c.data; out->fmt_n  = c.size; break;
        case FCC_INDX: out->indx = c.data; out->indx_n = c.size; break;
        case FCC_DATA: out->data = c.data; out->data_n = c.size; break;
        case FOURCC_LIST: break;
        default: {
            char fcc[5]; spfy_fourcc_str(c.fourcc, fcc);
            spfy_log_warn("vdb: unknown chunk '%s' (size=%u)", fcc, c.size);
            break;
        }
        }
    }
    if (ir < 0) { spfy_vdb_free(out); return SPFY_E_FORMAT; }

    if (!out->fmt || !out->indx || !out->data) {
        spfy_log_err("vdb: missing required chunk(s) "
                     "(fmt=%p indx=%p data=%p)",
                     (const void*)out->fmt, (const void*)out->indx,
                     (const void*)out->data);
        spfy_vdb_free(out);
        return SPFY_E_FORMAT;
    }

    /* Parse fmt: standard 16-byte WAVE fmt header. */
    if (out->fmt_n < 16) {
        spfy_vdb_free(out); return SPFY_E_FORMAT;
    }
    out->sample_rate = le_u32(out->fmt + 4);
    out->fmt_tag = (uint16_t)(out->fmt[0] | (out->fmt[1] << 8));
    /* ⚠ Derive from the TAG, never from blockAlign/bitsPerSample: tom8.vdb
     * advertises blockAlign 2 / 16-bit while storing 1-byte µ-law. */
    out->bytes_per_sample = (out->fmt_tag == 0x0007u) ? 1u : 2u;

    rc = parse_indx(out);
    if (rc != SPFY_OK) { spfy_vdb_free(out); return rc; }

    return SPFY_OK;
}

/* Format guard. */
int spfy_vdb_require_supported(const spfy_vdb_t *vdb, const char *path)
{
    if (!vdb) return SPFY_E_INVAL;
    /* SpeechWorks shipped each voice at both rates -- the User's Guide
     * gives `Speechify-Vox-en-US-tom-8kHz-3.0-0.i386.rpm` and the -16kHz
     * sibling -- and they install side by side in one voice directory. */
    int ok = (vdb->fmt_tag == 0x0007u && vdb->bytes_per_sample == 1u)
          || (vdb->fmt_tag == 0x0001u && vdb->bytes_per_sample == 2u);
    if (!ok) {
        fprintf(stderr,
                "spfy: error: vdb: '%s' has wFormatTag=%u at %u Hz; spfy "
                "supports u-law (tag 7) and 16-bit PCM (tag 1) only\n",
                path ? path : "?", (unsigned)vdb->fmt_tag,
                (unsigned)vdb->sample_rate);
        fflush(stderr);
        return SPFY_E_FORMAT;
    }
    if (vdb->sample_rate != 8000u && vdb->sample_rate != 16000u) {
        fprintf(stderr,
                "spfy: error: vdb: '%s' has sample_rate=%u; spfy supports "
                "8000 and 16000\n", path ? path : "?",
                (unsigned)vdb->sample_rate);
        fflush(stderr);
        return SPFY_E_FORMAT;
    }
    return SPFY_OK;
}

/* G.711 µ-law expansion, ITU-T G.711 Table 2a -- the same formula as
 * spfy_ulaw_decode in src/wsola.
 *
 * ⚠ NOT a call into that one, deliberately: spfy_wsola already links
 * spfy_voice, so depending on it here would close a cycle. Decoding VDB
 * storage is a voice concern anyway; WSOLA only happens to have owned the
 * table first. If these ever diverge, parity breaks loudly and immediately. */
static void vdb_ulaw_expand(const uint8_t *src, size_t n, uint8_t x, int16_t *dst)
{
    static int16_t lut[256];
    static int ready = 0;
    if (!ready) {
        for (int i = 0; i < 256; ++i) {
            uint8_t b = (uint8_t)~(uint8_t)i;
            int sign = (b & 0x80) ? -1 : 1;
            int exponent = (b >> 4) & 0x07;
            int mantissa = b & 0x0F;
            int magnitude = ((mantissa << 3) + 0x84) << exponent;
            magnitude -= 0x84;
            lut[i] = (int16_t)(sign * magnitude);
        }
        ready = 1;
    }
    for (size_t i = 0; i < n; ++i) dst[i] = lut[(uint8_t)(src[i] ^ x)];
}

size_t spfy_vdb_decode(const spfy_vdb_t *vdb, size_t rec_off,
                       size_t sample_off, size_t n_samples, int16_t *dst)
{
    if (!vdb || !dst || n_samples == 0) return 0;
    size_t bps = vdb->bytes_per_sample ? vdb->bytes_per_sample : 1u;
    size_t byte_off = rec_off + sample_off * bps;
    size_t avail = 0;
    if (byte_off < vdb->data_n) avail = (vdb->data_n - byte_off) / bps;
    size_t got = (n_samples < avail) ? n_samples : avail;
    if (got) {
        const uint8_t *src = vdb->data + byte_off;
        const uint8_t x = vdb->data_xor;
        if (bps == 1u) {
            vdb_ulaw_expand(src, got, x, dst);
        } else {
            for (size_t i = 0; i < got; ++i)
                dst[i] = (int16_t)((uint16_t)(uint8_t)(src[2 * i] ^ x)
                                   | ((uint16_t)(uint8_t)(src[2 * i + 1] ^ x) << 8));
        }
    }
    if (got < n_samples)
        memset(dst + got, 0, (n_samples - got) * sizeof *dst);
    return got;
}
