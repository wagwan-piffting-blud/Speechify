#include "vb_stages.h"

#include "vb_wagon.h"
#include "edge_frames.h"
#include "join_cost.h"
#include "../usel/hash.h"
#include "../usel/prsl.h"
#include "../voice/unit_table.h"
#include "../voice/voice.h"
#include "../common/log.h"
#include "../../include/spfy/spfy.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_f32(uint8_t *p, float v)
{
    union { float f; uint32_t u; } cv;
    cv.f = v;
    p[0] = (uint8_t)(cv.u & 0xFFu);
    p[1] = (uint8_t)((cv.u >> 8) & 0xFFu);
    p[2] = (uint8_t)((cv.u >> 16) & 0xFFu);
    p[3] = (uint8_t)((cv.u >> 24) & 0xFFu);
}

/* ====================================================================== */
/* S3 NORM                                                                 */

static void mean_sd(const double *v, size_t n, double *m, double *sd)
{
    if (!n) { *m = 0.0; *sd = 0.0; return; }
    double s = 0.0;
    for (size_t i = 0; i < n; ++i) s += v[i];
    double mu = s / (double)n;
    double q = 0.0;
    for (size_t i = 0; i < n; ++i) q += (v[i] - mu) * (v[i] - mu);
    *m = mu;
    *sd = sqrt(q / (double)n);       /* population sd, as numpy's default */
}

int spfy_vb_s3_norm(const spfy_vb_corpus *c, const spfy_vb_labl_map *labl,
                    uint8_t **out, size_t *out_n, size_t *n_populated,
                    size_t *n_have)
{
    size_t n = c->n_units;
    double (*rows)[8] = (double (*)[8])calloc(SPFY_VB_N_HP, sizeof *rows);
    if (!rows) return SPFY_E_NOMEM;

    /* ⚠ hp is pc*2 + (uid & 1) -- UID PARITY, not is_first_half. +0x15 marks
     * a syllable start, and units are emitted left-half-then-right-half per
     * phone, so parity is what actually carries the half. */
    uint8_t *hp = (uint8_t *)malloc(n ? n : 1u);
    if (!hp) { free(rows); return SPFY_E_NOMEM; }
    for (size_t i = 0; i < n; ++i) {
        int16_t f = labl->l2f[c->units[i].phone_center];
        int fid = f >= 0 ? f : c->units[i].phone_center;
        long v = (long)fid * 2L + (long)(i & 1u);
        hp[i] = (uint8_t)(v >= 0 && v < SPFY_VB_N_HP ? v : SPFY_VB_N_HP - 1);
    }

    double *buf = (double *)malloc((n ? n : 1u) * sizeof *buf);
    if (!buf) { free(hp); free(rows); return SPFY_E_NOMEM; }

    size_t have = 0;
    for (size_t i = 0; i < n; ++i) if (c->units[i].have_audio) ++have;

    for (int cls = 0; cls < SPFY_VB_N_HP; ++cls) {
        size_t m = 0;
        for (size_t i = 0; i < n; ++i)
            if (hp[i] == cls && c->units[i].dur_like < 20000u)
                buf[m++] = c->units[i].dur_like;
        mean_sd(buf, m, &rows[cls][0], &rows[cls][1]);

        m = 0;
        for (size_t i = 0; i < n; ++i)
            if (hp[i] == cls && c->units[i].have_audio && c->units[i].a_pitch > 0.0)
                buf[m++] = c->units[i].a_pitch;
        mean_sd(buf, m, &rows[cls][2], &rows[cls][3]);

        m = 0;
        for (size_t i = 0; i < n; ++i)
            if (hp[i] == cls && c->units[i].have_audio)
                buf[m++] = c->units[i].a_voice;
        mean_sd(buf, m, &rows[cls][4], &rows[cls][5]);

        m = 0;
        for (size_t i = 0; i < n; ++i)
            if (hp[i] == cls && c->units[i].have_audio)
                buf[m++] = c->units[i].a_power;
        mean_sd(buf, m, &rows[cls][6], &rows[cls][7]);
    }
    free(buf);
    free(hp);

    size_t live = 0;
    for (int cls = 0; cls < SPFY_VB_N_HP; ++cls) if (rows[cls][0] > 0.0) ++live;

    int rc = spfy_vb_encode_mean((const double (*)[8])rows, SPFY_VB_N_HP,
                                 out, out_n);
    free(rows);
    if (n_populated) *n_populated = live;
    if (n_have) *n_have = have;
    return rc;
}

/* ====================================================================== */
/* S6 TREES                                                                */

typedef struct {
    int      is_leaf;
    uint32_t yes, no, qi;
    size_t   mean_off;      /* absolute offset of the leaf mean, in the chunk */
} tree_node;

typedef struct {
    uint8_t   key;
    uint32_t *val;
    uint32_t  n;
} ques_t;

static int parse_tree(const uint8_t *chunk, size_t tree_off, size_t tree_n,
                      tree_node **out, size_t *out_n)
{
    const uint8_t *d = chunk + tree_off;
    if (tree_n < 4) return SPFY_E_FORMAT;
    uint32_t n = rd_u32(d);
    tree_node *v = (tree_node *)calloc(n ? n : 1u, sizeof *v);
    if (!v) return SPFY_E_NOMEM;
    size_t off = 4;
    for (uint32_t i = 0; i < n; ++i) {
        if (off + 8 > tree_n) { free(v); return SPFY_E_FORMAT; }
        off += 4;                                     /* idx, unused here */
        int32_t yc = (int32_t)rd_u32(d + off);
        off += 4;
        if (yc >= 0) {
            if (off + 8 > tree_n) { free(v); return SPFY_E_FORMAT; }
            v[i].is_leaf = 0;
            v[i].yes = (uint32_t)yc;
            v[i].no  = rd_u32(d + off);
            off += 4;
            v[i].qi  = rd_u32(d + off);
            off += 4;
        } else {
            if (off + 12 > tree_n) { free(v); return SPFY_E_FORMAT; }
            off += 4;                                 /* unused */
            v[i].is_leaf = 1;
            v[i].mean_off = tree_off + off;
            off += 8;                                 /* mean, var */
        }
    }
    *out = v;
    *out_n = n;
    return SPFY_OK;
}

static int parse_ques(const uint8_t *d, size_t n, ques_t **out, size_t *out_n)
{
    if (n < 4) return SPFY_E_FORMAT;
    uint32_t cnt = rd_u32(d);
    ques_t *q = (ques_t *)calloc(cnt ? cnt : 1u, sizeof *q);
    if (!q) return SPFY_E_NOMEM;
    size_t off = 4;
    for (uint32_t i = 0; i < cnt; ++i) {
        if (off + 5 > n) { goto bad; }
        q[i].key = d[off];
        off += 1;
        q[i].n = rd_u32(d + off);
        off += 4;
        if (off + (size_t)q[i].n * 4u > n) goto bad;
        q[i].val = (uint32_t *)malloc((q[i].n ? q[i].n : 1u) * sizeof *q[i].val);
        if (!q[i].val) goto bad;
        for (uint32_t k = 0; k < q[i].n; ++k) q[i].val[k] = rd_u32(d + off + k * 4u);
        off += (size_t)q[i].n * 4u;
    }
    *out = q;
    *out_n = cnt;
    return SPFY_OK;
bad:
    for (uint32_t i = 0; i < cnt; ++i) free(q[i].val);
    free(q);
    return SPFY_E_FORMAT;
}

static uint32_t traverse(const tree_node *nodes, size_t n_nodes,
                         const ques_t *q, size_t n_q, const uint32_t feat[16])
{
    uint32_t ni = 0;
    for (size_t guard = 0; guard < n_nodes + 1u; ++guard) {
        if (ni >= n_nodes) return 0;
        if (nodes[ni].is_leaf) return ni;
        if (nodes[ni].qi >= n_q) return ni;
        const ques_t *qq = &q[nodes[ni].qi];
        uint32_t fv = qq->key < 16u ? feat[qq->key] : 0u;
        int hit = 0;
        for (uint32_t k = 0; k < qq->n; ++k)
            if (qq->val[k] == fv) { hit = 1; break; }
        ni = hit ? nodes[ni].yes : nodes[ni].no;
    }
    return 0;
}

