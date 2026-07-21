/* spfy_wasm.c — WebAssembly entry layer for the in-browser demo.
 *
 * Exposes C functions to JavaScript via EMSCRIPTEN_KEEPALIVE:
 *
 *   spfy_wasm_init(const char *voice_dir,
 *                  const char *prefix)     -> int   (SPFY_OK or SPFY_E_*)
 *       Loads a voice triplet <prefix>.vin / <prefix>8.vdb / <prefix>.vcf
 *       from voice_dir on the emscripten virtual FS. JS fetches those
 *       files over the network and writes them into the FS (see
 *       web/index.js) BEFORE calling this — nothing is baked into the
 *       module's .data anymore. The hp_class table is derived from the
 *       VIN (exact for every voice), so no hpclass.bin is needed.
 *
 *       Reloadable: calling it with a DIFFERENT prefix frees the current
 *       voice and loads the new one; calling it with the SAME prefix that
 *       is already loaded is a no-op that returns SPFY_OK. NB the emulator
 *       backend boots ONE FE DLL per module instance (first voice's
 *       language wins), so switching LANGUAGE must be done by tearing the
 *       whole module down and creating a fresh one — JS handles that.
 *
 *   spfy_wasm_free_voice()                 -> void
 *       Drops the loaded voice (frees vin/vdb/vcf state). Same-language
 *       reload path; JS may call it before a fresh init.
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
 * The default build drives the emulator-backed hosted FE (the unmodified
 * SWIttsFe-<lang>.dll bytes run through src/host_emu/); spfy_voice_load()
 * picks the FE DLL for the voice's VCF language from the embedded
 * registry. -DSPFY_WASM_INHOUSE_FE=ON swaps in the pure-C FE instead. */

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
    char          prefix[64]; /* which voice is loaded (e.g. "tom") */
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
void spfy_wasm_free_voice(void)
{
    if (g_state.loaded) {
        spfy_voice_free(&g_state.voice);
        memset(&g_state.voice, 0, sizeof g_state.voice);
        g_state.loaded = 0;
        g_state.prefix[0] = '\0';
    }
}

EMSCRIPTEN_KEEPALIVE
int spfy_wasm_init(const char *voice_dir, const char *prefix)
{
    if (!prefix || !*prefix) prefix = "tom";

    /* Same voice already loaded -> no-op. Different voice -> reload. */
    if (g_state.loaded) {
        if (strncmp(g_state.prefix, prefix, sizeof g_state.prefix) == 0)
            return SPFY_OK;
        spfy_wasm_free_voice();
    }

    char f_vin[80], f_vdb[80], f_vcf[80];
    char p_vin[256], p_vdb[256], p_vcf[256];
    snprintf(f_vin, sizeof f_vin, "%s.vin",  prefix);
    snprintf(f_vdb, sizeof f_vdb, "%s8.vdb", prefix);
    snprintf(f_vcf, sizeof f_vcf, "%s.vcf",  prefix);
    join_path(p_vin, sizeof p_vin, voice_dir, f_vin);
    join_path(p_vdb, sizeof p_vdb, voice_dir, f_vdb);
    join_path(p_vcf, sizeof p_vcf, voice_dir, f_vcf);

    spfy_voice_paths_t paths = {
        .vin         = p_vin,
        .vdb         = p_vdb,
        .vcf         = p_vcf,
        /* hpclass empty -> derived from the VIN (exact for every voice). */
        .hpclass     = "",
        /* Emulator-backed hosted FE is in-process; vocab + fe_tables
         * come from the embedded DLL, not on-disk files. */
        .vocab       = "",
        .fe_tables_a = "",
        .fe_tables_b = "",
    };

    fprintf(stderr, "spfy_wasm_init: loading voice '%s' from %s\n",
            prefix, voice_dir ? voice_dir : "(null)");
    int rc = spfy_voice_load(&paths, &g_state.voice);
    if (rc != SPFY_OK) {
        fprintf(stderr, "spfy_voice_load failed: %d (%s)\n",
                rc, spfy_strerror(rc));
        memset(&g_state.voice, 0, sizeof g_state.voice);
        return rc;
    }
    g_state.sample_rate = g_state.voice.vdb.sample_rate;
    g_state.loaded = 1;
    snprintf(g_state.prefix, sizeof g_state.prefix, "%s", prefix);
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
