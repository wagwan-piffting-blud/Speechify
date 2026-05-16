#ifndef SPFY_VOICE_VDB_LOOKUP_H
#define SPFY_VOICE_VDB_LOOKUP_H

#include <stddef.h>
#include <stdint.h>

#include "voice.h"

/* Name -> VDB recording byte-range lookup.
 *
 * VDB indx ordering does NOT match VIN feat[].filename ordering -- the
 * engine looks up by recording name. We build a sorted index over the
 * vdb's indx_entries at load time so per-recording lookup is O(log n).
 *
 * Each lookup returns the start byte offset into vdb.data plus the size
 * (in bytes) of the recording. The size = next_entry.data_offset -
 * this_entry.data_offset (the indx ends with a sentinel that gives the
 * total data length). */

typedef struct {
    /* Sorted by name (lex). Each entry indexes into vdb->indx_entries[]. */
    uint32_t *order;      /* heap, n_entries indices */
    uint32_t  n_entries;
    /* Cached pointer to the vdb whose entries we're indexing -- NOT owned. */
    const spfy_vdb_t *vdb;
} spfy_vdb_lookup_t;

int  spfy_vdb_lookup_build(const spfy_vdb_t *vdb, spfy_vdb_lookup_t *out);
void spfy_vdb_lookup_free (spfy_vdb_lookup_t *l);

/* Look up by name (NOT NUL-terminated). Returns SPFY_OK and writes
 * data_offset + size on hit; SPFY_E_OOB on miss. */
int  spfy_vdb_lookup_by_name(const spfy_vdb_lookup_t *l,
                             const char *name, size_t name_len,
                             uint32_t *out_offset, uint32_t *out_size);

#endif
