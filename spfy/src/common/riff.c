#include "riff.h"

#include <string.h>

void spfy_riff_iter_init(spfy_riff_iter *it, const uint8_t *data, size_t n)
{
    it->base = data;
    it->cur  = data;
    it->end  = data + n;
}

static uint32_t le_u32(const uint8_t *p)
{
    return (uint32_t)p[0]        |
           ((uint32_t)p[1] << 8 ) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

int spfy_riff_iter_next(spfy_riff_iter *it, spfy_chunk *out)
{
    if (it->cur >= it->end) return 0;
    if ((size_t)(it->end - it->cur) < 8) return -1;

    uint32_t fcc  = le_u32(it->cur);
    uint32_t size = le_u32(it->cur + 4);

    const uint8_t *payload = it->cur + 8;
    if (payload > it->end || size > (size_t)(it->end - payload)) return -1;

    out->fourcc = fcc;
    out->size   = size;
    out->data   = payload;

    /* Advance past payload + RIFF pad-to-even byte. */
    size_t step = (size_t)size + ((size & 1u) ? 1u : 0u);
    it->cur = payload + step;
    if (it->cur > it->end) it->cur = it->end;
    return 1;
}

void spfy_fourcc_str(uint32_t fourcc, char out[5])
{
    out[0] = (char)(fourcc        & 0xFFu);
    out[1] = (char)((fourcc >> 8 )& 0xFFu);
    out[2] = (char)((fourcc >> 16)& 0xFFu);
    out[3] = (char)((fourcc >> 24)& 0xFFu);
    out[4] = '\0';
}
