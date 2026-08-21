#include "vb_wagon.h"

#include "../../include/spfy/spfy.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#define WGN_HUGE 1.0e20

int spfy_wgn_ask(const spfy_wgn_ques *q, const uint32_t *feat)
{
    if (q->key >= SPFY_WGN_MAX_FEAT) return 0;
    uint32_t v = feat[q->key];
    for (uint32_t i = 0; i < q->n; ++i)
        if (q->val[i] == v) return 1;
    return 0;
}

/* WImpurity::measure() for a float predictee: variance * samples. Kept as the
 * raw sums so a split can be scored in one pass. */
static double impurity(double sum, double sum2, size_t n)
{
    if (!n) return 0.0;
    double mean = sum / (double)n;
    double var = sum2 / (double)n - mean * mean;
    if (var < 0.0) var = 0.0;               /* rounding, on a constant leaf */
    return var * (double)n;
}

static void stats(const spfy_wgn_sample *s, size_t n, float *mean, float *var)
{
    double sum = 0.0, sum2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        sum += s[i].y;
        sum2 += (double)s[i].y * s[i].y;
    }
    double m = n ? sum / (double)n : 0.0;
    double v = n ? sum2 / (double)n - m * m : 0.0;
    if (v < 0.0) v = 0.0;
    *mean = (float)m;
    *var = (float)v;
}

static int grow(spfy_wgn_sample *s, size_t n,
                const spfy_wgn_ques *q, size_t n_q,
                const spfy_wgn_cfg *cfg, spfy_wgn_node **out, int depth)
{
    spfy_wgn_node *node = (spfy_wgn_node *)calloc(1, sizeof *node);
    if (!node) return SPFY_E_NOMEM;
    node->is_leaf = 1;
    node->n = (uint32_t)n;
    stats(s, n, &node->mean, &node->var);
    *out = node;

    /* A depth bound is not in wagon; it is here because a corpus with a
     * degenerate feature can otherwise recurse once per sample, and the
     * on-disk node index is a u32 shared with the child pointers. */
    if (depth > 64 || n < 2u * (size_t)cfg->min_cluster) return SPFY_OK;

    double node_imp = impurity(0, 0, 0);
    {
        double sum = 0.0, sum2 = 0.0;
        for (size_t i = 0; i < n; ++i) { sum += s[i].y; sum2 += (double)s[i].y * s[i].y; }
        node_imp = impurity(sum, sum2, n);
    }

    /* score_question_set: min_cluster is relaxed by wgn_balance when the node
     * is large, exactly as in wagon.cc. */
    size_t min_cluster = cfg->min_cluster;
    if (cfg->balance > 0.0f) {
        size_t b = (size_t)((double)n / (double)cfg->balance);
        if (b >= min_cluster) min_cluster = b;
    }

    double best = WGN_HUGE;
    size_t best_qi = (size_t)-1;
    for (size_t k = 0; k < n_q; ++k) {
        double ys = 0.0, ys2 = 0.0, ns = 0.0, ns2 = 0.0;
        size_t ny = 0, nn = 0;
        for (size_t i = 0; i < n; ++i) {
            double y = s[i].y;
            if (spfy_wgn_ask(&q[k], s[i].feat)) {
                ys += y; ys2 += y * y; ++ny;
            } else {
                ns += y; ns2 += y * y; ++nn;
            }
        }
        if (ny < min_cluster || nn < min_cluster) continue;
        double sc = (impurity(ys, ys2, ny) + impurity(ns, ns2, nn)) / 2.0;
        if (sc < best) { best = sc; best_qi = k; }
    }
    if (best_qi == (size_t)-1 || !(best < node_imp)) return SPFY_OK;

    /* Partition in place, stably, so a rebuild on the same corpus gives the
     * same tree. */
    spfy_wgn_sample *tmp = (spfy_wgn_sample *)malloc(n * sizeof *tmp);
    if (!tmp) return SPFY_E_NOMEM;
    size_t ny = 0;
    for (size_t i = 0; i < n; ++i)
        if (spfy_wgn_ask(&q[best_qi], s[i].feat)) tmp[ny++] = s[i];
    size_t nn = ny;
    for (size_t i = 0; i < n; ++i)
        if (!spfy_wgn_ask(&q[best_qi], s[i].feat)) tmp[nn++] = s[i];
    memcpy(s, tmp, n * sizeof *tmp);
    free(tmp);

    node->is_leaf = 0;
    node->qi = (uint32_t)best_qi;
    int rc = grow(s, ny, q, n_q, cfg, &node->yes, depth + 1);
    if (rc != SPFY_OK) return rc;
    return grow(s + ny, n - ny, q, n_q, cfg, &node->no, depth + 1);
}

