/* spfy_vb_patch -- write fitted selection tables into a COPY of a voice's VIN.
 *
 *   spfy_vb_patch --vin IN.vin --out OUT.vin [--ccos-raw F.f32] [--unit-cc F.u8]
 *
 * --ccos-raw  float32 raw triangle values in on-disk order: for hp in
 *             0..2N-1, slot 0..3, i = 1..N-1, j < i. The loader turns each into
 *             (raw + 0.1) * slot_scale, mirrored; the diagonal is not stored.
 * --unit-cc   one byte per unit: the record's context_cost (the flag term of
 *             the half-phone cost, byte * UNIT_BIAS_WEIGHT * 0.01).
 *
 * Everything else in the container is carried over untouched. The output is
 * re-read through the engine's own loaders before this exits 0. */

#include "../vb/vb_io.h"
#include "../voice/ccos.h"
#include "../voice/unit_table.h"
#include "../voice/voice.h"
#include "../../include/spfy/spfy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_all(const char *path, uint8_t **out, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return SPFY_E_IO;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc(sz > 0 ? (size_t)sz : 1u);
    if (!b) { fclose(f); return SPFY_E_NOMEM; }
    size_t got = fread(b, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(b); return SPFY_E_IO; }
    *out = b;
    *n = got;
    return SPFY_OK;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Rebuild a chunk body whose sub-chunk `sub` gets `data` (same length). */
static int replace_sub(const spfy_vb_chunk *c, const char *sub, const uint8_t *data, size_t n,
                       uint8_t **out, size_t *out_n)
{
    uint8_t *body = (uint8_t *)malloc(c->n);
    if (!body) return SPFY_E_NOMEM;
    memcpy(body, c->data, c->n);
    size_t pos = 0;
    char id[5];
    const uint8_t *d;
    size_t dn;
    while (spfy_vb_subchunk(c->data, c->n, &pos, id, &d, &dn)) {
        if (!strcmp(id, sub)) {
            if (dn != n) {
                fprintf(stderr, "%s/%s: %zu bytes on disk, %zu supplied\n", c->id, sub, dn, n);
                free(body);
                return SPFY_E_FORMAT;
            }
            memcpy(body + (d - c->data), data, n);
            *out = body;
            *out_n = c->n;
            return SPFY_OK;
        }
    }
    free(body);
    fprintf(stderr, "%s: no '%s' sub-chunk\n", c->id, sub);
    return SPFY_E_FORMAT;
}

static const uint8_t *find_sub(const spfy_vb_chunk *c, const char *sub, size_t *dn)
{
    size_t pos = 0;
    char id[5];
    const uint8_t *d;
    while (spfy_vb_subchunk(c->data, c->n, &pos, id, &d, dn))
        if (!strcmp(id, sub)) return d;
    return NULL;
}

