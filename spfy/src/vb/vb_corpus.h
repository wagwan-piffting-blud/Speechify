#ifndef SPFY_VB_CORPUS_H
#define SPFY_VB_CORPUS_H

#include <stddef.h>
#include <stdint.h>

#include "vb_chunk.h"
#include "vb_io.h"

/* S1 CORPUS -- (wav, boundaries, FE) -> typed, named half-phone units.
 *
 * Port of vb_build1.py's main loop. The constants below are all measured
 * rather than chosen, and each comment says against what; changing one is a
 * measurement, not a preference. */

/* Ceilings on a single half-phone, in ms. Measured against jill: her longest
 * non-pau unit tail sits in the low hundreds and her DP selects nothing over
 * 250 ms, while silence legitimately runs long. */
#define SPFY_VB_MAX_HALF_MS      250
#define SPFY_VB_MAX_PAU_HALF_MS  1500

/* `local_pos` is a u16 at unit +0x06, so a unit more than 65,535 ms into its
 * recording CANNOT be addressed. Clamping pinned 21.64% of Donna's units at
 * 0xFFFF, every one playing whatever sits at 65.535 s -- that is the stutter.
 * So a long recording is SPLIT into chunks with their own indx entries. THE
 * AUDIO IS NOT COPIED: the data chunk stays one contiguous run. 60 s leaves
 * 5.5 s of headroom for the boundary search to reach a unit edge. */
#define SPFY_VB_CHUNK_MS   60000
#define SPFY_VB_CHUNK_SEP  '~'

/* Split at gaps too. The engine emits a uid+1 run as ONE contiguous read and
 * only breaks it when local_pos jumps more than 64 ms, so a hole our units
 * failed to tile is SPOKEN. jill's whole inventory has 28 such gaps totalling
 * 61 ms, none above 5 ms -- which is where this comes from. */
#define SPFY_VB_GAP_SPLIT_MS  5

/* How closely a `.fe` must match its `.seg` before its word spans are
 * believed. The FE emits phrase-boundary pau the segmentation need not, so
 * this is a sequence ratio and not equality; 0.80 separates the observed good
 * recordings (~1.0) from the bad ones (~0.3) with room on both sides. */
#define SPFY_VB_FE_SEG_MIN_RATIO  0.80

/* Values <= this read as "no F0 here" and the engine skips the f0 cost. */
#define SPFY_VB_F0_ABSENT_MAX  21

typedef struct {
    spfy_vb_riff        vin;         /* the template VIN                   */
    spfy_vb_riff        vdb;         /* the template VDB                   */
    spfy_vb_phone_index pidx;
    spfy_vb_labl_map    labl;
    int                 pau_feat;
    /* f0_context = a*log(dur+1) + b, fitted on the TEMPLATE's own units. */
    double              f0ctx_a, f0ctx_b, f0ctx_r2;
    /* Our Hz -> the template's stored f0 quantisation. */
    double              f0q_slope, f0q_off;
    int                 f0q_fitted;
    /* --f0-slope/--f0-offset: use a MEASURED Hz->byte transfer instead of
     * matching the template's stored spread. See spfy_vb_template_fit_f0 for
     * why the sd match is wrong (it emits an encoding 1.3-1.7x too steep and
     * the engine's f0 terms are quadratic in the result). */
    double              f0q_user_slope, f0q_user_off;
    int                 f0q_user;
    /* The template's OWN unit record version. A build defaults to writing this
     * so the records and the VCF weights that read them stay coherent: jill is
     * v100008 and prices phoneInSyl at .3, tom is v100006 and prices it at 0. */
    uint32_t            unit_ver;
} spfy_vb_template;

int  spfy_vb_template_load(const char *vin_path, const char *vdb_path,
                           spfy_vb_template *t);
/* ⭐ The donor-free variant: containers built from nothing but the embedded
 * en-US language tables (vb_lang.h) plus an all-zero `ccos` carrying our own
 * label list. `unit_ver` 0 means v100008. See the definition for the one
 * judgement call -- the duration encoding -- and the two vendor fits that
 * make it a format constant rather than an inheritance. */
int  spfy_vb_template_new(spfy_vb_template *t, uint32_t unit_ver);
void spfy_vb_template_free(spfy_vb_template *t);
/* Second pass: needs a sample of OUR f0 tracks, so it cannot run at load. */
int  spfy_vb_template_fit_f0(spfy_vb_template *t,
                             const uint8_t *const *tracks,
                             const size_t *track_n, size_t n_tracks);