int spfy_wgn_grow(spfy_wgn_sample *s, size_t n,
                  const spfy_wgn_ques *q, size_t n_q,
                  const spfy_wgn_cfg *cfg, spfy_wgn_node **out)
{
    if (!s || !q || !cfg || !out) return SPFY_E_INVAL;
    *out = NULL;
    return grow(s, n, q, n_q, cfg, out, 0);
}

void spfy_wgn_free(spfy_wgn_node *n)
{
    if (!n) return;
    spfy_wgn_free(n->yes);
    spfy_wgn_free(n->no);
    free(n);
}

size_t spfy_wgn_nodes(const spfy_wgn_node *n)
{
    if (!n) return 0;
    return 1u + spfy_wgn_nodes(n->yes) + spfy_wgn_nodes(n->no);
}

size_t spfy_wgn_leaves(const spfy_wgn_node *n)
{
    if (!n) return 0;
    if (n->is_leaf) return 1;
    return spfy_wgn_leaves(n->yes) + spfy_wgn_leaves(n->no);
}

float spfy_wgn_predict(const spfy_wgn_node *root, const spfy_wgn_ques *q,
                       const uint32_t *feat, float *var)
{
    const spfy_wgn_node *n = root;
    while (n && !n->is_leaf)
        n = spfy_wgn_ask(&q[n->qi], feat) ? n->yes : n->no;
    if (!n) { if (var) *var = 0.0f; return 0.0f; }
    if (var) *var = n->var;
    return n->mean;
}

/* ====================================================================== */
/* On-disk trees.                                                          */

int spfy_wgn_tree_parse(const uint8_t *d, size_t dn, spfy_wgn_tree *out)
{
    if (!d || !out || dn < 4) return SPFY_E_FORMAT;
    uint32_t n = rd_u32(d);
    spfy_wgn_rec *v = (spfy_wgn_rec *)calloc(n ? n : 1u, sizeof *v);
    if (!v) return SPFY_E_NOMEM;
    size_t off = 4;
    for (uint32_t i = 0; i < n; ++i) {
        if (off + 8 > dn) { free(v); return SPFY_E_FORMAT; }
        v[i].idx = rd_u32(d + off);
        int32_t yc = (int32_t)rd_u32(d + off + 4);
        off += 8;
        if (yc >= 0) {
            if (off + 8 > dn) { free(v); return SPFY_E_FORMAT; }
            v[i].is_leaf = 0;
            v[i].yes = yc;
            v[i].no  = rd_u32(d + off);
            v[i].qi  = rd_u32(d + off + 4);
            off += 8;
        } else {
            if (off + 12 > dn) { free(v); return SPFY_E_FORMAT; }
            v[i].is_leaf = 1;
            v[i].yes = yc;
            v[i].no  = rd_u32(d + off);         /* the second -1 */
            memcpy(&v[i].mean, d + off + 4, 4);
            memcpy(&v[i].var,  d + off + 8, 4);
            off += 12;
        }
    }
    if (off != dn) { free(v); return SPFY_E_FORMAT; }
    out->n = v;
    out->n_nodes = n;
    return SPFY_OK;
}

