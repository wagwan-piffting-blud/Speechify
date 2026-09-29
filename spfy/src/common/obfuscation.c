#include "obfuscation.h"

#include <string.h>

/* Word-at-a-time: the 32-bit x87 build has no SSE to vectorize a byte loop,
 * and every voice load runs this over the whole vin + vdb (~90 MB for tom). */
void spfy_unobfuscate_ce(uint8_t *buf, size_t n)
{
    const uint32_t k = 0x01010101u * (uint32_t)SPFY_OBFUSCATION_BYTE;
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32_t w;
        memcpy(&w, buf + i, 4);
        w ^= k;
        memcpy(buf + i, &w, 4);
    }
    for (; i < n; ++i) buf[i] = (uint8_t)(buf[i] ^ SPFY_OBFUSCATION_BYTE);
}

void spfy_unobfuscate_ce_copy(uint8_t *dst, const uint8_t *src, size_t n)
{
    for (size_t i = 0; i < n; ++i) dst[i] = (uint8_t)(src[i] ^ SPFY_OBFUSCATION_BYTE);
}