int spfy_vb_s6_durt(const spfy_vb_corpus *c, const uint8_t *tmpl_durt,
                    size_t tmpl_n, uint8_t **out, size_t *out_n,
                    size_t *n_recomputed, size_t *n_kept)
{
    uint8_t *durt = (uint8_t *)malloc(tmpl_n ? tmpl_n : 1u);
    if (!durt) return SPFY_E_NOMEM;
    memcpy(durt, tmpl_durt, tmpl_n);

    /* Locate the per-phone trees and the shared question list. */
    size_t tree_off[64], tree_len[64];
    size_t n_trees = 0;
    const uint8_t *ques_d = NULL;
    size_t ques_n = 0;
    {
        size_t pos = 0;
        char id[5];
        const uint8_t *d;
        size_t dn;
        while (spfy_vb_subchunk(durt, tmpl_n, &pos, id, &d, &dn)) {
            if (!memcmp(id, "tree", 4) && n_trees < 64) {
                tree_off[n_trees] = (size_t)(d - durt);
                tree_len[n_trees] = dn;
                ++n_trees;
            } else if (!memcmp(id, "ques", 4)) {
                ques_d = d; ques_n = dn;
            } else if (!memcmp(id, "trhd", 4)) {
                size_t p2 = 0;
                char id2[5];
                const uint8_t *d2;
                size_t d2n;
                while (spfy_vb_subchunk(d, dn, &p2, id2, &d2, &d2n))
                    if (!memcmp(id2, "ques", 4)) { ques_d = d2; ques_n = d2n; }
            }
        }
    }
    if (!n_trees || !ques_d) { free(durt); return SPFY_E_FORMAT; }

    ques_t *q = NULL;
    size_t n_q = 0;
    int rc = parse_ques(ques_d, ques_n, &q, &n_q);
    if (rc != SPFY_OK) { free(durt); return rc; }

    /* Route every unit and accumulate f0_context per leaf. */
    tree_node **nodes = (tree_node **)calloc(n_trees, sizeof *nodes);
    size_t *n_nodes = (size_t *)calloc(n_trees, sizeof *n_nodes);
    double **acc = (double **)calloc(n_trees, sizeof *acc);
    uint32_t **cnt = (uint32_t **)calloc(n_trees, sizeof *cnt);
    if (!nodes || !n_nodes || !acc || !cnt) { rc = SPFY_E_NOMEM; goto done; }
    for (size_t i = 0; i < n_trees; ++i) {
        rc = parse_tree(durt, tree_off[i], tree_len[i], &nodes[i], &n_nodes[i]);
        if (rc != SPFY_OK) goto done;
        acc[i] = (double *)calloc(n_nodes[i] ? n_nodes[i] : 1u, sizeof **acc);
        cnt[i] = (uint32_t *)calloc(n_nodes[i] ? n_nodes[i] : 1u, sizeof **cnt);
        if (!acc[i] || !cnt[i]) { rc = SPFY_E_NOMEM; goto done; }
    }

    /* ⚠ WHICH QUESTION KEYS THIS TEMPLATE ACTUALLY ASKS, AND WHICH OF THEM WE
     * CAN ANSWER. A feature we leave at 0 does not fail -- it silently sorts
     * every unit down one branch -- so the only way this is visible is to
     * count it. Keys are the walker's: 1 sylType, 2 sylInPhrase, 3 left label,
     * 4 right label, 5 halfphones-in-syl, 8 wordInPhrase, 9 phoneInSyl. */
    {
        uint32_t by_key[16];
        memset(by_key, 0, sizeof by_key);
        for (size_t i = 0; i < n_q; ++i)
            if (q[i].key < 16u) ++by_key[q[i].key];
        printf("  durt questions by key:");
        for (uint32_t k = 0; k < 16u; ++k)
            if (by_key[k]) printf("  q%u=%u", k, by_key[k]);
        printf("  (of %zu)\n", n_q);
        uint32_t unanswered = 0;
        for (uint32_t k = 0; k < 16u; ++k)
            if (by_key[k] && k != 1u && k != 2u && k != 3u && k != 4u &&
                k != 5u && k != 8u && k != 9u)
                unanswered += by_key[k];
        /* q7 is deliberately absent: the engine's durt walker zeroes EBX
         * before each dispatch, so q7 reads 0 there too and matching it is
         * correct rather than a gap. */
        if (unanswered)
            printf("    ⚠ %u question(s) on a key this build does not "
                   "populate -- those nodes sort every unit the same way\n",
                   unanswered);
    }

    for (size_t u = 0; u < c->n_units; ++u) {
        const spfy_vb_unit *r = &c->units[u];
        uint32_t pc = r->phone_center;
        if (pc >= n_trees) continue;
        /* ⛔ CORRECTED 2026-08-16. This used to carry the legacy pipeline's
         * byte-offset mapping (q1=+0x0C, q2=+0x0D, q3=+0x17, q4=+0x18,
         * q5=+0x0E, q8=+0x0F) with a comment conceding its q1/q2 names were
         * "swapped". They were not just names: the walker really does read
         * the other field, so every unit was being routed to the wrong leaf
         * and the recomputed means assigned accordingly.
         *
         * The authority is the engine's own CART walker, FUN_08e87c90, as
         * decoded in spfy_synth.c::cart_feat and build_graph.h:
         *
         *   1 sylType   2 sylInPhrase   3 LEFT label   4 RIGHT label
         *   5 halfphones-in-syllable    8 wordInPhrase 9 phoneInSyl
         *
         * Three errors, each independently wrong: q1/q2 swapped, q8 reading
         * sylInWord where the walker reads wordInPhrase, and q3/q4 taken
         * from phone_ctx[0]/[1] when the four stored slots are
         * (pp2, pp1, pn1, pn2), making LEFT and RIGHT [1] and [2].
         *
         * MEASURED: under the old mapping the VENDORS' OWN trees scored
         * worse than predicting the training mean (jill 26.90 against a
         * floor of 22.68) for every candidate predictee. Corrected, they
         * beat it (21.78). A tree that cannot beat its own training mean is
         * the signature of a broken feature map, and nothing else here
         * reports it. */
        uint32_t feat[16];
        memset(feat, 0, sizeof feat);
        feat[1] = r->sp_syl_type;
        feat[2] = r->sp_syl_in_phrase;
        feat[3] = r->phone_ctx[1];
        feat[4] = r->phone_ctx[2];
        /* ⭐ ALSO USED TO BE ZERO, on the reasoning that q5 is "computed at
         * slot time and not in the unit record". True of the RECORD, false of
         * the DATA: spfy_synth.c's compute_q5_per_slot derives it from sp[2],
         * sp[3] and ctx[2], all of which the `.sp` sidecar carries, so
         * vb_corpus.c now runs that same function at build time.
         *
         * It matters more than q9 did -- jill asks q5 at 15 of 161 questions
         * against phoneInSyl's 7. */
        feat[5] = r->q5;
        /* ⛔ MEASURED, NOT NAMED. This read sp_word_in_phrase (= sidecar
         * sp[2]) because the walker calls q8 "wordInPhrase" and that field
         * carries the name. But the record field NAMES are inverted relative
         * to cart_feat's vocabulary -- disk 0x0E pairs with target sp[2],
         * proved by anchor_score's cand_bytes[2] -- and the walker's CODE is
         * `case 8: v = c->slot->sp[3]` (spfy_synth.c:1177).
         *
         * Settled with spfy_vb_wagon, which holds the VENDOR's tree fixed and
         * varies only our feature map. Held-out RMSE, lower is better:
         *
         *              q8 <- sp[2]   q8 <- sp[3]   mean-only floor
         *   jill          21.785        21.184         22.680
         *   tom           19.539        19.123         20.705
         *
         * Both vendors improve, independently. The map that lets their own
         * tree predict better is the map they trained it with. */
        feat[8] = r->sp_syl_in_word;      /* sidecar sp[3] */
        /* ⭐ q9 (phoneInSyl) USED TO BE ZERO HERE, for the stated reason that
         * the build-time unit did not carry it. It does now -- `.sp` had the
         * value all along -- and leaving it at 0 was not harmless: at
         * synthesis the engine walks these same trees with the TARGET's real
         * phoneInSyl, so a q9 node sorted units by a constant and was then
         * queried by a variable. The leaf means on both sides of every such
         * node were averages of the wrong population.
         *
         * Independent of --unit-version: this is build-time ROUTING, not a
         * stored column. A v100006 voice gets the corrected leaves too. */
        feat[9] = r->sp_phone_in_syl;
        uint32_t li = traverse(nodes[pc], n_nodes[pc], q, n_q, feat);
        if (li < n_nodes[pc] && nodes[pc][li].is_leaf) {
            acc[pc][li] += r->f0_context;
            cnt[pc][li] += 1u;
        }
    }

    size_t recomp = 0, kept = 0;
    for (size_t i = 0; i < n_trees; ++i) {
        for (size_t k = 0; k < n_nodes[i]; ++k) {
            if (!nodes[i][k].is_leaf) continue;
            if (cnt[i][k] >= SPFY_VB_TREE_MIN_SAMPLES) {
                wr_f32(durt + nodes[i][k].mean_off,
                       (float)(acc[i][k] / (double)cnt[i][k]));
                ++recomp;
            } else {
                ++kept;
            }
        }
    }
    *out = durt;
    *out_n = tmpl_n;
    if (n_recomputed) *n_recomputed = recomp;
    if (n_kept) *n_kept = kept;
    durt = NULL;
    rc = SPFY_OK;

done:
    if (nodes) for (size_t i = 0; i < n_trees; ++i) free(nodes[i]);
    if (acc)   for (size_t i = 0; i < n_trees; ++i) free(acc[i]);
    if (cnt)   for (size_t i = 0; i < n_trees; ++i) free(cnt[i]);
    free(nodes); free(n_nodes); free(acc); free(cnt);
    for (size_t i = 0; i < n_q; ++i) free(q[i].val);
    free(q);
    free(durt);
    return rc;
}

