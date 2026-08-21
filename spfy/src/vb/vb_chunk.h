#ifndef SPFY_VB_CHUNK_H
#define SPFY_VB_CHUNK_H

#include <stddef.h>
#include <stdint.h>

#include "vb_io.h"

/* Chunk readers and writers, ported from vb_vin.py / vb_prsl.py / vb_ckls.py /
 * vb_mean.py. Each one is byte-gated against the Python it replaces. */

/* ---------------------------------------------------------------------- */
/* feat -- the feature registry. It is a flat sequence of sections:
 *     u16 key_len, key, u32 n_values, n_values * { u16 len, bytes, u32 id }
 * A build replaces exactly one section (`filename`) and carries the rest
 * verbatim, because they are the SCHEMA and are not ours to invent. */

typedef struct {
    const uint8_t *raw;      /* the whole section, header included */
    size_t         raw_n;
    char           key[64];
} spfy_vb_feat_section;

int spfy_vb_feat_sections(const uint8_t *feat, size_t n,
                          spfy_vb_feat_section **out, size_t *out_n);

/* `cnts` words 0 and 1, read back off the feat we actually emit rather than
 * copied from the donor: cnts[0] is feat["name"]'s entry count and cnts[1] the
 * number of keys. Real Speechify validates the unit count in cnts[2] and
 * throws on a mismatch, so the other two are not worth leaving to luck. */
int spfy_vb_feat_counts(const uint8_t *feat, size_t n,
                        uint32_t *n_name, uint32_t *n_keys);

/* phone name -> phone id, from feat["name"]'s 92 half-phone names.
 * `names[i]` ends in '1' or '2'; the phone id is i/2. */
typedef struct {
    char   (*name)[8];
    size_t   n;              /* number of PHONES, i.e. 46 */
} spfy_vb_phone_index;

int  spfy_vb_phone_index_build(const uint8_t *feat, size_t n,
                               spfy_vb_phone_index *out);
int  spfy_vb_phone_id(const spfy_vb_phone_index *pi, const char *name);
void spfy_vb_phone_index_free(spfy_vb_phone_index *pi);

/* ccos labl index -> feat phone id, matched by NAME.
 *
 * ⚠ NOT the identity. The unit record's `phone_center` is in LABL space and
 * every hp_class in prsl/mean is in FEAT order; the two are permutations of
 * the same 46 names. On tom they differ for the dx/d/dh 3-cycle and the
 * er/en swap -- five very common phones, and getting it wrong scores a
 * misleading 91.5% rather than failing outright. */
typedef struct {
    int16_t l2f[256];
    int16_t f2l[256];
} spfy_vb_labl_map;

int spfy_vb_labl_map_build(const uint8_t *ccos, size_t ccos_n,
                           const uint8_t *feat, size_t feat_n,
                           spfy_vb_labl_map *out);

/* The template's sections with `filename` replaced by ours. */
int spfy_vb_build_feat(const uint8_t *tmpl_feat, size_t tmpl_n,
                       char *const *names, size_t n_names,
                       uint8_t **out, size_t *out_n);

/* ---------------------------------------------------------------------- */
/* unit -- v100006, 29-byte stride. Every byte of the stride is named, so
 * pack(unpack(x)) == x is a test of the map and not of the bytes we happened
 * to look at.
 *
 * ⛔ THE WRITE LAYOUT ONLY. A build emits v100006 and the matching `vers`, so
 * what we produce is self-consistent -- but a TEMPLATE need not be v100006 and
 * jill is not: she ships v100008 at stride 30, which inserts `phone_in_syl` at
 * 0x10 and pushes f0_mid/f0_context/phone_center/is_first_half one byte later.
 * Anything READING a template's units must go through vb_corpus.c's
 * version-aware view; reading jill under this map produced a full, plausible
 * per-phone duration table that was wrong on 42 of 43 phones. */

#define SPFY_VB_UNIT_VERSION 100006u
#define SPFY_VB_UNIT_STRIDE  29u