/* ⭐ PER-PHONE DURATION PERCENTILES, READ OFF A SHIPPED VOICE'S OWN UNITS.
 * Port of vb_build1.py's phone_dur_floors().
 *
 * ⚠ A FLAT MILLISECOND CUTOFF IS WRONG IN BOTH DIRECTIONS. jill keeps 3.91%
 * of her units at <=10 ms against our 0.20% -- a flap or a stop closure really
 * is that short -- so whatever rejects a 5 ms `ae` has to pass a 5 ms `dx`,
 * and only the phone separates them. The same at the top: "management" came
 * out "mahonagement" from a 160+160 ms /ae/ cut out of the county name
 * "Mahoning", against jill's own p99 for /ae/ of 142 ms.
 *
 * So the number is not chosen, it is READ: this is what a working inventory
 * actually contains for that phone.
 *
 * ⚠ REFERENCE PHONES ARE MATCHED TO TEMPLATE PHONES BY NAME. `phone_center`
 * is in LABL space and two voices need not agree on that ordering -- tom and
 * jill differ on the dx/d/dh 3-cycle and the er/en swap -- so indexing one
 * voice's table with another's ids gets five very common phones wrong and
 * scores a misleading 91.5% rather than failing outright.
 *
 * `out` is indexed by the TEMPLATE's feat phone id, sized `out_n`, and holds
 * 0.0 for every phone the reference could not speak to. `ref_vin` NULL reads
 * the template's own units. */
#define SPFY_VB_DUR_PCT_MIN_N  200   /* too few examples is no basis for a
                                      * floor; leave that phone alone */

int  spfy_vb_dur_percentiles(const spfy_vb_template *t, const char *ref_vin,
                             double pct, double *out, size_t out_n,
                             size_t *n_set, size_t *n_phones);


