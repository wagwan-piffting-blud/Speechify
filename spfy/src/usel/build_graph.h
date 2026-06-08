#ifndef SPFY_USEL_BUILD_GRAPH_H
#define SPFY_USEL_BUILD_GRAPH_H

#include <stddef.h>
#include <stdint.h>

/* Slot-tree allocator that mirrors the engine's USelGraph::Initialize +
 * BuildGraph (FUN_08e8d4a0 -> FUN_08e89a70 -> FUN_08e8a130) pipeline.
 *
 * Engine algorithm (Phase B2 of M3.4r):
 *
 *   1. Allocate a "phrase" root (`type = 9` in the engine; engine's
 *      `*puVar10 = 9`).
 *   2. For each word in the FE's `Word` relation (flat, head-to-tail):
 *      - Allocate a "word" node under the phrase.
 *      - Walk the word's `SylStructure` daughter chain to enumerate
 *        syllables. For each syllable:
 *        - Allocate a "syllable" node under the word.
 *        - Walk the syllable's `SylStructure` daughter chain to enumerate
 *          segments. For each segment:
 *          - Allocate TWO "halfphone" leaves under the syllable
 *            (left half then right half).
 *   3. Run a post-order traversal (`FUN_08e8a130`) over the resulting
 *      tree to assign each node a contiguous slot index in [0, N).
 *      Halfphones are visited first (they're deepest), then syllables,
 *      then words, then the phrase root.
 *
 * For utt 1 of `text_001` (3 words, 4 syllables, 6 segments):
 *
 *      n_slots = 1 phrase + 3 words + 4 syls + 2 * 6 halfphones = 20
 *
 * which matches the engine-captured viterbi_dp `n_slots` exactly.
 *
 * This module is cost-free w.r.t. the engine's per-cand state -- it
 * only produces the topology (kinds + parent/child links + post-order
 * slot indices). LinkGraph (Phase B3) consumes this to derive the
 * predecessor list per slot. PostScoringAdj (Phase B4) adds the
 * per-cand pre-DP costs and the `cand+0x10` run-tail uids that turn
 * tree-internal slots into multi-unit anchors.
 */

typedef enum {
    SPFY_SK_PHRASE    = 0,
    SPFY_SK_WORD      = 1,
    SPFY_SK_SYLLABLE  = 2,
    SPFY_SK_HALFPHONE = 3,
} spfy_slot_kind_t;

typedef struct {
    spfy_slot_kind_t kind;
    /* Index in the post-ordered slot list. Identity for cross-references. */
    uint32_t         post_idx;
    /* Parent slot index; UINT32_MAX = root (phrase). */
    uint32_t         parent_idx;
    /* Children (slot indices). Allocated; n_children may be 0 (leaf). */
    uint32_t        *child_idx;
    uint32_t         n_children;
    /* Source FE item identity. For PHRASE this is 0; for WORD/SYLLABLE
     * it's the shared-item ptr; for HALFPHONE it's the segment shared
     * item ptr (both halfphones of a segment share this). */
    uint32_t         fe_shared;
    /* For HALFPHONE only: 0 = left half, 1 = right half. */
    uint32_t         halfphone_side;
} spfy_slot_node_t;

typedef struct {
    spfy_slot_node_t *slots;        /* heap, indexed by post_idx */
    uint32_t          n_slots;
    /* Convenience counts (also derivable from `slots`). */
    uint32_t          n_phrase;
    uint32_t          n_word;
    uint32_t          n_syl;
    uint32_t          n_halfphone;
} spfy_slot_tree_t;

/* Flat in-memory representation of the FE-emitted utterance tree (the
 * subset BuildGraph cares about). The caller produces this from the
 * captured fe_tree trace (or, eventually, from a C frontend). Each
 * array is in FE relation order (head-to-tail). */
