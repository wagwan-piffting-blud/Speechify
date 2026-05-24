/* spfy_wasm.c — WebAssembly entry layer for the in-browser demo.
 *
 * Exposes four C functions to JavaScript via EMSCRIPTEN_KEEPALIVE:
 *
 *   spfy_wasm_init(const char *voice_dir)  -> int   (SPFY_OK or SPFY_E_*)
 *       Loads Tom's voice triplet from voice_dir on the emscripten
 *       virtual FS (we mount /voice/ via --preload-file). Idempotent:
 *       a second call while already loaded returns 0 without re-loading.
 *
 *   spfy_wasm_synth(const char *text)      -> int   (SPFY_OK or SPFY_E_*)
 *       Synthesizes text. On success the resulting int16 mono PCM lives
 *       in the module's wasm memory; JS reads it via the three getters
 *       below, then must call spfy_wasm_reset() before issuing another
 *       synth call.
 *
 *   spfy_wasm_pcm_ptr()                    -> int16_t *
 *   spfy_wasm_pcm_len()                    -> size_t   (samples)
 *   spfy_wasm_sample_rate()                -> uint32_t
 *   spfy_wasm_reset()                      -> void
 *
 * SPFY_FE_INTERNAL is implicit: spfy_voice_load() picks the in-house
 * pure-C FE when the hosted FE isn't compiled in (this build excludes
 * src/fe_host's PE-loader and links the stub instead). */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <emscripten/emscripten.h>

#include <spfy/spfy.h>
#include "../../src/synth/spfy_synth_lib.h"
#include "../../src/wsola/wav.h"

/* ------------------- module-level state ----------------------------- */

static struct {
    spfy_voice_t  voice;
    int           loaded;
    int16_t      *pcm;
    size_t        pcm_n;       /* used samples */
    size_t        pcm_cap;     /* allocated samples */
    uint32_t      sample_rate;
} g_state;

/* Growable PCM collector callback for spfy_wav_open_callback. */
static int collect_pcm_cb(void *ctx, const int16_t *samples, size_t n)
{
    (void)ctx;
    if (g_state.pcm_n + n > g_state.pcm_cap) {
        size_t new_cap = g_state.pcm_cap ? g_state.pcm_cap * 2 : 16384;
        while (new_cap < g_state.pcm_n + n) new_cap *= 2;
        int16_t *nb = (int16_t *)realloc(g_state.pcm,
                                          new_cap * sizeof(int16_t));
        if (!nb) return SPFY_E_NOMEM;
        g_state.pcm = nb;
        g_state.pcm_cap = new_cap;
    }
    memcpy(g_state.pcm + g_state.pcm_n, samples, n * sizeof(int16_t));
    g_state.pcm_n += n;
    return SPFY_OK;
}

/* Concatenate `dir` + "/" + `name` into out_buf. dir may be empty. */
static void join_path(char *out_buf, size_t cap, const char *dir, const char *name)
{
    if (!dir || !*dir) { snprintf(out_buf, cap, "%s", name); return; }
    size_t L = strlen(dir);
    char sep = (dir[L - 1] == '/' || dir[L - 1] == '\\') ? 0 : '/';
    if (sep) snprintf(out_buf, cap, "%s/%s", dir, name);
    else     snprintf(out_buf, cap, "%s%s",  dir, name);
}

/* ------------------- exported API ---------------------------------- */

EMSCRIPTEN_KEEPALIVE
int spfy_wasm_init(const char *voice_dir)
{
    if (g_state.loaded) return SPFY_OK;

    char p_vin[256], p_vdb[256], p_vcf[256], p_hpc[256];
    join_path(p_vin, sizeof p_vin, voice_dir, "tom.vin");
    join_path(p_vdb, sizeof p_vdb, voice_dir, "tom8.vdb");
    join_path(p_vcf, sizeof p_vcf, voice_dir, "tom.vcf");
    join_path(p_hpc, sizeof p_hpc, voice_dir, "tom_hpclass.bin");

    spfy_voice_paths_t paths = {
        .vin         = p_vin,
        .vdb         = p_vdb,
        .vcf         = p_vcf,
        .hpclass     = p_hpc,
        /* In-house pure-C FE is in-process; vocab + fe_tables are unused. */
        .vocab       = "",
        .fe_tables_a = "",
        .fe_tables_b = "",
    };

    fprintf(stderr, "spfy_wasm_init: loading voice from %s\n",
            voice_dir ? voice_dir : "(null)");
    int rc = spfy_voice_load(&paths, &g_state.voice);
    if (rc != SPFY_OK) {
        fprintf(stderr, "spfy_voice_load failed: %d (%s)\n",
                rc, spfy_strerror(rc));
        return rc;
    }
    g_state.sample_rate = g_state.voice.vdb.sample_rate;
    g_state.loaded = 1;
    fprintf(stderr,
        "voice loaded: %u units, %u feat entries, sample_rate=%u\n",
        g_state.voice.units.n_units,
        g_state.voice.feat.n_entries,
        g_state.sample_rate);
    return SPFY_OK;
}

EMSCRIPTEN_KEEPALIVE
int spfy_wasm_synth(const char *text)
{
    if (!g_state.loaded) {
        fprintf(stderr, "spfy_wasm_synth: not initialized\n");
        return SPFY_E_INVAL;
    }
    if (!text) return SPFY_E_INVAL;

    /* Reset previous output. Keep the allocation around so back-to-back
     * synths reuse the buffer; just zero the used-count. */
    g_state.pcm_n = 0;

    spfy_wav_writer_t sink = {0};
    int rc = spfy_wav_open_callback(&sink, collect_pcm_cb, NULL,
                                     g_state.sample_rate);
    if (rc != SPFY_OK) return rc;

    rc = spfy_synth_to_sink(&g_state.voice, text, &sink, NULL, NULL);
    if (rc != SPFY_OK) {
        fprintf(stderr, "spfy_synth_to_sink: %d (%s)\n",
                rc, spfy_strerror(rc));
    }
    spfy_wav_close(&sink);
    return rc;
}

EMSCRIPTEN_KEEPALIVE
int16_t *spfy_wasm_pcm_ptr(void)    { return g_state.pcm; }

EMSCRIPTEN_KEEPALIVE
size_t   spfy_wasm_pcm_len(void)    { return g_state.pcm_n; }

EMSCRIPTEN_KEEPALIVE
uint32_t spfy_wasm_sample_rate(void) { return g_state.sample_rate; }

EMSCRIPTEN_KEEPALIVE
void spfy_wasm_reset(void)
{
    /* Soft reset: keep the loaded voice but drop the PCM buffer. */
    free(g_state.pcm);
    g_state.pcm = NULL;
    g_state.pcm_n = 0;
    g_state.pcm_cap = 0;
}

/* main() exists only because emscripten wants a startup symbol. We
 * don't actually do any work here; JS drives via cwrap'd functions. */
int main(void)
{
    fprintf(stderr, "spfy_wasm ready — call Module.cwrap('spfy_wasm_init', ...)\n");
    return 0;
}
