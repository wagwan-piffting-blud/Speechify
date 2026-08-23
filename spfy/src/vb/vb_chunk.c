#include "vb_chunk.h"

#include "../common/log.h"
#include "../../include/spfy/spfy.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* ====================================================================== */
/* feat                                                                    */

int spfy_vb_feat_sections(const uint8_t *feat, size_t n,
                          spfy_vb_feat_section **out, size_t *out_n)
{
    spfy_vb_feat_section *v = NULL;
    size_t cnt = 0, cap = 0;
    size_t p = 0;
    while (p + 2 <= n) {
        size_t start = p;
        uint16_t klen = rd_u16(feat + p);
        p += 2;
        if (p + klen + 4 > n) break;
        char key[64];
        size_t kk = klen < sizeof key - 1u ? klen : sizeof key - 1u;
        memcpy(key, feat + p, kk);
        key[kk] = 0;
        p += klen;
        uint32_t cnt_v = rd_u32(feat + p);
        p += 4;
        int ok = 1;
        for (uint32_t i = 0; i < cnt_v; ++i) {
            if (p + 2 > n) { ok = 0; break; }
            uint16_t nlen = rd_u16(feat + p);
            p += 2u + nlen + 4u;
            if (p > n) { ok = 0; break; }
        }
        if (!ok) break;
        if (cnt == cap) {
            size_t nc = cap ? cap * 2u : 32u;
            spfy_vb_feat_section *nv =
                (spfy_vb_feat_section *)realloc(v, nc * sizeof *nv);
            if (!nv) { free(v); return SPFY_E_NOMEM; }
            v = nv; cap = nc;
        }
        v[cnt].raw = feat + start;
        v[cnt].raw_n = p - start;
        memcpy(v[cnt].key, key, sizeof v[cnt].key);
        ++cnt;
    }
    *out = v;
    *out_n = cnt;
    return SPFY_OK;
}

int spfy_vb_feat_counts(const uint8_t *feat, size_t n,
                        uint32_t *n_name, uint32_t *n_keys)
{
    spfy_vb_feat_section *fs = NULL;
    size_t n_fs = 0;
    int rc = spfy_vb_feat_sections(feat, n, &fs, &n_fs);
    if (rc != SPFY_OK) return rc;
    uint32_t nn = 0;
    for (size_t i = 0; i < n_fs; ++i) {
        if (strcmp(fs[i].key, "name")) continue;
        size_t klen = strlen(fs[i].key);
        if (fs[i].raw_n >= 2u + klen + 4u) nn = rd_u32(fs[i].raw + 2u + klen);
        break;
    }
    free(fs);
    if (n_name) *n_name = nn;
    if (n_keys) *n_keys = (uint32_t)n_fs;
    return SPFY_OK;
}

int spfy_vb_phone_index_build(const uint8_t *feat, size_t n,
                              spfy_vb_phone_index *out)
{
    memset(out, 0, sizeof *out);
    spfy_vb_feat_section *sec = NULL;
    size_t n_sec = 0;
    int rc = spfy_vb_feat_sections(feat, n, &sec, &n_sec);
    if (rc != SPFY_OK) return rc;

    for (size_t i = 0; i < n_sec; ++i) {
        if (strcmp(sec[i].key, "name") != 0) continue;
        const uint8_t *raw = sec[i].raw;
        size_t p = 2u + strlen(sec[i].key);
        uint32_t cnt = rd_u32(raw + p);
        p += 4;
        /* 92 half-phone names are 46 phones x {1,2}; the phone id is i/2. */
        out->name = (char (*)[8])calloc((cnt / 2u) + 1u, sizeof *out->name);
        if (!out->name) { free(sec); return SPFY_E_NOMEM; }
        for (uint32_t k = 0; k < cnt; ++k) {
            uint16_t nl = rd_u16(raw + p);
            p += 2;
            if (nl && raw[p + nl - 1u] == '1') {
                size_t base = nl - 1u;
                if (base > 7) base = 7;
                size_t id = k / 2u;
                memcpy(out->name[id], raw + p, base);
                out->name[id][base] = 0;
                if (id + 1u > out->n) out->n = id + 1u;
            }
            p += nl + 4u;
        }
        break;
    }
    free(sec);
    return out->name ? SPFY_OK : SPFY_E_FORMAT;
}

int spfy_vb_phone_id(const spfy_vb_phone_index *pi, const char *name)
{
    for (size_t i = 0; i < pi->n; ++i)
        if (strcmp(pi->name[i], name) == 0) return (int)i;
    return -1;
}

void spfy_vb_phone_index_free(spfy_vb_phone_index *pi)
{
    free(pi->name);
    pi->name = NULL;
    pi->n = 0;
}

int spfy_vb_labl_map_build(const uint8_t *ccos, size_t ccos_n,
                           const uint8_t *feat, size_t feat_n,
                           spfy_vb_labl_map *out)
{
    for (int i = 0; i < 256; ++i) { out->l2f[i] = -1; out->f2l[i] = -1; }

    const uint8_t *labl = NULL;
    size_t labl_n = 0, pos = 0;
    char id[5];
    const uint8_t *d;
    size_t dn;
    while (spfy_vb_subchunk(ccos, ccos_n, &pos, id, &d, &dn)) {
        if (!memcmp(id, "labl", 4)) { labl = d; labl_n = dn; break; }
    }
    if (!labl) return SPFY_E_FORMAT;

    spfy_vb_phone_index pi;
    int rc = spfy_vb_phone_index_build(feat, feat_n, &pi);
    if (rc != SPFY_OK) return rc;

    uint32_t n = rd_u32(labl);
    size_t p = 4;
    for (uint32_t i = 0; i < n && i < 256u; ++i) {
        if (p + 2 > labl_n) break;
        uint16_t ln = rd_u16(labl + p);
        p += 2;
        if (p + ln > labl_n) break;
        char nm[16];
        size_t k = ln < sizeof nm - 1u ? ln : sizeof nm - 1u;
        memcpy(nm, labl + p, k);
        nm[k] = 0;
        p += ln;
        int fid = spfy_vb_phone_id(&pi, nm);
        if (fid >= 0) {
            out->l2f[i] = (int16_t)fid;
            out->f2l[fid] = (int16_t)i;
        }
    }
    spfy_vb_phone_index_free(&pi);
    return SPFY_OK;
}

int spfy_vb_build_feat(const uint8_t *tmpl_feat, size_t tmpl_n,
                       char *const *names, size_t n_names,
                       uint8_t **out, size_t *out_n)
{
    spfy_vb_feat_section *sec = NULL;
    size_t n_sec = 0;
    int rc = spfy_vb_feat_sections(tmpl_feat, tmpl_n, &sec, &n_sec);
    if (rc != SPFY_OK) return rc;

    spfy_vb_buf b = {0};
    int wrote_names = 0;
    for (size_t i = 0; i < n_sec; ++i) {
        if (strcmp(sec[i].key, "filename") != 0) {
            rc = spfy_vb_buf_put(&b, sec[i].raw, sec[i].raw_n);
            if (rc != SPFY_OK) goto done;
            continue;
        }
        rc = spfy_vb_buf_u16(&b, 8);
        if (rc == SPFY_OK) rc = spfy_vb_buf_put(&b, "filename", 8);
        if (rc == SPFY_OK) rc = spfy_vb_buf_u32(&b, (uint32_t)n_names);
        if (rc != SPFY_OK) goto done;
        for (size_t k = 0; k < n_names; ++k) {
            rc = spfy_vb_buf_pstr(&b, names[k]);
            if (rc == SPFY_OK) rc = spfy_vb_buf_u32(&b, (uint32_t)k);
            if (rc != SPFY_OK) goto done;
        }
        wrote_names = 1;
    }
    /* ⚠ The embedded language table (vb_lang.h) has NO `filename` section --
     * that is the one per-voice part and is deliberately not shipped in it.
     * Without this the donor-free build emitted a feat with no recordings in
     * it at all, and the only symptom would be every unit resolving to
     * file_idx 0. */
    if (!wrote_names) {
        rc = spfy_vb_buf_u16(&b, 8);
        if (rc == SPFY_OK) rc = spfy_vb_buf_put(&b, "filename", 8);
        if (rc == SPFY_OK) rc = spfy_vb_buf_u32(&b, (uint32_t)n_names);
        if (rc != SPFY_OK) goto done;
        for (size_t k = 0; k < n_names; ++k) {
            rc = spfy_vb_buf_pstr(&b, names[k]);
            if (rc == SPFY_OK) rc = spfy_vb_buf_u32(&b, (uint32_t)k);
            if (rc != SPFY_OK) goto done;
        }
    }
    *out = b.p;
    *out_n = b.n;
    free(sec);
    return SPFY_OK;

done:
    spfy_vb_buf_free(&b);
    free(sec);
    return rc;
}

