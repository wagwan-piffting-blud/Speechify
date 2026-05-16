#ifndef SPFY_H
#define SPFY_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPFY_OK                 0
#define SPFY_E_INVAL           -1
#define SPFY_E_IO              -2
#define SPFY_E_FORMAT          -3
#define SPFY_E_NOMEM           -4
#define SPFY_E_NOTSUP          -5
#define SPFY_E_OOB             -6

typedef struct spfy_voice spfy_voice;
typedef struct spfy_synth spfy_synth;

int  spfy_voice_open(const char *vin_path,
                     const char *vdb_path,
                     const char *vcf_path,
                     spfy_voice **out);
void spfy_voice_close(spfy_voice *v);

int  spfy_synth_create(const spfy_voice *v, spfy_synth **out);
void spfy_synth_destroy(spfy_synth *s);

typedef int (*spfy_pcm_cb)(const int16_t *samples, size_t n_samples, void *user);

int  spfy_synth_speak_spr(spfy_synth *s,
                          const char *spr,
                          spfy_pcm_cb cb,
                          void *user);

int  spfy_synth_set_trace(spfy_synth *s, FILE *jsonl_out);

const char *spfy_strerror(int code);
const char *spfy_version(void);

#ifdef __cplusplus
}
#endif
#endif