typedef struct {
    /* Word relation, flat: word i's shared item id at word_shareds[i]. */
    uint32_t *word_shareds;
    /* Word i's "name" feature string (or NULL if unknown). Owned by the
     * caller (i.e. the parser that fills the struct); freed via
     * spfy_fe_utt_free if non-NULL. Used by spfy_derive_q5_table to
     * detect the boundary "_NULL_" wrapper words (q5=1 instead of
     * 2*n_segs for halfphones under such words). */
    char    **word_names;
    uint32_t  n_words;
    /* Phrase terminator char (e.g. ',', '.', '?', '!'). The engine's
     * sentence-final detection in FUN_08e8c7d0 reads this byte to
     * decide local_10. From the FE Phrase relation's `name` feature. */
    char      phrase_term;
    /* Per-syllable data, indexed by global syllable index (= sum of
     * word_n_syls[0..w-1] + s for word w's s-th syl). Length n_syls. */
    int32_t  *syl_stress;    /* raw FE syl stress (0/1/2 etc; -1 unknown) */
    uint32_t *syl_accent;    /* 0 = no accent, 1..6 = pitch-accent code */
    /* Per-syllable phrase-boundary tone target, in signed semitones
     * relative to the syllable's carrier F0. 0 = no boundary tone.
     * Derived from the ToBI boundary marker in the FE accent string
     * (L-L% → fall, L-H%/H-H% → rise). Drives the F0 target bias that
     * steers unit selection toward naturally-contoured units (Option A
     * of prosody realization). NULL if not populated. */
    int8_t   *syl_btone;
    /* For each word i, an ordered list of its syllable shared ids
     * (collected by walking the word's SylStructure daughter chain). */
    uint32_t **word_syls;       /* word_syls[i] = array of syl shared ids */
    uint32_t  *word_n_syls;
    /* Total syllables across all words (== sum of word_n_syls). */
    uint32_t  n_syls;
    /* For each syllable in a word-major flattening, an ordered list of
     * its segment shared ids (from SylStructure daughter chain). The
     * indexing is "global syllable index" = sum of word_n_syls[0..w-1]
     * + s for the s-th syllable of word w. */
    uint32_t **syl_segs;        /* syl_segs[g] = array of segment shared ids */
    uint32_t  *syl_n_segs;
    /* Total segments. */
    uint32_t  n_segs;
} spfy_fe_utt_t;

void spfy_fe_utt_free(spfy_fe_utt_t *u);
void spfy_slot_tree_free(spfy_slot_tree_t *t);

/* Build the slot tree from a parsed FE utterance. On success returns 0
 * and fills *out (caller must spfy_slot_tree_free it). */
int spfy_build_graph(const spfy_fe_utt_t *in, spfy_slot_tree_t *out);

/* --------------------------------------------------------------------- */
/* LinkGraph                                                              */
/* --------------------------------------------------------------------- */
/*
 * Produces the per-slot predecessor list (`slice+0x3c` in the engine,
 * with count at `slice+0x38`) that the Viterbi DP iterates.
 *
 * Empirical rule (decoded from the captured viterbi_dp predec topology
 * across the 32-entry corpus and cross-checked against the LinkGraph
 * decompile FUN_08e8c700):
 *
 *   - The root (phrase) has no predecessors.
 *   - For every other slot S, walk up via parent pointers until we
 *     find an ancestor A whose parent has a child immediately to the
 *     left of A in the post-order child list. Call that left-sibling
 *     subtree-root P.
 *     - If no such P exists, S is the leftmost-descending leaf chain
 *       starting from the root and has no predecessors.
 *     - Else `preds(S) = exit_chain(P)`, where `exit_chain(P)` is the
 *       sequence [P, P.last_child, P.last_child.last_child, ..., leaf]
 *       in OUTER-FIRST order.
 *
 * Internal slots (Word / Syllable) inherit the same predecessor list
 * as their first-leaf descendant -- the algorithm above handles this
 * naturally because both share the same "first ancestor with left
 * sibling" entry point.
 *
 * Output is appended to the existing slot tree: each slot's
 * `n_preds` and `preds[]` (heap-allocated, owned by the tree).
 */

typedef struct {
    uint32_t *preds;     /* slot indices, length n_preds */
    uint32_t  n_preds;
} spfy_slot_preds_t;

typedef struct {
    spfy_slot_preds_t *per_slot;   /* heap, indexed by slot post_idx */
    uint32_t           n_slots;
} spfy_slot_preds_table_t;

void spfy_slot_preds_table_free(spfy_slot_preds_table_t *t);

/* Compute the predecessor table for a slot tree. */
int spfy_link_graph(const spfy_slot_tree_t *tree,
                    spfy_slot_preds_table_t *out);