int main(int argc, char **argv)
{
    const char *in = NULL, *outp = NULL, *ccos_p = NULL, *cc_p = NULL;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--vin")) in = argv[i + 1];
        else if (!strcmp(argv[i], "--out")) outp = argv[i + 1];
        else if (!strcmp(argv[i], "--ccos-raw")) ccos_p = argv[i + 1];
        else if (!strcmp(argv[i], "--unit-cc")) cc_p = argv[i + 1];
    }
    if (!in || !outp || (!ccos_p && !cc_p)) {
        fprintf(stderr, "usage: spfy_vb_patch --vin IN --out OUT [--ccos-raw F.f32] [--unit-cc F.u8]\n");
        return 2;
    }

    spfy_vin_t vin;
    spfy_unit_table_t ut;
    spfy_ccos_t cc;
    if (spfy_vin_load(in, &vin) != SPFY_OK || spfy_unit_table_load(&vin, &ut) != SPFY_OK
        || spfy_ccos_load(&vin, &cc) != SPFY_OK) {
        fprintf(stderr, "%s: not a loadable VIN\n", in);
        return 3;
    }
    const uint32_t nl = cc.n_labels, n_units = ut.n_units, rec = ut.rec_size;
    const uint32_t off_cc = ut.off_context_cost;

    spfy_vb_riff r;
    if (spfy_vb_riff_load(in, &r) != SPFY_OK) { fprintf(stderr, "riff load failed\n"); return 3; }

    if (ccos_p) {
        uint8_t *raw; size_t raw_n;
        if (read_all(ccos_p, &raw, &raw_n) != SPFY_OK) { fprintf(stderr, "cannot read %s\n", ccos_p); return 4; }
        const size_t tri = (size_t)nl * (nl - 1) / 2;
        const size_t n_tab = 2u * nl * SPFY_CCOS_N_SLOTS;
        if (raw_n != n_tab * tri * 4u) {
            fprintf(stderr, "--ccos-raw: %zu bytes, expected %zu\n", raw_n, n_tab * tri * 4u);
            return 4;
        }
        const size_t data_n = n_tab * (8u + tri * 4u);
        uint8_t *data = (uint8_t *)malloc(data_n);
        if (!data) return 5;
        uint8_t *p = data;
        const uint8_t *s = raw;
        for (uint32_t hp = 0; hp < 2u * nl; ++hp) {
            for (uint32_t slot = 0; slot < SPFY_CCOS_N_SLOTS; ++slot) {
                put_u32(p, hp); put_u32(p + 4, slot); p += 8;
                memcpy(p, s, tri * 4u); p += tri * 4u; s += tri * 4u;
            }
        }
        const spfy_vb_chunk *c = spfy_vb_riff_get(&r, "ccos");
        uint8_t *body; size_t body_n;
        if (!c || replace_sub(c, "data", data, data_n, &body, &body_n) != SPFY_OK) return 6;
        if (spfy_vb_riff_set(&r, "ccos", body, body_n) != SPFY_OK) return 6;
        free(data);
        free(raw);
    }

    if (cc_p) {
        uint8_t *b; size_t bn;
        if (read_all(cc_p, &b, &bn) != SPFY_OK || bn != n_units) {
            fprintf(stderr, "--unit-cc: need exactly %u bytes\n", n_units);
            return 4;
        }
        const spfy_vb_chunk *c = spfy_vb_riff_get(&r, "unit");
        size_t dn = 0;
        const uint8_t *d = c ? find_sub(c, "data", &dn) : NULL;
        if (!d || dn != (size_t)n_units * rec) { fprintf(stderr, "unit/data size mismatch\n"); return 6; }
        uint8_t *recs = (uint8_t *)malloc(dn);
        if (!recs) return 5;
        memcpy(recs, d, dn);
        for (uint32_t u = 0; u < n_units; ++u) recs[(size_t)u * rec + off_cc] = b[u];
        uint8_t *body; size_t body_n;
        if (replace_sub(c, "data", recs, dn, &body, &body_n) != SPFY_OK) return 6;
        if (spfy_vb_riff_set(&r, "unit", body, body_n) != SPFY_OK) return 6;
        free(recs);
        free(b);
    }

    if (spfy_vb_riff_save(&r, outp) != SPFY_OK) { fprintf(stderr, "cannot write %s\n", outp); return 7; }
    spfy_vb_riff_free(&r);

    spfy_vin_t v2;
    spfy_unit_table_t ut2;
    spfy_ccos_t cc2;
    if (spfy_vin_load(outp, &v2) != SPFY_OK || spfy_unit_table_load(&v2, &ut2) != SPFY_OK
        || spfy_ccos_load(&v2, &cc2) != SPFY_OK || ut2.n_units != n_units) {
        fprintf(stderr, "%s: written, but does not re-load cleanly\n", outp);
        return 8;
    }
    printf("wrote %s (%u units, %u ccos labels)%s%s\n", outp, n_units, nl,
           ccos_p ? " ccos" : "", cc_p ? " unit-cc" : "");
    return 0;
}