/* ====================================================================== */
/* unit                                                                    */

size_t spfy_vb_unit_stride(uint32_t ver)
{
    if (ver == SPFY_VB_UNIT_VERSION)    return SPFY_VB_UNIT_STRIDE;
    if (ver == SPFY_VB_UNIT_V8_VERSION) return SPFY_VB_UNIT_V8_STRIDE;
    return 0;
}

/* The two layouts differ by a SINGLE INSERTION at 0x10, so the tail is one
 * expression with a shift rather than two tables that could drift apart. The
 * offsets are the same ones the engine's own loader uses (unit_table.c's
 * UNIT_LAYOUTS): v100008 puts phone_ctx at 0x18, flag_b at 0x1C and
 * context_cost at 0x1D -- each exactly one past v100006. */
size_t spfy_vb_pack_unit_ver(const spfy_vb_unit *u, uint32_t ver, uint8_t *o)
{
    size_t stride = spfy_vb_unit_stride(ver);
    if (!stride) return 0;
    const size_t s = (ver == SPFY_VB_UNIT_V8_VERSION) ? 1u : 0u;

    memset(o, 0, stride);
    o[0x00] = (uint8_t)(u->uid & 0xFFu);
    o[0x01] = (uint8_t)((u->uid >> 8) & 0xFFu);
    o[0x02] = (uint8_t)((u->uid >> 16) & 0xFFu);
    o[0x03] = (uint8_t)((u->uid >> 24) & 0xFFu);
    o[0x04] = (uint8_t)(u->file_idx & 0xFFu);
    o[0x05] = (uint8_t)(u->file_idx >> 8);
    o[0x06] = (uint8_t)(u->local_pos & 0xFFu);
    o[0x07] = (uint8_t)(u->local_pos >> 8);
    o[0x08] = (uint8_t)(u->u08 & 0xFFu);
    o[0x09] = (uint8_t)(u->u08 >> 8);
    o[0x0A] = (uint8_t)(u->dur_like & 0xFFu);
    o[0x0B] = (uint8_t)(u->dur_like >> 8);
    o[0x0C] = u->sp_syl_in_phrase;
    o[0x0D] = u->sp_syl_type;
    o[0x0E] = u->sp_word_in_phrase;
    o[0x0F] = u->sp_syl_in_word;
    if (s) o[0x10] = u->sp_phone_in_syl;      /* v100008's one extra column */
    o[0x10 + s] = u->f0_start;
    o[0x11 + s] = u->f0_end;
    o[0x12 + s] = u->f0_mid;
    o[0x13 + s] = u->f0_context;
    o[0x14 + s] = u->phone_center;
    o[0x15 + s] = u->is_first_half;
    o[0x16 + s] = u->voice_const;
    o[0x17 + s] = u->phone_ctx[0];
    o[0x18 + s] = u->phone_ctx[1];
    o[0x19 + s] = u->phone_ctx[2];
    o[0x1A + s] = u->phone_ctx[3];
    o[0x1B + s] = u->flag_b;
    o[0x1C + s] = u->context_cost;
    return stride;
}

void spfy_vb_pack_unit(const spfy_vb_unit *u, uint8_t o[SPFY_VB_UNIT_STRIDE])
{
    spfy_vb_pack_unit_ver(u, SPFY_VB_UNIT_VERSION, o);
}

/* ====================================================================== */
/* prsl                                                                    */

typedef struct { uint32_t key, uid; } key_uid;

static int cmp_key_uid(const void *a, const void *b)
{
    const key_uid *x = (const key_uid *)a, *y = (const key_uid *)b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    /* Stable by uid: the Python appends in uid order within a bucket. */
    return x->uid < y->uid ? -1 : (x->uid > y->uid);
}

int spfy_vb_group_units(const spfy_vb_unit *units, size_t n_units,
                        const uint8_t *withheld,
                        spfy_vb_group **out, size_t *out_n)
{
    key_uid *ku = (key_uid *)malloc((n_units ? n_units : 1u) * sizeof *ku);
    if (!ku) return SPFY_E_NOMEM;
    size_t n = 0;
    for (size_t i = 0; i < n_units; ++i) {
        if (withheld && withheld[i]) continue;
        ku[n].key = units[i].key;
        ku[n].uid = units[i].uid;
        ++n;
    }
    if (n > 1) qsort(ku, n, sizeof *ku, cmp_key_uid);

    size_t n_g = 0;
    for (size_t i = 0; i < n; ++i)
        if (i == 0 || ku[i].key != ku[i - 1].key) ++n_g;

    spfy_vb_group *g = (spfy_vb_group *)calloc(n_g ? n_g : 1u, sizeof *g);
    if (!g) { free(ku); return SPFY_E_NOMEM; }
    size_t gi = 0, i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && ku[j].key == ku[i].key) ++j;
        g[gi].key = ku[i].key;
        g[gi].n = (uint32_t)(j - i);
        g[gi].uid = (uint32_t *)malloc((j - i) * sizeof *g[gi].uid);
        if (!g[gi].uid) { spfy_vb_groups_free(g, gi); free(ku); return SPFY_E_NOMEM; }
        for (size_t k = i; k < j; ++k) g[gi].uid[k - i] = ku[k].uid;
        ++gi;
        i = j;
    }
    free(ku);
    *out = g;
    *out_n = n_g;
    return SPFY_OK;
}

/* ---- backoff membership; see the header for the measurement that named it */

/* Broad phonetic class, for the backoff tier where NEITHER context matches
 * exactly. That tier is 57% of a fill-24 build's listings and was being filled
 * by rotation alone -- i.e. arbitrarily -- which is the suspect for the
 * "never said" seams getting WORSE as the fill rose (1.34 -> 1.69 -> 1.93 st
 * on wisconsin_cdw). A neighbour of the same manner coarticulates similarly,
 * so it is a far better substitute than an arbitrary one.
 *
 * Names come from the template's own feat table, so a voice with a different
 * inventory degrades to PC_OTHER rather than mis-classifying. */
typedef enum {
    PC_OTHER = 0, PC_VOWEL, PC_STOP, PC_FRIC, PC_AFFR,
    PC_NASAL, PC_LIQUID, PC_GLIDE, PC_SIL, PC_N
} phone_class;