typedef struct {
    const char *wav_dir;
    const char *tg_dir;
    const char *seg_dir;        /* NULL = TextGrid path only            */
    /* vb_build1.py's --drop: JSON {"exclude": [stem, ...]}, for recordings
     * whose transcript describes different audio than the aligner put it on.
     * Their units carry labels for sound they do not hold. */
    const char *drop_path;
    /* ⭐ CHUNK-LEVEL COMPACTION, DONE AT BUILD TIME.
     *
     * One `indx` name per line (or the same JSON `exclude` form): `stem` for a
     * recording's first chunk, `stem~N` for the rest. A listed chunk
     * contributes NEITHER its audio NOR its units.
     *
     * ⚠ WHY THIS IS SAFE HERE AND NOT IN A POST-PROCESS. vb_compact.py has to
     * renumber an existing voice, and `spfy_anchor_score` resolves neighbours
     * by uid ARITHMETIC (cur_u-2 / cur_u+2 at anchor_score.c:114,130, forward
     * scan to cur_u+7 at :643), so renumbering slides different units into
     * those slots and moves picks -- 162 of 1,144 with everything else
     * byte-identical, and an [-2,+7] halo protecting 242,333 of 248,688 units
     * did NOT fix it. It also has to clear `flag_b` wherever a dropped
     * predecessor would make two unrelated units newly adjacent, which is the
     * difference between a smaller voice and a broken one.
     *
     * Here uids are assigned ONCE, after S1, to whatever survived; `flag_b`
     * comes from real audio contiguity inside each surviving chunk, and a
     * chunk head already gets 0. Every hazard vb_compact works around is a
     * hazard of retrofitting. */
    const char *drop_chunks_path;
    /* ⭐ --compress: a KEEP-span list, `stem<TAB>lo_ms<TAB>hi_ms`. Audio outside
     * a span is not emitted and the units inside it are not created, so the cut
     * lands below chunk granularity. `--drop-chunks` cannot: our chunks are
     * word-sized, only 8.9% of units are ever picked, and those picks touch
     * 62.8% of chunks holding 78.3% of the bytes.
     *
     * ⚠ Spans are TIMES, not uids, so the same list survives a rebuild.
     * ⚠ They must already include the WSOLA margin -- see keep_load(). */
    const char *compress_path;
    int         f0_calibrated;  /* 0 = write 0 into f0_start/end/mid     */
    /* ⭐ RENDER-ONLY f0. Writes real f0_start/f0_end but forces f0_mid to 0.
     *
     * The point is WSOLA, not selection. spfy_wsola_push_unit_psola() only
     * switches on its "Selective F0 smoothing" mode -- growing the overlap to
     * a full 2*T0 so a voiced cross-recording join gets a whole pitch period
     * of crossfade -- when BOTH boundary f0 bytes are nonzero. Under
     * `--f0 absent` every byte is 0, so that mode never fires and every voiced
     * seam is a plain fixed-window OLA. jill's bytes are populated, which is a
     * large part of why she measures 0.599 audible seams/s against our 1.834
     * on wisconsin_cdw -- the SAME spfy renders both, so the difference cannot
     * be in the code.
     *
     * Why f0_mid stays 0: it is the DP's voicing map (viterbi.c, `c68 >= 21`).
     * Leaving it 0 pins c80 to the 100 sentinel, which makes the join gate
     * (`prev.c80 < 15`) unable to fire, which in turn makes c6c -- the f0_end
     * byte we are now writing -- dead to the DP. f0_start's only other reader
     * is the anchor F0-span (anchor_score.c), and that is neutralised by
     * zeroing the f0tr leaf variances, the same knob `--f0 joinonly` uses.
     *
     * ⚠ The claim "selection is unchanged" is CHECKABLE, not argued: the picks
     * must come out identical to the same build with `--f0 absent`. Diff the
     * SPFY_UID_DUMP records before believing any seam number. */
    int         f0_render_only;
    int         trim_silence;
    int         limit;
    /* ⛔ Suppress ckls/cklx anchors for `rvc_*` recordings, keeping their
     * units. Measured with vb_anchoraudit --fe: converted _WORD_ records match
     * the FE 54.53% of the time against her own 77.00%, and 92 of 792
     * recordings are wrong WHOLESALE -- all converted, none of hers, which is
     * the control. donnaf0u's only `tornado` anchor spans `m dh ix sh ih f t`
     * and renders "the SHIFT", confirmed by ear.
     *
     * The PHONE labels are fine (they come from the engine's own segmentation
     * of the render), so this drops the anchors and keeps the audio.
     *
     * ⚠⚠ `rvc_*` ONLY. It used to catch `st2_*` as well, because both share
     * the `is_rvc` flag that --rvc-policy needs -- and the two flags mean
     * different things. The measurement above is about a TRANSCRIPT paired
     * with audio from elsewhere; a StyleTTS2 render is GENERATED FROM its
     * text, so that failure is impossible by construction. On a corpus of 302
     * real + 1,044 `st2_` and ZERO `rvc_`, the conflation silently suppressed
     * 35,976 anchors against 36,495 kept -- half the inventory, and the half
     * whose text is correct by definition. */
    int         rvc_anchors_drop;
    /* Level normalisation, applied BEFORE the u-law encode so there is no
     * double quantisation. 0 disables. The target is dBFS: jill's speech
     * median sits near -13.4 and tom's near -11.4, ours near -16.9, so the
     * whole voice is quiet and every arm inherits that. */
    double      level_target;
    double      level_max_gain;   /* LINEAR ceiling, as in vb_build1 (4.0) */
    /* ⚠ A LINEAR CEILING DOES NOT PREVENT CLIPPING, and measurement says the
     * difference is not academic: reaching jill's -13.2 dBFS costs +3.9 dB,
     * and 49.0% of our recordings peak too high to take it. Their transients
     * would be flattened into the u-law ceiling -- audible as exactly the
     * "volume-related artifacting" this pass is meant to remove. So each
     * recording's gain is also capped by its OWN peak against this dBFS
     * ceiling. 0 disables the cap and restores the clip-and-count behaviour. */
    double      level_peak_dbfs;
} spfy_vb_corpus_cfg;

