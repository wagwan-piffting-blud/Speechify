#ifndef SPFY_VB_H
#define SPFY_VB_H

#include <stddef.h>
#include <stdint.h>

/* Voice builder spine.
 *
 * Seven stages, derived from Festival's clunits build (lib/clunits_build.scm,
 * do_all) as it maps onto the shipped VIN chunks. See
 * reveng/README_TECHNICAL.md "Festival 1.4.2/1.4.3 provenance sweep" for the
 * stage -> chunk correspondence and the evidence behind it.
 *
 * The one deliberate divergence from Festival: clunits' acoustic cluster-tree
 * stage (acost:find_clusters) has no counterpart here. Speechify preselects
 * explicitly (prsl) and then runs Viterbi, so S5 replaces it rather than
 * implementing it.
 *
 * Each stage is a pure function of the stages before it. That is the whole
 * point of the spine: a stage may be recomputed without re-running its
 * predecessors, and no stage may reach backwards. */

#define SPFY_VB_STAGE_COUNT 7

typedef enum {
    SPFY_VB_S1_CORPUS   = 0,  /* utterances -> typed, named half-phone units  */
    SPFY_VB_S2_FEATURES = 1,  /* per-unit feature rows                        */
    SPFY_VB_S3_NORM     = 2,  /* mean/sd over the continuous features         */
    SPFY_VB_S4_JOIN     = 3,  /* boundary distance tables / join cache        */
    SPFY_VB_S5_PRESEL   = 4,  /* candidate admission (replaces cluster trees) */
    SPFY_VB_S6_TREES    = 5,  /* prosody CARTs and cost tables                */
    SPFY_VB_S7_PACK     = 6   /* catalogue + container emit                   */
} spfy_vb_stage;

/* Chunks each stage owns. A stage is COMPLETE when it can generate every
 * chunk listed for it from its inputs alone -- not when it can patch a
 * vendor chunk into place. */
typedef struct {
    spfy_vb_stage  stage;
    const char    *name;
    const char    *festival_origin;   /* the clunits function this descends from */
    const char    *chunks;            /* space-separated VIN/VDB chunk ids        */
} spfy_vb_stage_info;

extern const spfy_vb_stage_info spfy_vb_stages[SPFY_VB_STAGE_COUNT];

/* ---------------------------------------------------------------------- */
/* Shared build context.                                                    */

typedef struct {
    const char *voice_name;      /* e.g. "donna"                             */
    const char *work_dir;        /* stage artefacts live here                */
    const char *out_dir;         /* finished .vin/.vdb/.vcf land here        */
    int         sample_rate;     /* 8000 or 16000                            */
    int         verbose;
} spfy_vb_ctx;

/* ---------------------------------------------------------------------- */
/* S1  CORPUS -- Festival: db_utts_load + find_same_types + name_units       */
/*     Produces the unit inventory: every half-phone occurrence, typed by    */
/*     <phone><half> (92 classes for a 46-phone set) and named uniquely.     */
/*     Boundaries come from the engine (SPFY_UID_DUMP), never from MFA, for  */
/*     voices built from our own renders.                                    */
/*     Owns: ckls cklx                                                       */

typedef struct {
    uint32_t unit_count;
    uint32_t utterance_count;
    uint32_t class_count;        /* expect 92 for en-US                      */
} spfy_vb_corpus_stats;

int spfy_vb_s1_corpus(const spfy_vb_ctx *ctx, spfy_vb_corpus_stats *out);

/* ---------------------------------------------------------------------- */
/* S2  FEATURES -- Festival: acost:dump_features (item.feat per feature)     */
/*     One row per unit over the 16-key feature list the vendor used:        */
/*       name start duration dur_z pitch pitch_z voice voice_z power power_z */
/*       lisp_initial_boundary_strength lisp_final_boundary_strength         */
/*       Syllable.stress lisp_mod_tobi_accent lisp_mod_tobi_endtone filename */
/*     Linguistic features MUST come from the in-tree FE (src/fe), so that   */
/*     training and synthesis read the same values by construction.          */
/*     Owns: feat                                                            */

int spfy_vb_s2_features(const spfy_vb_ctx *ctx);

/* ---------------------------------------------------------------------- */
/* S3  NORM -- Festival: acost:utts_load_coeffs + EST meansd()              */
/*     92 half-phone classes x 8 continuous features (duration dur_z pitch   */
/*     pitch_z voice voice_z power power_z). Feeds the _z columns in S2 and  */
/*     the engine's scaling at selection time.                               */
/*     Owns: mean                                                            */

int spfy_vb_s3_norm(const spfy_vb_ctx *ctx);

/* ---------------------------------------------------------------------- */
/* S4  JOIN -- Festival: acost:build_disttabs                               */
/*     Boundary distance between every admissible unit pair, cached.         */
/*     Largest artefact in the voice by an order of magnitude.               */
/*     Owns: hash                                                            */

int spfy_vb_s4_join(const spfy_vb_ctx *ctx);

/* ---------------------------------------------------------------------- */
/* S5  PRESEL -- replaces Festival's acost:find_clusters                    */
/*     Vendor admission rule, per hp_class. No acoustic clustering.          */
/*     Owns: prsl                                                            */

int spfy_vb_s5_presel(const spfy_vb_ctx *ctx);

/* ---------------------------------------------------------------------- */
/* S6  TREES -- Festival: acost:collect_trees (wagon), plus cost tables      */
/*     durt/f0tr are per-phone CARTs over six categorical features; their    */
/*     questions are value SETS, so a generator must emit subset splits, not */
/*     the single-value tests wagon produces. See README_TECHNICAL.md.       */
/*     Owns: durt f0tr ccos hist                                             */

int spfy_vb_s6_trees(const spfy_vb_ctx *ctx);

/* ---------------------------------------------------------------------- */
/* S7  PACK -- Festival: acost:save_catalogue, then container emit          */
/*     Writes the unit catalogue and packs every chunk into the encrypted    */
/*     RIFF container via src/common/riff_write.                             */
/*     Owns: unit vers cnts + the .vdb and .vcf                              */

int spfy_vb_s7_pack(const spfy_vb_ctx *ctx);

/* Run stages [first, last] inclusive. */
int spfy_vb_run(const spfy_vb_ctx *ctx, spfy_vb_stage first, spfy_vb_stage last);

#endif
