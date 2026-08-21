#ifndef SPFY_VB_STAGES_H
#define SPFY_VB_STAGES_H

#include <stddef.h>
#include <stdint.h>

#include "vb_chunk.h"
#include "vb_corpus.h"
#include "../usel/hash_build.h"

/* ---------------------------------------------------------------------- */
/* S3 NORM -- `mean`, 92 half-phone classes x 8 columns.                    */
/*                                                                          */
/*   0,1  duration   CONFIRMED: recomputing tom's column 0 from tom's own    */
/*                   dur_like reproduces it, the error collapsing as class   */
/*                   size grows.                                             */
/*   2,3  pitch      APPROXIMATE. Real Hz, not the record's quantised byte.  */
/*   4,5  voice      UNVERIFIED definition: the fraction of the unit's ms     */
/*                   that carry an F0. Reproduces tom's shape.               */
/*   6,7  power      UNVERIFIED definition: ln(RMS + 1).                     */
/*                                                                          */
/* ⚠ Keeping the TEMPLATE's mean would be strictly worse than an approximate */
/* one of our own: it describes a different speaker, and this chunk exists   */
/* to Z-score THIS voice's features.                                         */

int spfy_vb_s3_norm(const spfy_vb_corpus *c, const spfy_vb_labl_map *labl,
                    uint8_t **out, size_t *out_n, size_t *n_populated,
                    size_t *n_have);

/* ---------------------------------------------------------------------- */
/* S6 TREES -- `durt` leaves, in the template's tree structure.             */
/*                                                                          */
/* durt predicts f0_context, not duration -- confirmed by the engine-        */
/* validated call site (spfy_viterbi_replay passes cand->f0_context to       */
/* spfy_cost_d), by the existing pipeline accumulating uf['f0_context'], and */
/* by an R^2 = 0.94 fit of f0_context against log(duration).                 */
/*                                                                          */
/* ⚠ build_voice_pipeline ships SKIP_DURT_RECOMPUTE = True, justified by     */
/* "DUR_WEIGHT=0 means these only affect WSOLA output duration". The VCF     */
/* this voice loads has DUR_WEIGHT = .2, so the leaves are inside the        */
/* selection cost and keeping tom's would score our units against a          */
/* different voice's statistics.                                             */
/*                                                                          */
/* The tree TOPOLOGY and the questions stay the template's. Generating those */
/* is S6's remaining gap and is named as such in README.md.                  */

#define SPFY_VB_TREE_MIN_SAMPLES 5

int spfy_vb_s6_durt(const spfy_vb_corpus *c, const uint8_t *tmpl_durt,
                    size_t tmpl_n, uint8_t **out, size_t *out_n,
                    size_t *n_recomputed, size_t *n_kept);

/* ---------------------------------------------------------------------- */
/* S6a2 f0tr -- the SAME argument as durt, for the pitch tree.
 *
 * ⛔ ONLY MEANINGFUL WITH `--f0 calibrated`. With f0_start == 0 everywhere the
 * engine takes the w_f0_miss branch and charges every candidate the same flat
 * penalty, so the tree is never consulted and regenerating it changes nothing.
 * With real F0 bytes and the TEMPLATE's leaves it is actively harmful: the
 * absolute cost then measures how well our units match the TEMPLATE SPEAKER's
 * pitch contour. That combination moved 82.3% of picks on one sentence and was
 * heard as a regression, which is what this stage exists to remove.
 *
 * ⚠ ONE tree, index 0, syllable-level. Its feature map is NOT durt's: the
 * engine clamps q3/q4/q5/q9 for f0tr and clamps q7 for durt, so f0tr reads q7
 * and durt does not. Predictee is `f0_start` (the byte the cost compares),
 * zeros excluded. `n_used` reports how many units contributed -- zero means
 * the voice has no F0 and the call was pointless. */
/* Zero every f0tr leaf VARIANCE, which makes the f0 target cost identically
 * zero while leaving f0_start and the `hist` join cost intact. See the
 * comment on the definition for the two measurements that motivate it. */
int spfy_vb_f0tr_zero_var(uint8_t *f0tr, size_t n, size_t *n_zeroed);

