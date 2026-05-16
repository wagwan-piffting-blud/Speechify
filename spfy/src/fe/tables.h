#ifndef SPFY_FE_TABLES_H
#define SPFY_FE_TABLES_H

#include <stddef.h>
#include <stdint.h>

/* FE data tables.
 *
 * The original FE engine state holds two pointer arrays at runtime:
 *   state[+0x30] = registry-A: 553 ptrs (lexical -- compounds, names,
 *                  contractions, units, TLDs, etc.)
 *   state[+0x54] = registry-B: 173 ptrs (phonetic -- LTS rules,
 *                  pronunciation transcriptions)
 *
 * Each table is a stream of NUL-terminated records. Each record byte
 * is an index into the FE symbol vocabulary (vocab.h). For lexical
 * tables, byte sequences typically spell out words (idx 1='a', 2='e',
 * ..., 31='z', 97-161 = accented Latin-1, etc.). For phonetic tables,
 * byte sequences include phoneme IDs (229+) interleaved with feature
 * IDs.
 *
 * On disk we store one binary file per table (carved from .data of
 * SWIttsFe-en-US.dll by reveng/fe_carve_tables.py and the inline
 * registry-A scanner in F2). At runtime they're memory-mapped or
 * read into a single contiguous arena.
 */

#define SPFY_FE_REG_A_N   553
#define SPFY_FE_REG_B_N   173

typedef struct {
    const uint8_t *data;        /* points into shared arena */
    uint32_t       size;
    /* Pre-computed cache of record offsets within this table for
     * O(1) random access. NULL until spfy_fe_table_index() is called. */
    uint32_t      *record_offs;
    uint32_t       n_records;
} spfy_fe_table_t;

typedef struct {
    /* Registry-A: 553 lexical tables. */
    spfy_fe_table_t a[SPFY_FE_REG_A_N];
    /* Registry-B: 173 phonetic tables. */
    spfy_fe_table_t b[SPFY_FE_REG_B_N];
    /* Single arena holding all 726 blobs back-to-back so we can free
     * everything with one call. NULL when not loaded. */
    uint8_t        *arena;
    size_t          arena_size;
} spfy_fe_tables_t;

/* Load all 726 tables from two directories of *.bin files. Each file
 * is named tNNN.bin where NNN is the slot index (zero-padded). */
int  spfy_fe_tables_load(const char       *tables_a_dir,
                          const char       *tables_b_dir,
                          spfy_fe_tables_t *out);

void spfy_fe_tables_free(spfy_fe_tables_t *t);

/* Build the per-table record-offset cache (lazy; safe to call
 * repeatedly). Returns the number of records found. */
uint32_t spfy_fe_table_index(spfy_fe_table_t *t);

/* Walk records of a table sequentially. cb returns 0 to continue,
 * non-zero to stop. Returns the number of records visited. */
typedef int (*spfy_fe_record_cb)(const uint8_t *bytes,
                                  uint32_t       n_bytes,
                                  uint32_t       record_idx,
                                  void          *user);

uint32_t spfy_fe_table_walk(const spfy_fe_table_t *t,
                             spfy_fe_record_cb     cb,
                             void                 *user);

#endif