static phone_class classify_phone(const char *nm)
{
    static const char *V[]  = {"aa","ae","ah","ao","aw","ax","ay","eh","er",
                               "ey","ih","ix","iy","ow","oy","uh","uw", NULL};
    static const char *ST[] = {"b","d","g","k","p","t","dx", NULL};
    static const char *FR[] = {"dh","f","hh","s","sh","th","v","z","zh", NULL};
    static const char *AF[] = {"ch","jh", NULL};
    static const char *NA[] = {"m","n","ng","en", NULL};
    static const char *LI[] = {"l","r","el", NULL};
    static const char *GL[] = {"w","y", NULL};
    static const char *SI[] = {"pau","sil","h#", NULL};
    static const struct { const char **set; phone_class c; } TAB[] = {
        {V, PC_VOWEL}, {ST, PC_STOP}, {FR, PC_FRIC}, {AF, PC_AFFR},
        {NA, PC_NASAL}, {LI, PC_LIQUID}, {GL, PC_GLIDE}, {SI, PC_SIL},
    };
    if (!nm || !*nm) return PC_OTHER;
    for (size_t t = 0; t < sizeof TAB / sizeof TAB[0]; ++t)
        for (const char **p = TAB[t].set; *p; ++p)
            if (!strcmp(nm, *p)) return TAB[t].c;
    return PC_OTHER;
}

/* Per centre class: the units carrying it, indexed by their own left and
 * right context so filling one key is O(n_fill) rather than a scan. */
typedef struct {
    uint32_t *all;   uint32_t  n_all;
    uint32_t *l_off; uint32_t *l_uid;   /* CSR over left  context class */
    uint32_t *r_off; uint32_t *r_uid;   /* CSR over right context class */
    /* Same, bucketed by the neighbour's BROAD CLASS rather than its identity:
     * the tier between "exact context" and "anything of this phone". */
    uint32_t *lc_off; uint32_t *lc_uid;
    uint32_t *rc_off; uint32_t *rc_uid;
} centre_idx;

/* Append uid if it is admissible and not already taken. Returns 1 if added. */
static int bo_take(uint32_t uid, uint32_t *dst, uint32_t *n, uint32_t cap,
                   uint8_t *seen, const uint8_t *withheld, size_t n_units)
{
    if (*n >= cap) return 0;
    if ((size_t)uid >= n_units) return 0;
    if (withheld && withheld[uid]) return 0;
    if (seen[uid]) return 0;
    seen[uid] = 1;
    dst[(*n)++] = uid;
    return 1;
}

static int cmp_group_key(const void *a, const void *b);   /* below */

static int cmp_u32_asc(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x < y) ? -1 : (x > y);
}

