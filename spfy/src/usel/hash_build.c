#include "hash_build.h"

#include "../common/log.h"
#include "../../include/spfy/spfy.h"

#include <stdlib.h>
#include <string.h>

#ifdef _OPENMP
#include <omp.h>
#endif

#define ALLOC_FAIL(what, n) \
    do { spfy_log_err("hash_build: %s alloc failed (%lu bytes)", \
                      (what), (unsigned long)(n)); goto done; } while (0)

#define ALL_BAD (~(uint64_t)0)

typedef struct {
    uint32_t left;
    float    cost;
} cell_entry;

typedef struct {
    uint32_t row;
    uint32_t first;
    uint32_t count;
} row_bucket;

static int cmp_entry_left(const void *a, const void *b)
{
    uint32_t x = ((const cell_entry *)a)->left;
    uint32_t y = ((const cell_entry *)b)->left;
    return (x < y) ? -1 : (x > y);
}

static int cmp_bucket_desc(const void *a, const void *b)
{
    const row_bucket *x = (const row_bucket *)a;
    const row_bucket *y = (const row_bucket *)b;
    if (x->count != y->count) return (x->count < y->count) ? 1 : -1;
    return (x->row < y->row) ? -1 : (x->row > y->row);
}

/* ---- occupancy bitmap ----
 *
 * One BIT per cell, not a 4-byte next-free index. At 24M cells that is 3 MB
 * instead of 100 MB, which is what makes it affordable to test 64 candidate
 * displacements at a time: the 64 occupancy bits starting anywhere in the
 * table are a single unaligned 64-bit read. */
typedef struct {
    uint64_t *w;
    size_t    cap;      /* cells */
} bmap;

static int bm_grow(bmap *m, size_t need)
{
    if (need <= m->cap) return SPFY_OK;
    size_t ncap = m->cap ? m->cap : (size_t)1 << 20;
    while (ncap < need) ncap += ncap / 2u + 1024u;
    ncap = (ncap + 63u) & ~(size_t)63;
    size_t ow = m->cap / 64u, nw = ncap / 64u;
    /* +2 words of slack so occ64() may read one word past the last cell. */
    uint64_t *p = (uint64_t *)realloc(m->w, (nw + 2u) * sizeof *p);
    if (!p) return SPFY_E_NOMEM;
    memset(p + ow, 0, (nw + 2u - ow) * sizeof *p);
    m->w = p;
    m->cap = ncap;
    return SPFY_OK;
}

/* The 64 occupancy bits starting at cell p; bit j is cell p+j. */
static inline uint64_t bm_occ64(const bmap *m, size_t p)
{
    size_t w = p >> 6;
    unsigned s = (unsigned)(p & 63u);
    uint64_t lo = m->w[w] >> s;
    return s ? (lo | (m->w[w + 1u] << (64u - s))) : lo;
}

/* Which of the 64 displacements p..p+63 collide. A ZERO bit fits. */
static inline uint64_t bm_probe64(const bmap *m, size_t p,
                                  const uint32_t *offs, uint32_t n)
{
    uint64_t bad = bm_occ64(m, p);
    for (uint32_t k = 1; k < n && bad != ALL_BAD; ++k)
        bad |= bm_occ64(m, p + offs[k]);
    return bad;
}

static inline void bm_take(bmap *m, size_t i)
{
    m->w[i >> 6] |= (uint64_t)1u << (i & 63u);
}

static size_t bm_first_free(const bmap *m, size_t i)
{
    size_t w = i >> 6;
    uint64_t f = ~m->w[w] & (ALL_BAD << (i & 63u));
    while (!f) {
        if (++w >= m->cap / 64u) return m->cap;
        f = ~m->w[w];
    }
    size_t p = (w << 6);
    while (f && !(f & 1u)) { f >>= 1; ++p; }
    return p;
}

/* Lowest displacement in [from, to) that fits and is not below the row's own
 * smallest left uid, or SIZE_MAX. The caller guarantees to+span is inside the
 * bitmap, so this never allocates -- which is what lets the dense pass sweep
 * the whole existing table without the scan itself enlarging it. */
static size_t place_scan(const bmap *m, size_t from, size_t to,
                         const uint32_t *offs, uint32_t n, size_t floor_anchor)
{
    for (size_t p = from & ~(size_t)63; p < to; p += 64u) {
        uint64_t bad = bm_probe64(m, p, offs, n);
        if (bad == ALL_BAD) continue;
        uint64_t fits = ~bad;
        while (fits) {
            unsigned j = 0;
            uint64_t t = fits;
            while (!(t & 1u)) { t >>= 1; ++j; }
            if (p + j >= floor_anchor) return p + j;
            fits &= ~((uint64_t)1u << j);
        }
    }
    return (size_t)-1;
}

