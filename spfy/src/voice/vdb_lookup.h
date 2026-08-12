#ifndef SPFY_VOICE_VDB_LOOKUP_H
#define SPFY_VOICE_VDB_LOOKUP_H

#include <stddef.h>
#include <stdint.h>

#include "voice.h"

/* Name -> VDB recording byte-range lookup. */

typedef struct {
    uint32_t *order;
    uint32_t  n_entries;
    const spfy_vdb_t *vdb;
} spfy_vdb_lookup_t;

int  spfy_vdb_lookup_build(const spfy_vdb_t *vdb, spfy_vdb_lookup_t *out);
void spfy_vdb_lookup_free (spfy_vdb_lookup_t *l);

/* Look up by name (NOT NUL-terminated). */
int  spfy_vdb_lookup_by_name(const spfy_vdb_lookup_t *l,
                             const char *name, size_t name_len,
                             uint32_t *out_offset, uint32_t *out_size);

#endif