/* ⭐ v100008 IS THE SAME RECORD PLUS ONE BYTE. `phone_in_syl` goes in at
 * 0x10 and every field from f0_start on shifts one later -- no field changes
 * meaning, which is what the loader decompile says and what jill's disk +0x11
 * confirms (it carries f0_start's ~25% zero-rate signature, not f0_end's).
 *
 * Worth writing because jill's VCF sets PHONE_IN_SYL_MISMATCH_COST = .3, her
 * second-largest SP weight after STRESS. Emitting v100006 under jill's VCF
 * leaves that weight ACTIVE while every candidate reports the decoder's
 * absent-column default of 6 (SyllUnknown) -- a value classify_phone_in_syl
 * never produces -- so the term costs something and discriminates nothing.
 * tom sets the same key to 0, which is why v100006 costs HIM nothing. */
#define SPFY_VB_UNIT_V8_VERSION 100008u
#define SPFY_VB_UNIT_V8_STRIDE  30u

/* What the engine's decoder yields when the record has no column. */
#define SPFY_VB_PHONE_IN_SYL_UNKNOWN 6u

typedef struct {
    uint32_t uid;
    uint16_t file_idx;
    uint16_t local_pos;
    uint16_t u08;
    uint16_t dur_like;
    uint8_t  sp_syl_in_phrase;
    uint8_t  sp_syl_type;
    uint8_t  sp_word_in_phrase;
    uint8_t  sp_syl_in_word;
    uint8_t  sp_phone_in_syl;   /* v100008 only; 6 when the sidecar lacks it */
    uint8_t  f0_start, f0_end, f0_mid, f0_context;
    uint8_t  phone_center;      /* LABL space */
    uint8_t  is_first_half;     /* SYLLABLE start, not phone half */
    uint8_t  voice_const;
    uint8_t  phone_ctx[4];
    uint8_t  flag_b;
    uint8_t  context_cost;
    /* Build-time only, never written.
     *
     * The three audio statistics are captured during S1, while the wav and
     * the f0 track are already open. vb_mean_apply.py re-derives the same
     * numbers from the WRITTEN VIN, which forces it to undo the chunk
     * renaming to find the sidecars again -- and getting that wrong cost
     * 25.2% of this voice's units their statistics, with the only symptom a
     * coverage line reading 74.8%. Here the recording-relative offset is
     * simply the offset we already have. */
    uint32_t key;               /* prsl context key                        */
    uint8_t  phone;             /* feat phone id                           */
    /* durt question 5, halfphones-in-syllable. Build-time ROUTING only --
     * never stored in any record version; the engine recomputes it per slot
     * at synthesis. Here so the leaves are filled by the same rule that will
     * later query them. */
    uint8_t  q5;
    uint8_t  have_audio;
    uint8_t  is_rvc;            /* cut from an `rvc_*` (converted) recording */
    /* ⭐ MEASURED edge F0, computed for EVERY build and NEVER packed.
     * `f0_start`/`f0_end` above are the STORED bytes and stay 0 under
     * `--f0 absent`, because a nonzero f0_start collapses the accent. These
     * carry the same measurement to the S4 join cost, whose dim 0 would
     * otherwise be identically zero and its partner ranking pitch-blind. */
    uint8_t  jf0_start, jf0_end;
    /* double, not float: the class aggregation below is done in float64 and
     * only rounded to float32 at pack time, so storing these narrow would
     * change the fourth decimal of every column. */
    double   a_voice, a_pitch, a_power;
} spfy_vb_unit;

void spfy_vb_pack_unit(const spfy_vb_unit *u, uint8_t out[SPFY_VB_UNIT_STRIDE]);

/* Writes one record in `ver`'s layout; returns the stride written, or 0 for a
 * version this does not map. `out` must have room for that stride. */
size_t spfy_vb_pack_unit_ver(const spfy_vb_unit *u, uint32_t ver, uint8_t *out);
size_t spfy_vb_unit_stride(uint32_t ver);

/* ---------------------------------------------------------------------- */
/* prsl                                                                     */

typedef struct {
    uint32_t  key;
    uint32_t *uid;
    uint32_t  n;
} spfy_vb_group;

/* Exact groups: one per distinct key, keys ascending, uids ascending within
 * a group. `units` must already carry their final uid.
 *
 * `withheld` is indexed by the unit's POSITION, which is also its uid. A key
 * whose every unit is withheld produces NO group -- which is deliberate, and
 * is what vb_build1.py's `del buckets[key]` does. Keeping an empty group would
 * be worse than dropping it: the engine's chain is exact -> one-sided ->
 * (92,c,92), and the wide groups are unions of the surviving EXACT groups, so
 * a dropped key falls through to real units of the right phone instead of
 * being served the one thing this build judged unfit. */