int spfy_wgn_tree_write(const spfy_wgn_tree *t, spfy_vb_buf *b)
{
    int rc = spfy_vb_buf_u32(b, (uint32_t)t->n_nodes);
    for (size_t i = 0; i < t->n_nodes && rc == SPFY_OK; ++i) {
        const spfy_wgn_rec *r = &t->n[i];
        rc = spfy_vb_buf_u32(b, r->idx);
        if (rc == SPFY_OK) rc = spfy_vb_buf_u32(b, (uint32_t)r->yes);
        if (rc != SPFY_OK) break;
        if (r->is_leaf) {
            rc = spfy_vb_buf_u32(b, r->no);
            if (rc == SPFY_OK) rc = spfy_vb_buf_f32(b, r->mean);
            if (rc == SPFY_OK) rc = spfy_vb_buf_f32(b, r->var);
        } else {
            rc = spfy_vb_buf_u32(b, r->no);
            if (rc == SPFY_OK) rc = spfy_vb_buf_u32(b, r->qi);
        }
    }
    return rc;
}

void spfy_wgn_tree_free(spfy_wgn_tree *t)
{
    if (!t) return;
    free(t->n);
    t->n = NULL;
    t->n_nodes = 0;
}

/* ⛔ THE LEAF'S SECOND FLOAT IS 1/sd, NOT A VARIANCE.
 *
 * The engine scores `delta = (f0_context - leaf_mean) * leaf_var` and squares
 * it (anchor_score.c:1068), which is a z-score only if the stored field is a
 * PRECISION. Measured on both vendors, and the arithmetic closes:
 *
 *              stored median   1/that   the tree's own held-out RMSE
 *   jill durt     0.0482        20.7            21.18
 *   jill f0tr     0.2055         4.87            5.46
 *   tom  durt     0.0515        19.4            19.12
 *   tom  f0tr     0.2091         4.78            4.81
 *
 * tom's durt maximum is exactly 1.0000, so the value is clamped at an sd of
 * 1.0 rather than being allowed to blow up on a near-constant leaf.
 *
 * ⚠ Writing wagon's raw variance here instead -- around 400 for durt -- makes
 * every duration cost about 10^6 times too large. Nothing downstream reports
 * it: the container verifies, the trees round-trip, and the voice simply
 * selects on duration and nothing else. */
static float leaf_precision(float var)
{
    double sd = var > 0.0f ? sqrt((double)var) : 0.0;
    if (!(sd > 1.0)) return 1.0f;
    return (float)(1.0 / sd);
}

void spfy_wgn_flatten(const spfy_wgn_node *n, spfy_wgn_rec *v,
                      uint32_t *next, uint32_t self)
{
    v[self].idx = self;
    if (n->is_leaf) {
        v[self].is_leaf = 1;
        v[self].yes = -1;
        v[self].no  = 0xFFFFFFFFu;
        v[self].mean = n->mean;
        v[self].var  = leaf_precision(n->var);
        return;
    }
    uint32_t y  = (*next)++;
    uint32_t no = (*next)++;
    v[self].is_leaf = 0;
    v[self].yes = (int32_t)y;
    v[self].no  = no;
    v[self].qi  = n->qi;
    spfy_wgn_flatten(n->yes, v, next, y);
    spfy_wgn_flatten(n->no,  v, next, no);
}

/* ====================================================================== */
/* Question sets.                                                          */

int spfy_wgn_qset_parse(const uint8_t *d, size_t n, spfy_wgn_qset *out)
{
    if (!d || !out || n < 4) return SPFY_E_FORMAT;
    memset(out, 0, sizeof *out);
    uint32_t cnt = rd_u32(d);
    size_t total = 0, off = 4;
    for (uint32_t i = 0; i < cnt; ++i) {
        if (off + 5 > n) return SPFY_E_FORMAT;
        uint32_t nv = rd_u32(d + off + 1);
        off += 5 + (size_t)nv * 4u;
        if (off > n) return SPFY_E_FORMAT;
        total += nv;
    }
    out->q = (spfy_wgn_ques *)calloc(cnt ? cnt : 1u, sizeof *out->q);
    out->store = (uint32_t *)calloc(total ? total : 1u, sizeof *out->store);
    if (!out->q || !out->store) { spfy_wgn_qset_free(out); return SPFY_E_NOMEM; }
    out->n = cnt;
    off = 4;
    size_t sp = 0;
    for (uint32_t i = 0; i < cnt; ++i) {
        out->q[i].key = d[off];
        uint32_t nv = rd_u32(d + off + 1);
        off += 5;
        out->q[i].n = nv;
        out->q[i].val = out->store + sp;
        for (uint32_t k = 0; k < nv; ++k)
            out->store[sp + k] = rd_u32(d + off + k * 4u);
        sp += nv;
        off += (size_t)nv * 4u;
    }
    return SPFY_OK;
}