/* ⭐ THE SAME FIRST FIT, FOUND ON N THREADS.
 *
 * Measured on the 626,170-unit crys corpus: S4 was 553.9 s of a 579.2 s
 * build, and inside it `frames 2.8s  score 4.1s  pack+write 546.6s`. The
 * OpenMP scoring costs 544 million candidate joins in four seconds; the
 * single-threaded packer costs nine minutes. It is quadratic -- each row
 * scans from the lowest free cell up to the high-water mark, and the mark
 * grows to 11.5 M cells -- which the 29 k-unit probe confirms by scaling:
 * 2.2 s there predicts ~790 s here against 546 s observed.
 *
 * ⚠ IDENTICAL OUTPUT, NOT AN APPROXIMATION. The range is cut into CONTIGUOUS
 * 64-aligned chunks, each thread runs the ordinary scan on its own chunk, and
 * the answer is the hit from the LOWEST chunk that has one -- which is the
 * lowest fitting position, exactly what the sequential first fit returns.
 * Capping the scan instead would be far simpler and is NOT safe: it lowers
 * the fill, and this voice's `hash` is already 94.5 MB against a 100 MB
 * budget.
 *
 * Threads only earn their overhead on a long scan, so short ranges -- which
 * is every row early on, while the table is still sparse -- stay sequential.
 */
#define PAR_MIN_RANGE ((size_t)1 << 18)
#define PAR_MAX_CHUNK 64

static size_t place_scan_par(const bmap *m, size_t from, size_t to,
                             const uint32_t *offs, uint32_t n,
                             size_t floor_anchor)
{
#ifdef _OPENMP
    size_t range = to > from ? to - from : 0u;
    if (range >= PAR_MIN_RANGE) {
        int nt = omp_get_max_threads();
        if (nt > PAR_MAX_CHUNK) nt = PAR_MAX_CHUNK;
        if (nt > 1) {
            size_t res[PAR_MAX_CHUNK];
            /* 64-aligned so the chunks tile the same p sequence the
             * sequential scan walks -- no position visited twice, none
             * skipped. */
            size_t step = ((range / (size_t)nt) + 63u) & ~(size_t)63u;
            if (step < 64u) step = 64u;
            size_t nc = (range + step - 1u) / step;
            if (nc > (size_t)nt) nc = (size_t)nt;
            int used = (int)nc;
            int c;
#pragma omp parallel for schedule(static, 1)
            for (c = 0; c < used; ++c) {
                size_t a = from + (size_t)c * step;
                size_t b = a + step;
                if (b > to) b = to;
                res[c] = (a < b)
                       ? place_scan(m, a, b, offs, n, floor_anchor)
                       : (size_t)-1;
            }
            for (c = 0; c < used; ++c)
                if (res[c] != (size_t)-1) return res[c];
            return (size_t)-1;
        }
    }
#else
    (void)PAR_MIN_RANGE;
#endif
    return place_scan(m, from, to, offs, n, floor_anchor);
}

