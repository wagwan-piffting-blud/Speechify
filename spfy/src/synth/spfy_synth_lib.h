#ifndef SPFY_SYNTH_LIB_H
#define SPFY_SYNTH_LIB_H

/* Reusable synth library shared by the CLI (spfy_synth.exe) and the
 * SAPI 5 voice DLL (spfy_sapi.dll). Splits the work into two stages:
 *
 *   spfy_voice_load()  — heavy one-time work (parse all voice tables,
 *                        bring up the FE host, build bucket index).
 *                        Voice handles are reusable across many synth
 *                        calls; load once at COM-server init.
 *
 *   spfy_synth_to_sink() — per-text synthesis. Streams s16 PCM at the
 *                          voice's native sample rate through the
 *                          caller-supplied write callback.
 *
 * Everything in spfy_voice_t is publicly defined because the synth
 * loop still reaches into the voice's tables directly. Treat the
 * fields as opaque from outside spfy/src/synth and spfy/src/cli;
 * a later cleanup pass may hide them behind accessors. */

#include <stddef.h>
#include <stdint.h>

#include <spfy/spfy.h>

#include "../voice/voice.h"
#include "../voice/unit_table.h"
#include "../voice/feat_table.h"
#include "../voice/vdb_lookup.h"
#include "../voice/ccos.h"
#include "../voice/voice_runtime.h"
#include "../voice/vcf_matrix.h"
#include "../voice/chunk_table.h"
#include "../usel/anchor_score.h"
#include "../usel/hash.h"
#include "../usel/prsl.h"
#include "../cart/cart.h"
#include "../fe/fe.h"
#include "../wsola/wav.h"

/* All paths needed to bring up a voice. Caller fills these in (CLI from
 * argv, SAPI DLL from registry + known directory layout). All strings
 * are borrowed; spfy_voice_load() does not retain pointers past return. */
typedef struct {
    const char *vin;        /* per-voice */
    const char *vdb;        /* per-voice (the 8kHz mu-law VDB) */
    const char *vcf;        /* per-voice */
    const char *hpclass;    /* shared: spfy/data/<voice_family>_hpclass.bin */
    const char *vocab;      /* shared: spfy/build/fe_symbol_table.json */
    const char *fe_tables_a;/* shared: spfy/data/fe_tables_a */
    const char *fe_tables_b;/* shared: spfy/data/fe_tables   */
} spfy_voice_paths_t;

/* Loaded voice. All fields are owned by the struct; freed by
 * spfy_voice_free(). Initialise with `spfy_voice_t v = {0};` before
 * passing to spfy_voice_load. */
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

    /* hpclass.bin (per voice family). */
    uint8_t                *hpc;
    uint32_t                hpc_n;

    /* Per-hp-class candidate bucket index, built from units. */
    uint32_t                hpc_buckets;     /* fixed to 256 currently */
    uint32_t               *bucket_n;
    uint32_t               *bucket_cap;
    uint32_t              **bucket;

    /* Anchor-scorer voice — references units/ccos/maps/pros/hpc/voicing. */
    spfy_anchor_voice_t     av;
    /* Backing array for av.voicing (built from FE phoneset + VIN feat). */
    uint32_t               *voicing_buf;

    /* FE host (loaded SWIttsFe-en-US.dll + parsed vocab + fe_tables). */
    spfy_fe_t              *fe;
} spfy_voice_t;

/* Load everything in `paths`. On failure returns SPFY_E_*; `out` is
 * partially initialised (call spfy_voice_free to clean up). */
int  spfy_voice_load(const spfy_voice_paths_t *paths, spfy_voice_t *out);
void spfy_voice_free(spfy_voice_t *v);

/* Per-call synth stats — filled by do_synth/spfy_synth_to_sink. CLI prints
 * them, SAPI ignores. */
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

/* Word boundary callback. Fires once per word transition during synthesis
 * (after each silence/silence-tail and once for the first non-silence
 * word). sample_offset is the running sample count at the moment WSOLA is
 * about to emit the new word — multiply by 2 to convert to byte offset
 * for SAPI's ullAudioStreamOffset. Callbacks are invoked in word order,
 * so the SAPI side can map the N-th firing to the N-th word in the input
 * text. */
typedef void (*spfy_word_event_cb_t)(void *ctx, uint32_t sample_offset);

typedef struct {
    spfy_word_event_cb_t word_cb;
    void                *ctx;
} spfy_synth_callbacks_t;

/* Per-call synth: text -> FE -> USel -> WSOLA -> sink. The voice is loaded
 * once by the caller (CLI main() or SAPI Speak()) and reused across calls.
 * The sink is any open spfy_wav_writer_t (file or callback mode); we do
 * NOT open or close it. `cb` is optional (NULL disables boundary events).
 * Implementation lives in spfy/src/cli/spfy_synth.c (compiled into both
 * the CLI and the SAPI DLL via the SPFY_SYNTH_NO_MAIN compile switch).
 * Returns SPFY_OK on success or SPFY_E_* on error. */
int spfy_synth_to_sink(spfy_voice_t *v, const char *text,
                       spfy_wav_writer_t *sink,
                       const spfy_synth_callbacks_t *cb,
                       spfy_synth_stats_t *out_stats);

#endif