int spfy_wgn_qset_write(const spfy_wgn_qset *s, spfy_vb_buf *b)
{
    int rc = spfy_vb_buf_u32(b, (uint32_t)s->n);
    for (size_t i = 0; i < s->n && rc == SPFY_OK; ++i) {
        rc = spfy_vb_buf_u8(b, s->q[i].key);
        if (rc == SPFY_OK) rc = spfy_vb_buf_u32(b, s->q[i].n);
        for (uint32_t k = 0; k < s->q[i].n && rc == SPFY_OK; ++k)
            rc = spfy_vb_buf_u32(b, s->q[i].val[k]);
    }
    return rc;
}

void spfy_wgn_qset_free(spfy_wgn_qset *s)
{
    if (!s) return;
    free(s->q);
    free(s->store);
    memset(s, 0, sizeof *s);
}

int spfy_wgn_qset_has(const spfy_wgn_qset *set, const spfy_wgn_ques *q)
{
    for (size_t i = 0; i < set->n; ++i) {
        if (set->q[i].key != q->key || set->q[i].n != q->n) continue;
        uint32_t hit = 0;
        for (uint32_t a = 0; a < q->n; ++a)
            for (uint32_t b = 0; b < set->q[i].n; ++b)
                if (set->q[i].val[b] == q->val[a]) { ++hit; break; }
        if (hit == q->n) return 1;
    }
    return 0;
}

/* ---- our own inventory ------------------------------------------------ */

/* The eleven classes, as ARPABET phonetics. Every one of them appears in both
 * vendors' durt; they are written here from the phonetics rather than lifted
 * from a donor's bytes, and --check-ques proves the containment. */
static const char *const CLS_VOWEL_SUB1[] = { "ae", "ay", "ey", "oy", NULL };
static const char *const CLS_VOWEL_SUB2[] = { "aw", "ao", "ow", NULL };
static const char *const CLS_VOWEL_FULL[] = {
    "aa", "ae", "ah", "ao", "aw", "ay", "eh", "ey", "ih", "iy", "ow", "oy",
    "uh", "uw", NULL };
static const char *const CLS_VOWEL_SYL[] = {
    "aa", "ae", "ah", "ao", "aw", "ay", "eh", "ey", "ih", "iy", "ow", "oy",
    "uh", "uw", "ix", "ax", "el", "er", "en", NULL };
static const char *const CLS_VOICED[] = {
    "aa", "ae", "ah", "ao", "aw", "ay", "eh", "ey", "ih", "iy", "ow", "oy",
    "uh", "uw", "ix", "ax", "el", "er", "en", "w", "y", "r", "l", "m", "n",
    "ng", "z", "zh", "b", "d", "dx", "g", "dh", NULL };
static const char *const CLS_REDUCED[]   = { "ix", "ax", NULL };
static const char *const CLS_NASAL[]     = { "m", "n", "ng", "en", NULL };
static const char *const CLS_STOP_VD[]   = { "b", "d", "g", "dh", "dx", NULL };
static const char *const CLS_FRIC_VL[]   = { "s", "f", "sh", NULL };
static const char *const CLS_FRIC_VD[]   = { "z", "zh", NULL };
static const char *const CLS_STOP_VL[]   = { "p", "t", "k", NULL };
static const char *const CLS_STOP_VL_H[] = { "p", "t", "k", "hh", NULL };

static const char *const *const CLASSES[] = {
    CLS_VOWEL_SUB1, CLS_VOWEL_SUB2, CLS_VOWEL_FULL, CLS_VOWEL_SYL,
    CLS_VOICED, CLS_REDUCED, CLS_NASAL, CLS_STOP_VD, CLS_FRIC_VL,
    CLS_FRIC_VD, CLS_STOP_VL, CLS_STOP_VL_H
};
#define N_CLASSES (sizeof CLASSES / sizeof CLASSES[0])