int spfy_vb_s6_f0tr(const spfy_vb_corpus *c, const uint8_t *tmpl_f0tr,
                    size_t tmpl_n, uint8_t **out, size_t *out_n,
                    size_t *n_recomputed, size_t *n_kept, size_t *n_used);

/* ---------------------------------------------------------------------- */
/* S6a3 durt / f0tr GROWN, not patched.                                    */
/*                                                                          */
/* The two stages above inherit the donor's topology, questions and leaf     */
/* variances. These build the whole chunk -- labels, questions, splits,      */
/* means, variances -- from our corpus alone. Gated on the vendors first:    */
/* growing over their own units beats their own shipped tree on held-out     */
/* RMSE for jill and tom, durt and f0tr (see vb_stages.c for the table).     */

typedef struct {
    size_t n_samples;
    size_t n_questions;
    size_t n_nodes;
    size_t n_leaves;
    size_t n_empty;       /* labels the corpus had no units for */
} spfy_vb_treestat;

int spfy_vb_s6_durt_grow(const spfy_vb_corpus *c, char (*labels)[8],
                         size_t n_labels, uint32_t min_cluster,
                         uint8_t **out, size_t *out_n, spfy_vb_treestat *st);
int spfy_vb_s6_f0tr_grow(const spfy_vb_corpus *c, char (*labels)[8],
                         size_t n_labels, uint32_t min_cluster,
                         uint8_t **out, size_t *out_n, spfy_vb_treestat *st);

/* ---------------------------------------------------------------------- */
/* S6b hist -- the F0-discontinuity curve, from our own natural joins.      */
/*                                                                          */
/* REPRODUCED against both vendors before being written. dag_join_cb indexes */
/* it as idx = f0_end(curr) - sub_off - c7c(prev), and viterbi.c shows c7c   */
/* is the last f0_mid >= 21 carried down the path; at a natural join that is */
/* simply unit uid-1. Histogramming that step over each vendor's OWN units   */
/* and pricing it -log(p/p_max) gives shape r = 0.985 (tom) and 0.986        */
/* (jill), and jill's floor log(peak) = 11.0197 against a shipped 10.9964.   */
/* Using f0_end for the previous unit instead of f0_mid scores MAE 0.83 vs   */
/* 0.42, so the field is discriminated, not assumed.                         */
/*                                                                          */
/* ⚠ ONE UNRESOLVED BIN. Both vendors' curves reach 0.0 at index 49 while    */
/* both vendors' own data peak at index 50, so their zero is one bin off     */
/* their modal step. We centre on the OBSERVED mode -- the modal natural     */
/* join costs nothing -- and do not imitate the offset, because nothing here */
/* explains it and copying an unexplained offset is how it becomes folklore. */
/*                                                                          */
/* ⚠ INERT WHEN F0 IS ABSENT. The engine's gate needs curr.f0_end > 20 and   */
/* prev.f0_mid >= 21; with --f0 absent every one of those bytes is 0, so the */
/* curve is never read. It is still written, correctly, for the day it is.   */

#define SPFY_VB_HIST_BINS    100
#define SPFY_VB_HIST_SUB_OFF (-50)

int spfy_vb_s6_hist(const spfy_vb_corpus *c, uint8_t **out, size_t *out_n,
                    size_t *n_obs, uint32_t *peak, int *mode_bin);