int spfy_vb_groups_backoff(const spfy_vb_unit *units, size_t n_units,
                           const uint8_t *withheld, const uint8_t *is_real,
                           const spfy_vb_phone_index *pidx,
                           const spfy_vb_group *exact, size_t n_exact,
                           uint32_t hp_bound, uint32_t n_fill, uint32_t gate,
                           spfy_vb_group **out, size_t *out_n,
                           spfy_vb_backoff_stats *st)
{
    memset(st, 0, sizeof *st);
    if (!hp_bound || hp_bound > 92u) hp_bound = 92u;
    if (!n_fill) { *out = NULL; *out_n = 0; return SPFY_OK; }

    /* half-phone class -> broad phonetic class. Class c covers phone c/2. */
    uint8_t hp_class[92];
    for (uint32_t x = 0; x < 92u; ++x) {
        size_t p = x / 2u;
        hp_class[x] = (uint8_t)((pidx && p < pidx->n)
                                ? classify_phone(pidx->name[p]) : PC_OTHER);
    }

    const uint32_t NC = hp_bound;              /* centre classes 0..NC-1 */
    centre_idx *ci = (centre_idx *)calloc(NC, sizeof *ci);
    uint8_t    *seen = (uint8_t *)calloc(n_units ? n_units : 1u, 1);
    /* Declared with ci/seen so the nomem path can free them unconditionally. */
    uint32_t *cur_l = (uint32_t *)calloc((size_t)NC * hp_bound, sizeof *cur_l);
    uint32_t *cur_r = (uint32_t *)calloc((size_t)NC * hp_bound, sizeof *cur_r);
    uint32_t *cur_a = (uint32_t *)calloc(NC, sizeof *cur_a);
    if (!ci || !seen || !cur_l || !cur_r || !cur_a) {
        free(ci); free(seen); free(cur_l); free(cur_r); free(cur_a);
        return SPFY_E_NOMEM;
    }

    /* A unit's own key decomposes into the three classes; its centre is what
     * indexes it here, and its own L/R are what a target key can match. */
    for (uint32_t c = 0; c < NC; ++c) {
        ci[c].l_off = (uint32_t *)calloc((size_t)hp_bound + 1u, sizeof(uint32_t));
        ci[c].r_off = (uint32_t *)calloc((size_t)hp_bound + 1u, sizeof(uint32_t));
        ci[c].lc_off = (uint32_t *)calloc((size_t)PC_N + 1u, sizeof(uint32_t));
        ci[c].rc_off = (uint32_t *)calloc((size_t)PC_N + 1u, sizeof(uint32_t));
        if (!ci[c].l_off || !ci[c].r_off || !ci[c].lc_off || !ci[c].rc_off)
            goto nomem;
    }
    for (size_t i = 0; i < n_units; ++i) {
        uint32_t k = units[i].key;
        uint32_t L = k / 10000u, C = (k / 100u) % 100u, R = k % 100u;
        if (!k || L >= hp_bound || C >= hp_bound || R >= hp_bound) continue;
        if (withheld && withheld[units[i].uid]) continue;
        ++ci[C].n_all;
        ci[C].l_off[L + 1u]++;
        ci[C].r_off[R + 1u]++;
        ci[C].lc_off[hp_class[L] + 1u]++;
        ci[C].rc_off[hp_class[R] + 1u]++;
    }
    for (uint32_t c = 0; c < NC; ++c) {
        for (uint32_t x = 0; x < hp_bound; ++x) {
            ci[c].l_off[x + 1u] += ci[c].l_off[x];
            ci[c].r_off[x + 1u] += ci[c].r_off[x];
        }
        for (uint32_t x = 0; x < (uint32_t)PC_N; ++x) {
            ci[c].lc_off[x + 1u] += ci[c].lc_off[x];
            ci[c].rc_off[x + 1u] += ci[c].rc_off[x];
        }
        uint32_t sz = ci[c].n_all ? ci[c].n_all : 1u;
        ci[c].all    = (uint32_t *)malloc((size_t)sz * 4u);
        ci[c].l_uid  = (uint32_t *)malloc((size_t)sz * 4u);
        ci[c].r_uid  = (uint32_t *)malloc((size_t)sz * 4u);
        ci[c].lc_uid = (uint32_t *)malloc((size_t)sz * 4u);
        ci[c].rc_uid = (uint32_t *)malloc((size_t)sz * 4u);
        if (!ci[c].all || !ci[c].l_uid || !ci[c].r_uid
            || !ci[c].lc_uid || !ci[c].rc_uid) goto nomem;
        ci[c].n_all = 0;
    }
    {
        uint32_t *lf = (uint32_t *)calloc((size_t)NC * hp_bound, sizeof *lf);
        uint32_t *rf = (uint32_t *)calloc((size_t)NC * hp_bound, sizeof *rf);
        uint32_t *lcf = (uint32_t *)calloc((size_t)NC * PC_N, sizeof *lcf);
        uint32_t *rcf = (uint32_t *)calloc((size_t)NC * PC_N, sizeof *rcf);
        if (!lf || !rf || !lcf || !rcf) {
            free(lf); free(rf); free(lcf); free(rcf); goto nomem;
        }
        for (size_t i = 0; i < n_units; ++i) {
            uint32_t k = units[i].key;
            uint32_t L = k / 10000u, C = (k / 100u) % 100u, R = k % 100u;
            if (!k || L >= hp_bound || C >= hp_bound || R >= hp_bound) continue;
            if (withheld && withheld[units[i].uid]) continue;
            uint32_t u = units[i].uid;
            ci[C].all[ci[C].n_all++] = u;
            ci[C].l_uid[ci[C].l_off[L] + lf[(size_t)C * hp_bound + L]++] = u;
            ci[C].r_uid[ci[C].r_off[R] + rf[(size_t)C * hp_bound + R]++] = u;
            uint32_t lc = hp_class[L], rcx = hp_class[R];
            ci[C].lc_uid[ci[C].lc_off[lc] + lcf[(size_t)C * PC_N + lc]++] = u;
            ci[C].rc_uid[ci[C].rc_off[rcx] + rcf[(size_t)C * PC_N + rcx]++] = u;
        }
        free(lf); free(rf); free(lcf); free(rcf);
    }

    /* ⚠ FAIRNESS (cur_l / cur_r / cur_a, allocated above). A key-derived start
     * offset (`key % n`) is NOT round-robin: different keys sharing a bucket
     * land wherever the modulus puts them, and the first arm built that way
     * listed one unit in 2,099 groups where jill's busiest is in 245. A cursor
     * that PERSISTS across keys hands the bucket out in rotation, so a thin
     * centre phone spreads its units over the contexts that need them instead
     * of repeating a favourite.
     *
     * ONLY the new groups come back. The caller must build the wide fallbacks
     * from the EXACT set first and merge these in afterwards: wide_add() does
     * not dedupe, so a unit listed in many backoff groups would be appended to
     * (92,c,92) once per listing. */
    size_t cap = (size_t)NC * ((size_t)hp_bound / 2u)
                            * ((size_t)hp_bound / 2u) + 16u;
    spfy_vb_group *g = (spfy_vb_group *)calloc(cap, sizeof *g);
    uint32_t *buf = (uint32_t *)malloc((size_t)n_fill * 4u);
    if (!g || !buf) { free(g); free(buf); goto nomem; }
    size_t ng = 0;

    for (uint32_t C = 0; C < NC; ++C) {
        uint32_t par = C & 1u;
        for (uint32_t L = par; L < hp_bound; L += 2u) {
            for (uint32_t R = par; R < hp_bound; R += 2u) {
                uint32_t key = L * 10000u + C * 100u + R;
                ++st->n_keys_possible;
                if (!ci[C].n_all) { ++st->n_keys_unfillable; continue; }
                /* ⭐ WHICH KEYS DESERVE A FILL. Filling every enumerable key
                 * puts contextually unrelated units under 97.8% of the space;
                 * the vendors populate ~39.5% and leave the rest to the
                 * engine's own fallback ladder. Requiring BOTH bigrams to
                 * occur in our units reproduces 35.6% here, and recovers
                 * 90-92% of a vendor's actual key set when scored against it. */
                if (gate == SPFY_VB_BG_BIGRAM
                    && (ci[C].l_off[L + 1u] == ci[C].l_off[L]
                        || ci[C].r_off[R + 1u] == ci[C].r_off[R])) {
                    ++st->n_keys_gated;
                    continue;
                }
                /* Binary search the exact groups; they are key-ascending. */
                size_t lo = 0, hi = n_exact, found = (size_t)-1;
                while (lo < hi) {
                    size_t mid = (lo + hi) / 2u;
                    if (exact[mid].key == key) { found = mid; break; }
                    if (exact[mid].key < key) lo = mid + 1u; else hi = mid;
                }
                if (found != (size_t)-1) continue;   /* ADDITIVE ONLY */

                uint32_t n = 0;
                /* Two passes so her own audio outranks converted at the same
                 * context rank, matching spfy_vb_groups_prefer_real. */
                for (int real_pass = (is_real ? 0 : 1); real_pass < 2; ++real_pass) {
                    uint32_t nl = ci[C].l_off[L + 1u] - ci[C].l_off[L];
                    uint32_t nr = ci[C].r_off[R + 1u] - ci[C].r_off[R];
                    uint32_t sl = nl ? cur_l[(size_t)C * hp_bound + L] % nl : 0;
                    uint32_t sr = nr ? cur_r[(size_t)C * hp_bound + R] % nr : 0;
                    uint32_t n0 = n;
                    for (uint32_t t = 0; t < nl + nr && n < n_fill; ++t) {
                        /* Interleave so neither context side crowds out the
                         * other when one is far better populated. */
                        uint32_t u; int have = 0;
                        if ((t & 1u) == 0u && (t / 2u) < nl) {
                            u = ci[C].l_uid[ci[C].l_off[L] + (sl + t / 2u) % nl];
                            have = 1;
                        } else if ((t & 1u) == 1u && (t / 2u) < nr) {
                            u = ci[C].r_uid[ci[C].r_off[R] + (sr + t / 2u) % nr];
                            have = 1;
                        }
                        if (!have) continue;
                        if (real_pass == 0 && !is_real[u]) continue;
                        if (bo_take(u, buf, &n, n_fill, seen, withheld, n_units))
                            ++st->n_rank_ctx;
                    }
                    cur_l[(size_t)C * hp_bound + L] += n - n0;
                    cur_r[(size_t)C * hp_bound + R] += n - n0;
                    n0 = n;

                    /* Tier 2: the neighbour is not this phone but is of the
                     * same MANNER, so it coarticulates the same way. This is
                     * what the arbitrary tail was doing badly. */
                    {
                        uint32_t lc = hp_class[L], rcx = hp_class[R];
                        uint32_t ncl = ci[C].lc_off[lc + 1u] - ci[C].lc_off[lc];
                        uint32_t ncr = ci[C].rc_off[rcx + 1u] - ci[C].rc_off[rcx];
                        uint32_t scl = ncl ? cur_a[C] % ncl : 0;
                        uint32_t scr = ncr ? cur_a[C] % ncr : 0;
                        for (uint32_t t = 0; t < ncl + ncr && n < n_fill; ++t) {
                            uint32_t u; int have = 0;
                            if ((t & 1u) == 0u && (t / 2u) < ncl) {
                                u = ci[C].lc_uid[ci[C].lc_off[lc]
                                                 + (scl + t / 2u) % ncl];
                                have = 1;
                            } else if ((t & 1u) == 1u && (t / 2u) < ncr) {
                                u = ci[C].rc_uid[ci[C].rc_off[rcx]
                                                 + (scr + t / 2u) % ncr];
                                have = 1;
                            }
                            if (!have) continue;
                            if (real_pass == 0 && !is_real[u]) continue;
                            if (bo_take(u, buf, &n, n_fill, seen, withheld,
                                        n_units))
                                ++st->n_rank_class;
                        }
                    }
                    /* Advance the shared cursor here too, or a key whose quota
                     * is filled by the class tier alone never moves it and the
                     * next such key takes the identical units. */
                    cur_a[C] += n - n0;
                    n0 = n;

                    uint32_t sa = ci[C].n_all ? cur_a[C] % ci[C].n_all : 0;
                    for (uint32_t t = 0; t < ci[C].n_all && n < n_fill; ++t) {
                        uint32_t u = ci[C].all[(sa + t) % ci[C].n_all];
                        if (real_pass == 0 && !is_real[u]) continue;
                        if (bo_take(u, buf, &n, n_fill, seen, withheld, n_units))
                            ++st->n_rank_any;
                    }
                    cur_a[C] += n - n0;
                }
                for (uint32_t t = 0; t < n; ++t) seen[buf[t]] = 0;
                if (!n) continue;
                if (ng == cap) { spfy_vb_groups_free(g, ng); free(buf);
                                 goto nomem; }
                qsort(buf, n, sizeof *buf, cmp_u32_asc);
                g[ng].key = key;
                g[ng].n   = n;
                g[ng].uid = (uint32_t *)malloc((size_t)n * 4u);
                if (!g[ng].uid) { spfy_vb_groups_free(g, ng); free(buf);
                                  goto nomem; }
                memcpy(g[ng].uid, buf, (size_t)n * 4u);
                ++ng;
                ++st->n_keys_added;
                st->n_listings_added += n;
            }
        }
    }
    free(buf);
    qsort(g, ng, sizeof *g, cmp_group_key);
    for (uint32_t c = 0; c < NC; ++c) {
        free(ci[c].all); free(ci[c].l_off); free(ci[c].l_uid);
        free(ci[c].r_off); free(ci[c].r_uid);
        free(ci[c].lc_off); free(ci[c].lc_uid);
        free(ci[c].rc_off); free(ci[c].rc_uid);
    }
    free(ci); free(seen); free(cur_l); free(cur_r); free(cur_a);
    *out = g; *out_n = ng;
    return SPFY_OK;

nomem:
    for (uint32_t c = 0; c < NC; ++c) {
        free(ci[c].all); free(ci[c].l_off); free(ci[c].l_uid);
        free(ci[c].r_off); free(ci[c].r_uid);
        free(ci[c].lc_off); free(ci[c].lc_uid);
        free(ci[c].rc_off); free(ci[c].rc_uid);
    }
    free(ci); free(seen); free(cur_l); free(cur_r); free(cur_a);
    return SPFY_E_NOMEM;
}

