/* G.711 u-law decoder. Standard ITU formula, no engine-specific quirks
 * known. Plan gap #7 calls for verifying the engine's actual LUT against
 * this; for now this is the textbook reference. */

#include "ulaw.h"

/* G.711 u-law decode: from a u-law byte to a 14-bit signed magnitude
 * sample, then sign-extended to 16-bit. Bias = 0x84 = 132.
 * Reference: ITU-T G.711, Table 2a. */
int16_t spfy_ulaw_decode_byte(uint8_t b)
{
    /* u-law uses inverted bits: invert the byte. */
    b = (uint8_t)~b;
    int sign = (b & 0x80) ? -1 : 1;
    int exponent = (b >> 4) & 0x07;
    int mantissa =  b       & 0x0F;
    int magnitude = ((mantissa << 3) + 0x84) << exponent;
    magnitude -= 0x84;
    return (int16_t)(sign * magnitude);
}

void spfy_ulaw_decode(const uint8_t *src, size_t n, int16_t *dst)
{
    static int16_t lut[256];
    static int initialized = 0;
    if (!initialized) {
        for (int i = 0; i < 256; ++i) lut[i] = spfy_ulaw_decode_byte((uint8_t)i);
        initialized = 1;
    }
    for (size_t i = 0; i < n; ++i) dst[i] = lut[src[i]];
}