/* ---------------------------------------------------------------------- */
/* S4 JOIN -- `hash`, generated from S5's own candidate structure.          */
/*                                                                          */
/* The domain rule, measured against jill (SPEC_S4_hash.md "(a) Domain"):    */
/*                                                                          */
/*   cost == 0  <=>  r == l + 1        exact bijection, 9.0% of pairs        */
/*   the rest: both endpoints in the prsl candidate set AND the pair is      */
/*   adjacency-compatible                                                    */
/*                                                                          */
/* ⛔ THE RULE HERE USED TO READ "a slot keyed (L,C,R) is followed by one    */
/* keyed (C,R,*)", i.e. the key as a sliding window. It is not one. A unit's */
/* key is the TRIPHONE REPLICATED AT ITS OWN HALF-PARITY, (2a+h,2p+h,2b+h).  */
/* The window rule enumerated a disjoint set: that table hit 0 of 601,768 DP */
/* lookups where tom hits 8.65%, which made every downstream reading inert   */
/* (K=1 and K=24 gave byte-identical audio).                                 */
/*                                                                          */
/* Measured off SPFY_JOIN_DUMP over 467,844 distinct requested pairs, the    */
/* DP asks for two families and nothing else -- half-phone concatenation,    */
/* L(P)->R(P) then R(P)->L(Q):                                               */
/*                                                                          */
/*   even -> odd  40.49%   C_r == C_l + 1                     (100% of it)   */
/*   odd  -> even 59.51%   C_r == R_l - 1 and L_r == C_l - 1  (~96% each)    */
/*                                                                          */
/* Festival's clunits builds candidates the same way, and optimal_couple()   */
/* returns 0.0 when u1->prev_unit == u0 -- the vendors' cost==0 <=> r==l+1   */
/* by another name. It needs no render pass and no pool harvest, which is    */
/* why this replaces steps 3, 6 and 7 of build_voice.ps1 outright.           */
/*                                                                          */
/* ⚠ Take the half from the KEY's parity, never uid & 1: on tom 3,390 of     */
/* 6,849 recordings start at an odd uid.                                     */
/*                                                                          */
/* The relation is far too wide to take whole (some (centre,right) buckets   */
/* hold tens of thousands of units), so each right unit keeps its K best     */
/* left partners. The vendors' mean row is 10.1 (tom) and 10.5 (jill), which */
/* is where the default comes from.                                          */
/*                                                                          */
/* ⚠ WITH A CONSTANT COST, MEMBERSHIP *IS* THE METRIC. A miss costs 1000 and */
/* a hit costs ~1, so which K partners are admitted decides everything the   */
/* join cost decides. They are ranked by the RECOVERED vendor formula in     */
/* join_cost.c over frames from the voice's own VDB, not by a guess.         */

typedef enum {
    SPFY_VB_JC_CONST = 0,   /* 0 on continuations, `const_cost` elsewhere   */
    SPFY_VB_JC_COST  = 1    /* the computed cost, affine-mapped             */
} spfy_vb_join_mode;

typedef struct {
    /* Per-unit CEILING. With k_per_key set this should be loose (a few
     * hundred) -- it exists so a unit listed in thousands of groups cannot eat
     * the table, not to size the row. */
    uint32_t          k_best;
    /* Partners cached per (right unit, KEY). 0 = the old behaviour, one
     * k_best heap over the union of every key. The vendors store ~1.87
     * (jill) / 2.0 (tom) per listing; see the loop in spfy_vb_s4_join. */
    uint32_t          k_per_key;
    spfy_vb_join_mode mode;
    float             const_cost;
    float             join_w, join_off;
    uint32_t          sample_rate;   /* 0 = read it from the VDB's own fmt */
    /* 1: cost the join with dim 0 forced to zero, so a voice that carries f0
     * bytes only for WSOLA gets the same table as one built --f0 absent. */
    int               zero_f0_dim;
    /* Measured edge F0 for dim 0 of the join cost, 2 bytes per uid, on the
     * f0_start quantiser. NULL = read the stored bytes (0 under --f0 absent,
     * which makes partner ranking pitch-blind). See spfy_vb_cfg_t.f0_edge. */
    const uint8_t    *f0_edge;
    uint32_t          n_f0_edge;
} spfy_vb_join_cfg;

typedef struct {
    size_t n_pairs, n_cont, n_rows, n_cells;
    size_t n_no_frames;
    size_t n_scored;      /* candidate joins costed; the loop's real size */
} spfy_vb_join_stats;

/* Needs the FINISHED vin/vdb on disk, because the frames come out of the
 * unit records and the VDB audio. Writes the packed `hash` chunk. */
int spfy_vb_s4_join(const char *vin_path, const char *vdb_path,
                    const spfy_vb_join_cfg *cfg,
                    uint8_t **out, size_t *out_n,
                    spfy_vb_join_stats *st);

#endif
