#ifndef SPFY_COMMON_RIFF_H
#define SPFY_COMMON_RIFF_H

#include <stddef.h>
#include <stdint.h>

/* RIFF reader. Walks 'fourcc + u32 size + payload + pad-to-even' chunks.
 * Top-level RIFF/LIST containers carry a form-type fourcc as the first 4
 * bytes of payload. See reveng/README_TECHNICAL.md for the Speechify
 * VIN ('svin') and VDB ('svdb' wrapping 'WAVE') containers. */

#define SPFY_FOURCC(a,b,c,d)  \
    ((uint32_t)(uint8_t)(a)        | \
     ((uint32_t)(uint8_t)(b) << 8) | \
     ((uint32_t)(uint8_t)(c) <<16) | \
     ((uint32_t)(uint8_t)(d) <<24))

#define SPFY_FOURCC_RIFF SPFY_FOURCC('R','I','F','F')
#define SPFY_FOURCC_LIST SPFY_FOURCC('L','I','S','T')

typedef struct {
    uint32_t          fourcc;
    uint32_t          size;       /* payload size, excludes header & pad byte */
    const uint8_t    *data;       /* points into the parent buffer */
} spfy_chunk;

typedef struct {
    const uint8_t *base;
    const uint8_t *cur;
    const uint8_t *end;
} spfy_riff_iter;

/* Initialise iter over [data, data+n). For top-level RIFF files pass the
 * buffer starting AT the form-type fourcc (i.e. data+12 of the file). */
void spfy_riff_iter_init(spfy_riff_iter *it, const uint8_t *data, size_t n);

/* Pull the next chunk. Returns 1 on success, 0 on end-of-buffer.
 * On success, *out is populated and the iterator advances past the chunk
 * (including any trailing pad byte). Returns -1 on truncation/format error. */
int  spfy_riff_iter_next(spfy_riff_iter *it, spfy_chunk *out);

/* Convenience: stringify a fourcc into a 5-byte buffer (4 chars + NUL). */
void spfy_fourcc_str(uint32_t fourcc, char out[5]);

#endif
