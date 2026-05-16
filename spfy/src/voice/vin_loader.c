/* VIN loader.
 *
 * Container: RIFF/svin obfuscated against 0xCE.
 *   <RIFF u32_size svin> [LIST vers cnts feat mean hist hash prsl ccos ckls
 *                         cklx unit f0tr durt ...]
 *
 * Reads the file, deobfuscates in-place, validates the top-level header,
 * walks chunks via spfy_riff_iter, and stores per-chunk pointers for
 * downstream loaders. The hash chunk's nested {head, rows, cell} is also
 * pre-parsed so the (n_rows, n_cells) header is exposed via
 * spfy_voice_hash_n_rows / spfy_voice_hash_n_cells.
 *
 * See reveng/README_TECHNICAL.md sections 'DLL Analysis: SWIttsEngineUtil.dll'
 * and 'hash' for byte-level layout. */

#include "voice.h"

#include "../common/file_io.h"
#include "../common/obfuscation.h"
#include "../common/riff.h"
#include "../common/log.h"
#include "../../include/spfy/spfy.h"

#include <stdlib.h>
#include <string.h>

#define FOURCC_RIFF SPFY_FOURCC('R','I','F','F')
#define FOURCC_SVIN SPFY_FOURCC('s','v','i','n')
#define FOURCC_LIST SPFY_FOURCC('L','I','S','T')

/* sub-chunk fourcc literals */
#define FCC_VERS SPFY_FOURCC('v','e','r','s')
#define FCC_CNTS SPFY_FOURCC('c','n','t','s')
#define FCC_FEAT SPFY_FOURCC('f','e','a','t')
#define FCC_MEAN SPFY_FOURCC('m','e','a','n')
#define FCC_HIST SPFY_FOURCC('h','i','s','t')
#define FCC_HASH SPFY_FOURCC('h','a','s','h')
#define FCC_PRSL SPFY_FOURCC('p','r','s','l')
#define FCC_CCOS SPFY_FOURCC('c','c','o','s')
#define FCC_CKLS SPFY_FOURCC('c','k','l','s')
#define FCC_CKLX SPFY_FOURCC('c','k','l','x')
#define FCC_UNIT SPFY_FOURCC('u','n','i','t')
#define FCC_F0TR SPFY_FOURCC('f','0','t','r')
#define FCC_DURT SPFY_FOURCC('d','u','r','t')

#define FCC_HEAD SPFY_FOURCC('h','e','a','d')
#define FCC_ROWS SPFY_FOURCC('r','o','w','s')
#define FCC_CELL SPFY_FOURCC('c','e','l','l')

static uint32_t le_u32(const uint8_t *p)
{
    return (uint32_t)p[0]        | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int parse_hash_subchunks(spfy_vin_t *v, const uint8_t *data, size_t n)
{
    /* Three nested RIFF-style sub-chunks: head (8B), rows, cell. */
    spfy_riff_iter it;
    spfy_riff_iter_init(&it, data, n);
    spfy_chunk c;
    int rc;
    int seen_head = 0;
    while ((rc = spfy_riff_iter_next(&it, &c)) == 1) {
        if (c.fourcc == FCC_HEAD) {
            if (c.size < 8) return SPFY_E_FORMAT;
            v->hash_n_rows  = le_u32(c.data);
            v->hash_n_cells = le_u32(c.data + 4);
            seen_head = 1;
        }
        /* rows + cell pointers retained transitively via v->hash; explicit
         * pointers will land when the hash module is implemented. */
    }
    if (rc < 0) return SPFY_E_FORMAT;
    if (!seen_head) {
        spfy_log_warn("vin: hash chunk missing 'head' sub-chunk");
        return SPFY_E_FORMAT;
    }
    return SPFY_OK;
}

int spfy_vin_load(const char *path, spfy_vin_t *out)
{
    if (!path || !out) return SPFY_E_INVAL;
    memset(out, 0, sizeof *out);

    uint8_t *buf = NULL;
    size_t   n   = 0;
    int rc = spfy_slurp_file(path, &buf, &n);
    if (rc != SPFY_OK) return rc;

    /* Whole file XOR'd with 0xCE. Symmetric: same op deobfuscates. */
    spfy_unobfuscate_ce(buf, n);

    /* Top-level: 'RIFF' u32 size 'svin' [chunks...] */
    if (n < 12) { free(buf); return SPFY_E_FORMAT; }
    if (le_u32(buf) != FOURCC_RIFF) {
        spfy_log_err("vin: expected 'RIFF' at offset 0");
        free(buf); return SPFY_E_FORMAT;
    }
    uint32_t riff_size = le_u32(buf + 4);
    if ((size_t)riff_size + 8 > n) {
        spfy_log_err("vin: RIFF size %u overruns file (%zu bytes)",
                     riff_size, n);
        free(buf); return SPFY_E_FORMAT;
    }
    if (le_u32(buf + 8) != FOURCC_SVIN) {
        spfy_log_err("vin: expected form-type 'svin' at offset 8");
        free(buf); return SPFY_E_FORMAT;
    }

    out->bytes   = buf;
    out->n_bytes = n;

    /* Walk top-level chunks starting at offset 12. */
    spfy_riff_iter it;
    spfy_riff_iter_init(&it, buf + 12, (size_t)riff_size - 4);
    spfy_chunk c;
    int ir;
    while ((ir = spfy_riff_iter_next(&it, &c)) == 1) {
        switch (c.fourcc) {
        case FCC_VERS: out->vers = c.data; out->vers_n = c.size; break;
        case FCC_CNTS: out->cnts = c.data; out->cnts_n = c.size; break;
        case FCC_FEAT: out->feat = c.data; out->feat_n = c.size; break;
        case FCC_MEAN: out->mean = c.data; out->mean_n = c.size; break;
        case FCC_HIST: out->hist = c.data; out->hist_n = c.size; break;
        case FCC_PRSL: out->prsl = c.data; out->prsl_n = c.size; break;
        case FCC_CCOS: out->ccos = c.data; out->ccos_n = c.size; break;
        case FCC_CKLS: out->ckls = c.data; out->ckls_n = c.size; break;
        case FCC_CKLX: out->cklx = c.data; out->cklx_n = c.size; break;
        case FCC_UNIT: out->unit = c.data; out->unit_n = c.size; break;
        case FCC_F0TR: out->f0tr = c.data; out->f0tr_n = c.size; break;
        case FCC_DURT: out->durt = c.data; out->durt_n = c.size; break;
        case FCC_HASH:
            out->hash = c.data; out->hash_n = c.size;
            rc = parse_hash_subchunks(out, c.data, c.size);
            if (rc != SPFY_OK) { spfy_vin_free(out); return rc; }
            break;
        case FOURCC_LIST: /* metadata; ignore for now */ break;
        default: {
            char fcc[5]; spfy_fourcc_str(c.fourcc, fcc);
            spfy_log_warn("vin: unknown top-level chunk '%s' (size=%u)",
                          fcc, c.size);
            break;
        }
        }
    }
    if (ir < 0) { spfy_vin_free(out); return SPFY_E_FORMAT; }

    /* Required chunks for an M0a "complete" voice. */
    if (!out->feat || !out->unit || !out->hash || !out->f0tr || !out->durt) {
        spfy_log_err("vin: missing required chunk(s) "
                     "(feat=%p unit=%p hash=%p f0tr=%p durt=%p)",
                     (const void*)out->feat, (const void*)out->unit,
                     (const void*)out->hash, (const void*)out->f0tr,
                     (const void*)out->durt);
        spfy_vin_free(out);
        return SPFY_E_FORMAT;
    }

    return SPFY_OK;
}