int spfy_vb_groups_merge(const spfy_vb_group *a, size_t na,
                         const spfy_vb_group *b, size_t nb,
                         spfy_vb_group **out, size_t *out_n)
{
    spfy_vb_group *g = (spfy_vb_group *)calloc(na + nb + 1u, sizeof *g);
    if (!g) return SPFY_E_NOMEM;
    size_t ng = 0;
    for (size_t i = 0; i < na; ++i) {
        g[ng].key = a[i].key; g[ng].n = a[i].n;
        g[ng].uid = (uint32_t *)malloc((a[i].n ? a[i].n : 1u) * 4u);
        if (!g[ng].uid) { spfy_vb_groups_free(g, ng); return SPFY_E_NOMEM; }
        memcpy(g[ng].uid, a[i].uid, (size_t)a[i].n * 4u);
        ++ng;
    }
    /* `a` wins a key collision: it holds the exact groups and the wide
     * fallbacks, both of which must survive a merge untouched. */
    for (size_t i = 0; i < nb; ++i) {
        size_t lo = 0, hi = na; int dup = 0;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2u;
            if (a[mid].key == b[i].key) { dup = 1; break; }
            if (a[mid].key < b[i].key) lo = mid + 1u; else hi = mid;
        }
        if (dup) continue;
        g[ng].key = b[i].key; g[ng].n = b[i].n;
        g[ng].uid = (uint32_t *)malloc((b[i].n ? b[i].n : 1u) * 4u);
        if (!g[ng].uid) { spfy_vb_groups_free(g, ng); return SPFY_E_NOMEM; }
        memcpy(g[ng].uid, b[i].uid, (size_t)b[i].n * 4u);
        ++ng;
    }
    qsort(g, ng, sizeof *g, cmp_group_key);
    *out = g; *out_n = ng;
    return SPFY_OK;
}

size_t spfy_vb_groups_prefer_real(spfy_vb_group *g, size_t n,
                                  const uint8_t *is_real, size_t n_uid)
{
    size_t dropped = 0;
    for (size_t i = 0; i < n; ++i) {
        uint32_t keep = 0;
        for (uint32_t k = 0; k < g[i].n; ++k) {
            uint32_t u = g[i].uid[k];
            if (u < n_uid && is_real[u]) ++keep;
        }
        /* ⚠ NEVER EMPTY A GROUP HERE. Unlike the phone allow-list, this group
         * may exist only because of her own audio's context coverage; a group
         * that is entirely converted is one she cannot reach any other way,
         * and emptying it walks the chain to error 7059 and SILENCE. */
        if (!keep || keep == g[i].n) continue;
        uint32_t w = 0;
        for (uint32_t k = 0; k < g[i].n; ++k) {
            uint32_t u = g[i].uid[k];
            if (u < n_uid && is_real[u]) g[i].uid[w++] = u;
        }
        dropped += g[i].n - w;
        g[i].n = w;
    }
    return dropped;
}

/* Wide-group accumulator: a key -> append-ordered uid list. Exact groups are
 * disjoint by construction (a unit has exactly one key), so a wide group is a
 * union of disjoint sets and needs no membership test -- but the vendors'
 * order does have to be reproduced, which is why this appends rather than
 * sorts. */
typedef struct wide_ent {
    uint32_t          key;
    uint32_t         *uid;
    uint32_t          n, cap;
    struct wide_ent  *next;
} wide_ent;

#define WIDE_BUCKETS 8192u

static wide_ent *wide_get(wide_ent **tab, uint32_t key)
{
    uint32_t h = (key * 2654435761u) % WIDE_BUCKETS;
    for (wide_ent *e = tab[h]; e; e = e->next)
        if (e->key == key) return e;
    wide_ent *e = (wide_ent *)calloc(1, sizeof *e);
    if (!e) return NULL;
    e->key = key;
    e->next = tab[h];
    tab[h] = e;
    return e;
}

static int wide_add(wide_ent *e, const uint32_t *uid, uint32_t n)
{
    if (e->n + n > e->cap) {
        uint32_t nc = e->cap ? e->cap : 16u;
        while (nc < e->n + n) nc *= 2u;
        uint32_t *nv = (uint32_t *)realloc(e->uid, (size_t)nc * sizeof *nv);
        if (!nv) return SPFY_E_NOMEM;
        e->uid = nv;
        e->cap = nc;
    }
    memcpy(e->uid + e->n, uid, (size_t)n * sizeof *uid);
    e->n += n;
    return SPFY_OK;
}

static int cmp_group_key(const void *a, const void *b)
{
    const spfy_vb_group *x = (const spfy_vb_group *)a;
    const spfy_vb_group *y = (const spfy_vb_group *)b;
    return x->key < y->key ? -1 : (x->key > y->key);
}

