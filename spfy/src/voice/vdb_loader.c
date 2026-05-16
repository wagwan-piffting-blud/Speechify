/* VDB loader.
 *
 * Container: standard RIFF/WAVE obfuscated against 0xCE.
 *   <RIFF u32_size WAVE> [LIST? fmt indx data]
 *
 * NB: the fmt header advertises wFormatTag=0x0007 (WAVE_FORMAT_MULAW) and
 * nBitsPerSample=16, but the audio bytes themselves are 1-byte u-law
 * (engine converts u-law->s16 at decode time). Audio is always 8 kHz mono.
 *
 * indx layout (post-XOR):
 *   u32 count
 *   repeated count entries:
 *       u32 data_byte_offset
 *       u16 name_len
 *       char[name_len] name        (NOT NUL-terminated)
 *   final entry is a sentinel (offset = data_size, name_len = 0)
 *
 * indx ordering does NOT match VIN feat[].filename ordering -- callers
 * must look up by name, not by positional index. */

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
     * and is implausible for the format. Defensive bound only. */
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
        arr[i].name = (const char *)p;     /* not NUL-terminated */
        p += arr[i].name_len;
    }

    v->indx_entries   = arr;
    v->n_indx_entries = count;
    return SPFY_OK;
}

int spfy_vdb_load(const char *path, spfy_vdb_t *out)
{
    if (!path || !out) return SPFY_E_INVAL;
    memset(out, 0, sizeof *out);

    uint8_t *buf = NULL;
    size_t   n   = 0;
    int rc = spfy_slurp_file(path, &buf, &n);
    if (rc != SPFY_OK) return rc;
    spfy_unobfuscate_ce(buf, n);

    if (n < 12) { free(buf); return SPFY_E_FORMAT; }
    if (le_u32(buf) != FOURCC_RIFF || le_u32(buf + 8) != FOURCC_WAVE) {
        spfy_log_err("vdb: not a RIFF/WAVE file");
        free(buf); return SPFY_E_FORMAT;
    }
    uint32_t riff_size = le_u32(buf + 4);
    if ((size_t)riff_size + 8 > n) {
        spfy_log_err("vdb: RIFF size %u overruns file (%zu bytes)",
                     riff_size, n);
        free(buf); return SPFY_E_FORMAT;
    }

    out->bytes   = buf;
    out->n_bytes = n;

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

    /* Parse fmt: standard 16-byte WAVE fmt header. We only need the
     * sample rate; the format tag is 0x0007 (MULAW) but actual codec
     * use is u-law 1 byte/sample at runtime. */
    if (out->fmt_n < 16) {
        spfy_vdb_free(out); return SPFY_E_FORMAT;
    }
    out->sample_rate = le_u32(out->fmt + 4);

    /* Parse indx into typed entries. */
    rc = parse_indx(out);
    if (rc != SPFY_OK) { spfy_vdb_free(out); return rc; }

    return SPFY_OK;
}

/* Format guard. The spfy CLI decode path (spfy_synth, spfy_synth_replay,
 * spfy_concat, …) assumes 8 kHz µ-law: 1 byte / sample, 8 bytes / ms,
 * decoded by spfy_ulaw_decode. A 16 kHz VDB (e.g. tom16.vdb) is
 * 16-bit PCM and produces 100% noise when fed through that path —
 * silently, because the unit-selection scores still look plausible.
 * Refuse loudly instead of letting users waste a minute synthesising
 * garbage. */
int spfy_vdb_require_8k_mulaw(const spfy_vdb_t *vdb, const char *path)
{
    if (!vdb) return SPFY_E_INVAL;
    if (vdb->sample_rate != 8000) {
        fprintf(stderr,
                "spfy: error: vdb: '%s' has sample_rate=%u; spfy CLIs "
                "only support the 8 kHz mu-law VDB (try *8.vdb instead "
                "of *16.vdb)\n",
                path ? path : "?", vdb->sample_rate);
        fflush(stderr);
        return SPFY_E_FORMAT;
    }
    return SPFY_OK;
}