/* The engine's own CART walker vocabulary, FUN_08e87c90 (spfy_synth.c:1156).
 * q7 is live for f0tr only and q3/q4/q5/q9 for durt only, but the inventory
 * is shared -- the vendors ship one `ques` per chunk holding both, and a
 * question on a clamped key simply never fires. */
enum { Q_SYL_TYPE = 1, Q_SYL_IN_PHRASE = 2, Q_LEFT = 3, Q_RIGHT = 4,
       Q_HP_IN_SYL = 5, Q_WORD_IN_PHRASE = 7, Q_SYL_IN_WORD = 8,
       Q_PHONE_IN_SYL = 9 };

typedef struct {
    spfy_wgn_qset *s;
    size_t         cap_q, cap_v, n_v;
    int            rc;
} qbuild;

static void qb_add(qbuild *b, uint8_t key, const uint32_t *val, uint32_t n)
{
    if (b->rc != SPFY_OK || !n) return;
    if (b->s->n == b->cap_q) {
        size_t c = b->cap_q ? b->cap_q * 2u : 64u;
        spfy_wgn_ques *p = (spfy_wgn_ques *)realloc(b->s->q, c * sizeof *p);
        if (!p) { b->rc = SPFY_E_NOMEM; return; }
        b->s->q = p; b->cap_q = c;
    }
    if (b->n_v + n > b->cap_v) {
        size_t c = b->cap_v ? b->cap_v * 2u : 512u;
        while (c < b->n_v + n) c *= 2u;
        uint32_t *p = (uint32_t *)realloc(b->s->store, c * sizeof *p);
        if (!p) { b->rc = SPFY_E_NOMEM; return; }
        b->s->store = p; b->cap_v = c;
    }
    memcpy(b->s->store + b->n_v, val, (size_t)n * sizeof *val);
    b->s->q[b->s->n].key = key;
    b->s->q[b->s->n].n   = n;
    /* ⚠ The pointer is fixed up AFTER the loop: `store` moves on realloc, so
     * anything captured now would dangle the moment the set grows. */
    b->s->q[b->s->n].val = (const uint32_t *)(uintptr_t)b->n_v;
    b->n_v += n;
    ++b->s->n;
}

static int label_of(char (*labels)[8], size_t n, const char *name)
{
    for (size_t i = 0; i < n; ++i)
        if (!strcmp(labels[i], name)) return (int)i;
    return -1;
}

/* Festival's construct_class_ques_subset, for an ordinal feature: rank the
 * observed values by their mean predictee and emit the cumulative prefixes of
 * that ranking. A "subset" is then always contiguous in the ranking, which is
 * how a set like {2,6,7,8} arises without being arbitrary. */
static void qb_add_ranked_subsets(qbuild *b, uint8_t key,
                                  const spfy_wgn_sample *sam, size_t n_sam)
{
    enum { MAXV = 16 };
    double sum[MAXV];
    uint32_t cnt[MAXV];
    memset(sum, 0, sizeof sum);
    memset(cnt, 0, sizeof cnt);
    for (size_t i = 0; i < n_sam; ++i) {
        uint32_t v = sam[i].feat[key];
        if (v == 0u || v >= MAXV) continue;      /* 0 is "unset", not a value */
        sum[v] += sam[i].y;
        ++cnt[v];
    }
    uint32_t vals[MAXV];
    double   mean[MAXV];
    uint32_t m = 0;
    for (uint32_t v = 1; v < MAXV; ++v) {
        /* Too few examples is not a class; it is one recording's accident. */
        if (cnt[v] < 20u) continue;
        vals[m] = v;
        mean[m] = sum[v] / (double)cnt[v];
        ++m;
    }
    for (uint32_t i = 1; i < m; ++i) {           /* insertion sort by mean */
        uint32_t kv = vals[i];
        double km = mean[i];
        uint32_t j = i;
        while (j > 0 && mean[j - 1] > km) {
            vals[j] = vals[j - 1];
            mean[j] = mean[j - 1];
            --j;
        }
        vals[j] = kv;
        mean[j] = km;
    }
    for (uint32_t k = 1; k < m; ++k)
        qb_add(b, key, vals, k);
}