int spfy_vb_with_fallbacks(spfy_vb_group *exact, size_t n_exact,
                           uint32_t hp_bound,
                           spfy_vb_group **out, size_t *out_n)
{
    wide_ent **tab = (wide_ent **)calloc(WIDE_BUCKETS, sizeof *tab);
    if (!tab) return SPFY_E_NOMEM;
    int rc = SPFY_OK;

    /* `exact` is already ascending by key, which is the visit order the
     * vendors' wide-group ordering depends on. */
    for (size_t i = 0; i < n_exact; ++i) {
        uint32_t k = exact[i].key;
        uint32_t l = k / 10000u, c = (k / 100u) % 100u, r = k % 100u;
        if (l >= hp_bound || r >= hp_bound) continue;   /* already wide */
        uint32_t side_key = (c & 1u) ? (hp_bound * 10000u + c * 100u + r)
                                     : (l * 10000u + c * 100u + hp_bound);
        wide_ent *e = wide_get(tab, side_key);
        if (!e) { rc = SPFY_E_NOMEM; goto done; }
        rc = wide_add(e, exact[i].uid, exact[i].n);
        if (rc != SPFY_OK) goto done;

        e = wide_get(tab, hp_bound * 10000u + c * 100u + hp_bound);
        if (!e) { rc = SPFY_E_NOMEM; goto done; }
        rc = wide_add(e, exact[i].uid, exact[i].n);
        if (rc != SPFY_OK) goto done;
    }

    size_t n_wide = 0;
    for (uint32_t h = 0; h < WIDE_BUCKETS; ++h)
        for (wide_ent *e = tab[h]; e; e = e->next) ++n_wide;

    spfy_vb_group *all =
        (spfy_vb_group *)calloc(n_exact + n_wide + 1u, sizeof *all);
    if (!all) { rc = SPFY_E_NOMEM; goto done; }
    size_t n_all = 0;
    for (size_t i = 0; i < n_exact; ++i) {
        all[n_all].key = exact[i].key;
        all[n_all].n = exact[i].n;
        all[n_all].uid = (uint32_t *)malloc((size_t)exact[i].n * sizeof(uint32_t));
        if (!all[n_all].uid) { spfy_vb_groups_free(all, n_all); rc = SPFY_E_NOMEM; goto done; }
        memcpy(all[n_all].uid, exact[i].uid, (size_t)exact[i].n * sizeof(uint32_t));
        ++n_all;
    }
    for (uint32_t h = 0; h < WIDE_BUCKETS; ++h) {
        for (wide_ent *e = tab[h]; e; e = e->next) {
            /* An exact group can legitimately collide with a wide key when a
             * real context uses hp_bound as a sentinel; the exact one wins.
             * `exact` is key-ascending, so this is a binary search -- a
             * linear scan here is 28M comparisons on this corpus. */
            size_t lo = 0, hi = n_exact;
            int dup = 0;
            while (lo < hi) {
                size_t mid = lo + (hi - lo) / 2u;
                if (exact[mid].key == e->key) { dup = 1; break; }
                if (exact[mid].key < e->key) lo = mid + 1u; else hi = mid;
            }
            if (dup) continue;
            all[n_all].key = e->key;
            all[n_all].n = e->n;
            all[n_all].uid = (uint32_t *)malloc((size_t)e->n * sizeof(uint32_t));
            if (!all[n_all].uid) { spfy_vb_groups_free(all, n_all); rc = SPFY_E_NOMEM; goto done; }
            memcpy(all[n_all].uid, e->uid, (size_t)e->n * sizeof(uint32_t));
            ++n_all;
        }
    }
    qsort(all, n_all, sizeof *all, cmp_group_key);
    *out = all;
    *out_n = n_all;
    rc = SPFY_OK;

done:
    for (uint32_t h = 0; h < WIDE_BUCKETS; ++h) {
        wide_ent *e = tab[h];
        while (e) { wide_ent *nx = e->next; free(e->uid); free(e); e = nx; }
    }
    free(tab);
    return rc;
}

int spfy_vb_encode_prsl(const spfy_vb_group *g, size_t n,
                        uint8_t **out, size_t *out_n)
{
    spfy_vb_buf b = {0};
    int rc = spfy_vb_buf_u32(&b, (uint32_t)n);
    for (size_t i = 0; i < n && rc == SPFY_OK; ++i) {
        rc = spfy_vb_buf_u32(&b, g[i].n + 1u);
        if (rc == SPFY_OK) rc = spfy_vb_buf_u32(&b, g[i].key);
        for (uint32_t k = 0; k < g[i].n && rc == SPFY_OK; ++k)
            rc = spfy_vb_buf_u32(&b, g[i].uid[k]);
    }
    if (rc != SPFY_OK) { spfy_vb_buf_free(&b); return rc; }
    *out = b.p;
    *out_n = b.n;
    return SPFY_OK;
}

void spfy_vb_groups_free(spfy_vb_group *g, size_t n)
{
    for (size_t i = 0; i < n; ++i) free(g[i].uid);
    free(g);
}

/* ====================================================================== */
/* ckls / cklx                                                             */

int spfy_vb_anchors_push(spfy_vb_anchors *a, const char *text,
                         uint32_t ss, uint32_t se, const char *file)
{
    if (a->n == a->cap) {
        size_t nc = a->cap ? a->cap * 2u : 1024u;
        spfy_vb_anchor *nv = (spfy_vb_anchor *)realloc(a->v, nc * sizeof *nv);
        if (!nv) return SPFY_E_NOMEM;
        a->v = nv;
        a->cap = nc;
    }
    size_t tn = strlen(text), fn = strlen(file);
    char *t = (char *)malloc(tn + 1u);
    char *f = (char *)malloc(fn + 1u);
    if (!t || !f) { free(t); free(f); return SPFY_E_NOMEM; }
    memcpy(t, text, tn + 1u);
    memcpy(f, file, fn + 1u);
    a->v[a->n].text = t;
    a->v[a->n].file = f;
    a->v[a->n].span_start = ss;
    a->v[a->n].span_end = se;
    ++a->n;
    return SPFY_OK;
}

void spfy_vb_anchors_free(spfy_vb_anchors *a)
{
    for (size_t i = 0; i < a->n; ++i) { free(a->v[i].text); free(a->v[i].file); }
    free(a->v);
    a->v = NULL;
    a->n = a->cap = 0;
}

size_t spfy_vb_anchors_filter(spfy_vb_anchors *a, const uint8_t *gated,
                              size_t n_uid)
{
    size_t w = 0, dropped = 0;
    for (size_t i = 0; i < a->n; ++i) {
        uint32_t ss = a->v[i].span_start, se = a->v[i].span_end;
        int bad = 0;
        for (uint32_t u = ss; u <= se && !bad; ++u)
            if (u < n_uid && gated[u]) bad = 1;
        if (bad) {
            free(a->v[i].text);
            free(a->v[i].file);
            ++dropped;
            continue;
        }
        a->v[w++] = a->v[i];
    }
    a->n = w;
    return dropped;
}

