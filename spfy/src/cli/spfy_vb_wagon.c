/* spfy_vb_wagon -- grow (or check) the `durt` / `f0tr` CART trees.
 *
 * A port of Festival's wagon (speech_tools/stats/wagon/wagon.cc) onto the
 * Speechify tree format. See vb_wagon.h for the algorithm and for why the
 * QUESTIONS come from the template while the TREE is grown from our corpus.
 *
 * Two gates, and neither is optional:
 *
 *   --roundtrip  parse the vendor's own chunk and write it straight back.
 *                Byte-identical or the record layout is wrong, and every
 *                tree generated with it would be wrong the same way.
 *   --grow       grow on a training split and report held-out RMSE beside
 *                THE VENDOR'S OWN TREE scored on the identical held-out
 *                samples. That is the ceiling; beating zero means nothing.
 *
 *   spfy_vb_wagon --vin V --chunk durt --roundtrip
 *   spfy_vb_wagon --vin V --chunk durt --grow --min-cluster 50
 */

#include "../vb/vb_wagon.h"
#include "../vb/vb_io.h"
#include "../voice/unit_table.h"
#include "../voice/voice.h"
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

/* The record layout, the question set and the flattener all moved into
 * src/vb/vb_wagon.c when the BUILDER started generating these chunks -- two
 * copies of a format decoder is how one of them goes stale. */
typedef spfy_wgn_rec  node_rec;
typedef spfy_wgn_tree tree_rec;
typedef spfy_wgn_qset ques_set;

#define parse_tree_rec spfy_wgn_tree_parse
#define write_tree_rec spfy_wgn_tree_write
#define parse_ques_set spfy_wgn_qset_parse

static void flatten(const spfy_wgn_node *n, node_rec *v, uint32_t *next,
                    uint32_t self)
{
    spfy_wgn_flatten(n, v, next, self);
}

/* --- the labl inside trhd -------------------------------------------- */

/* ⚠ u16 LENGTH PREFIX, NOT NUL TERMINATION. Reading these as C strings
 * shifts every name by one, which renumbers the LEFTlabel/RIGHTlabel value
 * sets and made the two vendors' inventories look 138/161 alike when they are
 * in fact 154/154. */
static size_t parse_labl(const uint8_t *d, size_t dn, char (*out)[8],
                         size_t max)
{
    if (dn < 4) return 0;
    uint32_t n = rd_u32(d);
    size_t q = 4, got = 0;
    while (q + 2u <= dn && got < n && got < max) {
        uint32_t ln = (uint32_t)d[q] | ((uint32_t)d[q + 1] << 8);
        if (q + 2u + ln > dn) break;
        size_t c = ln < 7u ? ln : 7u;
        memcpy(out[got], d + q + 2u, c);
        out[got][c] = 0;
        ++got;
        q += 2u + ln;
    }
    return got;
}

