#ifndef SPFY_SYNTH_LIB_H
#define SPFY_SYNTH_LIB_H

/* Reusable synth library shared by the CLI (spfy_synth.exe) and the SAPI 5
 * voice DLL (spfy_sapi.dll). */

#include <stddef.h>
#include <stdint.h>

#include <spfy/spfy.h>

#include "../voice/voice.h"
#include "../voice/unit_table.h"
#include "../voice/feat_table.h"
#include "../voice/vdb_lookup.h"
#include "../voice/ccos.h"
#include "../voice/voice_runtime.h"
#include "../voice/phone_order.h"
#include "../voice/vcf_matrix.h"
#include "../voice/chunk_table.h"
#include "../usel/anchor_score.h"
#include "../usel/hash.h"
#include "../usel/prsl.h"
#include "../cart/cart.h"
#include "../fe/fe.h"
#include "../wsola/wav.h"

/* All paths needed to bring up a voice. */
typedef struct {
    const char *vin;
    const char *vdb;
    const char *vcf;
    const char *hpclass;
    const char *vocab;
    const char *fe_tables_a;
    const char *fe_tables_b;
} spfy_voice_paths_t;

/* Loaded voice. */
typedef struct {
    spfy_vin_t              vin;
    spfy_vdb_t              vdb;
    spfy_vcf_t              vcf;
    spfy_unit_table_t       units;
    spfy_feat_table_t       feat;
    spfy_vdb_lookup_t       lookup;
    spfy_ccos_t             ccos;
    spfy_voice_maps_t       maps;
    spfy_proscost_matrix_t  pros[SPFY_PROSCOST_N];
    spfy_hash_t             hash;
    spfy_prsl_t             prsl;
    spfy_cart_t             durt_cart;
    spfy_cart_t             f0tr_cart;
    spfy_chunk_tables_t     chunks;

    /* Per-unit hp_class. */
    uint8_t                *hpc;
    uint32_t                hpc_n;

    /* feat/labl phone-order reconciliation. */
    spfy_phone_order_t      phone_order;

    uint32_t                hpc_buckets;
    uint32_t               *bucket_n;
    uint32_t               *bucket_cap;
    uint32_t              **bucket;

    /* Half-phone candidate histogram-prune constants, from the voice's
     * VCF (HALFPHONE_CAND_PRUNE_THRESH / _SLOPE / _MAX_UNITS). Cached here
     * because the prune runs per half-phone slot. Tom 0.8/0.005; Jill
     * 1.0/0.005, Felix 0.996, Javier 0.3, Paulina 0.4.
     *
     * All THREE are arguments to the engine's prune. FUN_08e88de0 loads them
     * from the config object and pushes them together at 0x08e8938b:
     *
     *     mov edx, [eax+0x50] / push edx     ; _PRUNE_SLOPE
     *     mov ecx, [eax+0x4c] / push ecx     ; _PRUNE_THRESH
     *     mov edx, [eax+0x48] / push edx     ; _MAX_UNITS
     *     call 0x8e88830                      ; the histogram prune
     *
     * and the config loader fills those same offsets from the three
     * `tts.voiceCfg.HALFPHONE_CAND_*` keys at 0x08e912ca..0x08e9131a --
     * MAX_UNITS as an INT via the int getter, the other two as floats.
     * Its constructor default is 0x32 = 50 (0x08e90e64) and a VCF value is
     * clamped to at least 1 (0x08e916ea), which is why hardcoding 50 was
     * right for every voice whose VCF omits the key and wrong for the one
     * that sets it. */
    float                   hp_prune_thresh;
    float                   hp_prune_slope;
    uint32_t                hp_prune_max;

    spfy_anchor_voice_t     av;
    uint32_t               *voicing_buf;
    /* Backing array for av.ctx4 (derived ccos context for v100005 voices;
     * NULL for v100006/8). */
    uint8_t                *ctx4_buf;

    /* FE host (loaded SWIttsFe-en-US.dll + parsed vocab + fe_tables). */
    spfy_fe_t              *fe;

    /* Pitch shift via unit-selection bias. */
    float                   pitch_scale;
} spfy_voice_t;

/* Load everything in `paths`. */
int  spfy_voice_load(const spfy_voice_paths_t *paths, spfy_voice_t *out);
void spfy_voice_free(spfy_voice_t *v);

/* Set the pitch shift for subsequent synth calls. */
void spfy_synth_set_pitch_semitones(spfy_voice_t *v, float semitones);

/* Split a user-facing pitch target into the natural-corpus part (handled
 * via spfy_synth_set_pitch_semitones) and the residual that needs
 * post-process PSOLA. */
void spfy_synth_split_pitch(float target_st,
                            float *out_selection_st,
                            float *out_psola_st);

/* Per-call synth stats - filled by do_synth/spfy_synth_to_sink. */
typedef struct {
    size_t   total_played;
    size_t   total_skipped;
    size_t   total_paired_same;
    size_t   total_paired_cross;
    size_t   total_interword_pauses;
    uint64_t wsola_aligned;
    uint64_t wsola_pushes;
    uint32_t n_phrases;
    uint32_t samples_emitted;
} spfy_synth_stats_t;

/* Word boundary callback. */
typedef void (*spfy_word_event_cb_t)(void *ctx, uint32_t sample_offset);

/* Phrase boundary callback. */
typedef void (*spfy_phrase_event_cb_t)(void *ctx, uint32_t phrase_idx,
                                       uint32_t sample_offset);

typedef struct {
    spfy_word_event_cb_t   word_cb;
    spfy_phrase_event_cb_t phrase_cb;
    void                  *ctx;
} spfy_synth_callbacks_t;

/* Per-call synth: text -> FE -> USel -> WSOLA -> sink. */
/* Record the loaded voice's VDB path. */
void spfy4_note_vdb_path(const char *vdb);

int spfy_synth_to_sink(spfy_voice_t *v, const char *text,
                       spfy_wav_writer_t *sink,
                       const spfy_synth_callbacks_t *cb,
                       spfy_synth_stats_t *out_stats);

#endif