int  spfy_vb_group_units(const spfy_vb_unit *units, size_t n_units,
                         const uint8_t *withheld,
                         spfy_vb_group **out, size_t *out_n);

/* ⭐ REAL AUDIO WINS. Converted (`rvc_*`) recordings are ingested to cover
 * contexts her own recordings cannot reach -- not to compete with them.
 * Ingested as equals they took over: join miss fell 5.28% -> 2.66% because the
 * DP found so many more whitelisted pairs among them, and ASR word error rose
 * 14.3% -> 19.0% at n=60 because they displaced real units that were fine.
 *
 * So a group keeps its REAL units when it has any, and falls back to converted
 * ones only where she has nothing. `is_real[uid]` is 1 for her own audio AND
 * for any converted unit the caller has exempted per phone. Returns the number
 * of uids removed; a group is never emptied. */
size_t spfy_vb_groups_prefer_real(spfy_vb_group *g, size_t n,
                                  const uint8_t *is_real, size_t n_uid);

/* ⭐⭐ BACKOFF MEMBERSHIP -- the keys the corpus never recorded.
 *
 * spfy_vb_group_units() above produces a PARTITION: a unit is listed under
 * exactly the triphone it was recorded in, and a key we never recorded has no
 * group at all. No vendor ships that. Measured 2026-08-18 over the shipped
 * prsl chunks:
 *
 *     voice   narrow groups   listings/keyed unit   units in >1   max
 *     jill        73,806            5.94               70.9%      245
 *     tom         73,082            4.95               61.9%      136
 *     felix       81,214            5.13               65.6%      104
 *     crsmara     13,958            1.00                0.0%        1
 *
 * The cost of the partition is paid at the preselection ladder. Over the
 * three demo texts jill and tom hit their EXACT group 100.00% of the time and
 * felix 99.3%; we hit 97.98 / 86.52 / 81.18%. A slot served by a fallback rung
 * gets candidates whose context is unrelated to the slot's, so their prsl key
 * cannot satisfy the family relation S4 builds `hash` from -- every join
 * touching one is a guaranteed miss at MISSING_JOIN_COST 1000 against a hit's
 * ~1. The delivered hash hit rate tracks the exact-group rate across the three
 * texts monotonically: 63.8 / 53.1 / 41.7% against jill's 97-98%.
 *
 * ⚠ THIS IS ADDITIVE ONLY, AND DELIBERATELY SO. Existing exact groups are
 * copied through byte-for-byte and never grown, reordered or trimmed, so every
 * slot that already reached an exact group keeps the identical pool and the
 * identical pick. Only keys with NO exact group gain one. That keeps the
 * blast radius on the 2-19% of slots that are currently falling back, rather
 * than on the whole voice -- which matters because five arms have already been
 * rejected by ear for moving the "the NAtional weather service" accent.
 *
 * Fill order for a key (L, C, R), best first: units of centre C whose own left
 * context is L, interleaved with those whose own right context is R, then any
 * unit of centre C. `is_real` (optional, same array
 * spfy_vb_groups_prefer_real takes) puts her own audio ahead of converted at
 * equal rank.
 *
 * ⚠ The within-rank order is a VARIETY heuristic, not a phonetic distance:
 * the start offset rotates with the key so different targets do not all take
 * the same first few units. A real phone-similarity ranking is the obvious
 * improvement and needs a feature table this layer does not have. */
typedef struct {
    size_t n_keys_added;      /* groups that did not exist before        */
    size_t n_listings_added;
    size_t n_keys_possible;   /* the enumerated key space                */
    size_t n_keys_unfillable; /* centre class with no units at all       */
    size_t n_keys_gated;      /* refused by `gate` (see SPFY_VB_BG_*)    */
    size_t n_rank_ctx;        /* fills that matched one context EXACTLY  */
    size_t n_rank_class;      /* ...matched only its broad manner class  */
    size_t n_rank_any;        /* fills that matched neither              */
} spfy_vb_backoff_stats;

/* Returns ONLY the newly created groups, key-ascending. `exact` must be
 * key-ascending. Merge them in with spfy_vb_groups_merge() AFTER
 * spfy_vb_with_fallbacks() has run on the exact set -- wide_add() does not
 * dedupe, so passing these through it would append each unit to (92,c,92)
 * once per backoff listing. */