/* ====================================================================== */
/* S6a2 f0tr -- the same treatment durt gets, for the PITCH tree.
 *
 * ⛔ WHY THIS EXISTS. `f0tr` was carried over from the template verbatim while
 * we started writing real F0 bytes, and that combination is worse than writing
 * no F0 at all. anchor_score.c:786 scores each candidate as
 *
 *     delta = |f0_start - f0tr_mean| * f0tr_var ;  cost += delta*w_f0*delta
 *
 * so with the template's leaves the cost measures how well OUR unit matches
 * JILL'S pitch contour for that context. Measured: it moved 130 of 158 picks
 * (82.3%) on one sentence, none of them to a different phone, and the user
 * heard "relative humidity" get worse. With f0_start == 0 the branch above it
 * charges a flat w_f0_miss to EVERY candidate instead -- a constant, which
 * decides nothing, which is exactly why `--f0 absent` sounded safe.
 *
 * ⚠ f0tr IS NOT durt AND THE FEATURE MAPS DIFFER IN BOTH DIRECTIONS.
 * cart_feat (spfy_synth.c:1138-1155) clamps q3/q4/q5/q9 to 0 for f0tr because
 * it is SYLLABLE-level, and clamps q7 only for durt. So f0tr reads q7 and durt
 * does not. Validated with spfy_vb_wagon --chunk f0tr --predictee f0start,
 * which holds the vendor's own tree fixed: it beats its mean-only floor on
 * both voices (jill 5.631 vs 6.637, tom 4.230 vs 5.331). A wrong map scores at
 * or below the floor -- that is how the durt q-mapping error surfaced.
 *
 * ⚠ THE PREDICTEE IS `f0_start`, not f0_mid, because f0_start is the byte the
 * cost compares. Zeros are EXCLUDED: the engine treats 0 as "no F0 here" and
 * takes the w_f0_miss branch, so averaging zeros into a leaf would drag every
 * prediction toward silence.
 *
 * Only the leaf MEAN is rewritten. The second f32 (`f0tr_var`) is a scale on
 * the delta and is kept from the template, because nothing here has measured
 * what it should be and inventing it would be the same guess this stage is
 * fixing. */

int spfy_vb_s6_f0tr(const spfy_vb_corpus *c, const uint8_t *tmpl_f0tr,
                    size_t tmpl_n, uint8_t **out, size_t *out_n,
                    size_t *n_recomputed, size_t *n_kept, size_t *n_used)
{
    uint8_t *f0tr = (uint8_t *)malloc(tmpl_n ? tmpl_n : 1u);
    if (!f0tr) return SPFY_E_NOMEM;
    memcpy(f0tr, tmpl_f0tr, tmpl_n);

    size_t tree_off[64], tree_len[64];
    size_t n_trees = 0;
    const uint8_t *ques_d = NULL;
    size_t ques_n = 0;
    {
        size_t pos = 0;
        char id[5];
        const uint8_t *d;
        size_t dn;
        while (spfy_vb_subchunk(f0tr, tmpl_n, &pos, id, &d, &dn)) {
            if (!memcmp(id, "tree", 4) && n_trees < 64) {
                tree_off[n_trees] = (size_t)(d - f0tr);
                tree_len[n_trees] = dn;
                ++n_trees;
            } else if (!memcmp(id, "ques", 4)) {
                ques_d = d; ques_n = dn;
            } else if (!memcmp(id, "trhd", 4)) {
                size_t p2 = 0;
                char id2[5];
                const uint8_t *d2;
                size_t d2n;
                while (spfy_vb_subchunk(d, dn, &p2, id2, &d2, &d2n))
                    if (!memcmp(id2, "ques", 4)) { ques_d = d2; ques_n = d2n; }
            }
        }
    }
    if (!n_trees || !ques_d) { free(f0tr); return SPFY_E_FORMAT; }

    ques_t *q = NULL;
    size_t n_q = 0;
    int rc = parse_ques(ques_d, ques_n, &q, &n_q);
    if (rc != SPFY_OK) { free(f0tr); return rc; }

    tree_node **nodes = (tree_node **)calloc(n_trees, sizeof *nodes);
    size_t *n_nodes = (size_t *)calloc(n_trees, sizeof *n_nodes);
    double **acc = (double **)calloc(n_trees, sizeof *acc);
    uint32_t **cnt = (uint32_t **)calloc(n_trees, sizeof *cnt);
    if (!nodes || !n_nodes || !acc || !cnt) { rc = SPFY_E_NOMEM; goto done; }
    for (size_t i = 0; i < n_trees; ++i) {
        rc = parse_tree(f0tr, tree_off[i], tree_len[i], &nodes[i], &n_nodes[i]);
        if (rc != SPFY_OK) goto done;
        acc[i] = (double *)calloc(n_nodes[i] ? n_nodes[i] : 1u, sizeof **acc);
        cnt[i] = (uint32_t *)calloc(n_nodes[i] ? n_nodes[i] : 1u, sizeof **cnt);
        if (!acc[i] || !cnt[i]) { rc = SPFY_E_NOMEM; goto done; }
    }

    {
        uint32_t by_key[16];
        memset(by_key, 0, sizeof by_key);
        for (size_t i = 0; i < n_q; ++i)
            if (q[i].key < 16u) ++by_key[q[i].key];
        printf("  f0tr questions by key:");
        for (uint32_t k = 0; k < 16u; ++k)
            if (by_key[k]) printf("  q%u=%u", k, by_key[k]);
        printf("  (of %zu)\n", n_q);
        uint32_t dead = by_key[3] + by_key[4] + by_key[5] + by_key[9];
        if (dead)
            printf("    note: %u question(s) on keys the engine CLAMPS to 0 "
                   "for f0tr; matching that is correct, not a gap\n", dead);
    }

    size_t used = 0;
    for (size_t u = 0; u < c->n_units; ++u) {
        const spfy_vb_unit *r = &c->units[u];
        /* ⚠ ONE tree, always index 0 -- spfy_synth.c:3902 traverses
         * `&v->f0tr_cart, 0`. It is not per-phone the way durt is. */
        if (!r->f0_start) continue;
        uint32_t feat[16];
        memset(feat, 0, sizeof feat);
        feat[1] = r->sp_syl_type;         /* q1 <- sp[1] */
        feat[2] = r->sp_syl_in_phrase;    /* q2 <- sp[0] */
        feat[7] = r->sp_word_in_phrase;   /* q7 <- sp[2], LIVE for f0tr    */
        feat[8] = r->sp_syl_in_word;      /* q8 <- sp[3], as durt (measured)*/
        /* feat[3]/[4]/[5]/[9] stay 0: the engine clamps them for f0tr. */
        uint32_t li = traverse(nodes[0], n_nodes[0], q, n_q, feat);
        if (li < n_nodes[0] && nodes[0][li].is_leaf) {
            acc[0][li] += r->f0_start;
            cnt[0][li] += 1u;
            ++used;
        }
    }

    size_t recomp = 0, kept = 0;
    for (size_t i = 0; i < n_trees; ++i) {
        for (size_t k = 0; k < n_nodes[i]; ++k) {
            if (!nodes[i][k].is_leaf) continue;
            if (cnt[i][k] >= SPFY_VB_TREE_MIN_SAMPLES) {
                wr_f32(f0tr + nodes[i][k].mean_off,
                       (float)(acc[i][k] / (double)cnt[i][k]));
                ++recomp;
            } else {
                ++kept;
            }
        }
    }
    *out = f0tr;
    *out_n = tmpl_n;
    if (n_recomputed) *n_recomputed = recomp;
    if (n_kept) *n_kept = kept;
    if (n_used) *n_used = used;
    f0tr = NULL;
    rc = SPFY_OK;

done:
    if (nodes) for (size_t i = 0; i < n_trees; ++i) free(nodes[i]);
    if (acc)   for (size_t i = 0; i < n_trees; ++i) free(acc[i]);
    if (cnt)   for (size_t i = 0; i < n_trees; ++i) free(cnt[i]);
    free(nodes); free(n_nodes); free(acc); free(cnt);
    for (size_t i = 0; i < n_q; ++i) free(q[i].val);
    free(q);
    free(f0tr);
    return rc;
}

