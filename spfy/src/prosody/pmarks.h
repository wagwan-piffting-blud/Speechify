/* Pitch-mark store — reads the engine's own pmindex/pmdata pair. */

#ifndef SPFY_PROSODY_PMARKS_H
#define SPFY_PROSODY_PMARKS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t  rate;
    uint32_t  n_units;
    uint32_t *index;
    int16_t  *data;
    size_t    n_data;
} spfy_pmarks_t;

/* Load "<stem>.pmindex" + "<stem>.pmdata". */
int  spfy_pmarks_load(const char *stem, spfy_pmarks_t *out);

void spfy_pmarks_free(spfy_pmarks_t *t);

/* Periods for one unit. */
int  spfy_pmarks_get(const spfy_pmarks_t *t, uint32_t uid,
                     const int16_t **periods);

#ifdef __cplusplus
}
#endif

#endif
