#ifndef SPFY_COMMON_RIFF_H
#define SPFY_COMMON_RIFF_H

#include <stddef.h>
#include <stdint.h>

/* RIFF reader. */

#define SPFY_FOURCC(a,b,c,d)  \
    ((uint32_t)(uint8_t)(a)        | \
     ((uint32_t)(uint8_t)(b) << 8) | \
     ((uint32_t)(uint8_t)(c) <<16) | \
     ((uint32_t)(uint8_t)(d) <<24))

#define SPFY_FOURCC_RIFF SPFY_FOURCC('R','I','F','F')
#define SPFY_FOURCC_LIST SPFY_FOURCC('L','I','S','T')

typedef struct {
    uint32_t          fourcc;
    uint32_t          size;
    const uint8_t    *data;
} spfy_chunk;

typedef struct {
    const uint8_t *base;
    const uint8_t *cur;
    const uint8_t *end;
} spfy_riff_iter;

/* Initialise iter over [data, data+n). */
void spfy_riff_iter_init(spfy_riff_iter *it, const uint8_t *data, size_t n);

/* Pull the next chunk. */
int  spfy_riff_iter_next(spfy_riff_iter *it, spfy_chunk *out);

void spfy_fourcc_str(uint32_t fourcc, char out[5]);

#endif
