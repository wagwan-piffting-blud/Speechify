#ifndef SPFY_USEL_HASH_BUILD_H
#define SPFY_USEL_HASH_BUILD_H

#include <stddef.h>
#include <stdint.h>

/* Builder for the join-cost row-displacement table read by hash.c.
 *
 * Layout and addressing are specified in spfy/src/vb/SPEC_S4_hash.md:
 *   idx = rows[uid_right] + uid_left
 *   key[idx] must equal uid_right, else miss
 *   empty cells are (key = 0xFFFFFFFF, cost = -1.0f)
 *
 * ⛔ n_cells IS NOT "the last populated cell". The vendor engine probes that
 * index with no bounds check (SWIttsUSel.dll+0xb7e6), so the table must be at
 * least max(rows[]) + n_rows cells wide or a MISS reads off the end of the
 * allocation and Speechify access-violates. spfy_hash_build pads to that;
 * spfy_vb_verify checks it.
 *
 * Placement is first-fit row displacement over an occupancy BITMAP, probing 64
 * candidate displacements at a time: the 64 occupancy bits starting anywhere in
 * the table are one unaligned 64-bit read, so a zero bit in the OR taken over a
 * row's offsets IS a displacement that fits.
 *
 * ⭐ THE SCAN RESTARTS AT THE LOWEST FREE CELL FOR EVERY ROW. The previous
 * version carried a forward-only cursor shared across rows, so once it advanced
 * the sparse cells behind it could never be used again. Measured on crsmara
 * that table was 31.0% full where jill ships 71.8%, and `hash` is 86% of the
 * VIN. Occupancy by slab named it: jill 100% for nine of sixteen slabs then
 * decaying, ours a flat ~30% end to end. */

#define SPFY_HASH_EMPTY_KEY  0xFFFFFFFFu
#define SPFY_HASH_EMPTY_COST (-1.0f)

/* The placement passes, in order, are:
 *
 *   DENSE     first fit from the lowest free cell over the table that already
 *             exists. This is the pass that buys the fill -- a small row drops
 *             into a hole the big ones left -- and it cannot grow the table.
 *   FRONTIER  for a row too wide for anything the dense region still has, lay
 *             it so its LAST entry lands at the high-water mark. The cells
 *             there are nearly all free and the mark does not move.
 *   TAIL      last resort at the mark itself, which costs a whole span.
 *
 * ⛔ THE FRONTIER PASS MUST AIM AT high_water - span, NOT AT high_water. The
 * obvious spelling costs a span of new cells every time it fires: on jill,
 * 13,683 rows of 560,534 reached it and the table grew to 445M cells -- a
 * 3.5 GB allocation -- where the correct table is 2.6M. */

/* Order in which rows are offered to the packer. Which one the vendor used is
 * an open question, and it is the only thing standing between a semantically
 * correct rebuild and a byte-identical one. */
typedef enum {
    SPFY_HASH_ORDER_FFD = 0,   /* largest row first - best density */
    SPFY_HASH_ORDER_ROW = 1    /* ascending uid_right - simplest possible */
} spfy_hash_order;

typedef struct {
    uint32_t uid_right;
    uint32_t uid_left;
    float    cost;
} spfy_hash_pair;

typedef struct {
    uint32_t  n_rows;
    uint32_t  n_cells;
    uint32_t *rows;    /* n_rows   */
    uint32_t *key;     /* n_cells  */
    float    *cost;    /* n_cells  */
} spfy_hash_table;

/* n_rows is the uid_right domain size. It is NOT derivable from the pairs --
 * a voice can carry rows no pair populates -- so the caller supplies it. */
int  spfy_hash_build(const spfy_hash_pair *pairs, size_t n_pairs,
                     uint32_t n_rows, spfy_hash_order order,
                     spfy_hash_table *out);

void spfy_hash_table_free(spfy_hash_table *t);

/* Serialise to the head/rows/cell sub-chunk sequence that hash.c reads.
 * Caller frees *out. */
int  spfy_hash_serialise(const spfy_hash_table *t, uint8_t **out, size_t *out_n);

#endif