typedef struct {
    spfy_vb_unit     *units;
    size_t            n_units;
    spfy_vb_indx_ent *indx;      /* n_indx includes the trailing sentinel */
    size_t            n_indx;
    uint8_t          *data;      /* u-law, one contiguous run             */
    size_t            n_data;
    spfy_vb_anchors   words, syls;

    size_t n_stems, n_used;
    size_t n_skip_align, n_skip_phone, n_skip_long, n_skip_silent;
    size_t n_seg_rec, n_seg_bad, n_fe_mismatch;
    /* FE syllabics (`el`/`en`) handed back the unpaired AH0/IH0 the aligner
     * left beside them. english_us_arpa has no syllabic phone, so without
     * this the unit holds the consonant alone -- see is_syllabic(). */
    size_t n_syllabic_merged;
    /* f0 bytes taken from the unit's own voiced frames after the sampled
     * frame came back empty. See f0_span(): a zero is the DP's VOICING bit,
     * so a tracker dropout must not be allowed to fake one. */
    size_t n_f0_rescued;
    size_t n_chunk_extra, n_gap_split, n_lp_over;
    size_t n_ctx_seen, n_ctx_bad, n_sp, n_no_syl;
    /* Units that got a REAL phoneInSyl from `.sp` rather than the absent-column
     * default. Counted because the value only reaches the container in
     * v100008: writing v100006 makes every one of these inert, and a build log
     * that never mentions it cannot tell the two cases apart. */
    size_t n_sp_phone_in_syl;
    size_t n_first_half;
    /* ⚠ PER-STEM SIDECAR ACCOUNTING. Each of these was read with its return
     * value discarded, so a stem missing one was processed with empty data and
     * the build reported success. A corpus 95% complete looked identical to a
     * whole one. Counted per stem so a partial ingest is visible in the log
     * rather than in the ear. */
    size_t n_no_fe, n_no_sp, n_no_f0, n_no_tg;
    /* Units whose end time ran past the recording's last whole millisecond.
     * ⚠ SHORTENED and DROPPED are NOT the same event and used to share one
     * counter, which hid the second entirely. A drop happens inside the
     * `for (side ...)` loop, so it emits ONE half of a phone: every later
     * `ckls` span in that recording is then off by one unit. Counted apart so
     * the log can say which happened. */
    size_t n_end_clamp;   /* shortened -- benign */
    size_t n_end_drop;    /* half-phone not emitted at all -- shifts spans */
    /* From the first wav read. The VDB's `fmt` has to declare it, and S4
     * needs it for the frame windows; both used to hard-code 8000, which
     * would have written a header that quietly disagreed with the audio. */
    uint32_t sample_rate;
    size_t   n_rate_mismatch;   /* wavs whose rate differed from the first */
    /* Level pass. n_clip is reported because scaling a recording that
     * already peaks near full scale is how levelling silently degrades a
     * voice, and a build log that never mentions it has not looked. */
    size_t   n_leveled, n_clip, n_samp_level;
    double   level_gain_db;     /* summed; divide by n_leveled for the mean */
    /* Recordings that could not reach the target because their own peak got
     * there first. A high count is not a fault -- it is the reason the median
     * lands short of the target -- but it must be visible, or the level pass
     * looks like it hit a number it did not. */
    size_t   n_level_peaklim;
    double   level_short_db;    /* summed shortfall over those recordings */
    /* Drop list. ⚠ n_drop_absent is reported by NAME, not just counted: a
     * list that matches nothing -- a renamed stem, a stale path -- builds the
     * UNFILTERED voice and reports success, and the comparison then measures
     * two identical inventories and calls the difference noise. */
    size_t   n_drop_listed, n_drop_hit, n_drop_absent;
    size_t   n_rvc_recs, n_rvc_units, n_rvc_anchor_sup;
    /* Anchors KEPT from synthetic recordings whose text is its own input
     * (`st2_*`). Reported because the suppression rule used to catch these
     * too: 35,976 anchors vanished from a corpus holding ZERO `rvc_*` files,
     * and the log only ever showed the suppressed count, never this one. */
    size_t   n_syn_anchor_kept;
    /* Chunk compaction. n_cdrop_absent is named, not just counted, for the
     * same reason the recording drop list is: a stale name silently builds the
     * UNCOMPACTED voice and reports success. */
    size_t   n_cdrop_listed, n_cdrop_hit, n_cdrop_absent;
    size_t   n_chunk_dropped, n_unit_dropped, n_bytes_dropped;
    /* --compress: spans listed / matched, and what it withheld */
    size_t   n_keep_spans, n_keep_stems, n_comp_units, n_comp_bytes;
    size_t   n_comp_skipped;
} spfy_vb_corpus;

/* ⭐ The per-phone duration percentile taken from OUR OWN units instead of a
 * shipped voice's. `--dur-floor-pct` was a donor dependency nobody had listed:
 * it decided which of our units were too short using jill's per-phone
 * durations. Used whenever there is no reference VIN, which is always the case
 * under --no-template. Indexed by FEAT phone id, like the other one. */
int  spfy_vb_dur_percentiles_corpus(const spfy_vb_corpus *c, double pct,
                                    double *out, size_t out_n,
                                    size_t *n_set, size_t *n_phones);

int  spfy_vb_corpus_build(const spfy_vb_template *t,
                          const spfy_vb_corpus_cfg *cfg,
                          spfy_vb_corpus *out);
void spfy_vb_corpus_free(spfy_vb_corpus *c);

/* Exposed because the .seg guard is the thing that failed silently once, and
 * a guard nobody can test is not a guard. Ports difflib.SequenceMatcher's
 * ratio(), autojunk included. */
double spfy_vb_seq_ratio(const int32_t *a, size_t na,
                         const int32_t *b, size_t nb);

#endif