int spfy_wgn_qset_build(char (*labels)[8], size_t n_labels,
                        int with_phone_in_syl,
                        const spfy_wgn_sample *sam, size_t n_sam,
                        spfy_wgn_qset *out)
{
    if (!labels || !out) return SPFY_E_INVAL;
    memset(out, 0, sizeof *out);
    qbuild b;
    memset(&b, 0, sizeof b);
    b.s = out;
    b.rc = SPFY_OK;

    uint32_t v[64];

    /* q5 halfphones-in-syllable: singletons 1..9 then the cumulative
     * prefixes 1..k, which is what both vendors carry. */
    for (uint32_t k = 1; k <= 9u; ++k) { v[0] = k; qb_add(&b, Q_HP_IN_SYL, v, 1); }
    for (uint32_t k = 2; k <= 7u; ++k) {
        for (uint32_t i = 0; i < k; ++i) v[i] = i + 1u;
        qb_add(&b, Q_HP_IN_SYL, v, k);
    }
    /* q1 sylType: singletons 1..7 and the three groupings. */
    for (uint32_t k = 1; k <= 7u; ++k) { v[0] = k; qb_add(&b, Q_SYL_TYPE, v, 1); }
    v[0] = 1; v[1] = 2;                       qb_add(&b, Q_SYL_TYPE, v, 2);
    v[0] = 3; v[1] = 4; v[2] = 5;             qb_add(&b, Q_SYL_TYPE, v, 3);
    v[0] = 6; v[1] = 7;                       qb_add(&b, Q_SYL_TYPE, v, 2);
    v[0] = 3; v[1] = 4; v[2] = 5; v[3] = 6; v[4] = 7;
    qb_add(&b, Q_SYL_TYPE, v, 5);
    /* q2 / q7 / q8: ordinal position singletons and cumulative prefixes.
     * The vendors also carry a few arbitrary unions (jill's {2,6,7,8} and
     * {3,5}); those are data-driven, not phonetic, so the prefixes stand in
     * for them and the grower picks what separates OUR corpus. */
    static const uint8_t ORD[3] = { Q_SYL_IN_PHRASE, Q_WORD_IN_PHRASE,
                                    Q_SYL_IN_WORD };
    for (size_t o = 0; o < 3u; ++o) {
        for (uint32_t k = 1; k <= 8u; ++k) { v[0] = k; qb_add(&b, ORD[o], v, 1); }
        for (uint32_t k = 2; k <= 6u; ++k) {
            for (uint32_t i = 0; i < k; ++i) v[i] = i + 1u;
            qb_add(&b, ORD[o], v, k);
        }
    }
    if (with_phone_in_syl) {
        for (uint32_t k = 1; k <= 6u; ++k)
            { v[0] = k; qb_add(&b, Q_PHONE_IN_SYL, v, 1); }
        v[0] = 1; v[1] = 2;             qb_add(&b, Q_PHONE_IN_SYL, v, 2);
        v[0] = 4; v[1] = 5;             qb_add(&b, Q_PHONE_IN_SYL, v, 2);
        v[0] = 1; v[1] = 2; v[2] = 3;   qb_add(&b, Q_PHONE_IN_SYL, v, 3);
    }
    /* q3 / q4: one singleton per label, then the phonetic classes. */
    static const uint8_t SIDE[2] = { Q_LEFT, Q_RIGHT };
    for (size_t s = 0; s < 2u; ++s) {
        for (uint32_t i = 0; i < (uint32_t)n_labels; ++i) {
            if (!labels[i][0]) continue;
            v[0] = i;
            qb_add(&b, SIDE[s], v, 1);
        }
        for (size_t c = 0; c < N_CLASSES; ++c) {
            uint32_t n = 0;
            for (const char *const *p = CLASSES[c]; *p; ++p) {
                int li = label_of(labels, n_labels, *p);
                if (li >= 0 && n < 64u) v[n++] = (uint32_t)li;
            }
            /* A class the phone set cannot express at all is dropped; one it
             * expresses partly is still a real distinction. */
            if (n >= 2u) qb_add(&b, SIDE[s], v, n);
        }
    }
    if (sam && n_sam) {
        static const uint8_t RANKED[4] = { Q_SYL_IN_PHRASE, Q_WORD_IN_PHRASE,
                                           Q_SYL_IN_WORD, Q_HP_IN_SYL };
        for (size_t o = 0; o < 4u; ++o)
            qb_add_ranked_subsets(&b, RANKED[o], sam, n_sam);
    }
    if (b.rc != SPFY_OK) { spfy_wgn_qset_free(out); return b.rc; }
    for (size_t i = 0; i < out->n; ++i)
        out->q[i].val = out->store + (size_t)(uintptr_t)out->q[i].val;
    return SPFY_OK;
}