/* ⭐⭐ Neutralise the f0tr TARGET cost and leave the JOIN path alone.
 *
 * `--f0 calibrated` switches on two mechanisms with one flag, and they pull
 * in opposite directions. Measured on the three demo texts with
 * `vb_seamf0.py`, against a within-run floor of ~0.5 st that is identical on
 * every arm including jill:
 *
 *   audible seams (>2 st) per second   absent 1.246   calibrated 0.904
 *   accent on "NAtional"               absent +7.4 st calibrated +1.2 st
 *
 * The JOIN half is the `hist` F0-probability curve plus the unit f0 bytes.
 * The TARGET half is anchor_score.c:
 *     delta = (unit.f0_start - f0tr_mean) * f0tr_var;  cost = delta^2 * w_f0
 * which pulls selection toward the tree's near-average prediction. A leaf
 * variance of ZERO makes that term identically 0 for every candidate in every
 * slot, so the target cost stops existing while f0_start stays populated and
 * the join cost keeps working.
 *
 * ⚠ The chunk cannot simply be dropped: spfy_cart_load_f0tr() refuses a
 * missing or zero-length f0tr and the voice fails to load. It has to remain a
 * structurally valid tree that happens to price nothing. */
int spfy_vb_f0tr_zero_var(uint8_t *f0tr, size_t n, size_t *n_zeroed)
{
    if (!f0tr || !n) return SPFY_E_INVAL;
    size_t pos = 0, zeroed = 0;
    char id[5];
    const uint8_t *d;
    size_t dn;
    while (spfy_vb_subchunk(f0tr, n, &pos, id, &d, &dn)) {
        if (memcmp(id, "tree", 4)) continue;
        tree_node *nodes = NULL;
        size_t n_nodes = 0;
        int rc = parse_tree(f0tr, (size_t)(d - f0tr), dn, &nodes, &n_nodes);
        if (rc != SPFY_OK) return rc;
        for (size_t k = 0; k < n_nodes; ++k) {
            if (!nodes[k].is_leaf) continue;
            wr_f32(f0tr + nodes[k].mean_off + 4u, 0.0f);   /* mean, THEN var */
            ++zeroed;
        }
        free(nodes);
    }
    if (n_zeroed) *n_zeroed = zeroed;
    return SPFY_OK;
}

/* ====================================================================== */
/* S6a3 -- GROW durt / f0tr outright, instead of patching a donor's leaves.
 *
 * The two stages above keep the template's topology, its questions and its
 * leaf VARIANCES and only recompute the means. That leaves 29 KB of jill in
 * every voice we ship and, worse, routes our units through HER splits.
 *
 * The grower is Festival's wagon (vb_wagon.c) and it was gated on the vendors
 * before being pointed at us -- held-out RMSE, over their OWN units, growing
 * over OUR OWN question inventory:
 *
 *              ours     vendor tree   mean-only floor
 *   jill durt  16.62      21.18           22.68
 *   jill f0tr   5.382      5.459           6.321
 *   tom  durt  15.45      19.12           20.70
 *   tom  f0tr   4.692      4.813           6.147
 *
 * Better than the tree it replaces on all four, on data neither tree saw. */

static void tree_feat(const spfy_vb_unit *r, int is_f0tr, uint32_t feat[16])
{
    memset(feat, 0, 16u * sizeof *feat);
    feat[1] = r->sp_syl_type;
    feat[2] = r->sp_syl_in_phrase;
    if (is_f0tr) {
        /* cart_feat clamps q3/q4/q5/q9 for f0tr -- it is syllable-level --
         * and clamps q7 only for durt. Filling one tree with the other's map
         * sorts every unit by a constant and then queries it with a
         * variable. */
        feat[7] = r->sp_word_in_phrase;
    } else {
        feat[3] = r->phone_ctx[1];
        feat[4] = r->phone_ctx[2];
        feat[5] = r->q5;
        feat[9] = r->sp_phone_in_syl;
    }
    feat[8] = r->sp_syl_in_word;
}

