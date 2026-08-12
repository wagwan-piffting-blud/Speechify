#ifndef SPFY_COMMON_LE_H
#define SPFY_COMMON_LE_H

#include <stdint.h>

/* Unaligned little-endian scalar reads. */

static inline uint16_t spfy_le_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t spfy_le_u32(const uint8_t *p)
{
    return (uint32_t)p[0]         | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline float spfy_le_f32(const uint8_t *p)
{
    union { uint32_t u; float f; } v;
    v.u = spfy_le_u32(p);
    return v.f;
}

#endif