/* `gate` picks WHICH keys deserve a backoff group:
 *   SPFY_VB_BG_ALL     every key whose centre phone has units -- fills 97.8%
 *                      of the 194,672-key space.
 *   SPFY_VB_BG_BIGRAM  only keys where BOTH halves occur in our own units,
 *                      i.e. some unit has centre C with left L, and some unit
 *                      has centre C with right R.
 *
 * Measured against the vendors' own key sets: an exactly-recorded triphone is
 * present with 95.5% (jill) / 93.7% (tom) precision but only ~42% recall, and
 * the both-bigrams rule reaches 90-92% recall. Vendors populate 39.6 / 39.2%
 * of the space; the bigram rule on OUR corpus selects 35.6%, against the 97.8%
 * we fill today. */
#define SPFY_VB_BG_ALL     0u
#define SPFY_VB_BG_BIGRAM  1u

int  spfy_vb_groups_backoff(const spfy_vb_unit *units, size_t n_units,
                            const uint8_t *withheld, const uint8_t *is_real,
                            const spfy_vb_phone_index *pidx,
                            const spfy_vb_group *exact, size_t n_exact,
                            uint32_t hp_bound, uint32_t n_fill, uint32_t gate,
                            spfy_vb_group **out, size_t *out_n,
                            spfy_vb_backoff_stats *st);

/* Union by key, `a` winning a collision. */
int  spfy_vb_groups_merge(const spfy_vb_group *a, size_t na,
                          const spfy_vb_group *b, size_t nb,
                          spfy_vb_group **out, size_t *out_n);

/* Adds the WIDE groups every vendor ships. Without them the real 3.0.5
 * raises 7059 and produces NO AUDIO -- the engine's fallback chain is
 * exact -> one-sided -> (92,c,92), and spfy_prsl_lookup is a plain binary
 * search, so the wide groups must be IN the chunk.
 *
 * ⚠ ORDER IS NOT SORTED. A wide group lists its uids in the order the exact
 * groups are visited by ASCENDING KEY, first occurrence winning. Sorting
 * instead gets every key and every candidate SET right and still leaves
 * 3,386 of tom's 3,594 groups in the wrong byte order. */
int  spfy_vb_with_fallbacks(spfy_vb_group *exact, size_t n_exact,
                            uint32_t hp_bound,
                            spfy_vb_group **out, size_t *out_n);

int  spfy_vb_encode_prsl(const spfy_vb_group *g, size_t n,
                         uint8_t **out, size_t *out_n);
void spfy_vb_groups_free(spfy_vb_group *g, size_t n);

/* ---------------------------------------------------------------------- */
/* ckls / cklx -- the whole-word and whole-syllable anchor index.           */

typedef struct {
    char    *text;
    uint32_t span_start, span_end;   /* GLOBAL unit ids, inclusive */
    char    *file;
} spfy_vb_anchor;

typedef struct {
    spfy_vb_anchor *v;
    size_t          n, cap;
} spfy_vb_anchors;

int  spfy_vb_anchors_push(spfy_vb_anchors *a, const char *text,
                          uint32_t ss, uint32_t se, const char *file);
void spfy_vb_anchors_free(spfy_vb_anchors *a);

/* Drop every anchor whose span touches a gated uid. An anchor is a contiguous
 * unit span, so if any unit inside it was judged unfit the whole anchor is
 * unfit -- selecting it PLAYS that unit. spfy_synth.c:5104 expands a multi-uid
 * anchor across its whole half-phone span, OVERWRITING the per-half-phone
 * picks the DP made from the prsl pools, so preselection is not the only path
 * into the output and gating one without the other is only half a gate. */
size_t spfy_vb_anchors_filter(spfy_vb_anchors *a, const uint8_t *gated,
                              size_t n_uid);

/* Is this recording stem one of the SYNTHETIC sources (`rvc_*`, `st2_*`)?
 * Shared so the prefix test exists once; it used to be spelled out in
 * vb_corpus.c and nowhere else, which is how anchor suppression came to mean
 * two different things. */
int spfy_vb_stem_is_synth(const char *stem);

/* ⭐ prefer-real ONE LEVEL UP FROM THE POOLS. A winning anchor OVERWRITES the
 * DP's picks across its whole span (spfy_synth.c:5104), so --rvc-policy
 * prefer-real -- which only edits prsl -- does not protect her real audio
 * here. Drops synthetic anchors for tokens her own recordings also provide,
 * keeping them only where she has nothing. Returns the number dropped. */