static int grow_chunk(const spfy_vb_corpus *c, char (*labels)[8],
                      size_t n_labels, int is_f0tr, uint32_t min_cluster,
                      uint8_t **out, size_t *out_n, spfy_vb_treestat *st)
{
    if (!c || !labels || !out || !out_n) return SPFY_E_INVAL;
    if (st) memset(st, 0, sizeof *st);

    spfy_wgn_sample *all = (spfy_wgn_sample *)
        calloc(c->n_units ? c->n_units : 1u, sizeof *all);
    uint8_t *phone = (uint8_t *)calloc(c->n_units ? c->n_units : 1u, 1);
    spfy_wgn_tree *trees = (spfy_wgn_tree *)
        calloc(n_labels ? n_labels : 1u, sizeof *trees);
    if (!all || !phone || !trees) {
        free(all); free(phone); free(trees); return SPFY_E_NOMEM;
    }

    size_t n_all = 0;
    double gsum = 0.0;
    for (size_t u = 0; u < c->n_units; ++u) {
        const spfy_vb_unit *r = &c->units[u];
        /* ⛔ f0tr NEVER SEES AN UNVOICED UNIT: anchor_score.c:1077 takes the
         * flat w_f0_miss branch when f0_start is 0 and does not walk the tree
         * at all, so training on those units fits a population the engine
         * never asks about. */
        if (is_f0tr && r->f0_start == 0u) continue;
        if (r->phone_center >= n_labels) continue;
        spfy_wgn_sample *s = &all[n_all];
        tree_feat(r, is_f0tr, s->feat);
        s->y = is_f0tr ? (float)r->f0_start : (float)r->f0_context;
        phone[n_all] = r->phone_center;
        gsum += s->y;
        ++n_all;
    }
    if (st) st->n_samples = n_all;
    if (!n_all && !is_f0tr) {
        free(all); free(phone); free(trees);
        return SPFY_E_FORMAT;
    }
    /* ⚠ f0tr WITH NO F0 AT ALL still has to exist: spfy_cart_load_f0tr()
     * refuses a missing or zero-length chunk and the voice fails to load.
     * Under `--f0 absent` every f0_start is 0, the engine takes the flat
     * w_f0_miss branch and never walks the tree -- so one leaf at mean 0 with
     * precision 0 is both valid and inert, and it is OURS rather than the
     * donor's pitch contours sitting in the file waiting to be consulted. */
    float gmean = n_all ? (float)(gsum / (double)n_all) : 0.0f;

    spfy_wgn_qset qs;
    int rc = spfy_wgn_qset_build(labels, n_labels, c->n_sp_phone_in_syl > 0,
                                 all, n_all, &qs);
    if (rc != SPFY_OK) { free(all); free(phone); free(trees); return rc; }
    if (st) st->n_questions = qs.n;

    spfy_wgn_cfg cfg;
    cfg.min_cluster = min_cluster ? min_cluster : 50u;
    cfg.balance = 0.0f;

    /* durt carries one tree per label and the engine indexes it by
     * phone_center, so EVERY label needs a tree even when the corpus has no
     * units for it -- a missing one would walk off the end of the chunk.
     * f0tr carries exactly one, walked for every unit. */
    const size_t n_trees = is_f0tr ? 1u : n_labels;
    spfy_wgn_sample *part = (spfy_wgn_sample *)
        malloc((size_t)n_all * sizeof *part);
    if (!part) { rc = SPFY_E_NOMEM; goto done; }

    for (size_t t = 0; t < n_trees && rc == SPFY_OK; ++t) {
        size_t np = 0;
        for (size_t u = 0; u < n_all; ++u)
            if (is_f0tr || phone[u] == (uint8_t)t) part[np++] = all[u];

        spfy_wgn_node *root = NULL;
        if (np >= 2u && spfy_wgn_grow(part, np, qs.q, qs.n, &cfg,
                                      &root) == SPFY_OK && root) {
            size_t nn = spfy_wgn_nodes(root);
            trees[t].n = (spfy_wgn_rec *)calloc(nn, sizeof *trees[t].n);
            if (!trees[t].n) { spfy_wgn_free(root); rc = SPFY_E_NOMEM; break; }
            trees[t].n_nodes = nn;
            uint32_t next = 1;
            spfy_wgn_flatten(root, trees[t].n, &next, 0);
            if (st) {
                st->n_nodes += nn;
                st->n_leaves += spfy_wgn_leaves(root);
            }
            spfy_wgn_free(root);
        } else {
            /* A label with no units still gets a structurally valid tree: one
             * leaf holding the corpus mean and zero variance, which prices
             * nothing rather than pricing something invented. */
            trees[t].n = (spfy_wgn_rec *)calloc(1, sizeof *trees[t].n);
            if (!trees[t].n) { rc = SPFY_E_NOMEM; break; }
            trees[t].n_nodes = 1;
            trees[t].n[0].idx = 0;
            trees[t].n[0].is_leaf = 1;
            trees[t].n[0].yes = -1;
            trees[t].n[0].no = 0xFFFFFFFFu;
            trees[t].n[0].mean = gmean;
            trees[t].n[0].var = 0.0f;
            if (st) { st->n_nodes += 1; st->n_leaves += 1; st->n_empty += 1; }
        }
    }
    free(part);
    if (rc == SPFY_OK)
        rc = spfy_wgn_chunk_write(labels, n_labels, &qs, trees, n_trees,
                                  out, out_n);

done:
    for (size_t t = 0; t < n_trees; ++t) spfy_wgn_tree_free(&trees[t]);
    spfy_wgn_qset_free(&qs);
    free(all); free(phone); free(trees);
    return rc;
}

int spfy_vb_s6_durt_grow(const spfy_vb_corpus *c, char (*labels)[8],
                         size_t n_labels, uint32_t min_cluster,
                         uint8_t **out, size_t *out_n, spfy_vb_treestat *st)
{
    return grow_chunk(c, labels, n_labels, 0, min_cluster, out, out_n, st);
}

int spfy_vb_s6_f0tr_grow(const spfy_vb_corpus *c, char (*labels)[8],
                         size_t n_labels, uint32_t min_cluster,
                         uint8_t **out, size_t *out_n, spfy_vb_treestat *st)
{
    return grow_chunk(c, labels, n_labels, 1, min_cluster, out, out_n, st);
}

/* ====================================================================== */
/* S6b hist -- see vb_stages.h for the vendor reproduction this rests on.  */

int spfy_vb_s6_hist(const spfy_vb_corpus *c, uint8_t **out, size_t *out_n,
                    size_t *n_obs, uint32_t *peak_out, int *mode_bin)
{
    const uint32_t N = SPFY_VB_HIST_BINS;
    uint32_t *cnt = (uint32_t *)calloc(N, sizeof *cnt);
    if (!cnt) return SPFY_E_NOMEM;

    size_t obs = 0;
    for (size_t u = 1; u < c->n_units; ++u) {
        const spfy_vb_unit *r = &c->units[u];
        const spfy_vb_unit *l = &c->units[u - 1u];
        /* flag_b is the engine's own "this continues the previous unit in the
         * same recording" test, so it is the definition of a natural join. */
        if (!r->flag_b || l->file_idx != r->file_idx) continue;
        /* The engine's gate. Below it the curve is never consulted, so a
         * sample from there would describe joins that are never priced. */
        if (l->f0_mid < 21u || r->f0_end <= 20u) continue;
        long idx = (long)r->f0_end - (long)SPFY_VB_HIST_SUB_OFF
                 - (long)l->f0_mid;
        if (idx < 0) idx = 0;
        else if (idx >= (long)N) idx = (long)N - 1;
        ++cnt[idx];
        ++obs;
    }

    uint32_t peak = 0;
    int mode = -1;
    for (uint32_t i = 0; i < N; ++i)
        if (cnt[i] > peak) { peak = cnt[i]; mode = (int)i; }

    int rc;
    if (!peak) {
        /* No F0 in the inventory (the --f0 absent default). A flat curve is
         * the only honest one: an all-zero penalty leaves a miss costing
         * exactly MISSING_JOIN_COST, which is what the engine already does
         * when the gate cannot fire. Inventing a shape here would price
         * joins on evidence we do not have. */
        uint32_t one = 1u;
        uint32_t *flat = (uint32_t *)calloc(N, sizeof *flat);
        if (!flat) { free(cnt); return SPFY_E_NOMEM; }
        for (uint32_t i = 0; i < N; ++i) flat[i] = one;
        rc = spfy_vb_encode_hist(flat, N, SPFY_VB_HIST_SUB_OFF, 0.0,
                                 out, out_n);
        free(flat);
    } else {
        rc = spfy_vb_encode_hist(cnt, N, SPFY_VB_HIST_SUB_OFF,
                                 log((double)peak), out, out_n);
    }
    free(cnt);
    if (n_obs)    *n_obs = obs;
    if (peak_out) *peak_out = peak;
    if (mode_bin) *mode_bin = mode;
    return rc;
}

/* ====================================================================== */
/* S4 JOIN                                                                 */

typedef struct { float cost; uint32_t uid; } cand_t;

