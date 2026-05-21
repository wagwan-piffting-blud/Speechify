/* Layout for the auto-generated CMU dict array.
 * The .c file containing the actual data is emitted by
 * spfy/tools/cmudict_codegen.py. */

#ifndef SPFY_G2P_CMUDICT_DATA_H
#define SPFY_G2P_CMUDICT_DATA_H

#include <stddef.h>

typedef struct {
    const char *word;       /* lowercase, NUL-terminated */
    const char *phonemes;   /* uppercase ARPAbet, space-sep, NUL-term */
} cmudict_entry_t;

extern const cmudict_entry_t cmudict_entries[];
extern const size_t cmudict_n_entries;

#endif