/* --------------------------------------------------------------------- */
/* Slice context derivation                                              */
/* --------------------------------------------------------------------- */
/*
 * For each halfphone-leaf slot, compute the engine's 5-tuple
 * `slice.ctx[]` (the value seen in `prsl_slot.ctx`). Encoding decoded
 * empirically from the captured prsl_slot trace:
 *
 *   slice.ctx[i]  =  hp_class of the SAME-SIDE phoneme at offset (i-2)
 *                    centered on the current halfphone slot. Out-of-
 *                    range positions use the silence sentinel (`pau_L`
 *                    = label_pau*2 = 64 for left halfphones, `pau_R`
 *                    = 65 for right halfphones).
 *
 * The hp_class encoding is interleaved: hp_class = label_idx*2 + side
 * where side = 0 (left) or 1 (right). Tom's hp_class_remap (voice+0x608)
 * additionally swaps phone classes 9/10/11 (3-cycle) and 14/15 -- those
 * swaps are applied on the LOOKUP path, not at encoding time, so we
 * just produce the raw interleaved value here.
 *
 * The phone_name -> label_idx mapping is voice-specific. For Tom we
 * hardcode the 47-phoneme inventory (see slot_ctx.c). When the FE
 * uses a phoneme outside the table we emit UINT32_MAX for that ctx
 * slot (caller can detect + skip / abort).
 */

typedef struct {
    /* Per-halfphone-leaf slot ctx. Indexed by post-order slot index;
     * non-halfphone slots are left zeroed. */
    uint32_t (*ctx)[5];        /* heap, n_slots arrays of 5 */
    uint32_t   n_slots;
    /* Per-halfphone-leaf flag (1 if this slot has a derived ctx). */
    uint8_t   *has;
} spfy_slice_ctx_table_t;

void spfy_slice_ctx_table_free(spfy_slice_ctx_table_t *t);

/* Derive ctx[5] for every halfphone-leaf slot in the tree. The
 * `fe_segments_in_order` argument is a flat array of segment phoneme
 * names in utterance order (= fe.syl_segs flattened). Length must
 * equal tree->n_halfphone / 2 == fe.n_segs.
 *
 * Returns SPFY_OK on success. */
int spfy_derive_slice_ctx(const spfy_slot_tree_t *tree,
                          const char         **fe_segments_in_order,
                          uint32_t              n_segments,
                          spfy_slice_ctx_table_t *out);

/* Lookup a phoneme name -> label idx for Tom. Returns UINT32_MAX if
 * the name isn't in the table. */
uint32_t spfy_tom_phone_to_label(const char *name);

/* --------------------------------------------------------------------- */
/* CART feature kernels (q_type 3, 4, 5)                                 */
/* --------------------------------------------------------------------- */
/*
 * The InnerScorer (FUN_08e88de0) precomputes 8 per-slot feature values
 * and passes them as stack args to the durt / f0tr CART walkers
 * (FUN_08e87d90 / 08e87e10). The walker forwards them to the question
 * dispatcher (FUN_08e87c90), which selects one based on the question's
 * q_type. The q_type -> source mapping decoded from disasm + verified
 * empirically against the 30-text corpus (`cart_walker_args_hook.js` +
 * `analyze_cart_walker_args.py`):
 *
 *   q_type 1, 2, 7, 8, 9 = SP_target indices at workspace+0x28..+0x3c
 *                          (also reused as proscost matrix row indices;
 *                          captured by `inner_scorer_hook.js`).
 *   q_type 3 = forest+0x8[ slice.ctx[1] ] = LEFT-context phone label
 *   q_type 4 = forest+0x8[ slice.ctx[3] ] = RIGHT-context phone label
 *   q_type 5 = halfphones-in-current-syllable, OR 1 for boundary-
 *              silence syllables (engine's FUN_08e8cbb0 init default;
 *              skipped when syl-name == "_NULL_").
 *
 * forest+0x8 is a remap table that maps `hp_class -> phone_label`. It
 * matches our `voice_runtime.c::s_ctx_remap` exactly: `s_ctx_remap[hp]
 * = SWAP[hp >> 1]` where SWAP is Tom's phone-pair remap (9->10, 10->11,
 * 11->9, 14<->15, identity otherwise). Validation evidence: 822/822
 * halfphone-leaf slots match across the corpus.
 */