static int cand_worse(const cand_t *x, const cand_t *y)
{
    if (x->cost != y->cost) return x->cost > y->cost;
    return x->uid > y->uid;              /* deterministic tie-break */
}

static int cmp_cand(const void *a, const void *b)
{
    const cand_t *x = (const cand_t *)a, *y = (const cand_t *)b;
    if (x->cost != y->cost) return x->cost < y->cost ? -1 : 1;
    return x->uid < y->uid ? -1 : (x->uid > y->uid);
}

/* Bounded K-best via a max-heap, NOT a sort of the whole bucket.
 *
 * ⚠ The bucket sizes here are not small: the relation is "every left unit
 * whose own (centre, right) matches this right unit's (left, centre)", and
 * for a common triphone that is thousands of units. Sorting each bucket is
 * O(sum m log m) over 345k right units and does not finish; the heap is
 * O(sum m log K) with K = 12. */
static void heap_sift_down(cand_t *h, size_t n, size_t i)
{
    for (;;) {
        size_t l = 2u * i + 1u, r = l + 1u, big = i;
        if (l < n && cand_worse(&h[l], &h[big])) big = l;
        if (r < n && cand_worse(&h[r], &h[big])) big = r;
        if (big == i) return;
        cand_t t = h[i]; h[i] = h[big]; h[big] = t;
        i = big;
    }
}

static void heap_offer(cand_t *h, size_t *n, size_t k, cand_t v)
{
    if (*n < k) {
        size_t i = (*n)++;
        h[i] = v;
        while (i) {
            size_t p = (i - 1u) / 2u;
            if (!cand_worse(&h[i], &h[p])) break;
            cand_t t = h[i]; h[i] = h[p]; h[p] = t;
            i = p;
        }
        return;
    }
    if (!k) return;
    if (cand_worse(&h[0], &v)) {          /* root is the worst kept */
        h[0] = v;
        heap_sift_down(h, *n, 0);
    }
}

/* Sub-stage clock for S4. Threading the K-best pass bought only 1.29x on a
 * no-backoff build (55.4s -> 42.8s at 24 threads), which says the scoring loop
 * was never the whole story there -- so the stage reports its own parts rather
 * than inviting another guess. WALL time; see the note in spfy_vb_build.c. */
#include <time.h>
#ifdef _OPENMP
#   include <omp.h>
#endif

static double s4_now(void)
{
#ifdef _OPENMP
    return omp_get_wtime();
#else
    return (double)clock() / (double)CLOCKS_PER_SEC;
#endif
}