int spfy_wgn_chunk_write(char (*labels)[8], size_t n_labels,
                         const spfy_wgn_qset *q,
                         const spfy_wgn_tree *trees, size_t n_trees,
                         uint8_t **out, size_t *out_n)
{
    if (!labels || !q || !trees || !out || !out_n) return SPFY_E_INVAL;
    spfy_vb_buf lab = {0}, qs = {0}, trhd = {0}, all = {0};
    int rc = spfy_vb_buf_u32(&lab, (uint32_t)n_labels);
    for (size_t i = 0; i < n_labels && rc == SPFY_OK; ++i)
        rc = spfy_vb_buf_pstr(&lab, labels[i]);
    if (rc == SPFY_OK) rc = spfy_wgn_qset_write(q, &qs);

    /* trhd is a container of labl + ques, each with its own 8-byte header. */
    if (rc == SPFY_OK) rc = spfy_vb_buf_put(&trhd, "labl", 4);
    if (rc == SPFY_OK) rc = spfy_vb_buf_u32(&trhd, (uint32_t)lab.n);
    if (rc == SPFY_OK) rc = spfy_vb_buf_put(&trhd, lab.p, lab.n);
    if (rc == SPFY_OK && (lab.n & 1u)) rc = spfy_vb_buf_u8(&trhd, 0);
    if (rc == SPFY_OK) rc = spfy_vb_buf_put(&trhd, "ques", 4);
    if (rc == SPFY_OK) rc = spfy_vb_buf_u32(&trhd, (uint32_t)qs.n);
    if (rc == SPFY_OK) rc = spfy_vb_buf_put(&trhd, qs.p, qs.n);
    if (rc == SPFY_OK && (qs.n & 1u)) rc = spfy_vb_buf_u8(&trhd, 0);

    if (rc == SPFY_OK) rc = spfy_vb_buf_put(&all, "trhd", 4);
    if (rc == SPFY_OK) rc = spfy_vb_buf_u32(&all, (uint32_t)trhd.n);
    if (rc == SPFY_OK) rc = spfy_vb_buf_put(&all, trhd.p, trhd.n);
    if (rc == SPFY_OK && (trhd.n & 1u)) rc = spfy_vb_buf_u8(&all, 0);

    for (size_t t = 0; t < n_trees && rc == SPFY_OK; ++t) {
        spfy_vb_buf tb = {0};
        rc = spfy_wgn_tree_write(&trees[t], &tb);
        if (rc == SPFY_OK) rc = spfy_vb_buf_put(&all, "tree", 4);
        if (rc == SPFY_OK) rc = spfy_vb_buf_u32(&all, (uint32_t)tb.n);
        if (rc == SPFY_OK) rc = spfy_vb_buf_put(&all, tb.p, tb.n);
        if (rc == SPFY_OK && (tb.n & 1u)) rc = spfy_vb_buf_u8(&all, 0);
        spfy_vb_buf_free(&tb);
    }
    spfy_vb_buf_free(&lab);
    spfy_vb_buf_free(&qs);
    spfy_vb_buf_free(&trhd);
    if (rc != SPFY_OK) { spfy_vb_buf_free(&all); return rc; }
    *out = all.p;
    *out_n = all.n;
    return SPFY_OK;
}
