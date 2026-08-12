/* Layout for the auto-generated CMU dict array. */

#ifndef SPFY_G2P_CMUDICT_DATA_H
#define SPFY_G2P_CMUDICT_DATA_H

#include <stddef.h>

typedef struct {
    const char *word;
    const char *phonemes;
} cmudict_entry_t;

extern const cmudict_entry_t cmudict_entries[];
extern const size_t cmudict_n_entries;

#endif