int spfy_vb_s4_join(const char *vin_path, const char *vdb_path,
                    const spfy_vb_join_cfg *cfg,
                    uint8_t **out, size_t *out_n,
                    spfy_vb_join_stats *st)
{
    memset(st, 0, sizeof *st);
    spfy_vin_t vin = {0};
    spfy_vdb_t vdb = {0};
    spfy_vb_frames_t fr = {0};
    spfy_hash_pair *pairs = NULL;
    spfy_hash_table tbl = {0};
    /* Hoisted so `goto done` never jumps over a declaration it then frees. */
    uint32_t *bucket_head = NULL, *bucket_next = NULL;
    uint32_t *mk_off = NULL, *mk_key = NULL, *memb = NULL, *seen_gen = NULL;
    uint32_t  n_listings = 0;
    cand_t *cand = NULL;

    int rc = spfy_vin_load(vin_path, &vin);
    if (rc != SPFY_OK) { spfy_log_err("s4: vin_load %d", rc); return rc; }
    rc = spfy_vdb_load(vdb_path, &vdb);
    if (rc != SPFY_OK) { spfy_log_err("s4: vdb_load %d", rc); spfy_vin_free(&vin); return rc; }

    spfy_unit_table_t ut;
    rc = spfy_unit_table_load(&vin, &ut);
    if (rc != SPFY_OK) { spfy_log_err("s4: unit_table_load %d", rc); goto done; }
    const uint32_t n_units = ut.n_units;

    /* Membership is read back out of the chunk rather than recomputed: a
     * second derivation that drifts is how the domain and the candidate pools
     * stop agreeing.
     *
     * ⛔⛔ THIS USED TO KEEP ONE KEY PER UNIT (`keys[u] = k`, last writer
     * wins). That is only sound while prsl is a PARTITION, which no vendor's
     * is -- jill lists a unit under 5.94 target contexts on average and up to
     * 245, and once spfy_vb_groups_backoff() exists neither is ours. With
     * multiple listings the single key is whichever group happened to be
     * visited last, so the enumerated relation is a slice of a DIFFERENT
     * unit's neighbourhood. Measured: turning backoff on with the old
     * read-back moved the domain the WRONG WAY, 9.57 -> 8.01 pairs per unit.
     *
     * The pair (l, r) is admissible when SOME group listing l and SOME group
     * listing r are an adjacent-slot key pair -- which is exactly the set the
     * DP can request, because a slot's pool IS a group. */
    {
        spfy_prsl_t prsl;
        rc = spfy_prsl_load(&vin, &prsl);
        if (rc != SPFY_OK) { spfy_log_err("s4: prsl_load %d", rc); goto done; }
        memb = (uint32_t *)calloc(n_units ? n_units : 1u, sizeof *memb);
        if (!memb) { spfy_prsl_free(&prsl); rc = SPFY_E_NOMEM; goto done; }
        for (uint32_t g = 0; g < prsl.n_groups; ++g) {
            uint32_t k = prsl.groups[g].context_key;
            uint32_t l = k / 10000u, r = k % 100u;
            /* Skip the wide groups: their key carries hp_bound sentinels and
             * names no triphone the family relation can use. */
            if (l >= 92u || r >= 92u) continue;
            for (uint32_t i = 0; i < prsl.groups[g].n_candidates; ++i) {
                uint32_t u = spfy_prsl_cand(prsl.groups[g].candidates, i);
                if (u < n_units) { ++memb[u]; ++n_listings; }
            }
        }
        mk_off = (uint32_t *)calloc((size_t)n_units + 1u, sizeof *mk_off);
        mk_key = (uint32_t *)malloc((n_listings ? n_listings : 1u) * sizeof *mk_key);
        if (!mk_off || !mk_key) { spfy_prsl_free(&prsl); rc = SPFY_E_NOMEM; goto done; }
        for (uint32_t u = 0; u < n_units; ++u) mk_off[u + 1u] = mk_off[u] + memb[u];
        memset(memb, 0, (size_t)(n_units ? n_units : 1u) * sizeof *memb);
        for (uint32_t g = 0; g < prsl.n_groups; ++g) {
            uint32_t k = prsl.groups[g].context_key;
            uint32_t l = k / 10000u, r = k % 100u;
            if (l >= 92u || r >= 92u) continue;
            for (uint32_t i = 0; i < prsl.groups[g].n_candidates; ++i) {
                uint32_t u = spfy_prsl_cand(prsl.groups[g].candidates, i);
                if (u < n_units) mk_key[mk_off[u] + memb[u]++] = k;
            }
        }
        spfy_prsl_free(&prsl);
    }

    /* ---- the domain rule, MEASURED off the DP's own requests ----
     *
     * ⚠ THIS REPLACED A GUESS THAT HIT 0 OF 601,768 LOOKUPS. The guess was
     * "a slot keyed (L,C,R) is followed by one keyed (C,R,*)", i.e. the key
     * read as a sliding window. It is not one. A unit's key is the TRIPHONE
     * REPLICATED AT ITS OWN HALF-PARITY: (2a+h, 2p+h, 2b+h). Dumping the
     * pairs the DP actually asks for (SPFY_JOIN_DUMP) shows two families and
     * nothing else:
     *
     *   even -> odd   40.49%   C_r == C_l + 1                    (100% of it)
     *   odd  -> even  59.51%   C_r == R_l - 1 and L_r == C_l - 1 (~96%)
     *
     * which is just half-phone concatenation: L(P)->R(P), then R(P)->L(Q).
     * Festival's clunits builds candidates the same way, and its
     * optimal_couple() returns 0.0 when `u1->prev_unit == u0` -- the same
     * free-continuation rule the vendors encode as cost==0 <=> r==l+1.
     *
     * Family A additionally has key_r - key_l == +10101 for 89% of its
     * members, i.e. the two halves carry the SAME context. Bucketing A on the
     * full key rather than on the centre alone keeps that bucket small; the
     * remaining 11% is what --wide-a exists to buy back.
     *
     * ⚠ The half is read from the KEY's parity, never from uid & 1. On tom,
     * 3,390 of 6,849 recordings start at an odd uid, so uid parity is not the
     * side at all past the first misalignment. */
    const uint32_t NB  = 92u * 92u;
    const uint32_t NKA = 1000000u;          /* max key is 91*10101 = 919,191 */
    const size_t   NBK = (size_t)NKA + NB;
    /* CSR, not a per-unit linked list: with backoff a unit belongs to many
     * buckets and a single `next[u]` can only hold it in one. */
    bucket_head = (uint32_t *)calloc(NBK + 1u, sizeof *bucket_head);
    if (!bucket_head) { rc = SPFY_E_NOMEM; goto done; }
    for (uint32_t u = 0; u < n_units; ++u)
        for (uint32_t i = mk_off[u]; i < mk_off[u + 1u]; ++i) {
            uint32_t k = mk_key[i];
            uint32_t C = (k / 100u) % 100u, R = k % 100u;
            /* A left unit feeds family A through its FULL key and family B
             * through (C, R) alone -- the two lookups S4 has always used. */
            size_t b = (C & 1u) ? ((size_t)NKA + C * 92u + R) : k;
            bucket_head[b + 1u]++;
        }
    for (size_t b = 0; b < NBK; ++b) bucket_head[b + 1u] += bucket_head[b];
    bucket_next = (uint32_t *)malloc((n_listings ? n_listings : 1u)
                                     * sizeof *bucket_next);
    {
        uint32_t *fill = (uint32_t *)calloc(NBK, sizeof *fill);
        if (!bucket_next || !fill) { free(fill); rc = SPFY_E_NOMEM; goto done; }
        for (uint32_t u = 0; u < n_units; ++u)
            for (uint32_t i = mk_off[u]; i < mk_off[u + 1u]; ++i) {
                uint32_t k = mk_key[i];
                uint32_t C = (k / 100u) % 100u, R = k % 100u;
                size_t b = (C & 1u) ? ((size_t)NKA + C * 92u + R) : k;
                bucket_next[bucket_head[b] + fill[b]++] = u;
            }
        free(fill);
    }

    /* Centre-anchored windows: spfy_vb_jcfit measured that the vendor's
     * frames sit LATER than the unit boundary, and centre is the anchor the
     * calibration harness settled on. */
    spfy_vb_cfg_t fcfg;
    spfy_vb_cfg_default(&fcfg);
    fcfg.anchor = SPFY_VB_ANCHOR_CENTER;
    fcfg.zero_f0_dim = cfg->zero_f0_dim;
    fcfg.f0_edge     = cfg->f0_edge;
    fcfg.n_f0_edge   = cfg->n_f0_edge;
    /* The VDB knows its own rate; a hard-coded 8000 here would silently
     * mis-window every frame on a voice built at any other rate. */
    uint32_t sr = cfg->sample_rate ? cfg->sample_rate : vdb.sample_rate;
    if (!sr) sr = 8000u;
    double t_a = s4_now();
    rc = spfy_vb_frames_build_ex(&vin, &vdb, sr, &fcfg, &fr);
    double t_frames = s4_now() - t_a;
    if (rc != SPFY_OK) { spfy_log_err("s4: frames_build %d", rc); goto done; }
    st->n_no_frames = fr.n_missing;
    spfy_jc_t jc;
    spfy_vb_frames_bind(&fr, &jc);
    rc = spfy_jc_derive_weights(&jc, 1.0f);
    if (rc != SPFY_OK) { spfy_log_err("s4: derive_weights %d", rc); goto done; }

    /* ⚠ SIZE THIS FROM THE EXPECTED TOTAL, NOT FROM THE CEILING. This is a
     * 32-bit binary: `n_units * (k_best + 1)` at k_best 400 asks for 183 M
     * pairs up front and the allocation simply fails, which is what killed
     * the first per-key arm. With a per-key budget the real total is one
     * continuation per unit plus k_per_key per prsl LISTING; the loop below
     * still doubles on demand, so an underestimate costs a realloc and
     * nothing else. */
    size_t cap = (size_t)n_units + 16u;
    cap += cfg->k_per_key ? (size_t)cfg->k_per_key * n_listings
                          : (size_t)n_units * cfg->k_best;
    pairs = (spfy_hash_pair *)malloc(cap * sizeof *pairs);
    if (!pairs) { rc = SPFY_E_NOMEM; goto done; }
    size_t n = 0, n_cont = 0;

    /* Natural continuations first, so a continuation is never displaced by
     * the K-best cut. cost == 0 <=> r == l+1 is the vendors' exact
     * invariant, and flag_b is the engine's own same-rec test. */
    for (uint32_t u = 1; u < n_units; ++u) {
        spfy_unit_record_t r;
        if (spfy_unit_record_get(&ut, u, &r) != SPFY_OK || !r.flag_b) continue;
        pairs[n].uid_right = u;
        pairs[n].uid_left  = u - 1u;
        pairs[n].cost      = 0.0f;
        ++n; ++n_cont;
    }

    (void)seen_gen;   /* the scratch is per-thread; see the parallel block */

    /* ⭐⭐ THE BUDGET IS PER (RIGHT UNIT, KEY), NOT PER RIGHT UNIT.
     *
     * Each key a unit is listed under is a DISTINCT slot context in which the
     * DP can choose it, and in each one it will be paired with units from that
     * key's own partner bucket. One K-best heap over the union spends the
     * whole budget on whichever context happens to score best and leaves the
     * others uncached -- which is why widening the relation with backoff made
     * the delivered hit rate FALL on two of three demo texts: the union grew
     * 468 -> 2,479 while K stayed 12.
     *
     * The vendors allocate exactly this way. Row size against the number of
     * prsl listings the right unit has:
     *
     *     listings     jill row mean    tom     ours (flat K)
     *     1                3.22         4.34        12.69
     *     2                5.28         6.73        12.99
     *     3                6.91         8.53        12.99
     *
     * ~1.87 partners per listing for jill, 2.0 for tom, against our constant.
     * `k_per_key` is that number; `k_best` stays as the per-unit ceiling so a
     * unit listed 2,000 times cannot eat the table. */
    const uint32_t kpk = cfg->k_per_key ? cfg->k_per_key : cfg->k_best;

    /* ⭐ PARALLEL, AND EXACTLY REPRODUCIBLE.
     *
     * Right units are independent: each one reads the shared buckets and
     * frames and produces its own row. Nothing is written that another
     * iteration reads. The only reason this could change the output is pair
     * ORDER, and spfy_hash_build() counting-sorts by uid_right and re-sorts
     * every row by uid_left precisely so that "the build is independent of
     * input pair order" -- so threads may append in any order at all. The
     * `--prsl-backoff 0` control (byte-identical to crsmara_syl_a) is what
     * proves it, and it must be re-run after any change here.
     *
     * Scratch is per-thread: the K-best heaps, the generation stamps, and an
     * output buffer that is spliced onto `pairs` once at the end. */
    int par_rc = SPFY_OK;
    t_a = s4_now();
#ifdef _OPENMP
#   pragma omp parallel
#endif
    {
        cand_t   *tc  = (cand_t *)malloc(((size_t)cfg->k_best + 1u) * sizeof *tc);
        cand_t   *tkc = (cand_t *)malloc(((size_t)kpk + 1u) * sizeof *tkc);
        uint32_t *tsg = (uint32_t *)calloc(n_units ? n_units : 1u, sizeof *tsg);
        /* Bucket ids for one right unit, deduped. A unit is listed under at
         * most as many keys as it has memberships. */
        size_t    tb_cap = 64, tb_n = 0;
        size_t   *tb  = (size_t *)malloc(tb_cap * sizeof *tb);
        size_t    tp_cap = 4096, tp_n = 0;
        spfy_hash_pair *tp =
            (spfy_hash_pair *)malloc(tp_cap * sizeof *tp);
        size_t    t_scored = 0;
        int       ok = (tc && tkc && tsg && tb && tp);
        if (!ok) {
#ifdef _OPENMP
#           pragma omp critical
#endif
            par_rc = SPFY_E_NOMEM;
        }

#ifdef _OPENMP
#       pragma omp for schedule(dynamic, 256)
#endif
        for (long ri = 0; ri < (long)n_units; ++ri) {
            if (!ok || par_rc != SPFY_OK) continue;
            const uint32_t r   = (uint32_t)ri;
            const uint32_t gen = r + 1u;
            size_t m = 0;

            /* ⛔⛔ DO NOT DEDUPE THESE BUCKETS. It looks like free money --
             * family B's bucket is keyed on (L+1, C+1) only, so every key of r
             * that shares an L resolves to the SAME bucket, and deduping cut
             * the fill-24 build from 2.43 BILLION scored joins to 1.33 B.
             *
             * It is not free: it is a SELECTION change wearing an
             * optimisation's clothes. A repeated bucket is scanned again with
             * its previous winners already stamped, so it yields its NEXT best
             * partners -- i.e. a bucket reached through n keys gets n helpings
             * of budget. Removing that re-weights the table away from family
             * B, and the accent on "the NAtional weather service" collapsed
             * from +8.24 st to +4.5 on every arm tried (fill 12, 24, 48, 96).
             *
             * ⚠ The `--prsl-backoff 0` byte-identity control CANNOT catch this:
             * with a partitioned prsl every unit has one key, so no bucket is
             * ever repeated and the two behaviours coincide exactly. The
             * accent measurement is the only gate that sees it. */
            tb_n = 0;
            for (uint32_t ir = mk_off[r]; ir < mk_off[r + 1u]; ++ir) {
                uint32_t k = mk_key[ir];
                uint32_t L = k / 10000u, C = (k / 100u) % 100u, R = k % 100u;
                if (L >= 92u || C >= 92u || R >= 92u) continue;
                /* All three components share the unit's half-parity. A key
                 * that does not is a wide group leaking through, and
                 * subtracting 10101 would borrow across components into a
                 * different triphone. */
                if (((L ^ C) | (C ^ R)) & 1u) continue;
                size_t b;
                if (C & 1u) {
                    /* Family A: r is an R half; its lefts are L halves of the
                     * same phone in this slot's context, key_l = key - 10101. */
                    if (k < 10101u) continue;
                    b = k - 10101u;
                } else {
                    /* Family B: r is an L half of phone B with left-context P;
                     * its lefts are R halves of P whose right-context is B, so
                     * C_l == L_r + 1 and R_l == C_r + 1. */
                    b = (size_t)NKA + (L + 1u) * 92u + (C + 1u);
                }
                if (tb_n == tb_cap) {
                    size_t nc = tb_cap * 2u;
                    size_t *nv = (size_t *)realloc(tb, nc * sizeof *nv);
                    if (!nv) { ok = 0; break; }
                    tb = nv; tb_cap = nc;
                }
                tb[tb_n++] = b;
            }

            for (size_t bi = 0; bi < tb_n; ++bi) {
                const size_t b = tb[bi];
                /* Best kpk from THIS bucket... */
                size_t km = 0;
                for (uint32_t q = bucket_head[b]; q < bucket_head[b + 1u]; ++q) {
                    uint32_t l = bucket_next[q];
                    if (l == r || r == l + 1u) continue;  /* a continuation */
                    if (tsg[l] == gen) continue;          /* another key had it */
                    cand_t v;
                    v.uid  = l;
                    v.cost = spfy_jc_raw(&jc, l, r);
                    heap_offer(tkc, &km, kpk, v);
                    ++t_scored;
                }
                /* ...then into the unit's row, under the per-unit ceiling. The
                 * stamp is set HERE, not at scoring time: a left unit that
                 * lost this bucket's cut must stay eligible for the next,
                 * where its competition is different. */
                for (size_t i = 0; i < km; ++i) {
                    if (tsg[tkc[i].uid] == gen) continue;
                    tsg[tkc[i].uid] = gen;
                    heap_offer(tc, &m, cfg->k_best, tkc[i]);
                }
            }
            if (!m) continue;
            if (m > 1) qsort(tc, m, sizeof *tc, cmp_cand);
            if (tp_n + m > tp_cap) {
                size_t nc = tp_cap;
                while (nc < tp_n + m) nc *= 2u;
                spfy_hash_pair *nv =
                    (spfy_hash_pair *)realloc(tp, nc * sizeof *nv);
                if (!nv) { ok = 0; continue; }
                tp = nv; tp_cap = nc;
            }
            for (size_t i = 0; i < m; ++i) {
                tp[tp_n].uid_right = r;
                tp[tp_n].uid_left  = tc[i].uid;
                tp[tp_n].cost      = (cfg->mode == SPFY_VB_JC_CONST)
                                   ? cfg->const_cost
                                   : tc[i].cost * cfg->join_w + cfg->join_off;
                if (tp[tp_n].cost < 0.0f) tp[tp_n].cost = 0.0f;
                /* ⚠ Never let a computed cost land on 0: the vendors reserve
                 * 0 for continuations exactly, and a zero here is a free
                 * join. */
                if (tp[tp_n].cost == 0.0f) tp[tp_n].cost = 1e-6f;
                ++tp_n;
            }
        }

#ifdef _OPENMP
#       pragma omp critical
#endif
        {
            if (!ok && par_rc == SPFY_OK) par_rc = SPFY_E_NOMEM;
            if (par_rc == SPFY_OK && tp_n) {
                if (n + tp_n > cap) {
                    size_t nc = cap ? cap : 1u;
                    while (nc < n + tp_n) nc *= 2u;
                    spfy_hash_pair *nv =
                        (spfy_hash_pair *)realloc(pairs, nc * sizeof *nv);
                    if (!nv) par_rc = SPFY_E_NOMEM;
                    else { pairs = nv; cap = nc; }
                }
                if (par_rc == SPFY_OK) {
                    memcpy(pairs + n, tp, tp_n * sizeof *tp);
                    n += tp_n;
                }
            }
            st->n_scored += t_scored;
        }
        free(tc); free(tkc); free(tsg); free(tb); free(tp);
    }
    if (par_rc != SPFY_OK) { rc = par_rc; goto done; }
    double t_score = s4_now() - t_a;
    t_a = s4_now();

    rc = spfy_hash_build(pairs, n, n_units, SPFY_HASH_ORDER_FFD, &tbl);
    if (rc != SPFY_OK) { spfy_log_err("s4: hash_build %d", rc); goto done; }
    rc = spfy_hash_serialise(&tbl, out, out_n);
    if (rc != SPFY_OK) { spfy_log_err("s4: serialise %d", rc); goto done; }
    printf("  S4 time: frames %.1fs  score %.1fs  pack+write %.1fs\n",
           t_frames, t_score, s4_now() - t_a);

    st->n_pairs = n;
    st->n_cont  = n_cont;
    st->n_rows  = tbl.n_rows;
    st->n_cells = tbl.n_cells;
    rc = SPFY_OK;

done:
    free(cand);
    free(pairs);
    free(bucket_head);
    free(bucket_next);
    free(mk_off);
    free(mk_key);
    free(memb);
    free(seen_gen);
    spfy_hash_table_free(&tbl);
    spfy_vb_frames_free(&fr);
    spfy_vdb_free(&vdb);
    spfy_vin_free(&vin);
    return rc;
}
