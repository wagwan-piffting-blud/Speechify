#ifndef SPFY_VOICE_FEAT_TABLE_H
#define SPFY_VOICE_FEAT_TABLE_H

#include <stddef.h>
#include <stdint.h>

#include "voice.h"

/* feat chunk: 2-byte name length + name bytes + 4-byte stored_id per entry.
 * Tom: 8118 entries. The names are recording filenames like "date_001",
 * "news15_040", etc. -- these are the keys used to look up audio in
 * vdb.indx_entries.
 *
 * NB: feat[].filename ordering does NOT match vdb.indx ordering -- callers
 * must look up by NAME, not by position (per reveng/README_TECHNICAL.md
 * "indx" section, "CRITICAL ordering" note in MEMORY.md). */

typedef struct {
    const char *name;       /* aliases VIN buffer; not NUL-terminated */
    uint16_t    name_len;
    uint32_t    stored_id;
} spfy_feat_entry_t;

typedef struct {
    spfy_feat_entry_t *entries;     /* heap, owned */
    uint32_t           n_entries;
} spfy_feat_table_t;

int  spfy_feat_table_load(const spfy_vin_t *vin, spfy_feat_table_t *out);
void spfy_feat_table_free(spfy_feat_table_t *t);

#endif
