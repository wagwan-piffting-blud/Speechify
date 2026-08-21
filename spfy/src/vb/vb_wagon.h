#ifndef SPFY_VB_WAGON_H
#define SPFY_VB_WAGON_H

#include <stddef.h>
#include <stdint.h>

#include "vb_io.h"

/* CART growing, ported from Festival's `wagon`
 * (speech_tools/stats/wagon/wagon.cc). This is what generates the `durt` and
 * `f0tr` trees.
 *
 * The algorithm, verbatim from wagon_split / score_question_set / WImpurity:
 *
 *   impurity of a node   = variance(y) * n_samples        (float predictee)
 *   score of a question  = (impurity_yes + impurity_no) / 2
 *                          or HUGE if either side < min_cluster
 *   split iff            best_score < HUGE && best_score < node impurity
 *
 * ⭐ THE QUESTIONS ARE A LANGUAGE ASSET, AND WE AUTHOR OUR OWN.
 *
 * Re-measured 2026-08-21 with the label names read correctly -- the `labl`
 * inside `trhd` is u16-LENGTH-PREFIXED, not NUL-terminated, and parsing it as
 * C strings shifts every name by one and makes the vendors look like they
 * disagree. Resolved to phone NAMES:
 *
 *     durt: 154 of tom's 154 questions are also jill's
 *     f0tr:  22 of tom's  22 questions are also jill's
 *
 * jill's 7 extras are all `phoneInSyl`, a column v100006 does not carry, so
 * tom could not have asked them. The inventory is the SAME across speakers:
 * singletons for every label, ordinal singletons and prefixes for the
 * position features, and eleven phonetic classes -- 14 full vowels, 19
 * vowels-plus-syllabics, 33 voiced, reduced {ix ax}, nasals, voiced stops,
 * voiceless fricatives, voiced fricatives, voiceless stops with and without
 * hh, and two vowel subsets.
 *
 * So it is not speaker data and it need not be inherited: those classes are
 * ARPABET phonetics, and spfy_wgn_qset_build writes them out directly.
 * `spfy_vb_wagon --check-ques` proves the generated inventory CONTAINS a
 * vendor's, which is the acceptance test -- a tree grown over a superset can
 * express anything theirs could.
 *
 * Festival's own class-question builders are the two special cases of this
 * inventory: construct_class_ques emits one value ("f is x") and
 * construct_class_ques_subset a set ("f in {...}"). The vendors use both --
 * 117 of tom's 154 durt questions are single-valued. */

#define SPFY_WGN_MAX_FEAT 16

typedef struct {
    uint32_t feat[SPFY_WGN_MAX_FEAT];
    float    y;
} spfy_wgn_sample;

/* One question from the fixed inventory. */
typedef struct {
    uint8_t         key;      /* which feature it reads */
    uint32_t        n;
    const uint32_t *val;      /* the value set; membership is the test */
} spfy_wgn_ques;

typedef struct {
    uint32_t min_cluster;     /* wgn_min_cluster_size; Festival default 50,
                                 clunits passes wagon_cluster_size (10) */
    float    balance;         /* wgn_balance; 0 disables, as in wagon */
} spfy_wgn_cfg;

typedef struct spfy_wgn_node {
    int                    is_leaf;
    uint32_t               qi;       /* index into the question inventory */
    struct spfy_wgn_node  *yes, *no;
    float                  mean, var;
    uint32_t               n;
} spfy_wgn_node;

int    spfy_wgn_grow(spfy_wgn_sample *s, size_t n,
                     const spfy_wgn_ques *q, size_t n_q,
                     const spfy_wgn_cfg *cfg, spfy_wgn_node **out);
void   spfy_wgn_free(spfy_wgn_node *n);
size_t spfy_wgn_nodes(const spfy_wgn_node *n);
size_t spfy_wgn_leaves(const spfy_wgn_node *n);

/* Membership test, matching the engine's own question evaluation. */
int    spfy_wgn_ask(const spfy_wgn_ques *q, const uint32_t *feat);

/* Walks to a leaf and returns its mean; *var gets the leaf variance. */
float  spfy_wgn_predict(const spfy_wgn_node *root, const spfy_wgn_ques *q,
                        const uint32_t *feat, float *var);

/* ---------------------------------------------------------------------- */
/* The on-disk form: trhd { labl, ques } followed by one `tree` per label.  */
/*                                                                          */
/* A node record is 16 bytes for a branch and 20 for a leaf, discriminated  */
/* on the SIGN of the second word. Round-tripped byte-identical against     */
/* both vendors' durt (46/46) and f0tr (1/1) before anything was generated. */

typedef struct {
    uint32_t idx;
    int32_t  yes;        /* < 0 marks a leaf */
    uint32_t no, qi;
    float    mean, var;
    int      is_leaf;
} spfy_wgn_rec;

typedef struct {
    spfy_wgn_rec *n;
    size_t        n_nodes;
} spfy_wgn_tree;

int  spfy_wgn_tree_parse(const uint8_t *d, size_t dn, spfy_wgn_tree *out);
int  spfy_wgn_tree_write(const spfy_wgn_tree *t, spfy_vb_buf *b);
void spfy_wgn_tree_free(spfy_wgn_tree *t);
/* Grown tree -> on-disk numbering. `v` must hold spfy_wgn_nodes(n) records;
 * call with *next = 1 and self = 0. */
void spfy_wgn_flatten(const spfy_wgn_node *n, spfy_wgn_rec *v,
                      uint32_t *next, uint32_t self);

typedef struct {
    spfy_wgn_ques *q;
    uint32_t      *store;    /* the value sets, packed back to back */
    size_t         n;
} spfy_wgn_qset;

int  spfy_wgn_qset_parse(const uint8_t *d, size_t n, spfy_wgn_qset *out);
/* OUR inventory, authored from the label names. `with_phone_in_syl` adds the
 * q9 questions a v100008 voice can answer and a v100006 one cannot.
 * `labels[i]` is the name at LABL index i.
 *
 * ⭐ `sam`/`n_sam` optionally add Festival's construct_class_ques_subset: for
 * each ordinal feature, sort its observed values by MEAN PREDICTEE and emit
 * the cumulative prefixes of that order. That is where the vendors' four
 * arbitrary-looking sets come from -- jill's sylInPhrase {2,6,7,8} and {3,5},
 * sylInWord {3,6} and {1,5} are contiguous once the values are sorted by mean
 * duration, and nothing in phonetics explains them. Pass NULL for the static
 * inventory alone. */
int  spfy_wgn_qset_build(char (*labels)[8], size_t n_labels,
                         int with_phone_in_syl,
                         const spfy_wgn_sample *sam, size_t n_sam,
                         spfy_wgn_qset *out);
int  spfy_wgn_qset_write(const spfy_wgn_qset *q, spfy_vb_buf *b);
void spfy_wgn_qset_free(spfy_wgn_qset *q);
/* Is question `q` present in the set? Compares key and value SET. */
int  spfy_wgn_qset_has(const spfy_wgn_qset *set, const spfy_wgn_ques *q);

/* Whole chunk: trhd { labl, ques } + one `tree` per entry of `trees`. */
int  spfy_wgn_chunk_write(char (*labels)[8], size_t n_labels,
                          const spfy_wgn_qset *q,
                          const spfy_wgn_tree *trees, size_t n_trees,
                          uint8_t **out, size_t *out_n);

#endif