int spfy_hash_build(const spfy_hash_pair *pairs, size_t n_pairs,
                    uint32_t n_rows, spfy_hash_order order,
                    spfy_hash_table *out)
{
    if (!pairs || !out || !n_pairs || !n_rows) return SPFY_E_INVAL;
    memset(out, 0, sizeof *out);

    int rc = SPFY_E_NOMEM;
    uint32_t   *counts  = NULL;
    row_bucket *buckets = NULL, *order_v = NULL;
    cell_entry *ents    = NULL;
    uint32_t   *rows    = NULL, *fill = NULL, *offs = NULL;
    bmap        bm      = { NULL, 0 };

    counts = (uint32_t *)calloc(n_rows, sizeof *counts);
    if (!counts) ALLOC_FAIL("counts", (size_t)n_rows * 4u);

    uint32_t max_left = 0, max_count = 0;
    for (size_t i = 0; i < n_pairs; ++i) {
        if (pairs[i].uid_right >= n_rows) { rc = SPFY_E_INVAL; goto done; }
        counts[pairs[i].uid_right]++;
        if (pairs[i].uid_left > max_left) max_left = pairs[i].uid_left;
    }

    buckets = (row_bucket *)malloc((size_t)n_rows * sizeof *buckets);
    fill    = (uint32_t *)malloc((size_t)n_rows * sizeof *fill);
    ents    = (cell_entry *)malloc(n_pairs * sizeof *ents);
    rows    = (uint32_t *)calloc(n_rows, sizeof *rows);
    if (!buckets) ALLOC_FAIL("buckets", (size_t)n_rows * sizeof *buckets);
    if (!fill)    ALLOC_FAIL("fill",    (size_t)n_rows * 4u);
    if (!ents)    ALLOC_FAIL("ents",    n_pairs * sizeof *ents);
    if (!rows)    ALLOC_FAIL("rows",    (size_t)n_rows * 4u);

    uint32_t running = 0;
    for (uint32_t r = 0; r < n_rows; ++r) {
        buckets[r].row   = r;
        buckets[r].first = running;
        buckets[r].count = counts[r];
        fill[r] = running;
        if (counts[r] > max_count) max_count = counts[r];
        running += counts[r];
    }
    for (size_t i = 0; i < n_pairs; ++i) {
        uint32_t r = pairs[i].uid_right;
        ents[fill[r]].left = pairs[i].uid_left;
        ents[fill[r]].cost = pairs[i].cost;
        fill[r]++;
    }

    /* Sort each row by left uid: the placement works on offsets from the
     * smallest, and it makes the build independent of input pair order. */
    for (uint32_t r = 0; r < n_rows; ++r)
        if (buckets[r].count > 1)
            qsort(ents + buckets[r].first, buckets[r].count,
                  sizeof *ents, cmp_entry_left);

    order_v = (row_bucket *)malloc((size_t)n_rows * sizeof *order_v);
    offs    = (uint32_t *)malloc((size_t)max_count * sizeof *offs);
    if (!order_v) ALLOC_FAIL("order_v", (size_t)n_rows * sizeof *order_v);
    if (!offs)    ALLOC_FAIL("offs",    (size_t)max_count * 4u);
    memcpy(order_v, buckets, (size_t)n_rows * sizeof *order_v);
    if (order == SPFY_HASH_ORDER_FFD)
        qsort(order_v, n_rows, sizeof *order_v, cmp_bucket_desc);

    if (bm_grow(&bm, (size_t)max_left + 1024u) != SPFY_OK)
        ALLOC_FAIL("bitmap", ((size_t)max_left + 1024u) / 8u);

    size_t high_water = 0;
    size_t first_free = 0;
    size_t n_tail     = 0;

    for (uint32_t i = 0; i < n_rows; ++i) {
        uint32_t n = order_v[i].count;
        if (!n) continue;

        const cell_entry *E = ents + order_v[i].first;
        uint32_t span = E[n - 1].left - E[0].left;
        for (uint32_t k = 0; k < n; ++k) offs[k] = E[k].left - E[0].left;

        /* The anchor holds the row's SMALLEST left uid, so it can never sit
         * below it -- the displacement d = anchor - E[0].left is unsigned. */
        size_t floor_anchor = E[0].left;
        size_t placed = (size_t)-1;

        /* Dense pass: first fit from the lowest free cell, over the table that
         * already exists. This is what the old forward-only cursor could not
         * do, and it is the entire difference between a 31%-full table and a
         * 93%-full one. It cannot grow the table, so its cost is bounded by
         * the high-water mark -- cheap early, and by the time it is expensive
         * the table is nearly full and most rows fit at once. */
        size_t from = floor_anchor > first_free ? floor_anchor : first_free;
        if (high_water > (size_t)span && from < high_water - span)
            placed = place_scan_par(&bm, from, high_water - span,
                                offs, n, floor_anchor);

        /* Frontier pass: the dense region has nothing for a row this wide, so
         * try to lay it so its LAST entry lands at the high-water mark. The
         * cells there are nearly all free, and a row placed this way does not
         * advance the mark at all.
         *
         * ⛔ NOT "place it AT high_water". That is the obvious spelling and it
         * costs a whole SPAN of new cells every time: on jill, 13,683 rows of
         * 560,534 reached the fallback and the table grew to 445M cells --
         * a 3.5 GB allocation -- where the correct table is 2.6M. */
        if (placed == (size_t)-1) {
            size_t back = high_water > (size_t)span ? high_water - span : 0u;
            if (back < floor_anchor) back = floor_anchor;
            size_t to = back + (size_t)span + 64u;
            if (bm_grow(&bm, to + (size_t)span + 128u) != SPFY_OK)
                ALLOC_FAIL("bitmap(frontier)", (to + span + 128u) / 8u);
            placed = place_scan(&bm, back, to, offs, n, floor_anchor);
        }

        if (placed == (size_t)-1) {
            /* At or past the high-water mark every cell is virgin, so the
             * whole pattern fits. Last resort, and COUNTED: a packer that
             * lands here often is appending, not packing. The old code left
             * `placed` at 0 here and wrote rows[r] = 0 - E[0].left, which
             * underflows. */
            placed = high_water > floor_anchor ? high_water : floor_anchor;
            if (bm_grow(&bm, placed + span + 128u) != SPFY_OK)
                ALLOC_FAIL("bitmap(tail)", (placed + span + 128u) / 8u);
            ++n_tail;
        }

        for (uint32_t k = 0; k < n; ++k) bm_take(&bm, placed + offs[k]);
        rows[order_v[i].row] = (uint32_t)(placed - E[0].left);
        if (placed <= first_free) first_free = bm_first_free(&bm, first_free);

        size_t last = placed + offs[n - 1] + 1u;
        if (last > high_water) high_water = last;
    }

    if (high_water > 0xFFFFFFFFu) {
        spfy_log_err("hash_build: %zu cells overflows the u32 n_cells field",
                     high_water);
        rc = SPFY_E_INVAL;
        goto done;
    }
    if (n_tail)
        spfy_log_warn("hash_build: %zu of %u rows appended at the high-water "
                      "mark (no dense placement found)", n_tail, n_rows);

    {
        uint32_t n_cells = (uint32_t)high_water;
        uint32_t *key  = (uint32_t *)malloc((size_t)n_cells * sizeof *key);
        float    *cost = (float *)malloc((size_t)n_cells * sizeof *cost);
        if (!key || !cost) {
            free(key); free(cost);
            ALLOC_FAIL("key/cost", (size_t)n_cells * 8u);
        }

        for (uint32_t i = 0; i < n_cells; ++i) {
            key[i]  = SPFY_HASH_EMPTY_KEY;
            cost[i] = SPFY_HASH_EMPTY_COST;
        }
        for (uint32_t r = 0; r < n_rows; ++r) {
            uint32_t n = buckets[r].count;
            if (!n) continue;
            const cell_entry *E = ents + buckets[r].first;
            for (uint32_t k = 0; k < n; ++k) {
                size_t idx = (size_t)rows[r] + E[k].left;
                key[idx]  = r;
                cost[idx] = E[k].cost;
            }
        }
        out->n_rows  = n_rows;
        out->n_cells = n_cells;
        out->rows    = rows;
        out->key     = key;
        out->cost    = cost;
        rows = NULL;
        rc = SPFY_OK;
    }

done:
    free(counts); free(buckets); free(order_v); free(ents);
    free(fill); free(offs); free(rows); free(bm.w);
    return rc;
}

