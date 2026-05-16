#include "obfuscation.h"

void spfy_unobfuscate_ce(uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; ++i) buf[i] = (uint8_t)(buf[i] ^ SPFY_OBFUSCATION_BYTE);
}

void spfy_unobfuscate_ce_copy(uint8_t *dst, const uint8_t *src, size_t n)
{
    for (size_t i = 0; i < n; ++i) dst[i] = (uint8_t)(src[i] ^ SPFY_OBFUSCATION_BYTE);
}