static int cmp_strptr(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* ⛔ AN ALLOW-LIST OF REAL, NOT A DENY-LIST OF SYNTHETIC.
 *
 * This used to answer "does the stem start with rvc_ or st2_". Every synthetic
 * source added since -- `genph_` (Brown renders), `place_` (place-name
 * carriers) -- therefore counted as the speaker's OWN AUDIO, and
 * --rvc-policy prefer-real silently protected nothing. It fails quietly: the
 * build log reports "converted (rvc_*): 0 of N recordings" and looks correct.
 *
 * Measured consequence, crstom 2026-08-22: 125 carrier lines carried a
 * StyleTTS2 rendition of "the national weather service" stretched to 1.15x, and
 * the selector preferred them over the real recordings. The phrase came out
 * audibly wrong in a build that verified 46/0.
 *
 * A REAL recording is named <office>_<timestamp>_<product>: three lowercase
 * letters, underscore, then at least eight digits (akq_20041105142049_WBCEFPSBY,
 * psr_20041101200020_pil=PHXHWRNW1). Anything that does not match that shape did
 * not come off a broadcast feed, so it is synthetic -- INCLUDING a source nobody
 * has invented yet, which is the point of inverting the test.
 */
int spfy_vb_stem_is_synth(const char *stem)
{
    if (!stem) return 1;
    size_t i;
    for (i = 0; i < 3u; ++i)
        if (stem[i] < 'a' || stem[i] > 'z') return 1;
    if (stem[3] != '_') return 1;
    size_t digits = 0;
    for (i = 4u; stem[i] >= '0' && stem[i] <= '9'; ++i) ++digits;
    return digits < 8u;
}

/* ⭐ PREFER-REAL, AT THE ANCHOR LEVEL.
 *
 * --rvc-policy prefer-real edits the prsl POOLS, and anchors do not go through
 * them: spfy_synth.c:5104 expands a winning anchor across its whole
 * half-phone span and OVERWRITES the picks the DP made. So without this, a
 * synthetic whole word can beat her real one even under prefer-real, and the
 * words most exposed are the commonest -- the st2 corpus holds 1,888 "the"
 * against her 1,282.
 *
 * Same rule as the pools, one level up: a synthetic anchor survives only for a
 * token her own recordings cannot say. That keeps the 419 tokens ONLY the
 * renders have -- `tornado`, `townships`, `coastal`, the severe-weather
 * register she was never recorded speaking -- and drops the 272 she already
 * covers. Comparison is on the anchor TEXT, which is the token identity the
 * engine matches on. */
size_t spfy_vb_anchors_prefer_real(spfy_vb_anchors *a)
{
    /* Tokens her OWN recordings provide, as a plain sorted list of pointers.
     * n is tens of thousands, so an O(n log n) build plus bsearch is ample and
     * avoids a hash table this file does not otherwise need. */
    size_t n_real = 0;
    for (size_t i = 0; i < a->n; ++i)
        if (!spfy_vb_stem_is_synth(a->v[i].file)) ++n_real;
    if (!n_real) return 0;

    const char **real = (const char **)malloc(n_real * sizeof *real);
    if (!real) return 0;
    size_t m = 0;
    for (size_t i = 0; i < a->n; ++i)
        if (!spfy_vb_stem_is_synth(a->v[i].file)) real[m++] = a->v[i].text;
    qsort(real, m, sizeof *real, cmp_strptr);

    size_t w = 0, dropped = 0;
    for (size_t i = 0; i < a->n; ++i) {
        if (spfy_vb_stem_is_synth(a->v[i].file)
            && bsearch(&a->v[i].text, real, m, sizeof *real, cmp_strptr)) {
            free(a->v[i].text);
            free(a->v[i].file);
            ++dropped;
            continue;
        }
        a->v[w++] = a->v[i];
    }
    a->n = w;
    free(real);
    return dropped;
}

/* ⛔ DROP EVERY SYNTHETIC ANCHOR IN THIS LIST.
 *
 * Used for the _SYL_ list by default, and here is why the two lists differ.
 * A _WORD_ anchor plays a whole word, which is a coherent unit of
 * pronunciation from one render. A _SYL_ anchor plays a FRAGMENT, and the
 * engine will splice it into a DIFFERENT word: measured on nws_warning, the
 * word "body" was served by uids 424604..424613 -- a contiguous run from
 * `st2_wxa_0173`, whose text is "Seek shelter now on the lowest floor of a
 * sturdy building anywhere in the metro area" and contains no "body" at all.
 * A syllable of "sturdy" was pasted into the middle of it, and the user heard
 * that word acquire "a slight British inflection".
 *
 * Her own syllable anchors do not have this problem: splicing her syllables
 * into her words keeps one speaker. The defect is CROSS-SOURCE fragment
 * splicing, so the fix is per source, not per anchor type. */
size_t spfy_vb_anchors_drop_synth(spfy_vb_anchors *a)
{
    size_t w = 0, dropped = 0;
    for (size_t i = 0; i < a->n; ++i) {
        if (spfy_vb_stem_is_synth(a->v[i].file)) {
            free(a->v[i].text);
            free(a->v[i].file);
            ++dropped;
            continue;
        }
        a->v[w++] = a->v[i];
    }
    a->n = w;
    return dropped;
}

static int encode_ckls_group(spfy_vb_buf *b, const char *name,
                             const spfy_vb_anchors *a)
{
    int rc = spfy_vb_buf_pstr(b, name);
    if (rc == SPFY_OK) rc = spfy_vb_buf_u32(b, (uint32_t)a->n);
    /* ⛔ AN EMPTY GROUP WRITES NO SEQUENCE INDEX.
     * Each record is really {u32 seq_index, text, span_start, span_end, file},
     * so this leading word is record 0's index and the trailing word below is
     * record i+1's -- which is why the last record has none. Emitting it
     * unconditionally desynchronises a reader by 4 bytes on an empty group:
     * it then reads a zero name length and rejects the whole chunk table.
     * felix ships an empty `_WORD_` group and omits the field, so this is the
     * vendor's own rule, not an inference. Reachable here on defaults --
     * `--syn-anchors word` drops every synthetic syllable anchor, so an
     * all-synthetic corpus empties `syl`. */
    if (rc == SPFY_OK && a->n) rc = spfy_vb_buf_u32(b, 0);
    for (size_t i = 0; i < a->n && rc == SPFY_OK; ++i) {
        rc = spfy_vb_buf_pstr(b, a->v[i].text);
        if (rc == SPFY_OK) rc = spfy_vb_buf_u32(b, a->v[i].span_start);
        if (rc == SPFY_OK) rc = spfy_vb_buf_u32(b, a->v[i].span_end);
        if (rc == SPFY_OK) rc = spfy_vb_buf_pstr(b, a->v[i].file);
        if (rc == SPFY_OK && i + 1u < a->n)
            rc = spfy_vb_buf_u32(b, (uint32_t)(i + 1u));
    }
    return rc;
}

int spfy_vb_encode_ckls(const spfy_vb_anchors *word, const spfy_vb_anchors *syl,
                        uint8_t **out, size_t *out_n)
{
    spfy_vb_buf b = {0};
    int rc = spfy_vb_buf_u32(&b, 2);
    if (rc == SPFY_OK) rc = encode_ckls_group(&b, "_WORD_", word);
    if (rc == SPFY_OK) rc = encode_ckls_group(&b, "_SYL_", syl);
    if (rc != SPFY_OK) { spfy_vb_buf_free(&b); return rc; }
    *out = b.p;
    *out_n = b.n;
    return SPFY_OK;
}

/* Postings by token text. Python builds `{text: [indices]}` then writes the
 * keys with `sorted()`, i.e. code-point order -- which for latin-1 is plain
 * byte order with a shorter prefix first. */
typedef struct post_ent {
    char            *key;
    uint32_t        *id;
    uint32_t         n, cap;
    struct post_ent *next;
} post_ent;

#define POST_BUCKETS 65536u

static uint32_t str_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; ++s) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}

static int cmp_post(const void *a, const void *b)
{
    const post_ent *const *x = (const post_ent *const *)a;
    const post_ent *const *y = (const post_ent *const *)b;
    return strcmp((*x)->key, (*y)->key);
}

static int encode_cklx_group(spfy_vb_buf *b, const char *name,
                             const spfy_vb_anchors *a)
{
    post_ent **tab = (post_ent **)calloc(POST_BUCKETS, sizeof *tab);
    if (!tab) return SPFY_E_NOMEM;
    int rc = SPFY_OK;
    size_t n_keys = 0;

    for (size_t i = 0; i < a->n; ++i) {
        const char *k = a->v[i].text;
        uint32_t h = str_hash(k) % POST_BUCKETS;
        post_ent *e = NULL;
        for (post_ent *q = tab[h]; q; q = q->next)
            if (strcmp(q->key, k) == 0) { e = q; break; }
        if (!e) {
            e = (post_ent *)calloc(1, sizeof *e);
            if (!e) { rc = SPFY_E_NOMEM; goto done; }
            size_t kn = strlen(k);
            e->key = (char *)malloc(kn + 1u);
            if (!e->key) { free(e); rc = SPFY_E_NOMEM; goto done; }
            memcpy(e->key, k, kn + 1u);
            e->next = tab[h];
            tab[h] = e;
            ++n_keys;
        }
        if (e->n == e->cap) {
            uint32_t nc = e->cap ? e->cap * 2u : 4u;
            uint32_t *nv = (uint32_t *)realloc(e->id, (size_t)nc * sizeof *nv);
            if (!nv) { rc = SPFY_E_NOMEM; goto done; }
            e->id = nv;
            e->cap = nc;
        }
        e->id[e->n++] = (uint32_t)i;
    }

    {
        post_ent **flat = (post_ent **)malloc((n_keys ? n_keys : 1u) * sizeof *flat);
        if (!flat) { rc = SPFY_E_NOMEM; goto done; }
        size_t m = 0;
        for (uint32_t h = 0; h < POST_BUCKETS; ++h)
            for (post_ent *e = tab[h]; e; e = e->next) flat[m++] = e;
        if (m > 1) qsort(flat, m, sizeof *flat, cmp_post);

        rc = spfy_vb_buf_pstr(b, name);
        if (rc == SPFY_OK) rc = spfy_vb_buf_u32(b, (uint32_t)m);
        for (size_t i = 0; i < m && rc == SPFY_OK; ++i) {
            rc = spfy_vb_buf_pstr(b, flat[i]->key);
            if (rc == SPFY_OK) rc = spfy_vb_buf_u32(b, flat[i]->n);
            for (uint32_t k = 0; k < flat[i]->n && rc == SPFY_OK; ++k)
                rc = spfy_vb_buf_u32(b, flat[i]->id[k]);
        }
        free(flat);
    }

done:
    for (uint32_t h = 0; h < POST_BUCKETS; ++h) {
        post_ent *e = tab[h];
        while (e) { post_ent *nx = e->next; free(e->key); free(e->id); free(e); e = nx; }
    }
    free(tab);
    return rc;
}