size_t spfy_vb_anchors_prefer_real(spfy_vb_anchors *a);

/* ⛔ Remove every synthetic anchor from this list. Applied to _SYL_ by
 * default: a WORD anchor plays a whole word from one render, but a SYLLABLE
 * anchor plays a FRAGMENT the engine splices into a different word. Measured:
 * "body" came out of `st2_wxa_0173`, a recording that never says it, and was
 * heard as that word gaining an accent. */
size_t spfy_vb_anchors_drop_synth(spfy_vb_anchors *a);

/* Two groups, always: _WORD_ then _SYL_. */
int  spfy_vb_encode_ckls(const spfy_vb_anchors *word, const spfy_vb_anchors *syl,
                         uint8_t **out, size_t *out_n);
int  spfy_vb_encode_cklx(const spfy_vb_anchors *word, const spfy_vb_anchors *syl,
                         uint8_t **out, size_t *out_n);

/* ---------------------------------------------------------------------- */
/* mean -- 92 half-phone classes x 8 columns, as four (mean, sd) pairs.     */

#define SPFY_VB_N_HP 92

int spfy_vb_encode_mean(const double rows[][8], size_t n_rows,
                        uint8_t **out, size_t *out_n);

/* ---------------------------------------------------------------------- */
/* indx -- the VDB's recording directory.                                   */

typedef struct {
    uint32_t off;
    char    *name;
} spfy_vb_indx_ent;

int spfy_vb_encode_indx(const spfy_vb_indx_ent *e, size_t n,
                        uint8_t **out, size_t *out_n);

/* ---------------------------------------------------------------------- */
/* The four chunks that used to be copied out of the template wholesale.    */
/* All are short enough to read directly, and tom and jill agree on the     */
/* structure of every one, so none of this is inferred from a single voice. */

/* vers -- a Pascal string: u16 length then the bytes. tom carries
 * "3.0.0.0alpha" and jill "3.0.0.0"; the engine gates on the numeric part,
 * so a build writes the release form. */
int spfy_vb_encode_vers(const char *version, uint8_t **out, size_t *out_n);

/* LIST -- RIFF INFO, "INFO" then word-aligned <tag,u32 len,bytes> records.
 *
 * ⚠ NOT COPYABLE. The template's reads "Copyright 2003 SpeechWorks
 * International, Inc. All Rights Reserved." A voice built from someone
 * else's recordings must not carry that line, and no measurement will ever
 * complain about it -- which is exactly why it stayed wrong for so long. */
int spfy_vb_encode_list(const char *copyright, const char *date_iso,
                        uint8_t **out, size_t *out_n);

/* fmt -- the VDB's format record. Byte-identical on tom and jill:
 *   u16 tag=7 (WAVE_FORMAT_MULAW), u16 channels, u32 sample_rate,
 *   u32 bytes_per_sec, u16 block_align, u16 bits
 * ⚠ The last three describe the DECODED 16-bit stream, not the stored
 * u-law: at 8 kHz they are 16000, 2, 16 -- not 8000, 1, 8. Writing the
 * u-law figures would be the "obviously correct" version and would not
 * match either vendor. */
int spfy_vb_encode_fmt(uint32_t sample_rate, uint16_t channels,
                       uint8_t **out, size_t *out_n);

/* hist -- the F0-discontinuity penalty curve read by dag_join_cb as
 * `jc->curve`:  idx = clamp((curr.c6c - sub_off) - prev.c7c, 0, n-1),
 *               miss = MISSING_JOIN_COST + F0_EDGE_CHANGE_WEIGHT*curve[idx]
 *
 *   "head" u32 n, i32 sub_off        (both vendors: 100, -50)
 *   "data" n * f32                   V-shaped, exactly 0.0 at idx -n/2
 *
 * So it is a histogram of the F0-byte STEP seen at natural joins, turned
 * into a cost: free when the two bytes agree, dearer as they diverge.
 * `floor_cost` fills bins the corpus never populated -- tom sits at 10.963
 * and jill at 10.996, and a plateau of exactly-equal maxima at both ends of
 * both curves is what identifies it as a floor rather than data. */
int spfy_vb_encode_hist(const uint32_t *counts, uint32_t n, int32_t sub_off,
                        double floor_cost, uint8_t **out, size_t *out_n);

#endif