/* Compute q_type 3 input: LEFT-context phone label.
 *
 *   q3 = s_ctx_remap[ slice_ctx[1] ]
 *
 * `s_ctx_remap` is a uint8_t[2 * n_labels] table from
 * `spfy_voice_maps_build` (voice+0x604 in the engine).
 * Returns UINT32_MAX if ctx1 is out of bounds. */
uint32_t spfy_cart_feature_q3(uint32_t       ctx1,
                              const uint8_t *s_ctx_remap,
                              uint32_t       n_remap);

/* Compute q_type 4 input: RIGHT-context phone label.
 *
 *   q4 = s_ctx_remap[ slice_ctx[3] ]
 */
uint32_t spfy_cart_feature_q4(uint32_t       ctx3,
                              const uint8_t *s_ctx_remap,
                              uint32_t       n_remap);

/* --------------------------------------------------------------------- */
/* SP_target populator (workspace+0x28..0x3c)                            */
/* --------------------------------------------------------------------- */
/*
 * Reproduces the engine's FUN_08e8c7d0 + FUN_08e8a670 + FUN_08e8a880
 * chain (all called from the end of FUN_08e8cbb0). Produces the 5
 * per-slot SP_target indices that double as q_type 1/2/7/8/9 inputs to
 * the durt/f0tr CART trees. Validated bit-exact (822/822) against
 * captured `inner_scorer.sp_target` over the 30-text corpus by the
 * Python reference at `c:/tmp/sp_target_full.py`.
 *
 * Indexed in the engine's cost-formula order (matches inner_scorer hook):
 *   sp[0] = workspace+0x2c = sylInPhrase   (matrix 0 row, q_type 2)
 *   sp[1] = workspace+0x28 = sylType       (matrix 1 row, q_type 1)
 *   sp[2] = workspace+0x34 = sylInWord     (matrix 2 row, q_type 7)
 *   sp[3] = workspace+0x38 = wordInPhrase  (matrix 3 row, q_type 8)
 *   sp[4] = workspace+0x3c = phoneInSyl    (matrix 4 row, q_type 9)
 */

typedef struct {
    /* Per-slot 5-tuple of SP_target indices (all uint32). For non-
     * halfphone slots the entries are zero; halfphone-leaf slots get
     * the populated values. */
    uint32_t (*sp)[5];        /* heap, length n_slots */
    uint32_t   n_slots;
    /* 1 if slot received a populated SP_target tuple, 0 otherwise. */
    uint8_t   *has;
} spfy_sp_target_table_t;

void spfy_sp_target_table_free(spfy_sp_target_table_t *t);

int spfy_derive_sp_targets(const spfy_slot_tree_t *tree,
                           const spfy_fe_utt_t    *utt,
                           uint32_t                sentence_idx_in_para,
                           int                     voice_d4_flag,
                           spfy_sp_target_table_t *out);

/* Per-halfphone-leaf-slot q_type 5 derivation.
 *
 *   q5[slot] = 2 * num_segs_in_slot's_syllable, EXCEPT when the
 *              containing word's name is "_NULL_" (boundary silence,
 *              i.e. the FE-emitted silence wrapper at utterance
 *              start/end), in which case q5[slot] = 1 -- this matches
 *              the engine's FUN_08e8cbb0 behaviour where the per-syl
 *              write is skipped for "_NULL_" syls and the workspace's
 *              init default of 1 stays.
 *
 * Inputs:
 *   tree         : the slot tree from spfy_build_graph (B2 output)
 *   word_names   : flat array of word name strings, length n_words
 *                  (must equal tree->n_word). Pass NULL pointers for
 *                  words whose name is unknown -- those will be
 *                  treated as non-NULL (i.e. q5 = 2*num_segs).
 *   n_words      : length of word_names array
 *
 * Output:
 *   q5_per_slot  : caller-allocated u32[tree->n_slots]. For non-
 *                  halfphone slots the value is 0 and `has[slot] = 0`.
 *   has_q5       : caller-allocated u8[tree->n_slots]; set to 1 for
 *                  halfphone-leaf slots that received a q5 value.
 *
 * Returns SPFY_OK on success. */
int spfy_derive_q5_table(const spfy_slot_tree_t *tree,
                         const char       **word_names,
                         uint32_t            n_words,
                         uint32_t           *q5_per_slot,
                         uint8_t            *has_q5);

#endif