int spfy_vb_encode_cklx(const spfy_vb_anchors *word, const spfy_vb_anchors *syl,
                        uint8_t **out, size_t *out_n)
{
    spfy_vb_buf b = {0};
    int rc = spfy_vb_buf_u32(&b, 2);
    if (rc == SPFY_OK) rc = encode_cklx_group(&b, "_WORD_", word);
    if (rc == SPFY_OK) rc = encode_cklx_group(&b, "_SYL_", syl);
    if (rc != SPFY_OK) { spfy_vb_buf_free(&b); return rc; }
    *out = b.p;
    *out_n = b.n;
    return SPFY_OK;
}

/* ====================================================================== */
/* mean / indx                                                             */

int spfy_vb_encode_mean(const double rows[][8], size_t n_rows,
                        uint8_t **out, size_t *out_n)
{
    spfy_vb_buf b = {0};
    int rc = spfy_vb_buf_u32(&b, (uint32_t)n_rows);
    if (rc == SPFY_OK) rc = spfy_vb_buf_u32(&b, 8);
    for (size_t i = 0; i < n_rows && rc == SPFY_OK; ++i)
        for (int k = 0; k < 8 && rc == SPFY_OK; ++k)
            rc = spfy_vb_buf_f32(&b, (float)rows[i][k]);
    if (rc != SPFY_OK) { spfy_vb_buf_free(&b); return rc; }
    *out = b.p;
    *out_n = b.n;
    return SPFY_OK;
}

int spfy_vb_encode_indx(const spfy_vb_indx_ent *e, size_t n,
                        uint8_t **out, size_t *out_n)
{
    spfy_vb_buf b = {0};
    int rc = spfy_vb_buf_u32(&b, (uint32_t)n);
    for (size_t i = 0; i < n && rc == SPFY_OK; ++i) {
        rc = spfy_vb_buf_u32(&b, e[i].off);
        if (rc == SPFY_OK) rc = spfy_vb_buf_pstr(&b, e[i].name ? e[i].name : "");
    }
    if (rc != SPFY_OK) { spfy_vb_buf_free(&b); return rc; }
    *out = b.p;
    *out_n = b.n;
    return SPFY_OK;
}

int spfy_vb_encode_vers(const char *version, uint8_t **out, size_t *out_n)
{
    spfy_vb_buf b = {0};
    int rc = spfy_vb_buf_pstr(&b, version ? version : "3.0.0.0");
    if (rc != SPFY_OK) { spfy_vb_buf_free(&b); return rc; }
    *out = b.p;
    *out_n = b.n;
    return SPFY_OK;
}

/* <tag><u32 len><bytes incl. NUL><pad to even> */
static int info_rec(spfy_vb_buf *b, const char *tag, const char *val)
{
    size_t n = strlen(val) + 1u;
    int rc = spfy_vb_buf_put(b, tag, 4);
    if (rc == SPFY_OK) rc = spfy_vb_buf_u32(b, (uint32_t)n);
    if (rc == SPFY_OK) rc = spfy_vb_buf_put(b, val, n);
    if (rc == SPFY_OK && (n & 1u)) rc = spfy_vb_buf_u8(b, 0);
    return rc;
}

int spfy_vb_encode_list(const char *copyright, const char *date_iso,
                        uint8_t **out, size_t *out_n)
{
    spfy_vb_buf b = {0};
    int rc = spfy_vb_buf_put(&b, "INFO", 4);
    if (rc == SPFY_OK && copyright && *copyright)
        rc = info_rec(&b, "ICOP", copyright);
    if (rc == SPFY_OK && date_iso && *date_iso)
        rc = info_rec(&b, "ICRD", date_iso);
    if (rc != SPFY_OK) { spfy_vb_buf_free(&b); return rc; }
    *out = b.p;
    *out_n = b.n;
    return SPFY_OK;
}

int spfy_vb_encode_fmt(uint32_t sample_rate, uint16_t channels,
                       uint8_t **out, size_t *out_n)
{
    spfy_vb_buf b = {0};
    if (!channels) channels = 1u;
    /* See the header: the trailing three fields describe the DECODED 16-bit
     * stream, which is what both vendors write. */
    int rc = spfy_vb_buf_u16(&b, 7u);                    /* WAVE_FORMAT_MULAW */
    if (rc == SPFY_OK) rc = spfy_vb_buf_u16(&b, channels);
    if (rc == SPFY_OK) rc = spfy_vb_buf_u32(&b, sample_rate);
    if (rc == SPFY_OK) rc = spfy_vb_buf_u32(&b, sample_rate * channels * 2u);
    if (rc == SPFY_OK) rc = spfy_vb_buf_u16(&b, (uint16_t)(channels * 2u));
    if (rc == SPFY_OK) rc = spfy_vb_buf_u16(&b, 16u);
    if (rc != SPFY_OK) { spfy_vb_buf_free(&b); return rc; }
    *out = b.p;
    *out_n = b.n;
    return SPFY_OK;
}

int spfy_vb_encode_hist(const uint32_t *counts, uint32_t n, int32_t sub_off,
                        double floor_cost, uint8_t **out, size_t *out_n)
{
    if (!counts || !n) return SPFY_E_INVAL;

    /* Cost is -log(p / p_max), so the modal step is free and the curve rises
     * away from it. Both vendors are exactly 0.0 at the centre bin, which is
     * what a p_max normalisation gives and an unnormalised -log(p) does not. */
    uint32_t peak = 0;
    for (uint32_t i = 0; i < n; ++i) if (counts[i] > peak) peak = counts[i];
    if (!peak) return SPFY_E_INVAL;

    spfy_vb_buf b = {0};
    int rc = spfy_vb_buf_put(&b, "head", 4);
    if (rc == SPFY_OK) rc = spfy_vb_buf_u32(&b, 8u);
    if (rc == SPFY_OK) rc = spfy_vb_buf_u32(&b, n);
    if (rc == SPFY_OK) rc = spfy_vb_buf_u32(&b, (uint32_t)sub_off);
    if (rc == SPFY_OK) rc = spfy_vb_buf_put(&b, "data", 4);
    if (rc == SPFY_OK) rc = spfy_vb_buf_u32(&b, n * 4u);
    for (uint32_t i = 0; i < n && rc == SPFY_OK; ++i) {
        double c = counts[i]
                 ? -log((double)counts[i] / (double)peak)
                 : floor_cost;
        if (c > floor_cost) c = floor_cost;
        /* -log(1.0) is NEGATIVE zero, and !(c > 0) catches that and NaN where
         * `c < 0` catches neither. */
        if (!(c > 0.0)) c = 0.0;
        rc = spfy_vb_buf_f32(&b, (float)c);
    }
    if (rc != SPFY_OK) { spfy_vb_buf_free(&b); return rc; }
    *out = b.p;
    *out_n = b.n;
    return SPFY_OK;
}