void spfy_hash_table_free(spfy_hash_table *t)
{
    if (!t) return;
    free(t->rows); free(t->key); free(t->cost);
    memset(t, 0, sizeof *t);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

int spfy_hash_serialise(const spfy_hash_table *t, uint8_t **out, size_t *out_n)
{
    if (!t || !out || !out_n) return SPFY_E_INVAL;

    size_t rows_n = (size_t)t->n_rows * 4u;
    size_t cell_n = (size_t)t->n_cells * 8u;
    size_t total  = (8u + 8u) + (8u + rows_n) + (8u + cell_n);

    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return SPFY_E_NOMEM;

    uint8_t *p = buf;
    memcpy(p, "head", 4); put_u32(p + 4, 8); p += 8;
    put_u32(p, t->n_rows);  put_u32(p + 4, t->n_cells); p += 8;

    memcpy(p, "rows", 4); put_u32(p + 4, (uint32_t)rows_n); p += 8;
    for (uint32_t i = 0; i < t->n_rows; ++i) put_u32(p + (size_t)i * 4u, t->rows[i]);
    p += rows_n;

    memcpy(p, "cell", 4); put_u32(p + 4, (uint32_t)cell_n); p += 8;
    for (uint32_t i = 0; i < t->n_cells; ++i) put_u32(p + (size_t)i * 4u, t->key[i]);
    for (uint32_t i = 0; i < t->n_cells; ++i) {
        union { float f; uint32_t u; } cv;
        cv.f = t->cost[i];
        put_u32(p + (size_t)t->n_cells * 4u + (size_t)i * 4u, cv.u);
    }

    *out = buf;
    *out_n = total;
    return SPFY_OK;
}
