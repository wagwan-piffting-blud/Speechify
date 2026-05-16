#ifndef SPFY_VOICE_H
#define SPFY_VOICE_H

#include <spfy/spfy.h>   /* spfy_voice opaque + error codes */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Inspection API for a loaded voice. Stable surface: only use these accessors,
 * never reach into the spfy_voice struct directly. */

size_t      spfy_voice_n_units      (const spfy_voice *v);
size_t      spfy_voice_n_recordings (const spfy_voice *v);
uint32_t    spfy_voice_sample_rate  (const spfy_voice *v);

/* Hash chunk metadata (n_rows, n_cells). Set to 0 if voice has no hash. */
size_t      spfy_voice_hash_n_rows  (const spfy_voice *v);
size_t      spfy_voice_hash_n_cells (const spfy_voice *v);

/* Look up a VCF parameter by dotted key (e.g. "tts.usel.weights.JOIN_COST_WEIGHT").
 * Returns SPFY_OK and writes the value, or SPFY_E_INVAL if not found. */
int         spfy_voice_vcf_get_str  (const spfy_voice *v, const char *key,
                                     const char **out);
int         spfy_voice_vcf_get_f64  (const spfy_voice *v, const char *key,
                                     double *out);

#ifdef __cplusplus
}
#endif
#endif