int main(int argc, char **argv)
{
    const char *vin_path = NULL, *chunk = "durt", *out_path = NULL;
    /* ⚠ WHICH FIELD durt PREDICTS IS THE QUESTION, not a setting. If the
     * vendor's own tree cannot beat predicting the training mean, then either
     * this is the wrong target or the question keys do not map onto the unit
     * fields the way the legacy pipeline assumed. Sweep it. */
    /* NULL = "not given"; resolved per chunk below, because the two trees
     * predict different quantities and a shared default silently scored f0tr
     * against durt's predictee. */
    const char *predictee = NULL;
    int do_rt = 0, do_grow = 0, min_cluster = 50, holdout = 5;
    /* --check-ques is the ownership gate: our authored inventory has to
     * CONTAIN the vendor's, or a tree grown over it cannot express what
     * theirs could. --own-ques then grows over ours instead of the donor's. */
    int do_checkq = 0, own_ques = 0;

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        int has = (i + 1 < argc);
        if (!strcmp(a, "--vin") && has) { vin_path = argv[++i]; continue; }
        if (!strcmp(a, "--chunk") && has) { chunk = argv[++i]; continue; }
        if (!strcmp(a, "--roundtrip")) { do_rt = 1; continue; }
        if (!strcmp(a, "--grow")) { do_grow = 1; continue; }
        if (!strcmp(a, "--out") && has) { out_path = argv[++i]; do_grow = 1; continue; }
        if (!strcmp(a, "--min-cluster") && has) { min_cluster = atoi(argv[++i]); continue; }
        if (!strcmp(a, "--holdout") && has) { holdout = atoi(argv[++i]); continue; }
        if (!strcmp(a, "--predictee") && has) { predictee = argv[++i]; continue; }
        if (!strcmp(a, "--check-ques")) { do_checkq = 1; continue; }
        if (!strcmp(a, "--own-ques")) { own_ques = 1; continue; }
        fprintf(stderr, "unknown option %s\n", a); return 2;
    }
    if (!vin_path) {
        fprintf(stderr,
            "usage: %s --vin V [--chunk durt|f0tr] [--roundtrip] [--grow]\n"
            "  --min-cluster N  wagon's wgn_min_cluster_size (default 50)\n"
            "  --holdout N      every Nth unit is held out (default 5)\n"
            "  --check-ques     does OUR authored inventory contain this\n"
            "                   vendor's? The ownership gate for the trees.\n"
            "  --own-ques       grow over our inventory, not the donor's\n",
            argv[0]);
        return 2;
    }

    spfy_vb_riff riff;
    int rc = spfy_vb_riff_load(vin_path, &riff);
    if (rc != SPFY_OK) { fprintf(stderr, "riff_load %d\n", rc); return 1; }
    const spfy_vb_chunk *ck = spfy_vb_riff_get(&riff, chunk);
    if (!ck) { fprintf(stderr, "no %s chunk\n", chunk); return 1; }
    printf("%s: %s is %zu bytes\n", vin_path, chunk, ck->n);

    /* Split into trhd / ques / tree, keeping every subchunk's bytes so the
     * round-trip can put the container back exactly as it was. */
    tree_rec trees[128];
    size_t tree_at[128], tree_len[128], n_trees = 0;
    const uint8_t *ques_d = NULL, *labl_d = NULL;
    size_t ques_n = 0, labl_n = 0;
    memset(trees, 0, sizeof trees);
    {
        size_t pos = 0;
        char id[5];
        const uint8_t *d;
        size_t dn;
        while (spfy_vb_subchunk(ck->data, ck->n, &pos, id, &d, &dn)) {
            if (!memcmp(id, "tree", 4) && n_trees < 128) {
                tree_at[n_trees] = (size_t)(d - ck->data);
                tree_len[n_trees] = dn;
                if (parse_tree_rec(d, dn, &trees[n_trees]) != SPFY_OK) {
                    fprintf(stderr, "tree %zu failed to parse\n", n_trees);
                    return 1;
                }
                ++n_trees;
            } else if (!memcmp(id, "ques", 4)) {
                ques_d = d; ques_n = dn;
            } else if (!memcmp(id, "trhd", 4)) {
                size_t p2 = 0;
                char id2[5];
                const uint8_t *d2;
                size_t d2n;
                while (spfy_vb_subchunk(d, dn, &p2, id2, &d2, &d2n)) {
                    if (!memcmp(id2, "ques", 4)) { ques_d = d2; ques_n = d2n; }
                    if (!memcmp(id2, "labl", 4)) { labl_d = d2; labl_n = d2n; }
                }
            }
        }
    }
    if (!n_trees || !ques_d) { fprintf(stderr, "no trees or questions\n"); return 1; }
    ques_set qs;
    memset(&qs, 0, sizeof qs);
    if (parse_ques_set(ques_d, ques_n, &qs) != SPFY_OK) {
        fprintf(stderr, "question set failed to parse\n"); return 1;
    }
    size_t tot_nodes = 0, tot_leaves = 0;
    for (size_t i = 0; i < n_trees; ++i) {
        tot_nodes += trees[i].n_nodes;
        for (size_t k = 0; k < trees[i].n_nodes; ++k)
            tot_leaves += trees[i].n[k].is_leaf ? 1u : 0u;
    }
    printf("  %zu trees, %zu questions, %zu nodes, %zu leaves\n",
           n_trees, qs.n, tot_nodes, tot_leaves);

    static char labels[256][8];
    size_t n_lab = parse_labl(labl_d, labl_n, labels, 256);
    ques_set ours;
    memset(&ours, 0, sizeof ours);
    /* ⚠ The VENDOR's tree indexes the VENDOR's inventory. Swapping `qs` for
     * ours would silently re-point every one of their branch nodes at a
     * different question and turn the ceiling into noise, so the two sets are
     * kept apart: `gq` grows and predicts OUR tree, `qs` walks theirs. */
    ques_set *gq = &qs;

    if (do_rt) {
        size_t bad = 0;
        for (size_t i = 0; i < n_trees; ++i) {
            spfy_vb_buf b = {0};
            if (write_tree_rec(&trees[i], &b) != SPFY_OK) { ++bad; continue; }
            if (b.n != tree_len[i]
                || memcmp(b.p, ck->data + tree_at[i], b.n) != 0) {
                if (bad < 3)
                    printf("    tree %zu DIFFERS (%zu vs %zu bytes)\n",
                           i, b.n, tree_len[i]);
                ++bad;
            }
            spfy_vb_buf_free(&b);
        }
        printf("  ROUNDTRIP: %zu/%zu trees byte-identical -- %s\n",
               n_trees - bad, n_trees,
               bad ? "LAYOUT IS WRONG" : "record layout confirmed");
        if (bad) return 1;
    }

    if (!do_grow && !do_checkq) return 0;

    /* ---- grow ---- */
    spfy_vin_t vin = {0};
    spfy_unit_table_t ut = {0};
    if (spfy_vin_load(vin_path, &vin) != SPFY_OK
        || spfy_unit_table_load(&vin, &ut) != SPFY_OK) {
        fprintf(stderr, "cannot load units\n"); return 1;
    }
    /* durt scores unit_mem[+0x12] = f0_context; f0tr scores f0_start. Resolve
     * before the banner so the log names what was actually used. */
    if (!predictee) predictee = !strcmp(chunk, "f0tr") ? "f0start" : "f0ctx";
    printf("\n  growing on %u units (min_cluster %d, holdout 1/%d, "
           "predictee %s)\n", ut.n_units, min_cluster, holdout, predictee);

    /* Feature ids are the on-disk question keys; the mapping is the legacy
     * pipeline's byte offsets, kept because that is what the shipped trees
     * were trained against. */
    spfy_wgn_sample *all = (spfy_wgn_sample *)
        calloc(ut.n_units ? ut.n_units : 1u, sizeof *all);
    uint8_t *phone = (uint8_t *)calloc(ut.n_units ? ut.n_units : 1u, 1);
    if (!all || !phone) return 1;
    /* ⛔ f0tr IS ONE GLOBAL TREE, NOT ONE TREE PER PHONE.
     * `durt` carries n_labels trees and the engine indexes them by
     * phone_center, so its samples partition by phone. `f0tr` carries exactly
     * ONE tree that the engine walks for every unit. Partitioning its samples
     * the same way trained and scored it on phone_center == 0 alone -- 719 of
     * 185,475 jill units -- which is why its sample count looked absurd. */
    const int f0tr_mode = !strcmp(chunk, "f0tr");

    uint32_t n_all = 0;
    for (uint32_t u = 0; u < ut.n_units; ++u) {
        spfy_unit_record_t r;
        if (spfy_unit_record_get(&ut, u, &r) != SPFY_OK) continue;
        /* ⛔ f0tr NEVER SEES AN UNVOICED UNIT. anchor_score.c:1077 takes the
         * flat `w_f0_miss` branch when f0_start == 0 and does not walk the
         * tree at all, so training on those units fits the tree to a
         * population the engine will never ask it about -- 22.8% of jill's
         * units, and enough variance to put her OWN tree below the
         * mean-only floor (61.63 against 54.79). */
        if (f0tr_mode && r.f0_start == 0u) continue;
        spfy_wgn_sample *s = &all[n_all];
        memset(s, 0, sizeof *s);
        /* The q_type table decoded from the engine's own CART walker
         * (FUN_08e87c90), per spfy_synth.c:1156 and build_graph.h:
         *
         *   1 sylType   2 sylInPhrase   3 LEFT label   4 RIGHT label
         *   5 halfphones-in-syllable    8 wordInPhrase 9 phoneInSyl
         *
         * ⚠ THE LEGACY MAPPING IN vb_stages.c IS WRONG IN THREE WAYS: it has
         * q1/q2 swapped, uses sylInWord for q8 where the walker reads
         * wordInPhrase, and takes q3/q4 from phone_ctx[0]/[1] when the four
         * stored context slots are (pp2, pp1, pn1, pn2) -- so LEFT and RIGHT
         * are [1] and [2]. Under that mapping the vendors' OWN trees score
         * worse than predicting the training mean, for every candidate
         * predictee, which is how the error surfaced. */
        /* ⭐ f0tr AND durt DO NOT SHARE A FEATURE MAP, and they disagree in
         * BOTH directions. cart_feat (spfy_synth.c:1138-1155) clamps
         * q3/q4/q5/q9 to 0 when the tree is f0tr -- it is syllable-level --
         * and clamps q7 to 0 only for durt, where the engine's walker zeroes
         * EBX before the dispatch. So f0tr reads q7 and durt does not.
         * Filling one tree with the other's map is the same defect that had
         * durt routing every unit down one branch of q5/q9. */
        const int is_f0tr = f0tr_mode;
        s->feat[1] = r.sp_syl_type;                 /* q1 <- sp[1] */
        s->feat[2] = r.sp_syl_in_phrase;            /* q2 <- sp[0] */
        if (is_f0tr) {
            s->feat[7] = r.sp_word_in_phrase;       /* q7 <- sp[2], LIVE here */
        } else {
            s->feat[3] = r.phone_ctx[1];
            s->feat[4] = r.phone_ctx[2];
        }
        /* q5 is half-phones-in-current-syllable, computed at slot time and
         * absent from the unit record. Left 0; 15 of tom's 154 durt
         * questions read it, so this is a known gap, not a silent one. */
        s->feat[5] = 0;
        /* ⚠ WHICH SLOT q8 READS IS AN OPEN QUESTION, SO IT IS A SWITCH.
         *
         * The walker's own code is `case 8: v = c->slot->sp[3]`
         * (spfy_synth.c:1177), but the record field NAMES are inverted
         * relative to cart_feat's vocabulary: disk 0x0E pairs with target
         * sp[2] -- proved by anchor_score's cand_bytes[2] -- while being
         * called sp_word_in_phrase, which is cart_feat's name for sp[3].
         * The previous correction chose by NAME and so may have taken the
         * wrong slot.
         *
         * SETTLED by this very switch: holding the vendor tree fixed and
         * varying only the map, held-out RMSE went jill 21.785 -> 21.184 and
         * tom 19.539 -> 19.123 (floors 22.680 / 20.705). Both improve, so
         * sp[3] is the slot they trained on and is now the DEFAULT.
         * SPFY_WGN_Q8=2 restores the old reading to re-run the comparison. */
        static int q8_src = -1;
        if (q8_src < 0) {
            const char *e = getenv("SPFY_WGN_Q8");
            q8_src = (e && *e) ? atoi(e) : 3;
        }
        s->feat[8] = (q8_src == 3) ? r.sp_syl_in_word : r.sp_word_in_phrase;
        if (!is_f0tr) s->feat[9] = r.sp_phone_in_syl;   /* clamped for f0tr */
        if      (!strcmp(predictee, "dur"))     s->y = (float)r.dur_like;
        else if (!strcmp(predictee, "logdur"))  s->y = (float)log((double)r.dur_like + 1.0);
        else if (!strcmp(predictee, "f0start")) s->y = (float)r.f0_start;
        else if (!strcmp(predictee, "f0mid"))   s->y = (float)r.f0_mid;
        else if (!strcmp(predictee, "f0end"))   s->y = (float)r.f0_end;
        else                                    s->y = (float)r.f0_context;
        phone[n_all] = r.phone_center;
        ++n_all;
    }

    /* Our own inventory. Built HERE and not at parse time because the ranked
     * ordinal subsets are derived from the samples, exactly as Festival's
     * construct_class_ques_subset derives them. */
    if (n_lab) {
        /* phoneInSyl only exists in v100008; ask for it whenever the vendor
         * did, so the containment check is like for like. */
        int wants_q9 = 0;
        for (size_t i = 0; i < qs.n; ++i) if (qs.q[i].key == 9u) wants_q9 = 1;
        if (spfy_wgn_qset_build(labels, n_lab, wants_q9, all, n_all,
                                &ours) != SPFY_OK) {
            fprintf(stderr, "qset_build failed\n"); return 1;
        }
    }

    if (do_checkq) {
        if (!n_lab) { fprintf(stderr, "  no labl inside trhd\n"); return 1; }
        size_t miss = 0;
        for (size_t i = 0; i < qs.n; ++i) {
            if (spfy_wgn_qset_has(&ours, &qs.q[i])) continue;
            if (miss < 12) {
                printf("    MISSING key %u n=%u :", qs.q[i].key, qs.q[i].n);
                for (uint32_t k = 0; k < qs.q[i].n; ++k) {
                    uint32_t v = qs.q[i].val[k];
                    if ((qs.q[i].key == 3u || qs.q[i].key == 4u)
                        && v < n_lab) printf(" %s", labels[v]);
                    else printf(" %u", v);
                }
                printf("\n");
            }
            ++miss;
        }
        printf("  CHECK-QUES: ours holds %zu questions and covers %zu of "
               "%zu of this vendor's -- %s\n",
               ours.n, qs.n - miss, qs.n,
               miss ? "NOT A SUPERSET" : "SUPERSET, safe to author our own");
        if (!do_grow) return miss ? 1 : 0;
    }

    if (own_ques) {
        if (!ours.n) {
            fprintf(stderr, "no labl to author questions from\n"); return 1;
        }
        printf("  ⭐ growing over OUR OWN %zu questions, not the donor's %zu\n",
               ours.n, qs.n);
        gq = &ours;
    }

    spfy_wgn_cfg cfg;
    cfg.min_cluster = (uint32_t)(min_cluster > 0 ? min_cluster : 50);
    cfg.balance = 0.0f;

    double se_ours = 0.0, se_theirs = 0.0, se_base = 0.0;
    size_t n_eval = 0, our_nodes = 0, our_leaves = 0;

    spfy_wgn_sample *train = (spfy_wgn_sample *)malloc((size_t)n_all * sizeof *train);
    spfy_wgn_sample *test  = (spfy_wgn_sample *)malloc((size_t)n_all * sizeof *test);
    if (!train || !test) return 1;
    tree_rec *grown = NULL;
    if (out_path) {
        grown = (tree_rec *)calloc(n_trees, sizeof *grown);
        if (!grown) return 1;
    }

    for (size_t t = 0; t < n_trees; ++t) {
        size_t ntr = 0, nte = 0;
        double tr_sum = 0.0;
        for (uint32_t u = 0; u < n_all; ++u) {
            if (!f0tr_mode && phone[u] != (uint8_t)t) continue;
            if (holdout > 1 && (u % (uint32_t)holdout) == 0u) test[nte++] = all[u];
            else { train[ntr] = all[u]; tr_sum += all[u].y; ++ntr; }
        }
        if (!ntr || !nte) continue;
        spfy_wgn_node *root = NULL;
        if (spfy_wgn_grow(train, ntr, gq->q, gq->n, &cfg, &root) != SPFY_OK) continue;
        our_nodes  += spfy_wgn_nodes(root);
        our_leaves += spfy_wgn_leaves(root);
        double tr_mean = tr_sum / (double)ntr;

        if (grown) {
            size_t nn = spfy_wgn_nodes(root);
            grown[t].n = (node_rec *)calloc(nn ? nn : 1u, sizeof *grown[t].n);
            if (grown[t].n) {
                grown[t].n_nodes = nn;
                uint32_t next = 1;
                flatten(root, grown[t].n, &next, 0);
            }
        }

        for (size_t k = 0; k < nte; ++k) {
            float v;
            double p = spfy_wgn_predict(root, gq->q, test[k].feat, &v);
            double d = p - test[k].y;
            se_ours += d * d;

            /* The vendor's own tree, walked over the same sample. */
            const tree_rec *vt = &trees[t];
            size_t ni = 0;
            for (size_t guard = 0; guard <= vt->n_nodes; ++guard) {
                if (ni >= vt->n_nodes || vt->n[ni].is_leaf) break;
                uint32_t qi = vt->n[ni].qi;
                int hit = (qi < qs.n) ? spfy_wgn_ask(&qs.q[qi], test[k].feat) : 0;
                ni = hit ? (uint32_t)vt->n[ni].yes : vt->n[ni].no;
            }
            double pv = (ni < vt->n_nodes && vt->n[ni].is_leaf)
                      ? vt->n[ni].mean : tr_mean;
            double dv = pv - test[k].y;
            se_theirs += dv * dv;

            /* Predict the training mean and nothing else: the floor any tree
             * has to beat to have earned its nodes. */
            double db = tr_mean - test[k].y;
            se_base += db * db;
            ++n_eval;
        }
        spfy_wgn_free(root);
    }

    if (!n_eval) { printf("  nothing evaluated\n"); return 1; }
    double ro = sqrt(se_ours / (double)n_eval);
    double rt = sqrt(se_theirs / (double)n_eval);
    double rb = sqrt(se_base / (double)n_eval);
    printf("  held-out samples: %zu\n", n_eval);
    printf("  our tree   : %zu nodes, %zu leaves,  RMSE %.4f\n",
           our_nodes, our_leaves, ro);
    printf("  VENDOR tree: %zu nodes, %zu leaves,  RMSE %.4f   <- the ceiling\n",
           tot_nodes, tot_leaves, rt);
    printf("  mean-only  : RMSE %.4f   <- the floor\n", rb);
    printf("  %s\n",
           (ro <= rt * 1.05 && ro < rb)
           ? "GROWS A COMPETITIVE TREE"
           : "does NOT match the vendor tree yet");

    if (grown && own_ques) {
        /* Nothing of the donor survives here: our labels, our questions, our
         * topology, our leaf means and variances. */
        uint8_t *body = NULL;
        size_t body_n = 0;
        const size_t old_n = ck->n;
        rc = spfy_wgn_chunk_write(labels, n_lab, gq, grown, n_trees,
                                  &body, &body_n);
        if (rc == SPFY_OK) rc = spfy_vb_riff_set(&riff, chunk, body, body_n);
        if (rc == SPFY_OK) rc = spfy_vb_riff_save(&riff, out_path);
        if (rc != SPFY_OK) { fprintf(stderr, "write failed %d\n", rc); return 1; }
        printf("  wrote %s with a FULLY OWN %s (%zu B, was %zu) -- labels, "
               "questions, topology and leaves\n",
               out_path, chunk, body_n, old_n);
        return 0;
    }

    if (grown) {
        /* Rebuild the whole chunk: the template's trhd/ques verbatim -- the
         * question inventory is a language asset, not ours to reinvent --
         * with our own `tree` subchunks in place of the template's. */
        spfy_vb_buf b = {0};
        size_t pos = 0, ti = 0;
        /* ⚠ Capture the old size BEFORE the riff_set below: `ck` points into
         * the riff, so reading ck->n afterwards reports the NEW length and the
         * log claimed "136488 B, was 136488" for a chunk that grew 5x. */
        const size_t old_n = ck->n;
        char id[5];
        const uint8_t *d;
        size_t dn;
        rc = SPFY_OK;
        while (rc == SPFY_OK
               && spfy_vb_subchunk(ck->data, ck->n, &pos, id, &d, &dn)) {
            spfy_vb_buf body = {0};
            const uint8_t *src = d;
            size_t srcn = dn;
            if (!memcmp(id, "tree", 4) && ti < n_trees) {
                if (grown[ti].n_nodes) {
                    rc = write_tree_rec(&grown[ti], &body);
                    src = body.p; srcn = body.n;
                }
                ++ti;
            }
            if (rc == SPFY_OK) rc = spfy_vb_buf_put(&b, id, 4);
            if (rc == SPFY_OK) rc = spfy_vb_buf_u32(&b, (uint32_t)srcn);
            if (rc == SPFY_OK) rc = spfy_vb_buf_put(&b, src, srcn);
            if (rc == SPFY_OK && (srcn & 1u)) rc = spfy_vb_buf_u8(&b, 0);
            spfy_vb_buf_free(&body);
        }
        if (rc == SPFY_OK) {
            rc = spfy_vb_riff_set(&riff, chunk, b.p, b.n);
            if (rc == SPFY_OK) rc = spfy_vb_riff_save(&riff, out_path);
            if (rc == SPFY_OK)
                printf("  wrote %s with our own %s (%zu B, was %zu)\n",
                       out_path, chunk, b.n, old_n);
        }
        if (rc != SPFY_OK) { fprintf(stderr, "write failed %d\n", rc); return 1; }
    }
    return 0;
}
